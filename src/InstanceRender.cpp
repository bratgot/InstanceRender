// InstanceRender.cpp
//
// InstanceRender: a renderer for Nuke's USD 3D system (Nuke 15+, preview on
// 14.1) that keeps PointInstancer / instanceable copies as INSTANCES while
// rendering - CPU (Embree 4, two-level BVH) and GPU (OptiX, GAS per prototype
// + IAS) run the same shading kernel (src/ir/Kernel.h) for parity.
//
// Inputs: scn = the stage (any GeomOp / GeometryProviderI), cam = optional
// Camera (falls back to a UsdGeomCamera prim in the stage), bg = optional
// background image (gives the format).  No extra nodes: materials are the
// stage's UsdPreviewSurfaces, lights are the stage's UsdLux lights.
//
// Strict ASCII.  Created by Marten Blumen.

#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/ViewerContext.h"
#if IR_NUKE_VER >= 1600
#include "DDImage/GeometryProviderI.h"    // the USD 3D system, 16.0 and later
#endif
#include "DDImage/Knob.h"
#include "DDImage/Hash.h"
#include "DDImage/Thread.h"
#include "DDImage/Channel.h"
#include "DDImage/Format.h"
#include "DDImage/CameraOp.h"
#include "DDImage/MetaData.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/DeepOp.h"
#if IR_NUKE_VER >= 1601
#include "DDImage/OpState.h"      // 16.1 and later only; nothing here needs it before that
#endif
#include "DDImage/OpMessage.h"
#include "DDImage/OpMessageHandler.h"
#include "DDImage/ddImageVersionNumbers.h"
#include "DDImage/Application.h"

#include "ir/NukeCompat.h"     // everything that moved between Nuke 14.1 and 17

#if IR_HAVE_USD
#include <pxr/usd/usd/stage.h>
#include "ir/StageLoader.h"
#include "ir/Blackbody.h"
#include "ir/Texture.h"
#endif

#include "ir/Scene.h"
#include "ir/Project.h"
#include "ir/Crypto.h"
#if IR_HAVE_OPTIX
#include "ir/Denoise.h"
#endif
#include "ir/GeoLoader.h"
#include "ir/Trace.h"
#include "ir/Env.h"
#include "ir/Watchdog.h"
#include "ir/MatchInstances.h"
#include "ir/NukeOpImage.h"
#include "ir/EmbreeBackend.h"
#if IR_HAVE_OPTIX
#  include "ir/OptixBackend.h"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <cctype>
#include <map>
#include <mutex>
#include <functional>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

using namespace DD::Image;

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

// The plugin's own directory is not on the DLL search path; embree4.dll (and the
// CUDA runtime for the GPU build) are delay-loaded, so load them from next to
// InstanceRender.dll before anything touches them.
struct SideBySideLoader {
  SideBySideLoader()
  {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&sideBySideAnchor), &self)) {
      wchar_t path[MAX_PATH];
      const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
      if (n > 0 && n < MAX_PATH) {
        std::wstring dir(path);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        // tbb12 FIRST: Embree 4 needs oneTBB, and only Nuke 17 ships it - the
        // older versions carry the pre-oneTBB tbb.dll, so embree4.dll would
        // fail to load there and take the render down with it.  Where a tbb12
        // is already in the process (Nuke 17), that one is left alone.
        if (!GetModuleHandleW(L"tbb12.dll")) LoadLibraryW((dir + L"tbb12.dll").c_str());
        const wchar_t* names[] = { L"embree4.dll", L"cudart64_12.dll" };
        for (int i = 0; i < 2; ++i) LoadLibraryW((dir + names[i]).c_str());
      }
    }
    // usd_hio (image reading for textures / dome HDRIs) is NOT one of the USD
    // libraries Nuke loads at startup, and FnUSD/lib is not on the DLL search
    // path - so it is delay-loaded and fetched from next to the running Nuke.
    wchar_t exe[MAX_PATH];
    const DWORD ne = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (ne > 0 && ne < MAX_PATH) {
      std::wstring dir(exe);
      const size_t slash = dir.find_last_of(L"\\/");
      if (slash != std::wstring::npos) dir.resize(slash + 1);
      LoadLibraryW((dir + L"FnUSD/lib/usd_hio.dll").c_str());
    }
#endif
  }
  static void sideBySideAnchor() {}
};
static SideBySideLoader g_sideBySideLoader;

#define IR_VERSION "0.52.0"
const char* const kClass = "InstanceRender";
const char* const kHelp =
  "@b;InstanceRender@n; renders Nuke's USD 3D scene (GeoScene, GeoImport, CopyToPointsUSD ...) "
  "and keeps instances as instances: PointInstancers and instanceable copies share one prototype "
  "in the BVH, so thousands of copies cost a transform each, not a mesh each.\n\n"
  "scn: the stage.  cam: optional Camera (else the first UsdGeomCamera in the stage).  "
  "bg: optional background image (sets the format).\n\n"
  "Materials: UsdPreviewSurface, including the UsdUVTexture maps feeding diffuseColor, emissiveColor, "
  "roughness, metallic, opacity and normal.  Lights: UsdLux distant / sphere / rect / disk / dome "
  "(with its lat-long HDRI, importance sampled).  A headlight is used when the stage has no light.\n\n"
  "device: CPU (Embree) or GPU (OptiX) - both run the same kernel.  "
  "Output: rgba, depth.Z, N (normal), instance (id), albedo.";

// debug trace: IR_LOG, see ir/Trace.h.  This is a forwarder so that the
// loaders can write to the same file - a stall inside one of them used to
// trace as a gap between two lines with nothing in between.
void irlog(const std::string& msg) { ir::trace(msg); }

// The op instance a panel talks to is not the instance Nuke renders with, so the
// render report is published per Node and picked up when the panel refreshes -
// otherwise the info knob always shows the previous render.  Entries are not
// erased when an op dies: op instances come and go constantly while the Node
// (and its panel) lives on, and one string per node is not worth reclaiming.
std::mutex& reportMutex() { static std::mutex m; return m; }
std::map<const Node*, std::string>& reportStore() { static std::map<const Node*, std::string> m; return m; }

void publishReport(const Node* n, const std::string& text)
{
  if (!n) return;
  std::lock_guard<std::mutex> lock(reportMutex());
  reportStore()[n] = text;
}

std::string fetchReport(const Node* n)
{
  std::lock_guard<std::mutex> lock(reportMutex());
  std::map<const Node*, std::string>::const_iterator it = reportStore().find(n);
  return (it == reportStore().end()) ? std::string() : it->second;
}

// IR_EXECUTING is set by the before/afterRender callbacks in nuke/menu.py: while
// a Write is executing, progressive refinement must not hand back a preview pass.
// Which thread is this? Only ever used to label a log line, so the exact shape
// does not matter - but GetCurrentThreadId is Win32 and this is the only reason
// the file needed <windows.h> in three places.
unsigned long long currentThreadId()
{
#ifdef _WIN32
  return static_cast<unsigned long long>(GetCurrentThreadId());
#else
  return static_cast<unsigned long long>(
      std::hash<std::thread::id>()(std::this_thread::get_id()));
#endif
}

bool envFlag(const char* name)
{
#ifdef _WIN32
  char buf[16];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && n < sizeof(buf) && buf[0] != '0';
#else
  const char* v = std::getenv(name);
  return v && v[0] && v[0] != '0';
#endif
}

// An error raised by an upstream op while a stage is being built belongs in our
// report: it is the only way to tell which build provoked it, and the user
// otherwise just sees a red node somewhere else in the tree.
std::string upstreamError(Op* op)
{
  if (!op) return std::string();
  if (!op->opOrChildHasError()) return std::string();
  const Op* bad = op->getErroredOp();
  if (!bad) bad = op;
  std::string text;
  const DD::Image::OpMessage* msg = bad->getMsgHandler().getFirstError();
  if (msg) text = msg->getText();
  std::string name = bad->node_name();
  if (name.empty()) name = bad->Class() ? bad->Class() : "upstream";
  return name + ": " + (text.empty() ? "error" : text);
}

// "refresh render" has to reach the op that renders, which is not the op the
// panel talks to - so the counter lives per Node, exactly like the report, and
// goes into the content hash: bumping it invalidates Nuke's cache of this node
// (and any progressive accumulation) without touching anything else.
std::map<const Node*, unsigned>& renderEpochStore() { static std::map<const Node*, unsigned> m; return m; }

unsigned renderEpoch(const Node* n)
{
  std::lock_guard<std::mutex> lock(reportMutex());
  std::map<const Node*, unsigned>::const_iterator it = renderEpochStore().find(n);
  return (it == renderEpochStore().end()) ? 0u : it->second;
}

void bumpRenderEpoch(const Node* n)
{
  if (!n) return;
  std::lock_guard<std::mutex> lock(reportMutex());
  ++renderEpochStore()[n];
}

enum { kShutterCentred = 0, kShutterStart = 1, kShutterEnd = 2 };
const char* const kShutterNames[] = { "centred", "start", "end", nullptr };
const char* const kMotionUnitNames[] = { "per frame", "shutter", nullptr };
// What a cryptomatte name selects: the mesh, or the single copy.
const char* const kCryptoIdNames[] = { "object (one per mesh)", "copy (one per instance)", nullptr };
enum { kCryptoIdObject = 0, kCryptoIdCopy = 1 };

enum { kDeviceCpu = 0, kDeviceGpu = 1, kDeviceAuto = 2 };
const char* const kDeviceNames[] = { "CPU (Embree)", "GPU (OptiX)", "auto (GPU if available)", nullptr };

} // namespace

// A DUAL Iop / DeepOp: the same render, offered flat or deep.
//
// DeepOp.h says in as many words that it is an interface "so that there can be
// dual DeepOp/Iops", which is exactly what a renderer wants - one node, and
// whoever is downstream decides which face of it they want.  The render happens
// once either way; doDeepEngine() serves deep tiles out of the same framebuffer
// engine() serves rows out of.
class InstanceRender : public Iop, public DD::Image::DeepOp
{
public:
  static const Description description;
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }

  explicit InstanceRender(Node* node);

  // ---- the deep face of this node -------------------------------------------
  DD::Image::Op* op() override { return this; }
  bool doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels,
                    DD::Image::DeepOutputPlane& plane) override;
  void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels,
                       int count, std::vector<DD::Image::RequestData>& reqData) override;
  ~InstanceRender() override;

  int minimum_inputs() const override { return 3; }
  int maximum_inputs() const override { return 3; }
  const DD::Image::MetaData::Bundle& _fetchMetaData(const char* key) override;
  void build_handles(DD::Image::ViewerContext* ctx) override;
  // Nuke ASKS before it builds, and if the answer is "no handles" it never calls
  // build_handles at all - which is why the card's handles were missing over the
  // 2D render however right the projection was: the function was never reached.
  // Traced with IR_LOG, which showed not one call.
  //
  // The default answer is the most any input or knob claims, and in 2D that
  // comes back as nothing: Nuke does not look through an Iop at the 3D ops
  // feeding it.  ScanlineRender's base class overrides this for the same reason.
  // Cooked, because the projection is built from the camera this node validated.
  DD::Image::Op::HandlesMode doAnyHandles(DD::Image::ViewerContext* ctx) override
  {
    const DD::Image::Op::HandlesMode base = Iop::doAnyHandles(ctx);
    return (base != DD::Image::Op::eNoHandles) ? base : DD::Image::Op::eHandlesCooked;
  }
  // world -> pixel for the camera this node renders through, so the geometry's
  // own handles can be drawn over the 2D render - see build_handles()
  bool worldToPixelMatrix(DD::Image::Matrix4& out);
#if IR_NUKE_VER >= 1600
  // What makes the 3D viewport keep drawing while this node is being viewed.
  //
  // Nuke's 3D view draws the scene of the op you are looking at, and it finds
  // it by asking the op for a geometry provider.  A GeoCard has one; so does
  // ScanlineRender2, which is why viewing either of them keeps the card on
  // screen.  This node had none, so the viewport went empty and it looked like
  // the node had broken it - measured: GeoCard yes, ScanlineRender2 yes, this
  // node no, which is exactly the set of nodes that do and do not draw.
  //
  // So it hands on what it was given.  The stage behind this node IS the scene
  // being rendered, and saying so costs nothing.
#if IR_NUKE_VER >= 1701
  DD::Image::GeometryProviderI* geometryProvider() override;
#else
  // Plain forwarding.  Enough to be recognised as a geometry source, NOT enough
  // to make the 3D viewport draw - for that the provider has to name this node
  // as its owner, which needs the ForwardedGeometry below, and that interface
  // has a different shape in every version so far (16.0 const, 17.0 non-const,
  // 17.1 taking a NodeEvalContext).  Only 17.1 is implemented, being the one it
  // could be tested on.
  DD::Image::GeometryProviderI* geometryProvider() override
  {
    return input(0) ? input(0)->geometryProvider() : nullptr;
  }
#endif
  // Forwarded for the same reason as the provider: the viewer may look for the
  // 3D op rather than the interface.  GeoCard has one and ScanlineRender2 does
  // not, so this is not what decides whether the card is drawn - but it costs
  // nothing to be as visible as the geometry we are rendering.
  DD::Image::GeomOp* geomOp() override
  {
    return input(0) ? input(0)->geomOp() : nullptr;
  }
  const DD::Image::GeomOp* geomOp() const override
  {
    const Op* in = input(0);
    return in ? in->geomOp() : nullptr;
  }
#if IR_NUKE_VER >= 1601
  // SceneOpI arrived in 16.1; 16.0 has geometryProvider and geomOp only
  DD::Image::SceneOpI* sceneOpI() override
  {
    return input(0) ? input(0)->sceneOpI() : nullptr;
  }
  const DD::Image::SceneOpI* sceneOpI() const override
  {
    const Op* in = input(0);
    return in ? in->sceneOpI() : nullptr;
  }
#endif
  const DD::Image::GeometryProviderI* geometryProvider() const override
  {
    return const_cast<InstanceRender*>(this)->geometryProvider();
  }
#endif
  bool test_input(int input, Op* op) const override;
  Op* default_input(int input) const override;
  const char* input_label(int input, char* buffer) const override;
  // NB: never return 0 here - Nuke then may not build the op on that input at all
  // (Op.h: "the pointer may be null"), which silently kills the scn / cam inputs.
  float uses_input(int input) const override { return input == 2 ? 1.0f : 0.005f; }

  void knobs(Knob_Callback f) override;
  void appendContent(Hash& hash);          // everything except the progressive counter
  int knob_changed(Knob* k) override;
  void append(Hash& hash) override;
  void _validate(bool for_real) override;
  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override;
  void _open() override;
  void engine(int y, int x, int r, ChannelMask channels, Row& row) override;
  void _close() override;

private:
  bool loadScene(std::string& err);          // picks the front end for what is connected
  bool loadClassicScene(std::string& err);   // Nuke's classic 3D system (GeoOp)
#if IR_HAVE_USD
  bool loadStageScene(std::string& err);     // the USD stage
#endif
  void cameraFromInput(int W, int H, double pixelAspect);
  bool nukeOpTexture(const std::string& path, int maxSize, ir::ImageData& out);
  bool cameraAtFrame(double frame, int W, int H, double pixelAspect, ir::Camera& out);
  bool renderFrame();               // false if Nuke cancelled it
  void prepareScene();
  void settlePreparedScene();              // may this scene be remembered?              // graph work, at validate time only
  std::string deviceReport() const;

  // knobs
  int    _device;
  int    _samples;
  int    _maxBounces;
  int    _seed;
  double _clamp;
  float  _bgColor[3];
  bool   _bgVisible;
  bool   _denoise;
  bool   _outDepth, _outNormal, _outInstanceId, _outAlbedo;
  bool   _outPosition, _outMotion, _outUV, _outMaterialId, _outObjectId, _outLighting;
  bool   _outSurface, _outGeoNormal, _outOcclusion, _outShadow, _outLightGroups;
  bool   _outCrypto;
  int    _cryptoLayers;
  int    _cryptoId;
  bool   _deepPerObject;
  int    _deepSlots;
  int    _occlusionSamples;
  double _occlusionDistance;
  DD::Image::FormatPair _format;   // "format" knob: what size to render at
  int    _motionUnits;
  const char* _cameraPath;
  bool   _includeGuides;
  bool   _lightsVisible;
  bool   _reportStageErrors;
  bool   _textures;
  bool   _mipFilter;
  int    _maxTextureSize;
  int    _subdivLevels;
  int    _pointDetail;
  int    _curveSides;
  int    _curveSegments;
  double _shutter;
  int    _shutterOffset;
  int    _motionSamples;
  int    _motionSteps;
  bool   _deformationBlur;
  bool   _cameraBlur;
  int    _volumeSteps;
  int    _volumeShadowSteps;
  bool   _volumeBlur;
  int    _volumeOctaves;
  int    _volumeDeepSegments;
  bool   _spectralBlackbody;
  double _volumeAlbedo;
  double _motionOutlier;
  bool   _useVelocities;
  bool   _progressive;
  int    _previewSamples;
  double _maxTriangles;      // millions
  double _headlight;
  const char* _infoText;

  // state
  ir::Scene        _scene;
  ir::FrameBuffers _fb;
  ir::CpuRenderer  _cpu;
#if IR_HAVE_OPTIX
  ir::Denoiser     _denoiser;
#endif
#if IR_HAVE_OPTIX
  ir::GpuRenderer  _gpu;
#endif
  Hash   _renderedHash;
  // progressive refinement: the running mean and the pass count for one content hash
  std::vector<float> _accumRgba, _accDepth, _accNormal, _accInstance, _accAlbedo, _accExtra;
  int      _accumSamples;
  Hash     _accumHash;
  unsigned _progressCounter;
  bool     _progressiveActive() const;
  bool   _rendered;
  bool   _sceneOk;
  int    _usedDevice;
  std::string _status;
  // The Nuke ops the last load baked nkop: textures from.
  //
  // Nuke does not maintain the light's inputs:texture:file after the input is
  // made, so grading the node feeding a GeoDomeLight changes nothing that this
  // node's hash could see and the viewer never asks for another frame. Hashing
  // the ops THEMSELVES fixes that: the next time anything nudges the viewer,
  // the hash has moved and the frame catches up.
  std::vector<std::string> _nkopOps;
  mutable std::mutex _nkopMutex;
  std::string _sceneInfo;
  std::string _stageDiag;      // upstream errors seen while building the stages
  // built by prepareScene() in _validate(), consumed by renderFrame()
  bool  _scenePrepared = false;
  // one automatic retry after a load that was cut short - see settlePreparedScene()
  bool  _retriedCutShort = false;
  int   _cutShortInjected = 0;      // IR_CUT_SHORT self-test, see settlePreparedScene()
  int   _textureFailInjected = 0;  // IR_TEXTURE_FAIL self-test
  bool  _injectedCutShort = false;  // this load was cut short on purpose
  Hash  _sceneHash;
  std::string _sceneErr;

#if IR_NUKE_VER >= 1701
  // the geometry this node hands to the viewer, on its own behalf - see
  // ForwardedGeometry below
  std::unique_ptr<class ForwardedGeometry> _geometry;
#endif

#if IR_HAVE_USD
  // textures survive a scene rebuild, so dragging a handle does not re-bake and
  // re-mip an image that has not changed
  ir::TextureCache _textureCache;
#endif

  std::mutex _mutex;
  std::atomic<bool> _cancel;      // set while Nuke wants this render stopped
  bool _lastRenderAborted = false;  // did the render that just ran get cut short?
  Channel _chanN[3], _chanInstance, _chanAlbedo[3];
  Channel _chanP[3], _chanMotion[2], _chanUV[2], _chanMatId, _chanObjId;
  Channel _chanLight[5][3];        // direct/indirect diffuse, direct/indirect specular, emission
  Channel _chanSurface[4];         // roughness, metallic, opacity, facing
  Channel _chanSpecCol[3];         // reflectance at normal incidence
  Channel _chanNg[3];              // geometric normal
  Channel _chanOcclusion;          // ambient occlusion
  Channel _chanShadow[3];          // direct light the geometry stopped
  // one per light plus a last one for everything else; built from the scene,
  // so these are the only channels here that are not known in the constructor
  std::vector<std::string> _lightGroupNames;
  std::vector<Channel> _chanLightGroup;      // 3 per group, flat
  // How many groups the LAYOUT has.  Kept separately, and atomic, because
  // aovLayout() is called from the render thread while _validate() may be
  // rewriting the names on the main thread - reading a std::vector someone else
  // is assigning is undefined, and it shows up as the kind of intermittent
  // wrongness that passes on its own and fails in a full sweep.
  std::atomic<int> _lightGroupCount;
  void updateLightGroups();

  // cryptomatte: the layers, and the manifest that says which name is which
  std::vector<Channel> _chanCrypto;          // 4 per layer, flat
  std::string _cryptoManifest;               // the json, rebuilt when the scene changes
  std::atomic<int> _cryptoSlots;             // (id, coverage) pairs the layout holds
  void updateCrypto();
  void mergeCryptoTables(std::vector<float>& acc, const std::vector<float>& add,
                         const ir::AovLayout& a, float wOld, float wNew) const;
  DD::Image::Hash _cryptoNamesHash;
  DD::Image::MetaData::Bundle _meta;
  ir::AovLayout _aov;              // the layout _fb was rendered with
  bool   _mvOnly;                  // motion vectors wanted, but the shutter is shut
  double _motionFrames;            // frames the motion keys span (0 = nothing moves)
  ir::AovLayout aovLayout() const;

  // Where one channel's value comes from.  engine() used to decide this with a
  // chain of else-ifs run for every channel of every pixel, which was fine for
  // ten channels and is not for what the AOVs are growing into.  Built once per
  // validate instead, and looked up.
  struct ChannelSource {
    enum Buffer { kRgba, kDepth, kNormal, kInstance, kAlbedo, kExtra };
    Buffer buffer = kRgba;
    int    offset = 0;      // component within that buffer's per-pixel stride
  };
  // Held as an IMMUTABLE SNAPSHOT, because _validate() builds it on the main
  // thread while engine() is reading it on render threads.  Reading a std::map
  // that another thread is rebuilding is undefined, and it behaved like it:
  // texture_test on 16.1 passed on its own and failed two or three checks in a
  // full sweep, differently each time.  Building a new map and swapping the
  // pointer under the lock means a reader either sees the old one whole or the
  // new one whole.
  typedef std::map<int, ChannelSource> ChannelSourceMap;
  std::shared_ptr<const ChannelSourceMap> _chanSource;
  void buildChannelSources(const DD::Image::ChannelSet& chans);
};

// ---------------------------------------------------------------------------
InstanceRender::InstanceRender(Node* node)
  : Iop(node)
  , _device(kDeviceAuto), _samples(16), _maxBounces(2), _seed(0), _clamp(0.0)
  , _bgVisible(false), _denoise(false), _outDepth(true), _outNormal(true), _outInstanceId(true), _outAlbedo(false)
  , _outPosition(false), _outMotion(false), _outUV(false), _outMaterialId(false), _outObjectId(false), _outLighting(false)
  , _outSurface(false), _outGeoNormal(false), _outOcclusion(false), _outShadow(false), _outLightGroups(false)
  , _outCrypto(false), _cryptoLayers(2), _cryptoId(kCryptoIdObject)
  , _deepPerObject(false), _deepSlots(4)
  , _cryptoSlots(0)
  , _lightGroupCount(0)
  , _occlusionSamples(8), _occlusionDistance(0.0)
  , _motionUnits(0), _mvOnly(false), _motionFrames(0.0)
  , _cameraPath(""), _includeGuides(false), _lightsVisible(true), _reportStageErrors(false), _textures(true), _mipFilter(true), _maxTextureSize(4096), _subdivLevels(0), _pointDetail(2), _curveSides(6), _curveSegments(4), _shutter(0.0), _shutterOffset(kShutterCentred), _motionSamples(2), _motionSteps(0), _deformationBlur(false), _cameraBlur(true), _volumeSteps(64), _volumeShadowSteps(16), _volumeAlbedo(0.9), _volumeBlur(true), _volumeOctaves(3), _volumeDeepSegments(8), _spectralBlackbody(true), _motionOutlier(8.0), _useVelocities(true)
  , _progressive(false), _previewSamples(2), _maxTriangles(500.0), _headlight(1.0), _infoText("")
  , _accumSamples(0), _progressCounter(0)
  , _rendered(false), _sceneOk(false), _usedDevice(kDeviceCpu)
{
  _bgColor[0] = _bgColor[1] = _bgColor[2] = 0.0f;
  _chanN[0] = getChannel("N.x"); _chanN[1] = getChannel("N.y"); _chanN[2] = getChannel("N.z");
  _chanInstance = getChannel("instance.id");
  _chanAlbedo[0] = getChannel("albedo.red"); _chanAlbedo[1] = getChannel("albedo.green"); _chanAlbedo[2] = getChannel("albedo.blue");
  _chanP[0] = getChannel("P.x"); _chanP[1] = getChannel("P.y"); _chanP[2] = getChannel("P.z");
  // "forward" is what ScanlineRender writes and what VectorBlur reads
  _chanMotion[0] = getChannel("forward.u"); _chanMotion[1] = getChannel("forward.v");
  // "uv" is Nuke's own layer for geometry attributes and cannot be added to an
  // Iop's channel set, so the pass goes out under USD's own name for it
  _chanUV[0] = getChannel("st.u"); _chanUV[1] = getChannel("st.v");
  _chanMatId = getChannel("material.id");
  _chanObjId = getChannel("object.id");
  {
    static const char* kLightLayers[5] = { "direct_diffuse", "indirect_diffuse", "direct_specular", "indirect_specular", "emission" };
    static const char* kRGB[3] = { "red", "green", "blue" };
    for (int i = 0; i < 5; ++i)
      for (int c = 0; c < 3; ++c)
        _chanLight[i][c] = getChannel((std::string(kLightLayers[i]) + "." + kRGB[c]).c_str());
  }
  {
    // One "surface" layer with four named channels rather than four
    // one-channel layers, the same shape as material.id and object.id.
    static const char* kSurface[4] = { "roughness", "metallic", "opacity", "facing" };
    for (int i = 0; i < 4; ++i)
      _chanSurface[i] = getChannel((std::string("surface.") + kSurface[i]).c_str());
    static const char* kRGB[3] = { "red", "green", "blue" };
    for (int c = 0; c < 3; ++c)
      _chanSpecCol[c] = getChannel((std::string("specular_color.") + kRGB[c]).c_str());
    static const char* kXYZ[3] = { "x", "y", "z" };
    for (int c = 0; c < 3; ++c)
      _chanNg[c] = getChannel((std::string("Ng.") + kXYZ[c]).c_str());
    _chanOcclusion = getChannel("occlusion.red");
    for (int c = 0; c < 3; ++c)
      _chanShadow[c] = getChannel((std::string("shadow.") + kRGB[c]).c_str());
  }
  irlog("ctor");
  ir::watchdogStart();
}

InstanceRender::~InstanceRender() {}

// Handles for the inputs while this node is viewed.  This is NOT what makes the
// 3D viewport draw - see geometryProvider() below, which is - but it is right
// on its own account: the camera and the scene should still offer their
// manipulators when this node is the one being looked at.
// The camera's world -> pixel matrix, exactly as the renderer projects.
//
// This is Kernel.h's projectToPixel() written as a matrix, and it has to STAY
// that: the whole point of it is that a handle lands on the pixels the geometry
// it belongs to produced.  So the numbers come from _scene.camera - what the
// render actually used - rather than from input 1, which is only one of the
// places a camera can come from (a stage camera named by "camera prim" is
// another).
bool InstanceRender::worldToPixelMatrix(DD::Image::Matrix4& out)
{
  // The projection itself lives in ir/Project.h, next to the kernel it has to
  // agree with, and is tested against it there.  This only copies it into the
  // matrix the viewer wants: DD::Image::Matrix4 names its elements aROWCOL, so
  // the row-major array maps across one for one.
  ir::Camera cam;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scene.hasCamera) return false;
    cam = _scene.camera;
  }
  float m[16];
  if (!ir::worldToPixelMatrix(cam, m)) return false;
  out.a00 = m[0];  out.a01 = m[1];  out.a02 = m[2];  out.a03 = m[3];
  out.a10 = m[4];  out.a11 = m[5];  out.a12 = m[6];  out.a13 = m[7];
  out.a20 = m[8];  out.a21 = m[9];  out.a22 = m[10]; out.a23 = m[11];
  out.a30 = m[12]; out.a31 = m[13]; out.a32 = m[14]; out.a33 = m[15];
  return true;
}

void InstanceRender::build_handles(DD::Image::ViewerContext* ctx)
{
  if (ctx && ctx->viewer_mode() != DD::Image::VIEWER_2D) {
    // the scene first, then the camera, so a selected camera draws over it
    if (Op* geo = input(0)) geo->build_handles(ctx);
    if (Op* cam = input(1)) cam->build_handles(ctx);
    // and this node's own knobs, so its handles still work
    build_knob_handles(ctx);
    return;
  }

  // 2D: the image is what is being looked at, but the geometry that made it
  // still has handles, and a card's bezier is only worth having if you can see
  // it over the render.  ScanlineRender draws them, and the mechanism is that a
  // handle is drawn through ctx->modelmatrix: put the camera's world-to-pixel
  // matrix there and 3D handles land on the pixels they produced.
  //
  // The default Iop traversal is what was happening before, and it draws the
  // same handles with NO projection - world units read as pixels, so a card a
  // couple of units across becomes a few pixels huddled at the bottom left
  // corner.  So this replaces that traversal rather than adding to it.
  DD::Image::Matrix4 worldToPixel;
  if (ctx && worldToPixelMatrix(worldToPixel)) {
    // compose, do not replace: whatever is already there is the pixel space of
    // anything between this node and the viewer
    const DD::Image::Matrix4 saved = ctx->modelmatrix;
    const int savedMode = ctx->transform_mode();
    // A 3D op asks what space it is drawing into, and in a 2D viewer the answer
    // is "2D, one unit is one pixel" - so it draws no 3D handles at all.  That
    // is what ViewerContext::transform_mode(int) is for: the header calls it
    // "some clever nodes can change the mode of their input", and this is the
    // case it means.  Telling the input it is drawing in 3D, with the matrix
    // above taking its 3D coordinates to pixels, is the whole trick.
    ctx->transform_mode(DD::Image::VIEWER_PERSP);
    ctx->modelmatrix = saved * worldToPixel;
    if (Op* geo = input(0)) geo->build_handles(ctx);
    if (Op* cam = input(1)) cam->build_handles(ctx);
    ctx->modelmatrix = saved;
    ctx->transform_mode(savedMode);
    build_knob_handles(ctx);
    return;
  }
  Iop::build_handles(ctx);
}

#if IR_NUKE_VER >= 1701
// The geometry this node offers the 3D viewport, on its OWN behalf.
//
// Handing back the input's provider was not enough, and the reason is one line
// of the interface: GeometryProviderI::getGeometryProviderOp() names the Op the
// provider belongs to.  Forwarding the GeoCard's provider therefore told the
// viewer "this geometry belongs to GeoCard1" while it was asking about
// InstanceRender1, and it drew nothing.  ScanlineRender2 has its OWN provider,
// which is why it draws - it exposes no more interfaces than this node did,
// which is what made the difference so hard to see.
//
// So this one says the geometry is ours and asks the input for the substance of
// it.  Nothing is copied: the layer and stage come straight from upstream.
class ForwardedGeometry : public DD::Image::GeometryProviderI
{
public:
  explicit ForwardedGeometry(InstanceRender* owner) : _owner(owner) {}

  DD::Image::GeometryProviderI* asGeometryProvider() override { return this; }

  DD::Image::Op* getGeometryProviderOp() override { return _owner; }

  // the state hashes decide when the viewer rebuilds: they must be the
  // UPSTREAM's, or the geometry would look unchanged when it is not
  fdk::Hash geometryComposeState(const ndk::NodeEvalContext& ctx) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src ? src->geometryComposeState(ctx) : fdk::Hash();
  }

  fdk::Hash geometryEditVersionState(const ndk::NodeEvalContext& ctx) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src ? src->geometryEditVersionState(ctx) : fdk::Hash();
  }

  bool geometryStateVaries(const ndk::NodeEvalContext& ctx) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src && src->geometryStateVaries(ctx);
  }

  usg::LayerRef buildGeometryLayerForContext(const ndk::NodeEvalContext& ctx,
                                             bool appendTo,
                                             const fdk::TimeValueSet& sampleTimes) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src ? src->buildGeometryLayerForContext(ctx, appendTo, sampleTimes) : usg::LayerRef();
  }

  usg::LayerRef getGeometryLayer(const ndk::NodeEvalContext& ctx,
                                 const fdk::TimeValueSet& sampleTimes) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src ? src->getGeometryLayer(ctx, sampleTimes) : usg::LayerRef();
  }

  bool canProvideGeometryStage() const override
  {
    const DD::Image::GeometryProviderI* src = upstream();
    return src && src->canProvideGeometryStage();
  }

  bool canProvideGeometryFor(const usg::Token& purpose) const override
  {
    const DD::Image::GeometryProviderI* src = upstream();
    return src ? src->canProvideGeometryFor(purpose) : false;
  }

  void buildGeometryStageForContext(usg::StageRef& stage,
                                    const usg::ArgSet& requestArgs,
                                    const ndk::NodeEvalContext& ctx,
                                    const fdk::TimeValueSet& sampleTimes) override
  {
    if (DD::Image::GeometryProviderI* src = upstream())
      src->buildGeometryStageForContext(stage, requestArgs, ctx, sampleTimes);
  }

  usg::StageRef getGeometryStage(const ndk::NodeEvalContext& ctx,
                                 const fdk::TimeValueSet& sampleTimes) override
  {
    DD::Image::GeometryProviderI* src = upstream();
    return src ? src->getGeometryStage(ctx, sampleTimes) : usg::StageRef();
  }

private:
  DD::Image::GeometryProviderI* upstream() const
  {
    DD::Image::Op* in = _owner ? _owner->input(0) : nullptr;
    return in ? in->geometryProvider() : nullptr;
  }
  InstanceRender* _owner;
};

DD::Image::GeometryProviderI* InstanceRender::geometryProvider()
{
  DD::Image::Op* in = input(0);
  if (!in || !in->geometryProvider()) return nullptr;   // nothing behind us to offer
  if (!_geometry) _geometry.reset(new ForwardedGeometry(this));
  return _geometry.get();
}
#endif  // IR_NUKE_VER >= 1701

bool InstanceRender::test_input(int input, Op* op) const
{
  if (input == 0) return ir::isGeometrySource(op);
  if (input == 1) return dynamic_cast<CameraOp*>(op) != nullptr;
  return dynamic_cast<Iop*>(op) != nullptr;
}

Op* InstanceRender::default_input(int input) const
{
  if (input == 2) return Iop::default_input(0);   // black background with the root format
  return nullptr;
}

const char* InstanceRender::input_label(int input, char*) const
{
  switch (input) { case 0: return "scn"; case 1: return "cam"; default: return "bg"; }
}

// ---------------------------------------------------------------------------
void InstanceRender::knobs(Knob_Callback f)
{
  Tab_knob(f, "InstanceRender");
  Named_Text_knob(f, "title", "", "<b><font size=+2>InstanceRender</font></b>");
  SetFlags(f, Knob::STARTLINE);
  Named_Text_knob(f, "subtitle", "", "<i>Instancing renderer for Nuke's USD 3D system - CPU (Embree) / GPU (OptiX), same kernel</i>");
  SetFlags(f, Knob::STARTLINE);
  Divider(f, "render");
  Enumeration_knob(f, &_device, kDeviceNames, "device", "device");
  Tooltip(f, "CPU: Embree 4 two-level BVH on all cores.  GPU: OptiX (GAS per prototype + IAS).  Both run the same "
             "shading kernel; auto picks the GPU when the build has OptiX and a device is present.");
  Int_knob(f, &_samples, IRange(1, 1024), "samples", "samples");
  Tooltip(f, "Paths per pixel.  1-4 for look-dev, 64+ for finals.");
  Int_knob(f, &_maxBounces, IRange(0, 16), "max_bounces", "max bounces");
  Tooltip(f, "0 = direct lighting only; 1-2 = cheap indirect; more = full path tracing.");
  Int_knob(f, &_seed, "seed", "seed");
  Tooltip(f, "Starting point for the sampling pattern.  Change it to get a different set of "
             "paths for the same scene, which is how you tell noise apart from something that "
             "is really there - the noise moves, the feature does not.");
  Float_knob(f, &_clamp, IRange(0.0, 100.0), "clamp", "clamp radiance");
  Tooltip(f, "Clamp per-sample radiance (kills fireflies); 0 = off.");
  Bool_knob(f, &_lightsVisible, "lights_visible", "area lights visible to camera");
  Tooltip(f, "Draw rect / disk / sphere lights where they actually are when a camera ray hits them.  "
             "Bounced rays always find lights through next event estimation instead, so this only "
             "affects what the camera sees directly.");
  Float_knob(f, &_headlight, IRange(0.0, 10.0), "headlight", "headlight (no lights in stage)");
  Tooltip(f, "When the stage has no UsdLux light, a distant light from the camera with this intensity is used (0 = none).");
  Bool_knob(f, &_denoise, "denoise", "denoise");
  Tooltip(f, "Run the OptiX AI denoiser over the finished frame, guided by the albedo and "
             "normal passes.  It needs a GPU but not the GPU renderer: a CPU render is "
             "denoised the same way.  The alpha is left alone - coverage is not noise.");
  Bool_knob(f, &_progressive, "progressive", "progressive (viewer)");
  Tooltip(f, "Refine in the viewer: the first pass renders 'preview samples' and the node keeps adding "
             "passes until it reaches 'samples', redrawing after each one.  Only in the GUI, and never "
             "while a Write is executing (the render always gets the full sample count in one pass).");
  Int_knob(f, &_previewSamples, IRange(1, 64), "preview_samples", "preview samples");
  Tooltip(f, "Paths per pixel in the first progressive pass; each following pass doubles until 'samples' "
             "is reached.");
  Button(f, "refresh_render", "refresh render");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS | Knob::STARTLINE);
  Tooltip(f, "Throw away this node's cached image and render it again - the scene is re-read from the "
             "input, so it also picks up anything upstream that changed without changing its hash.  "
             "Any progressive accumulation starts over.");
  // the credit sits at the foot of the tab, quietly
  Named_Text_knob(f, "credit", "",
                  "<font size=-2 color=\"#808080\">InstanceRender v" IR_VERSION
                  "&nbsp;&nbsp;&middot;&nbsp;&nbsp;Marten Blumen</font>");
  SetFlags(f, Knob::STARTLINE);
  // ---- Scene ------------------------------------------------------------------
  Tab_knob(f, "Scene");
  Divider(f, "motion blur");
  Double_knob(f, &_shutter, IRange(0.0, 2.0), "shutter", "shutter");
  Tooltip(f, "Shutter length in frames; 0 = no motion blur.  Instances are sampled at shutter open and "
             "close and interpolated by the acceleration structure itself (Embree instance time steps / "
             "OptiX matrix motion), so blurred copies stay instanced.  Transform motion only - deforming "
             "geometry is rendered at shutter open.");
  Int_knob(f, &_motionSamples, IRange(2, 8), "motion_samples", "motion samples");
  Tooltip(f, "Transforms sampled across the shutter.  2 is one straight segment between shutter open and "
             "close - enough for anything travelling in a line, but a spinning or arcing object needs more, "
             "and each one is another sub-frame the scene is evaluated at.");
  Divider(f, "volumes");
  Int_knob(f, &_volumeSteps, IRange(4, 512), "volume_steps", "volume steps");
  Tooltip(f, "How many times the camera ray is sampled as it crosses a volume. More is smoother "
             "and slower; too few shows as banding, which the jitter turns into noise rather than "
             "stripes.");
  Int_knob(f, &_volumeShadowSteps, IRange(1, 128), "volume_shadow_steps", "shadow steps");
  Tooltip(f, "The same, for the ray from a point inside the volume towards a light - which is "
             "what makes a volume shadow itself. Cheaper than the camera march and can be much "
             "coarser.");
  Double_knob(f, &_volumeAlbedo, IRange(0.0, 1.0), "volume_albedo", "volume albedo");
  Tooltip(f, "How much of what the volume absorbs comes back out as scattered light." "\n\n"
             "1 is smoke lit from outside; 0 absorbs everything and renders as a black shape, "
             "which is what a shadow-catching volume wants.");
  Bool_knob(f, &_spectralBlackbody, "spectral_blackbody", "spectral blackbody");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Work out a temperature's colour by integrating Planck's law against the CIE colour "
             "matching functions, rather than sampling it at one wavelength per channel.\n\n"
             "It is the colour an eye would see, and the difference shows at the cool end of a "
             "fireball - deep red where the cheap version goes pink. Both are built into a table "
             "once per render, so neither costs anything while marching.");
  Int_knob(f, &_volumeDeepSegments, IRange(1, 32), "volume_deep_segments", "deep segments");
  Tooltip(f, "How many deep samples a volume is cut into through its depth, when deep output is "
             "on.\n\n"
             "One sample would put the whole volume at a single distance, and geometry inside the "
             "smoke could not sit between its front and its back - which is most of the reason to "
             "render a volume deep at all. They come out of the same march, so this costs deep "
             "samples rather than time.");
  Int_knob(f, &_volumeOctaves, IRange(1, 8), "volume_multi_scatter", "multi scatter");
  Tooltip(f, "How much light that has BOUNCED inside the volume is put back.\n\n"
             "1 is single scattering: every step is lit only by what came straight from a light, "
             "which leaves thick smoke far too dark and far too contrasty - a cloud lit from "
             "behind should glow, not go black.\n\n"
             "Each step above 1 adds an octave that sees the volume as half as thick and carries "
             "half the energy. It costs nothing - the same shadow march is reused - so raise it "
             "until the cloud stops getting brighter.");
  Bool_knob(f, &_volumeBlur, "volume_motion_blur", "volume motion blur");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Blur a volume across the shutter by reading the NEXT frame of the sequence and "
             "cross-fading.\n\n"
             "A simulation carries no velocity to advect by - the grids are density, temperature "
             "and flames - so mixing the two frames is the only way to smear it, and without this "
             "a volume renders sharp while everything around it blurs.\n\n"
             "It reads a second .vdb per frame, which the grid cache mostly absorbs during "
             "playback since that frame is the next one wanted anyway. Nothing to read means "
             "nothing is spent: a single .vdb, or a shutter of 0, skips it.");
  Bool_knob(f, &_cameraBlur, "camera_motion_blur", "camera motion blur");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Blur what a MOVING CAMERA sweeps past, as well as what moves in front of it.\n\n"
             "A still scene shot by a moving camera still moves on screen, and no instance transform can say so - that describes geometry moving past a camera that is standing still. With this off, a pan renders identically with the shutter shut and wide open.\n\n"
             "The camera is read at shutter open and close and every ray takes its own instant in between, so a pan, a dolly and a zoom all blur. It needs a camera on input 2 that is actually animated; a still one costs nothing.");
  Int_knob(f, &_motionSteps, IRange(0, 16), "motion_steps", "shutter steps");
  Tooltip(f, "0 renders a smooth streak: every ray gets its own instant across the shutter.\n\n"
             "Anything else samples the shutter at that many FIXED instants instead, which is how "
             "ScanlineRender blurs - 2 puts a copy at each end of the shutter, 4 gives four copies. "
             "It is here to match a shot already graded against ScanlineRender.  Leave it at 0 "
             "otherwise: a streak needs far fewer samples to look clean than a row of copies does.");
  Bool_knob(f, &_useVelocities, "use_velocities", "use velocities");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Blur each object along the velocity its geometry carries, when it has one, instead of "
             "pairing objects between the two ends of the shutter.\n\n"
             "A velocity needs no partner, so nothing is left sharp - not even the particles born or "
             "dying inside the shutter, which pairing can never reach.  CopyToPoints supplies it for "
             "its copies, and particle systems are asked directly.\n\n"
             "Turn it off to go back to pairing: worth doing if the velocities in a scene do not "
             "describe the motion, though the renderer already checks that they agree with the "
             "pairing before preferring them.");
  Double_knob(f, &_motionOutlier, IRange(0.0, 32.0), "motion_outlier", "reject motion beyond");
  Tooltip(f, "How far a matched object may travel across the shutter before the match is "
             "disbelieved, as a multiple of the median travel of everything else.  Nuke's "
             "particles RECYCLE their ids - a particle that dies hands its id to one born "
             "elsewhere - and matching those two smears a streak across the frame.  Anything "
             "beyond this is held still instead.  0 = trust every match.");
  Bool_knob(f, &_deformationBlur, "deformation_blur", "deformation blur");
  Tooltip(f, "Blur geometry that changes shape as well as geometry that moves.  It costs a second full "
             "read of the scene at shutter close, and the two ends have to agree on vertex count, so it "
             "is off by default.  Two keys, whatever 'motion samples' says.");
  Enumeration_knob(f, &_shutterOffset, kShutterNames, "shutter_offset", "shutter offset");
  Tooltip(f, "Where the shutter sits around the frame: centred, opening at the frame, or closing on it.");
  Divider(f, "background");
  Color_knob(f, _bgColor, "bg_color", "background colour");
  Tooltip(f, "What a ray that hits nothing returns.  It lights the scene like a uniform dome, "
             "so it is not only what you see behind the geometry - lifting it lifts everything. "
             "Whether it is actually DRAWN behind the geometry is 'background visible' below.");
  Format_knob(f, &_format, "format", "format");
  Tooltip(f, "What size to render.  Left empty the node renders at the bg input's format, "
             "or the project format when nothing is connected there - which is what it always did.");
  Bool_knob(f, &_bgVisible, "bg_visible", "background visible");
  Tooltip(f, "Write the background colour (or the dome light) into rgb where nothing is hit; alpha stays 0 either way.");
  Divider(f, "camera and scene");
  String_knob(f, &_cameraPath, "camera_path", "camera prim");
  Tooltip(f, "UsdGeomCamera prim path used when no Camera is connected to cam (empty = first camera in the stage; "
             "none at all = a default camera looking at the origin).");
  Bool_knob(f, &_includeGuides, "include_guides", "render guide / proxy purposes");
  Tooltip(f, "USD marks prims with a purpose, and normally only 'render' and 'default' ones are "
             "drawn - guides, rigs and stand-in proxies are meant to stay out of the picture. "
             "Turn this on to render them too, which is mostly useful for seeing where a guide "
             "actually is.");
  // ---- Geometry ---------------------------------------------------------------
  Tab_knob(f, "Geometry");
  Divider(f, "points and curves");
  Int_knob(f, &_pointDetail, IRange(1, 6), "point_detail", "point detail");
  Tooltip(f, "How round a UsdGeomPoints point is drawn.  Every point shares ONE sphere and costs a "
             "transform, so this buys silhouette quality for almost nothing.");
  Int_knob(f, &_curveSides, IRange(3, 16), "curve_sides", "curve sides");
  Tooltip(f, "Sides of the tube a UsdGeomBasisCurves curve is swept into.");
  Int_knob(f, &_curveSegments, IRange(1, 16), "curve_segments", "curve segments");
  Tooltip(f, "Samples along each span of a cubic curve.  Linear curves use their control points as they are.");
  Int_knob(f, &_subdivLevels, IRange(0, 4), "subdiv_levels", "subdivision levels");
  Tooltip(f, "Catmull-Clark refinement for meshes whose subdivisionScheme is catmullClark (USD's fallback "
             "value) or bilinear.  0 renders the control cage, which is what every version before this one "
             "did.  Each level turns one face into four, so the triangle guard still applies.");
  Divider(f, "textures");
  Bool_knob(f, &_textures, "textures", "textures");
  Tooltip(f, "Read the UsdUVTexture images feeding UsdPreviewSurface (diffuseColor, emissiveColor, roughness, "
             "metallic, opacity, normal) and the dome light's HDRI.  Off = constants only.");
  Bool_knob(f, &_mipFilter, "mip_filter", "mip filtering");
  Tooltip(f, "Build a mip pyramid per texture and pick the level from the ray's footprint, so textures "
             "shrinking into the distance stop shimmering.  Costs about a third more texture memory.");
  Int_knob(f, &_maxTextureSize, IRange(64, 16384), "max_texture_size", "max texture size");
  Tooltip(f, "Textures larger than this are box-downsampled at load (an 8K HDRI is 500 MB as float RGBA).  "
             "0 = full resolution.");
  Divider(f, "limits");
  Float_knob(f, &_maxTriangles, IRange(0.0, 5000.0), "max_triangles", "max rendered triangles (M)");
  Tooltip(f, "Guard on instances x prototype triangles (what a non-instancing renderer would see).  Instancing "
             "keeps memory at prototype + transforms, but ray tracing time still scales with what is on screen.  "
             "0 = no guard.");
  // ---- AOVs -------------------------------------------------------------------
  Tab_knob(f, "AOVs");
  Named_Text_knob(f, "aov_note", "",
                  "<i>Write with <b>channels: all</b> and <b>32 bit float</b> - half float rounds "
                  "the ids in object.id and cryptomatte into the wrong objects.</i>");

  Divider(f, "geometry");
  Bool_knob(f, &_outDepth, "out_depth", "depth.Z");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Distance from the camera to the surface, in depth.Z.");
  Bool_knob(f, &_outNormal, "out_normal", "N");
  Tooltip(f, "The shading normal in N.x, N.y and N.z - world space, facing the camera side. "
             "This is the normal the lighting used, so it carries any normal map.");
  Bool_knob(f, &_outGeoNormal, "out_geometric_normal", "Ng");
  Tooltip(f, "The geometry's OWN normal, before any normal map: where N carries the mapped "
             "detail, Ng carries the shape. Useful when you want the form and not the texture.");
  Bool_knob(f, &_outPosition, "out_position", "P");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The world-space position of the surface the camera sees, in P.x, P.y and P.z.");
  Bool_knob(f, &_outUV, "out_uv", "st");
  Tooltip(f, "The surface's texture coordinates, in st.u and st.v - for re-texturing in the "
             "comp. Called st rather than uv because Nuke reserves the layer name \"uv\".");
  Bool_knob(f, &_outAlbedo, "out_albedo", "albedo");
  Tooltip(f, "The surface colour before any light touched it. Divide the beauty by it to get "
             "light alone, and it is what a denoiser wants alongside N.");

  Divider(f, "mattes");
  Bool_knob(f, &_outObjectId, "out_object_id", "object.id");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The prototype the surface belongs to: every copy of one mesh shares an object id, "
             "while instance.id tells the copies apart.");
  Bool_knob(f, &_outInstanceId, "out_instance", "instance.id");
  Tooltip(f, "Which COPY this is - the PointInstancer's own id where there is one, otherwise a "
             "running index. Use it to vary a grade across the copies.");
  Bool_knob(f, &_outMaterialId, "out_material_id", "material.id");
  Tooltip(f, "The index of the material the surface uses; -1 where nothing was hit.");
  Bool_knob(f, &_outCrypto, "out_cryptomatte", "cryptomatte");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Proper mattes, by NAME, with anti-aliased edges - CryptoObject00, CryptoObject01 "
             "and so on, plus the manifest that tells Nuke's Cryptomatte node which name is "
             "which.\n\n"
             "Pipe this into a Cryptomatte node and pick objects from its list, or click them "
             "in the viewer. Unlike object.id there is nothing to key and no fringing: a pixel "
             "on an edge carries both objects and how much of each.\n\n"
             "Every copy of one mesh shares a name, so picking it takes all of them.\n\n"
             "MUST be written as 32 bit float. Half float rounds the ids and they stop "
             "matching any name at all.");

  Divider(f, "lighting");
  Bool_knob(f, &_outLighting, "out_lighting", "lighting split");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Splits the beauty into direct_diffuse, indirect_diffuse, direct_specular, "
             "indirect_specular and emission. The five add back up to rgb exactly, so you can "
             "grade one and put it back.");
  Bool_knob(f, &_outLightGroups, "out_light_groups", "light groups");
  Tooltip(f, "One colour layer per light - light_<name> - carrying everything that light put "
             "into the picture, direct and indirect, at every bounce. Rebalance a rig in the "
             "comp without re-rendering.\n\n"
             "The layers ADD UP TO THE BEAUTY exactly, because everything goes in one: the "
             "last layer, light_other, carries surface emission, the background, and any "
             "lights past the first few (there is a fixed cap - a scene with more lights than "
             "that keeps the first ones separate and pools the rest).\n\n"
             "Named after the light, so RENAMING a light makes new layers; Nuke never forgets "
             "a channel name for the rest of the session. With a radiance clamp on, the layers "
             "are the light BEFORE clamping and will sum a little above the beauty.");
  Bool_knob(f, &_outShadow, "out_shadow", "shadow");
  Tooltip(f, "The direct light that geometry stopped from arriving - what is MISSING where a "
             "shadow is, rather than a matte of where one lies. Add it back to the beauty and "
             "the shadows go; grade it down to soften them.\n\n"
             "It costs no extra rays: the shadow ray was being traced anyway, and this is the "
             "same contribution worked out a second time as if nothing were in the way. Direct "
             "light at the first hit only, which is the shadow a comp means.");
  Bool_knob(f, &_outOcclusion, "out_occlusion", "occlusion");
  Tooltip(f, "Ambient occlusion: how open the sky is above the surface - 1 where nothing is in "
             "the way, 0 where it is closed in. For contact shadows and dirt.\n\n"
             "The ONLY pass here that costs rays of its own, so it is traced only when it is "
             "on. Set an occlusion distance below, or in an interior it reads as closed "
             "everywhere and tells you nothing.");

  Divider(f, "shading");
  Bool_knob(f, &_outSurface, "out_surface", "surface properties");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "surface.roughness, surface.metallic, surface.opacity and surface.facing, plus "
             "specular_color - the reflectance at normal incidence that the metallic and the "
             "specular workflow both come down to.\n\n"
             "surface.facing is the angle to the camera: 1 head on, 0 at a grazing edge, which "
             "is the falloff a rim or a fresnel grade wants. Note that surface.opacity is the "
             "MATERIAL's opacity - coverage is still alpha.");

  Divider(f, "motion");
  Bool_knob(f, &_outMotion, "out_motion", "forward (motion vectors)");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Screen-space motion in forward.u and forward.v, the channels ScanlineRender "
             "writes and VectorBlur reads. With the shutter shut the render stays sharp and "
             "the vectors are still measured, over the frame that follows - which is the usual "
             "way to blur in the comp instead of in the render.");

  BeginClosedGroup(f, "aov_settings", "pass settings");
  Enumeration_knob(f, &_motionUnits, kMotionUnitNames, "motion_units", "motion units");
  Tooltip(f, "per frame: pixels the surface travels in one frame (ScanlineRender's 'velocity').\n"
             "shutter: half the travel across the open shutter (ScanlineRender's 'distance').");
  Int_knob(f, &_occlusionSamples, "occlusion_samples", "occlusion rays");
  SetRange(f, 1, 64);
  Tooltip(f, "Rays per camera sample for the occlusion pass. They multiply with \"samples\", "
             "so 8 of these at 16 samples is 128 rays a pixel.");
  Double_knob(f, &_occlusionDistance, IRange(0.0, 100.0), "occlusion_distance", "occlusion distance");
  Tooltip(f, "How far an occlusion ray looks, in scene units. 0 = as far as the scene goes, "
             "which inside a closed room reads as fully occluded everywhere and says nothing. "
             "A distance is what makes the pass describe the surface rather than the room - "
             "start around a tenth of the scene's size.");
  Bool_knob(f, &_deepPerObject, "deep_per_object", "deep: one sample per object");
  Tooltip(f, "This node has a deep output as well as a flat one - connect a deep node to it "
             "and it is there. Off, every pixel carries ONE sample: correct deep, and it "
             "flattens back to the render exactly, but a pixel that sees two objects reports "
             "them as one surface.\n\n"
             "On, a pixel carries one sample per OBJECT it can see, which is what holdouts "
             "and deep merges need. It costs room in the buffer whether or not anything "
             "downstream reads it, which is why it is off by default.");
  Int_knob(f, &_deepSlots, "deep_slots", "deep samples per pixel");
  SetRange(f, 1, 8);
  Tooltip(f, "How many surfaces a pixel can report. A pixel that sees more keeps the ones "
             "covering most of it. 4 is plenty unless a lot of fine geometry overlaps.");
  Enumeration_knob(f, &_cryptoId, kCryptoIdNames, "crypto_id", "cryptomatte id");
  Tooltip(f, "What ONE picked matte covers.\n\n"
             "object - every copy of a mesh shares its name, so picking one takes the whole scatter. This is the ordinary CryptoObject.\n\n"
             "copy - each copy gets its own name, <prim>/<id>, so a click isolates the single copy under the cursor. The id is the point id it was scattered onto, so a copy keeps its matte from frame to frame even as particles are born and die around it.\n\n"
             "Per copy the manifest carries one entry per copy, so it grows with the scatter - the node says how large it got.");
  Int_knob(f, &_cryptoLayers, "crypto_layers", "cryptomatte layers");
  SetRange(f, 1, 3);
  Tooltip(f, "How many CryptoObject layers to write. Each one holds two objects per pixel, so "
             "2 layers cover 4 overlapping objects - enough for almost anything. Raise it only "
             "where a lot of fine geometry overlaps in one pixel, like hair or foliage.");
  EndGroup(f);

  // ---- Info -------------------------------------------------------------------
  Tab_knob(f, "Info");
  Bool_knob(f, &_reportStageErrors, "stage_errors", "report stage-build errors");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Off (the default) hides errors raised while the scene's USD stage is built.  Nuke's own "
             "material ops fail there - \"Can't set time sample on .../<node>_UVTexture.inputs:scale ... "
             "expected a value of type TfToken\" - which turns an upstream node red and aborts the render "
             "even though the render itself is fine.  Turn this on to see what the nodes upstream are "
             "actually saying.");
  Button(f, "refresh_info", "refresh");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Fill the box below with what the last render actually did - what was loaded, how "
             "long it took, and any warnings. Warnings only appear here, so this is where to "
             "look when a render comes out wrong but nothing turned red.");
  Button(f, "dump_freeze_report", "freeze report");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Writes a stack for every thread in this Nuke, for diagnosing a freeze.  "
             "Needs IR_WATCHDOG set before Nuke starts (see 'If Nuke seems to freeze' in the "
             "README); with it set, the same report is written automatically when a render - or "
             "Nuke's whole interface - stops for longer than the limit.");
  Multiline_String_knob(f, &_infoText, "info", "", 6);
  Tooltip(f, "The last render's report: instance and light counts, timings, and any warnings "
             "the scene produced. Press 'refresh' above to bring it up to date.");
  SetFlags(f, Knob::NO_ANIMATION | Knob::READ_ONLY | Knob::OUTPUT_ONLY | Knob::DO_NOT_WRITE | Knob::STARTLINE);
}

int InstanceRender::knob_changed(Knob* k)
{
  if (k && k->is("refresh_render")) {
    irlog("refresh_render pressed");
    bumpRenderEpoch(node());
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _rendered = false;
      _scenePrepared = false;
#if IR_HAVE_USD
      // and forget the kept textures: "refresh" is what a person presses when
      // something upstream changed WITHOUT changing its hash, and a texture
      // read from a file is exactly that case
      _textureCache = ir::TextureCache();
#endif
#if IR_HAVE_OPTIX
      _gpu.forgetTextures();     // and the copy of them on the device
#endif
      _accumRgba.clear(); _accumSamples = 0; _accumHash = Hash();
    }
    invalidateSameHash();      // the hash itself changes through appendContent
    asapUpdate();
    return 1;
  }
  if (k && k->is("dump_freeze_report")) {
    const std::string path = ir::watchdogDumpNow("asked for from the node's panel");
    if (Knob* kk = knob("info"))
      kk->set_text(path.empty()
                   ? "The watchdog is off: set IR_WATCHDOG=<seconds> before starting Nuke."
                   : ("freeze report written to " + path).c_str());
    return 1;
  }
  if (k && (k->is("refresh_info") || k->is("showPanel") || k->is("updateUI"))) {
    std::string text = fetchReport(node());
    if (text.empty()) {
      std::lock_guard<std::mutex> lock(_mutex);
      text = _sceneInfo + (_sceneInfo.empty() ? "" : "\n") + _status;
    }
    if (text.empty()) text = "(not rendered yet - view or render the node)";
    if (Knob* kk = knob("info")) kk->set_text(text.c_str());
    return 1;
  }
  return Iop::knob_changed(k);
}

// Defined below, next to the nkop: notes; needed here because the content hash
// depends on the ops those paths name.
static std::string nukeOpNameFromPath(const std::string& path);
static DD::Image::Iop* findIopByName(DD::Image::Op* root, const std::string& name, int depth);

void InstanceRender::append(Hash& hash)
{
  appendContent(hash);
  // asapUpdate() only redraws if the hash changes (Op.h), so the pass counter is
  // part of it - which is exactly why the accumulation is keyed off the CONTENT
  // hash instead, or every refinement pass would look like a new scene.
  hash.append(_progressCounter);
}

void InstanceRender::appendContent(Hash& hash)
{
  hash.append(_device); hash.append(_samples); hash.append(_maxBounces); hash.append(_seed); hash.append(_clamp);
  hash.append(_bgColor[0]); hash.append(_bgColor[1]); hash.append(_bgColor[2]); hash.append(_bgVisible);
  hash.append(_denoise);
  hash.append(_outDepth); hash.append(_outNormal); hash.append(_outInstanceId); hash.append(_outAlbedo);
  hash.append(_outPosition); hash.append(_outMotion); hash.append(_outUV); hash.append(_outMaterialId);
  hash.append(_outObjectId); hash.append(_outLighting); hash.append(_motionUnits);
  hash.append(_outSurface); hash.append(_outGeoNormal);
  hash.append(_outOcclusion); hash.append(_outShadow); hash.append(_outLightGroups);
  hash.append(_outCrypto); hash.append(_cryptoLayers); hash.append(_cryptoId);
  hash.append(_deepPerObject); hash.append(_deepSlots);
  hash.append(_occlusionSamples); hash.append(_occlusionDistance);
  hash.append(_cameraPath); hash.append(_includeGuides); hash.append(_maxTriangles); hash.append(_headlight);
  hash.append(_textures); hash.append(_mipFilter); hash.append(_maxTextureSize); hash.append(_subdivLevels);
  hash.append(_pointDetail); hash.append(_curveSides); hash.append(_curveSegments);
  hash.append(_lightsVisible); hash.append(_reportStageErrors);
  hash.append(_shutter); hash.append(_shutterOffset); hash.append(_motionSamples); hash.append(_motionSteps); hash.append(_deformationBlur); hash.append(_cameraBlur); hash.append(_volumeSteps); hash.append(_volumeShadowSteps); hash.append(_volumeAlbedo); hash.append(_volumeBlur); hash.append(_volumeOctaves); hash.append(_volumeDeepSegments); hash.append(_spectralBlackbody);
  hash.append(_motionOutlier); hash.append(_useVelocities);
  if (_format.format()) { hash.append(_format.format()->width()); hash.append(_format.format()->height()); }
  hash.append(outputContext().frame());
  hash.append(renderEpoch(node()));
  for (int i = 0; i < 2; ++i) if (Op* in = input(i)) hash.append(in->hash());
  // ...and the ops behind any nkop: textures, whose own hashes DO move when the
  // picture does even though the URI describing them does not.
  {
    std::vector<std::string> names;
    { std::lock_guard<std::mutex> lock(_nkopMutex); names = _nkopOps; }
    for (size_t i = 0; i < names.size(); ++i) {
      DD::Image::Iop* iop = nullptr;
      for (int in = 0; in < 3 && !iop; ++in) iop = findIopByName(input(in), names[i], 0);
      if (iop) hash.append(iop->hash());
    }
  }
}

// The packed record the kernel writes: only what was asked for, in this order.
// A texture path that names a Nuke op looks like
//
//   nkop:/NkRoot/Read1:12:main:0xffffffffffffffff:0[1,1,0,0]:0x9740....nkiop
//         \_________/ \/
//          the op     the frame
//
// Nuke's own reader only answers one of these while it happens to be holding a
// texture image for that op, and hands back a 1x1 grey when it is not - which
// is why such a texture appeared and vanished as the timeline moved.  The op
// itself is upstream of this node, so it can simply be rendered.
static std::string nukeOpNameFromPath(const std::string& path)
{
  const size_t root = path.find("/NkRoot/");
  size_t start = (root == std::string::npos) ? 0 : (root + 8);
  if (root == std::string::npos) {
    const size_t slash = path.find_last_of('/');
    start = (slash == std::string::npos) ? 0 : (slash + 1);
  }
  const size_t colon = path.find(':', start);
  if (colon == std::string::npos || colon <= start) return std::string();
  return path.substr(start, colon - start);
}

static DD::Image::Iop* findIopByName(DD::Image::Op* root, const std::string& name, int depth)
{
  if (!root || depth > 64 || name.empty()) return nullptr;
  const std::string n = root->node_name();
  if (name == n) {
    if (DD::Image::Iop* iop = dynamic_cast<DD::Image::Iop*>(root)) return iop;
  }
  for (int i = 0; i < root->inputs(); ++i)
    if (DD::Image::Iop* found = findIopByName(root->input(i), name, depth + 1)) return found;
  return nullptr;
}

bool InstanceRender::nukeOpTexture(const std::string& path, int maxSize, ir::ImageData& out)
{
  // IS THE OP STILL CONNECTED?
  //
  // Disconnecting a node from a GeoDomeLight does NOT clear the light's
  // inputs:texture:file - Nuke authors the nkop: URI when the input is made and
  // leaves it in the stage afterwards, which is checkable in its own GeoExport
  // output. Worse, Nuke's own reader keeps answering that URI, so the light went
  // on being lit by an image that is no longer wired to anything.
  //
  // The op named by the URI is upstream of THIS node whenever it really feeds a
  // light in this scene, so that is the question to ask. It is asked BEFORE
  // Nuke's reader is consulted, because that reader is exactly what answers when
  // it should not.
  const std::string opName = nukeOpNameFromPath(path);
  DD::Image::Iop* upstream = nullptr;
  for (int i = 0; i < 3 && !upstream; ++i) upstream = findIopByName(input(i), opName, 0);
  if (!opName.empty() && !upstream) {
    irlog("nukeOpTexture: " + opName + " is not upstream of this node any more - the "
          "texture is treated as disconnected");
    return false;
  }

  // the path names the op AND the context it was published at, so Nuke can hand
  // back exactly that op - see src/ir/NukeOpImage.cpp
  std::string why;
  if (ir::loadNukeOpImage(path, maxSize, out, why)) {
    {
      // the SIZE alone says nothing: a bake that returns a flat grey image is
      // exactly what a card textured by a Nuke node looked like
      std::string px;
      if (out.valid() && out.rgba.size() >= 4) {
        const size_t mid = (size_t(out.height / 2) * size_t(out.width) + size_t(out.width / 2)) * 4;
        char b[128];
        std::snprintf(b, sizeof(b), " first=(%.3f %.3f %.3f) centre=(%.3f %.3f %.3f)",
                      out.rgba[0], out.rgba[1], out.rgba[2],
                      mid + 2 < out.rgba.size() ? out.rgba[mid] : 0.0f,
                      mid + 2 < out.rgba.size() ? out.rgba[mid + 1] : 0.0f,
                      mid + 2 < out.rgba.size() ? out.rgba[mid + 2] : 0.0f);
        px = b;
      }
      irlog("nukeOpTexture: " + path + " -> " + std::to_string(out.width) + "x"
            + std::to_string(out.height) + px);
    }
    return true;
  }
  irlog("nukeOpTexture: " + why + "; looking for the node upstream instead");

  // failing that, the op is upstream of this node, so find it by name
  const std::string& name = opName;
  DD::Image::Iop* iop = upstream;      // already located above
  if (!iop) {
    irlog("nukeOpTexture: no op named " + name + " upstream of this node");
    return false;
  }
  const bool ok = ir::bakeIop(iop, maxSize, out);
  irlog("nukeOpTexture: " + name + (ok ? " baked " : " could not be baked ")
        + std::to_string(out.width) + "x" + std::to_string(out.height));
  return ok;
}

// One entry per channel this node offers, saying which buffer holds it and
// where in that buffer's pixel it sits.  The packed AOV buffer's offsets come
// from the same aovLayout() the kernel wrote through, so the two cannot drift.
void InstanceRender::buildChannelSources(const DD::Image::ChannelSet& chans)
{
  std::shared_ptr<ChannelSourceMap> built(new ChannelSourceMap);
  const ir::AovLayout a = aovLayout();
  typedef ChannelSource CS;
  auto put = [&](DD::Image::Channel ch, CS::Buffer buf, int off) {
    if (ch != DD::Image::Chan_Black && chans.contains(ch)) {
      CS cs; cs.buffer = buf; cs.offset = off;
      (*built)[int(ch)] = cs;
    }
  };
  put(DD::Image::Chan_Red, CS::kRgba, 0);
  put(DD::Image::Chan_Green, CS::kRgba, 1);
  put(DD::Image::Chan_Blue, CS::kRgba, 2);
  put(DD::Image::Chan_Alpha, CS::kRgba, 3);
  put(DD::Image::Chan_Z, CS::kDepth, 0);
  for (int c = 0; c < 3; ++c) put(_chanN[c], CS::kNormal, c);
  put(_chanInstance, CS::kInstance, 0);
  for (int c = 0; c < 3; ++c) put(_chanAlbedo[c], CS::kAlbedo, c);
  if (a.position >= 0) for (int c = 0; c < 3; ++c) put(_chanP[c], CS::kExtra, a.position + c);
  if (a.motion >= 0) for (int c = 0; c < 2; ++c) put(_chanMotion[c], CS::kExtra, a.motion + c);
  if (a.uv >= 0) for (int c = 0; c < 2; ++c) put(_chanUV[c], CS::kExtra, a.uv + c);
  if (a.materialId >= 0) put(_chanMatId, CS::kExtra, a.materialId);
  if (a.objectId >= 0) put(_chanObjId, CS::kExtra, a.objectId);
  if (a.directDiffuse >= 0) {
    const int base[5] = { a.directDiffuse, a.indirectDiffuse, a.directSpecular, a.indirectSpecular, a.emission };
    for (int i = 0; i < 5; ++i)
      for (int c = 0; c < 3; ++c)
        put(_chanLight[i][c], CS::kExtra, base[i] + c);
  }
  if (a.surface >= 0) for (int i = 0; i < 4; ++i) put(_chanSurface[i], CS::kExtra, a.surface + i);
  if (a.specularColor >= 0) for (int c = 0; c < 3; ++c) put(_chanSpecCol[c], CS::kExtra, a.specularColor + c);
  if (a.geoNormal >= 0) for (int c = 0; c < 3; ++c) put(_chanNg[c], CS::kExtra, a.geoNormal + c);
  if (a.occlusion >= 0) put(_chanOcclusion, CS::kExtra, a.occlusion);
  if (a.shadow >= 0) for (int c = 0; c < 3; ++c) put(_chanShadow[c], CS::kExtra, a.shadow + c);
  if (a.lightGroups >= 0)
    for (size_t i = 0; i < _chanLightGroup.size(); ++i)
      put(_chanLightGroup[i], CS::kExtra, a.lightGroups + int(i));
  // CryptoObject<nn>.rgba is rank 2n's (id, coverage) then rank 2n+1's, which is
  // exactly how the sorted table is laid out - so the channels map straight onto
  // it and there is nothing to rearrange at output time.
  if (a.crypto >= 0)
    for (size_t i = 0; i < _chanCrypto.size(); ++i)
      put(_chanCrypto[i], CS::kExtra, a.crypto + int(i));
  std::lock_guard<std::mutex> lock(_mutex);
  _chanSource = built;
}

// A channel name that Nuke will accept, out of a name that came from a stage.
//
// Prim paths are the usual case ("/World/Lights/Key"), and the leaf is what a
// person calls the light, so that is what the layer is called.  Anything that is
// not a letter, a digit or an underscore becomes one, because a channel name
// with a dot in it would read as a layer separator and one with a slash would
// not be accepted at all.
static std::string lightLayerName(const std::string& raw)
{
  size_t start = raw.find_last_of('/');
  std::string leaf = (start == std::string::npos) ? raw : raw.substr(start + 1);
  std::string out;
  for (size_t i = 0; i < leaf.size(); ++i) {
    const char c = leaf[i];
    out += (isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  }
  if (out.empty()) out = "light";
  if (isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;   // not a leading digit
  return out;
}

// The per-light channels, from the scene that was just loaded.
//
// These are the only channels this node has that are not known in the
// constructor, and they carry a cost worth knowing about: Nuke's channel
// registry is append-only for the session, so every distinct name minted here
// stays for good.  Renaming a light therefore leaves the old layer behind until
// Nuke restarts, which is why this only ever names the lights the scene
// actually has, and pools the rest into one.
void InstanceRender::updateLightGroups()
{
  std::vector<std::string> raw;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    raw = _scene.lightNames;
  }
  std::vector<std::string> names;
  if (!raw.empty()) {
    // room for the lights, keeping the last slot for everything else
    const size_t named = std::min(raw.size(), size_t(ir::kMaxLightGroups - 1));
    for (size_t i = 0; i < named; ++i) {
      std::string n = lightLayerName(raw[i]);
      // two lights can easily share a leaf name; a layer may not
      int suffix = 2;
      std::string candidate = n;
      while (std::find(names.begin(), names.end(), candidate) != names.end())
        candidate = n + "_" + std::to_string(suffix++);
      names.push_back(candidate);
    }
    names.push_back("other");
  }
  if (names == _lightGroupNames) return;      // the scene has not changed its lights
  _lightGroupNames = names;
  _lightGroupCount.store(int(names.size()));
  _chanLightGroup.clear();
  static const char* kRGB[3] = { "red", "green", "blue" };
  for (size_t i = 0; i < names.size(); ++i)
    for (int c = 0; c < 3; ++c)
      _chanLightGroup.push_back(getChannel(("light_" + names[i] + "." + kRGB[c]).c_str()));
}

// The cryptomatte TYPE name.  It has to differ between the two granularities:
// the channels are named after it and so is the metadata, and a comp that has
// picked copies must not silently start reading whole objects out of the same
// channels when the knob moves.
static const char* cryptoTypeName(int mode) { return mode == kCryptoIdCopy ? "CryptoCopy" : "CryptoObject"; }

// hash a list of names once
static std::vector<float> cryptoIdsOf(const std::vector<std::string>& names)
{
  std::vector<float> out(names.size());
  for (size_t i = 0; i < names.size(); ++i) out[i] = ir::cryptoIdOf(names[i]);
  return out;
}

// The cryptomatte layers, the ids, and the manifest that ties them together.
//
// A name is hashed to an id, the id is what the pixel carries, and the manifest
// lists the name against the same hash - that pairing IS the format.  Get the
// two out of step and a pick selects nothing, silently, so BOTH come from the
// same string here and the device never hashes anything.
//
// Two granularities:
//   object  every copy of a mesh takes the prototype's name, so picking one
//           takes the whole scatter.  The ordinary CryptoObject.
//   copy    each copy takes <prototype>/<id>, so a pick isolates one copy.  The
//           id is the instance's stable id - the point or particle id it was
//           scattered onto, not its position in the list - so a copy keeps its
//           matte from frame to frame while particles are born and die.
void InstanceRender::updateCrypto()
{
  std::vector<std::string> names;
  const bool perCopy = (_cryptoId == kCryptoIdCopy);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!perCopy) {
      names = _scene.protoNames;
      const std::vector<float> ids = cryptoIdsOf(names);
      for (size_t i = 0; i < _scene.instances.size(); ++i) {
        const int p = _scene.instances[i].protoId;
        _scene.instances[i].cryptoId = (p >= 0 && size_t(p) < ids.size()) ? ids[size_t(p)] : 0.0f;
      }
    }
    else {
      // One name per COPY.  Built once here and hashed straight away, so the
      // strings are not kept around: the manifest needs them in order and
      // nothing else does.
      names.reserve(_scene.instances.size());
      for (size_t i = 0; i < _scene.instances.size(); ++i) {
        const int p = _scene.instances[i].protoId;
        std::string n = (p >= 0 && size_t(p) < _scene.protoNames.size())
                      ? _scene.protoNames[size_t(p)] : std::string("<copy>");
        n += '/';
        n += std::to_string(_scene.instances[i].instanceId);
        _scene.instances[i].cryptoId = ir::cryptoIdOf(n);
        names.push_back(n);
      }
    }
    // Volumes are named in BOTH modes: a volume has no copies to tell apart, so
    // "per copy" means the same thing as "per object" for one, and leaving it
    // out of the manifest would make it the one thing in the frame a matte
    // cannot pick.
    for (size_t i = 0; i < _scene.volumes.size(); ++i) {
      const std::string vn = (i < _scene.volumeNames.size()) ? _scene.volumeNames[i]
                                                             : std::string("<volume>");
      _scene.volumes[i].cryptoId = ir::cryptoIdOf(vn);
      names.push_back(vn);
    }
    // the prototype ids are left in step with the object-mode names, because
    // anything else reading them (object.id, the info line) still means "mesh"
    for (size_t i = 0; i < _scene.protos.size(); ++i)
      _scene.protos[i].cryptoId = (i < _scene.protoNames.size())
                                ? ir::cryptoIdOf(_scene.protoNames[i]) : 0.0f;
  }

  const int layers = std::max(1, std::min(_cryptoLayers, 3));
  const int ranks = layers * 2;
  // Room to accumulate beyond what is written out.  A pixel can see more objects
  // than it has ranks to report, and the ones that get dropped should be the
  // lightest - which only works if they were all counted first.
  const int slots = std::min(ranks + 2, 8);
  _cryptoSlots.store(_outCrypto ? slots : 0);

  Hash h;
  for (size_t i = 0; i < names.size(); ++i) h.append(names[i]);
  h.append(layers);
  h.append(_cryptoId);
  if (h == _cryptoNamesHash && !_chanCrypto.empty()) return;
  _cryptoNamesHash = h;

  _chanCrypto.clear();
  static const char* kRGBA[4] = { "red", "green", "blue", "alpha" };
  for (int L = 0; L < layers; ++L) {
    char layer[32];
    std::snprintf(layer, sizeof(layer), "%s%02d", cryptoTypeName(_cryptoId), L);
    for (int c = 0; c < 4; ++c)
      _chanCrypto.push_back(getChannel((std::string(layer) + "." + kRGBA[c]).c_str()));
  }

  // The manifest is the raw hash in hex, NOT the bits of the float that is in
  // the image - Nuke converts for itself when it matches, and writing the
  // converted value here selects nothing, silently.  See Crypto.h.
  std::ostringstream js;
  js << '{';
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) js << ',';
    js << '"' << ir::cryptoJsonEscape(names[i]) << "\":\"" << ir::cryptoManifestHex(names[i]) << '"';
  }
  js << '}';
  {
    // Under the lock, because _fetchMetaData reads it and metadata is pulled by
    // whatever thread happens to want it while this is being rebuilt in
    // _validate.  Third time on this project: anything _validate writes and
    // another thread reads has to say so.  It shows up as a test that passes on
    // its own and fails differently in a full sweep.
    std::lock_guard<std::mutex> lock(_mutex);
    _cryptoManifest = js.str();
    // Said out loud, because per copy this grows with the scatter and lands in
    // the EXR header: measured, 14850 copies make a 0.97 MB manifest and take the
    // file from 11.5 to 14.2 MB. Nothing is capped - a matte that silently
    // stopped naming some copies would be worse than a large header - but a
    // number nobody was told about is how a 3 MB header becomes a surprise.
    _scene.info += ", cryptomatte " + std::string(cryptoTypeName(_cryptoId)) + " "
                 + std::to_string(names.size()) + " name(s), manifest "
                 + std::to_string((_cryptoManifest.size() + 512) / 1024) + " KB";
  }
  if (_cryptoManifest.size() > 4u * 1024u * 1024u) {
    warning("the cryptomatte manifest is %.1f MB (%d names) - every copy is named, so it grows "
            "with the scatter. Switch 'cryptomatte id' to object if a per-copy pick is not needed.",
            double(_cryptoManifest.size()) / (1024.0 * 1024.0), int(names.size()));
  }
}

// Merge one progressive pass's cryptomatte tables into the accumulated ones, BY
// ID.
//
// Blending these slot by slot, the way the light aovs are blended, is nonsense:
// each pass sorts its own table by its own coverage, so slot 2 is a different
// object from one pass to the next, and averaging two ids gives a third object
// that is not in the scene.  So the ids are matched, their coverages weighted,
// anything new is added if there is room, and the result is sorted again.
void InstanceRender::mergeCryptoTables(std::vector<float>& acc, const std::vector<float>& add,
                                       const ir::AovLayout& a, float wOld, float wNew) const
{
  if (a.crypto < 0 || a.cryptoSlots <= 0 || a.stride <= 0) return;
  if (acc.size() != add.size()) return;
  const size_t stride = size_t(a.stride);
  const int slots = a.cryptoSlots;
  const size_t pixels = acc.size() / stride;
  for (size_t p = 0; p < pixels; ++p) {
    float* dst = &acc[p * stride + size_t(a.crypto)];
    const float* src = &add[p * stride + size_t(a.crypto)];
    for (int i = 0; i < slots; ++i) dst[i * 2 + 1] *= wOld;
    for (int i = 0; i < slots; ++i) {
      const float id = src[i * 2], w = src[i * 2 + 1] * wNew;
      if (w > 0.0f) ir::cryptoAdd(dst, slots, id, w);
    }
    ir::cryptoSort(dst, slots);
  }
}

ir::AovLayout InstanceRender::aovLayout() const
{
  ir::AovLayout a;
  int o = 0;
  if (_outPosition) { a.position = o; o += 3; }
  if (_outMotion) { a.motion = o; o += 2; }
  if (_outUV) { a.uv = o; o += 2; }
  if (_outMaterialId) { a.materialId = o; o += 1; }
  if (_outObjectId) { a.objectId = o; o += 1; }
  if (_outLighting) {
    a.directDiffuse = o; o += 3;
    a.indirectDiffuse = o; o += 3;
    a.directSpecular = o; o += 3;
    a.indirectSpecular = o; o += 3;
    a.emission = o; o += 3;
  }
  if (_outSurface) {
    a.surface = o; o += 4;             // roughness, metallic, opacity, facing
    a.specularColor = o; o += 3;
  }
  if (_outGeoNormal) { a.geoNormal = o; o += 3; }
  if (_outOcclusion) { a.occlusion = o; o += 1; }
  if (_outShadow) { a.shadow = o; o += 3; }
  const int groups = _lightGroupCount.load();
  if (_outLightGroups && groups > 0) {
    a.lightGroups = o;
    a.lightGroupCount = groups;
    o += 3 * groups;
  }
  if (_deepPerObject) {
    a.deep = o;
    a.deepSlots = std::max(1, std::min(_deepSlots, 8));
    o += ir::kDeepSlotFloats * a.deepSlots;
  }
  const int slots = _cryptoSlots.load();
  if (_outCrypto && slots > 0) {
    a.crypto = o;
    a.cryptoSlots = slots;
    o += 2 * slots;
  }
  a.stride = o;
  return a;
}

void InstanceRender::_validate(bool for_real)
{
  irlog(std::string("_validate for_real=") + (for_real ? "1" : "0") + " in0=" + (input(0) ? "yes" : "no")
        + " in2=" + (input(2) ? "yes" : "no") + " tid=" + std::to_string(currentThreadId()));
  // format / bbox from the bg input (black with the root format when unconnected)
  Iop* bg = dynamic_cast<Iop*>(input(2));
  if (bg) { bg->validate(for_real); copy_info(2); }
  else { info_.format(input_format()); info_.full_size_format(input_format()); info_.set(input_format()); }
  // An explicit format wins over both: rendering at a size of its own is what
  // the knob is for, and a bg input then only supplies pixels behind the render.
  if (_format.format() && _format.format() != &Format::None) {
    const Format* fmt = _format.format();
    const Format* full = _format.fullSizeFormat() ? _format.fullSizeFormat() : fmt;
    info_.format(*fmt);
    info_.full_size_format(*full);
    info_.set(*fmt);
  }
  info_.set(info_.format());
  // The inputs and the scene come FIRST, because the per-light channels are
  // named after the lights and there is no way to know their names until the
  // stage has been read.
  if (for_real) {
    if (Op* s = input(0)) s->validate(for_real);
    if (Op* c = input(1)) c->validate(for_real);
    // the ONLY place this node is allowed to walk the graph - see prepareScene()
    prepareScene();
  }
  updateLightGroups();
  updateCrypto();
  ChannelSet chans(Mask_RGBA);
  if (_outDepth) chans += Mask_Z;
  if (_outNormal) { chans += _chanN[0]; chans += _chanN[1]; chans += _chanN[2]; }
  if (_outInstanceId) chans += _chanInstance;
  if (_outAlbedo) { chans += _chanAlbedo[0]; chans += _chanAlbedo[1]; chans += _chanAlbedo[2]; }
  if (_outPosition) { chans += _chanP[0]; chans += _chanP[1]; chans += _chanP[2]; }
  if (_outMotion) { chans += _chanMotion[0]; chans += _chanMotion[1]; }
  if (_outUV) { chans += _chanUV[0]; chans += _chanUV[1]; }
  if (_outMaterialId) chans += _chanMatId;
  if (_outObjectId) chans += _chanObjId;
  if (_outLighting) for (int i = 0; i < 5; ++i) for (int c = 0; c < 3; ++c) chans += _chanLight[i][c];
  if (_outSurface) {
    for (int i = 0; i < 4; ++i) chans += _chanSurface[i];
    for (int c = 0; c < 3; ++c) chans += _chanSpecCol[c];
  }
  if (_outGeoNormal) for (int c = 0; c < 3; ++c) chans += _chanNg[c];
  if (_outOcclusion) chans += _chanOcclusion;
  if (_outShadow) for (int c = 0; c < 3; ++c) chans += _chanShadow[c];
  if (_outLightGroups) for (size_t i = 0; i < _chanLightGroup.size(); ++i) chans += _chanLightGroup[i];
  if (_outCrypto) for (size_t i = 0; i < _chanCrypto.size(); ++i) chans += _chanCrypto[i];
  info_.turn_on(chans);
  set_out_channels(chans);
  buildChannelSources(chans);
  info_.black_outside(true);

  // The deep face of the node describes the same picture: same format, same box,
  // and rgba with the two depth channels the format is made of.  DeepOp.h asks
  // for this to be filled by the same _validate that serves the Iop, which is
  // why there is one of them doing both.
  {
    ChannelSet deepChans(Mask_RGBA);
    deepChans += DD::Image::Mask_Deep;
    _deepInfo = DD::Image::DeepInfo(info_.formats(), info_, deepChans);
  }
}

void InstanceRender::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  irlog("_request");
  if (Iop* bg = dynamic_cast<Iop*>(input(2))) bg->request(x, y, r, t, channels, count);
}

void InstanceRender::_close()
{
}

// ---------------------------------------------------------------------------
// Two front ends: a USD stage where the input is part of Nuke's USD 3D system,
// and the classic 3D system where it is a GeoOp.  Nuke 14.1 has only the second
// one available to a plugin - it ships the USD libraries but not their headers.
bool InstanceRender::loadScene(std::string& err)
{
  Op* in0 = input(0);
  if (!in0) { err = "no scene connected to scn"; return false; }
#if IR_HAVE_USD
  if (ir::isStageSource(in0)) return loadStageScene(err);
#endif
  if (dynamic_cast<DD::Image::GeoOp*>(in0)) return loadClassicScene(err);
#if IR_HAVE_USD
  err = "the scn input is neither a USD scene nor classic 3D geometry";
#elif IR_NUKE_VER < 1600
  // Nuke 14.1 ships no USD headers at all, and 15.x has no supported way for a
  // plugin to be handed a composed stage (GeometryProviderI arrives in 16.0).
  err = "USD scenes need Nuke 16.0 or newer - this Nuke renders classic 3D geometry "
        "(Card, Sphere, Cube, ReadGeo, CopyToPoints), so connect that to scn instead";
#else
  err = "the scn input is not classic 3D geometry (this build of Nuke has no USD front end)";
#endif
  return false;
}

// ---- the classic 3D system -------------------------------------------------------
// Nuke 14.1 can only be reached this way (it ships no USD headers), but the path
// is not version-specific: classic geometry renders the same in every version.
// Motion blur comes from evaluating the same op at the shutter times, exactly as
// the USD path does.
bool InstanceRender::loadClassicScene(std::string& err)
{
  const double frameNow = outputContext().frame();
  double shutterOpen = 0.0, shutterClose = 0.0;
  if (_shutter > 0.0) {
    if (_shutterOffset == kShutterStart) { shutterOpen = 0.0; shutterClose = _shutter; }
    else if (_shutterOffset == kShutterEnd) { shutterOpen = -_shutter; shutterClose = 0.0; }
    else { shutterOpen = -0.5 * _shutter; shutterClose = 0.5 * _shutter; }
  }
  _mvOnly = (_outMotion && _shutter <= 0.0);
  if (_mvOnly) { shutterOpen = 0.0; shutterClose = 1.0; }
  _motionFrames = shutterClose - shutterOpen;

  ir::GeoLoaderOptions opt;
  opt.maxTriangles = _maxTriangles * 1e6;   // the knob is in millions
  opt.maxTextureSize = _maxTextureSize;
  opt.textures = _textures;
  opt.mipFilter = _mipFilter;
  opt.pointDetail = _pointDetail;
  opt.aborted = [this] { return aborted(); };    // see GeoLoader.h

  // Read the SHUTTER-CLOSE end first, and the rendered end last.
  //
  // Asking an input for its geometry at another frame leaves the whole upstream
  // tree evaluated THERE.  If that is where this function stops, the ops feeding
  // this node are sitting at a different frame than the one being rendered, so
  // their hashes - which appendContent() folds into this node's - come out
  // different after the render than before it.  In the viewer that is an endless
  // loop: every redraw changes the hash, which asks for another redraw, and Nuke
  // appears to hang the moment the shutter is opened.  Doing the close end first
  // means the last evaluation is the one being rendered, and the explicit refetch
  // at the end puts it beyond doubt.
  ir::Scene closeScene;
  bool haveClose = false;
  std::string closeErr;
  if (_motionFrames > 0.0) {
    OutputContext ctx = outputContext();
    ctx.setFrame(frameNow + shutterClose);
    if (DD::Image::GeoOp* geoClose = dynamic_cast<DD::Image::GeoOp*>(ir::inputAtContext(this, 0, ctx)))
      haveClose = loadClassicGeometry(geoClose, input(0), opt, closeScene, closeErr);
  }

  // the geometry at shutter open, which is what gets rendered
  DD::Image::GeoOp* geoOpen = nullptr;
  {
    OutputContext ctx = outputContext();
    ctx.setFrame(frameNow + shutterOpen);
    geoOpen = dynamic_cast<DD::Image::GeoOp*>(ir::inputAtContext(this, 0, ctx));
  }
  if (!geoOpen) geoOpen = dynamic_cast<DD::Image::GeoOp*>(input(0));
  const bool loaded = loadClassicGeometry(geoOpen, input(0), opt, _scene, err);
  // whatever happened, leave the graph evaluated at the frame being rendered
  if (_motionFrames > 0.0) {
    // ONLY while the render is still wanted.  Scrubbing the playbar is a
    // stream of renders, each cancelled by the next, and validating an input
    // after Nuke has cancelled this one DEADLOCKS: the main thread is holding
    // the graph waiting for this render to notice it should stop, and this
    // call waits for the graph.  Nuke stops responding and never recovers -
    // measured with aborted() already true at exactly this line.
    //
    // Skipping it is safe precisely when it is unsafe to run: the refetch
    // exists so the hashes left behind match the frame being rendered, and a
    // cancelled render's output is thrown away.
    if (aborted()) {
      // Skipped, not failed.  The scene is loaded; the refetch only leaves the
      // graph tidy for the NEXT render, and there is not going to be a useful
      // one until whatever aborted this has finished.
      irlog("loadScene: aborted - skipping the refetch");
    }
    else {
      irlog("loadScene: refetch at the rendered frame");
      ir::WatchdogPhase wp("loadScene: refetch (validate upstream at the rendered frame)");
      if (Op* back = ir::inputAtContext(this, 0, outputContext())) back->validate(true);
      irlog("loadScene: refetch done");
    }
  }
  if (!loaded) return false;

  // ---- motion: match the two ends up -------------------------------------------
  irlog("loadScene: matching");
  if (_motionFrames > 0.0 && !_scene.instances.empty()) {
    // Match instances by KEY, not by position in the list.  Particles are born
    // and die across the shutter, so the lists are different lengths and in
    // different order; matching by index would blur one particle into another,
    // and refusing to blur when the counts differ would mean particles never
    // blur at all.  Anything with no partner keeps still.
    if (haveClose) {
      // Where the objects carry no id of their own, their "key" is only their
      // place in the list - and one particle dying shifts every particle after
      // it, so they would all pair with a neighbour and the whole cloud would
      // smear by the spacing between particles.  Those are paired by WHERE THEY
      // ARE instead; see src/ir/MatchInstances.h.
      std::map<uint64_t, size_t> closeByKey;
      for (size_t i = 0; i < closeScene.instanceMatchKey.size(); ++i)
        closeByKey[closeScene.instanceMatchKey[i]] = i;

      // Two ways to pair the ends up, and the scene decides which is right.
      //
      // By KEY is exact when the objects have ids, and when they do not it is
      // only their place in the list - which shifts by one every time a
      // particle is born or dies, pairing every later particle with a
      // neighbour.  By POSITION survives that, but is guesswork if the objects
      // moved further than they are apart.
      //
      // So both are built and the one that explains the motion more coherently
      // wins: the wrong pairing scatters objects across the gaps between them,
      // and its median travel is far larger.  No threshold to tune - the two
      // candidates are compared against each other.
      std::vector<int> byKey(_scene.instances.size(), -1);
      for (size_t i = 0; i < _scene.instances.size(); ++i) {
        if (i >= _scene.instanceMatchKey.size()) continue;
        std::map<uint64_t, size_t>::const_iterator it = closeByKey.find(_scene.instanceMatchKey[i]);
        if (it != closeByKey.end() && it->second < closeScene.instances.size())
          byKey[i] = int(it->second);
      }

      std::vector<int> byProximity;
      float spacing = 0.0f;
      std::vector<ir::Vec3> a0(_scene.instances.size()), b0(closeScene.instances.size());
      for (size_t i = 0; i < _scene.instances.size(); ++i)
        a0[i] = ir::Vec3(_scene.instances[i].xf.m[3], _scene.instances[i].xf.m[7], _scene.instances[i].xf.m[11]);
      for (size_t i = 0; i < closeScene.instances.size(); ++i)
        b0[i] = ir::Vec3(closeScene.instances[i].xf.m[3], closeScene.instances[i].xf.m[7], closeScene.instances[i].xf.m[11]);
      if (!_scene.matchKeysAreIds && _motionOutlier > 0.0 && _scene.instances.size() >= 4)
        byProximity = ir::matchByProximity(a0, b0, float(_motionOutlier), &spacing);

      bool useProximity = false;
      if (!byProximity.empty()) {
        const double keyTravel = ir::medianTravel(a0, b0, byKey);
        const double posTravel = ir::medianTravel(a0, b0, byProximity);
        // a pairing that matched nothing explains nothing
        const bool keyUseless = (keyTravel < 0.0);
        useProximity = (posTravel >= 0.0) && (keyUseless || posTravel < keyTravel);
      }
      const std::vector<int>& chosen = useProximity ? byProximity : byKey;

      // ---- does the velocity agree with what the pairing sees? ---------------
      //
      // A velocity is only worth preferring if it describes THIS motion.  A
      // particle system will happily report one that the particles do not
      // follow - a bare ParticleEmitter hands out velocities of unit length to
      // particles that never move at all - and blurring by that invents motion,
      // which is worse than the sharp copies it was meant to fix.  It showed up
      // as a blurred render with LESS coverage than the sharp one: the streaks
      // were sweeping particles off their real positions.
      //
      // So the two are compared where both have an opinion - the paired
      // instances - and the velocity is believed everywhere only if it agrees.
      // Where it does not, it is still used for whatever the pairing could not
      // pair, because there the alternative is not a better answer but no
      // answer: those are the ones held still and rendered sharp.
      bool velAgrees = false;
      if (_scene.hasVelocities && _useVelocities) {
        // The reference has to be the PROXIMITY pairing, and it is built here
        // even when the knob has switched proximity matching off for rendering.
        // Checking against the by-key pairing instead means checking against
        // Nuke's particle ids, which are RECYCLED - measured on the reported
        // scene at frame 20, a median travel of 3.93 where the truth was 0.50.
        // Compared against that, a perfectly good velocity looks wrong and gets
        // rejected, which is the opposite of what this guard is for.
        std::vector<int> refMatch;
        {
          float refSpacing = 0.0f;
          if (_scene.instances.size() >= 4)
            refMatch = ir::matchByProximity(a0, b0, 8.0f, &refSpacing);
        }
        std::vector<double> velTravel, pairTravel;
        for (size_t i = 0; i < _scene.instances.size(); ++i) {
          if (i >= _scene.instanceVelValid.size() || !_scene.instanceVelValid[i]) continue;
          if (i >= refMatch.size() || refMatch[i] < 0 || size_t(refMatch[i]) >= closeScene.instances.size()) continue;
          const ir::Xform& a = _scene.instances[i].xf;
          const ir::Xform& b = closeScene.instances[size_t(refMatch[i])].xf;
          const double dx = b.m[3] - a.m[3], dy = b.m[7] - a.m[7], dz = b.m[11] - a.m[11];
          pairTravel.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
          const ir::Vec3& v = _scene.instanceVel[i];
          velTravel.push_back(std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z)
                              * std::fabs(_motionFrames));
        }
        if (pairTravel.size() >= 8) {
          std::sort(pairTravel.begin(), pairTravel.end());
          std::sort(velTravel.begin(), velTravel.end());
          const double mp = pairTravel[pairTravel.size() / 2];
          const double mv = velTravel[velTravel.size() / 2];
          velAgrees = (mp > 1e-9) && (mv > 1e-9) && (mv / mp > 0.5) && (mv / mp < 2.0);
          irlog("loadScene: median travel pairing " + std::to_string(mp) + " velocity "
                + std::to_string(mv) + (velAgrees ? " - agree" : " - DISAGREE, velocity only fills gaps"));
        }
        else {
          // nothing to compare against: fill the gaps and leave the rest alone
          irlog("loadScene: too few pairs to check the velocity against");
        }
      }

      size_t moved = 0, matched = 0, fromVel = 0;
      double travel = 0.0;
      // The MEDIAN as well as the worst.  One wrong pair sets the worst and says
      // nothing about what the frame looks like; the median is what the blur
      // actually is, and it is the only one of the two worth comparing between
      // two different ways of deciding how things moved.
      std::vector<double> travels;
      // How far one frame of velocity carries: the shutter, in frames.  Nuke's
      // "vel" is per frame (checked against the matcher, which independently
      // measured a median travel of 0.5100 over a 0.51-frame shutter on
      // velocities of unit length).
      const float velScale = float(_motionFrames);
      std::vector<ir::Xform> keys(_scene.instances.size() * 2);
      for (size_t i = 0; i < _scene.instances.size(); ++i) {
        const ir::Xform& a = _scene.instances[i].xf;
        ir::Xform b = a;
        // A velocity beats any pairing: it is the object's own account of where
        // it is going, so it is right for the ones being born or dying inside
        // the shutter - which have no partner to be paired with, and which used
        // to be held still and render SHARP in the middle of a blurred cloud.
        // It moves the object without turning it; a pairing turns it too, but
        // can also pair the wrong two things, and a wrong streak is far more
        // visible than an unturned one.
        // A velocity of exactly zero says nothing.  It is what an unset field
        // looks like, and taking it at its word pins a moving object still -
        // which is how the particle suite caught this: blurred coverage fell
        // back to the unblurred width the moment velocities arrived.  Zero
        // therefore falls through to the pairing, which for something genuinely
        // stationary also says it did not move, so nothing is lost either way.
        const bool paired = (i < chosen.size() && chosen[i] >= 0
                             && size_t(chosen[i]) < closeScene.instances.size());
        bool haveVel = _scene.hasVelocities && _useVelocities
                    && i < _scene.instanceVelValid.size() && _scene.instanceVelValid[i];
        if (haveVel) {
          const ir::Vec3& v0 = _scene.instanceVel[i];
          // A velocity of exactly zero says nothing: it is what an unset field
          // looks like, and taking it at its word pins a moving object still.
          if (v0.x == 0.0f && v0.y == 0.0f && v0.z == 0.0f) haveVel = false;
        }
        // where the pairing has an answer, only override it if the velocity has
        // been shown to agree with the pairing generally
        if (haveVel && paired && !velAgrees) haveVel = false;
        if (haveVel) {
          const ir::Vec3& v = _scene.instanceVel[i];
          b.m[3]  = a.m[3]  + v.x * velScale;
          b.m[7]  = a.m[7]  + v.y * velScale;
          b.m[11] = a.m[11] + v.z * velScale;
          ++fromVel;
        }
        else if (i < chosen.size() && chosen[i] >= 0 && size_t(chosen[i]) < closeScene.instances.size()) {
          b = closeScene.instances[size_t(chosen[i])].xf;
          ++matched;
        }
        keys[i * 2] = a; keys[i * 2 + 1] = b;
        _scene.instances[i].xf1 = b;
        _scene.instances[i].firstKey = int(i) * 2;
        bool differs = false;
        for (int k = 0; k < 12 && !differs; ++k) differs = (a.m[k] != b.m[k]);
        if (differs) {
          ++moved;
          const double dx = b.m[3] - a.m[3], dy = b.m[7] - a.m[7], dz = b.m[11] - a.m[11];
          const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
          travel = std::max(travel, d);
          travels.push_back(d);
        }
      }
      // A match is only believable if it moved like its neighbours.  Nuke's
      // particle system RECYCLES ids: a particle that dies hands its id to one
      // born somewhere else entirely, and matching the two smears a streak
      // across the frame.  Objects with no id at all are matched by their place
      // in the list, which shifts by one every time a particle dies - the same
      // failure, on every particle after it.
      //
      // So the travel of every matched pair is compared with the median: a pair
      // that moved far more than the rest of the cloud did is not the same
      // particle, and it is held still rather than smeared.
      size_t rejected = 0;
      if (_motionOutlier > 0.0 && matched > 1 && !useProximity) {
        std::vector<double> travels;
        travels.reserve(matched);
        ir::Vec3 lo(1e30f), hi(-1e30f);
        for (size_t i = 0; i < _scene.instances.size(); ++i) {
          const ir::Xform& a2 = keys[i * 2];
          const ir::Xform& b2 = keys[i * 2 + 1];
          const double dx = b2.m[3] - a2.m[3], dy = b2.m[7] - a2.m[7], dz = b2.m[11] - a2.m[11];
          const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (d > 0.0) travels.push_back(d);
          lo.x = std::min(lo.x, a2.m[3]); hi.x = std::max(hi.x, a2.m[3]);
          lo.y = std::min(lo.y, a2.m[7]); hi.y = std::max(hi.y, a2.m[7]);
          lo.z = std::min(lo.z, a2.m[11]); hi.z = std::max(hi.z, a2.m[11]);
        }
        if (!travels.empty()) {
          std::sort(travels.begin(), travels.end());
          const double median = travels[travels.size() / 2];
          // the size of the cloud itself, so a scene where nothing much moves
          // still has a sane absolute ceiling
          const double sx = double(hi.x - lo.x), sy = double(hi.y - lo.y), sz = double(hi.z - lo.z);
          const double spread = std::sqrt(sx * sx + sy * sy + sz * sz);
          const double limit = std::max(_motionOutlier * median, 0.02 * spread);
          for (size_t i = 0; i < _scene.instances.size(); ++i) {
            ir::Xform& a2 = keys[i * 2];
            ir::Xform& b2 = keys[i * 2 + 1];
            const double dx = b2.m[3] - a2.m[3], dy = b2.m[7] - a2.m[7], dz = b2.m[11] - a2.m[11];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= limit) continue;
            b2 = a2;                                   // not the same particle
            _scene.instances[i].xf1 = a2;
            ++rejected;
            if (moved > 0) --moved;
          }
          if (rejected > 0) {
            travel = 0.0;
            for (size_t i = 0; i < _scene.instances.size(); ++i) {
              const ir::Xform& a2 = keys[i * 2];
              const ir::Xform& b2 = keys[i * 2 + 1];
              const double dx = b2.m[3] - a2.m[3], dy = b2.m[7] - a2.m[7], dz = b2.m[11] - a2.m[11];
              travel = std::max(travel, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
          }
        }
      }
      if (rejected > 0) {
        _scene.info += ", " + std::to_string(rejected)
                     + " match(es) rejected as recycled ids (held still)";
      }
      if (useProximity) {
        char sb[64];
        std::snprintf(sb, sizeof(sb), "%.4f", spacing);
        _scene.info += ", matched by position (no ids; spacing " + std::string(sb) + ")";
      }
      if (fromVel > 0)
        _scene.info += ", " + std::to_string(fromVel) + " blurred from their own velocity";
      // Only the ones that really are standing still.  Counting everything the
      // matcher did not pair says "held still" about objects that are blurring
      // perfectly well from a velocity, which is the opposite of what the
      // reader needs to know.
      const size_t stillHeld = _scene.instances.size() - matched - fromVel;
      if (stillHeld > 0) {
        _scene.info += ", " + std::to_string(stillHeld)
                     + " with no match at shutter close (held still)";
      }
      // deformation: the same object list with points that move
      if (_deformationBlur && closeScene.vertices.size() == _scene.vertices.size()
          && closeScene.instances.size() == _scene.instances.size()) {
        bool deforms = false;
        for (size_t i = 0; i < closeScene.vertices.size() && !deforms; ++i) {
          const ir::Vec3& a = _scene.vertices[i];
          const ir::Vec3& b = closeScene.vertices[i];
          deforms = (a.x != b.x || a.y != b.y || a.z != b.z);
        }
        if (deforms) {
          _scene.vertices1 = closeScene.vertices;
          _scene.info += ", deformation blur";
        }
      }
      if (moved > 0) {
        _scene.motionKeys.swap(keys);
        _scene.motionKeyCount = 2;
        _scene.hasMotion = true;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", travel);
        _scene.info += ", motion blur on " + std::to_string(moved) + " object(s), max travel " + buf;
        if (!travels.empty()) {
          std::sort(travels.begin(), travels.end());
          char mb[64];
          std::snprintf(mb, sizeof(mb), "%.4f", travels[travels.size() / 2]);
          _scene.info += ", median travel " + std::string(mb);
        }
      }
    }
    else if (!closeErr.empty()) {
      _scene.warnings += "motion blur: the geometry could not be read at shutter close (" + closeErr + ")\n";
    }
  }
  return true;
}

#if IR_HAVE_USD
bool InstanceRender::loadStageScene(std::string& err)
{
  ir::textureProbe(&irlog);
  Op* in0 = input(0);
  irlog(std::string("loadScene: inputs=") + std::to_string(inputs()) + " in0=" + (in0 ? in0->Class() : "null") +
        " node_inputs=" + std::to_string(node_inputs()));
  try {
    irlog("loadScene: validating input");
    in0->validate(true);
    {
      const std::string validateErr = upstreamError(in0);
      if (!validateErr.empty()) {
        _stageDiag += "upstream error while validating the scene: " + validateErr + "\n";
        irlog("loadScene: " + _stageDiag);
      }
    }
    const double frameNow = outputContext().frame();
    double shutterOpen = 0.0, shutterClose = 0.0;
    if (_shutter > 0.0) {
      if (_shutterOffset == kShutterStart) { shutterOpen = 0.0; shutterClose = _shutter; }
      else if (_shutterOffset == kShutterEnd) { shutterOpen = -_shutter; shutterClose = 0.0; }
      else { shutterOpen = -0.5 * _shutter; shutterClose = 0.5 * _shutter; }
    }
    // Motion vectors with a shut shutter: comps usually want a SHARP render plus
    // vectors to blur it with later, so open a one-frame window purely to see
    // where everything goes next.  Every ray is then pinned to the start of that
    // window (fixedShutterTime 0 in renderFrame), so nothing actually blurs.
    _mvOnly = (_outMotion && _shutter <= 0.0);
    if (_mvOnly) { shutterOpen = 0.0; shutterClose = 1.0; }
    _motionFrames = shutterClose - shutterOpen;
    const bool motionWindow = (_motionFrames > 0.0);

    // Motion blur: ask for ONE stage carrying every shutter sample.
    //
    // BuildStage has an overload that takes a whole set of sample times, which
    // is how Nuke means motion blur to be requested: the engine authors real
    // time samples for those times, including sub-frame ones, and everything is
    // consistent because it is one stage built once.  Building the stage
    // repeatedly at different times instead makes Nuke re-author each material
    // into a second stage, which is where users saw
    //   "Can't set time sample on .../<node>_UVTexture.inputs:scale ...
    //    expected a value of type TfToken"
    // and it also costs a stage build per key.
    const int motionKeys = !motionWindow ? 0 : (_mvOnly ? 2 : std::max(2, std::min(8, _motionSamples)));
    std::vector<double> keyTimes;
    if (motionKeys >= 2) {
      for (int k = 0; k < motionKeys; ++k)
        keyTimes.push_back(frameNow + shutterOpen + (shutterClose - shutterOpen) * (double(k) / double(motionKeys - 1)));
    }

    // Building the close frame above left the upstream op evaluated there, so
    // fetch it back at THIS frame before building the main stage - otherwise the
    // geometry baked into it is the shutter-close one.
    // Always fetch the input through the graph rather than using input(0) as it
    // stands: that is what hands back an op tree built for this context, which
    // is what the stage build needs (Nuke 15 crashes outright without it).
    Op* in0Main = in0;
    if (Op* at = ir::inputAtContext(this, 0, outputContext())) in0Main = at;
    in0Main->validate(true);
    // whatever validating the input reported, recorded before the stage build
    // goes quiet below
    {
      const std::string validateErr = upstreamError(in0Main);
      if (!validateErr.empty()) {
        _stageDiag += "upstream error while validating the scene: " + validateErr + "\n";
        irlog("loadScene: " + _stageDiag);
      }
    }

    const double frame = frameNow;
    fdk::TimeValue sampleTime(frame);

    // Ask the PROVIDER for its stage rather than authoring a new one.
    //
    // BuildStage() writes into whatever stage it is handed, and a stage created
    // here has no compose state in common with the one the provider already
    // built, so every op upstream re-authors itself from scratch - including
    // Nuke's material ops, which fail the second time round with
    //   Can't set time sample on .../<node>_UVTexture.inputs:scale ...
    //   expected a value of type "TfToken"
    // and take the whole render down with them.  getGeometryStage() hands back
    // the provider's own composed stage (that is what ScanlineRender2 gets, and
    // why it renders the same scene happily), and it takes the sample times we
    // want, so motion blur comes out of the same call.
    irlog("loadScene: getGeometryStage");
    // Sample times are only asked for when they are needed: with no motion the
    // set stays EMPTY, so the provider authors default values instead of time
    // samples - and it is the time-sample path that Nuke's material ops fail on.
    fdk::TimeValueSet stageTimes;
    for (size_t k = 0; k < keyTimes.size(); ++k) stageTimes.insert(fdk::TimeValue(keyTimes[k]));

    // Building the stage is done with error observation OFF.
    //
    // Nuke's own material ops fail while authoring into a stage this node asked
    // for - "Can't set time sample on .../<node>_UVTexture.inputs:scale ...
    // expected a value of type TfToken" - which turns an upstream node red and
    // aborts the render even though the geometry and the render are fine.  The
    // message cannot be cleared afterwards, so it is kept from being recorded in
    // the first place.  Genuine failures are not lost: if the stage comes back
    // invalid or carries no geometry at all, it is built again with errors
    // observed so the real problem surfaces the normal way.
    struct QuietErrors {
      bool on;
      explicit QuietErrors(bool q) : on(q) { if (on) DD::Image::OpMessageHandler::IgnoreErrors(); }
      ~QuietErrors() { if (on) DD::Image::OpMessageHandler::ObserveErrors(); }
    };

    usg::StageRef usgStage;
    {
      QuietErrors quiet(!_reportStageErrors);
      bool fallback = false;
      ir::compatLog() = &irlog;
      usgStage = ir::acquireStage(in0Main, stageTimes, fallback);
      if (fallback) irlog("loadScene: fell back to BuildStage");
    }
    if (!usgStage) { err = "could not create an in-memory stage"; return false; }
    irlog("loadScene: stage ready");
    if (!usgStage->isValid()) { err = "BuildStage produced an invalid stage"; return false; }
    usg::Stage::Handle* handle = usgStage->getUsdStageRefPtr(usg::usdAPIVersion());
    irlog(std::string("loadScene: handle=") + (handle ? "yes" : "no"));
    if (!handle) { err = "usg/pxr version mismatch (getUsdStageRefPtr)"; return false; }
    PXR_NS::UsdStageRefPtr* pxrStage = reinterpret_cast<PXR_NS::UsdStageRefPtr*>(handle);
    if (!pxrStage || !*pxrStage) { err = "null pxr stage"; return false; }
    irlog("loadScene: pxr stage ok, loading");
    ir::LoaderOptions opt;
    opt.timeCode = frame;
    opt.cameraPath = _cameraPath ? _cameraPath : "";
    opt.includeGuides = _includeGuides;
    opt.lightsVisible = _lightsVisible;
    opt.textures = _textures;
    opt.maxTextureSize = _maxTextureSize;
    opt.textureCache = &_textureCache;
    opt.mipFilter = _mipFilter;
    opt.subdivLevels = _subdivLevels;
    // Only the MAIN load reads a second frame - the motion-key passes below want
    // transforms and would otherwise read the whole sequence again per key.
    // Which frame the shutter closes on, worked out the same way the camera's is.
    {
      double open = -0.5 * _shutter;
      if (_shutterOffset == kShutterStart) open = 0.0;
      else if (_shutterOffset == kShutterEnd) open = -_shutter;
      opt.volumeBlur = _volumeBlur && _shutter > 0.0;
      opt.volumeCloseFrame = int(std::floor(frame + open + _shutter + 0.5));
    }
    // textures fed by Nuke nodes are read from the node, not through Hio
    opt.nukeOpImage = [this](const std::string& p, int m, ir::ImageData& o) {
      // Self-test hook: IR_TEXTURE_FAIL=<n> makes the first n bakes of a
      // Nuke-node texture fail, the same idea as IR_CUT_SHORT.  A real one fails
      // only when the timeline is moving and Nuke abandons the op mid render,
      // which is not something a test can arrange.
      {
        const int want = ir::envInt("IR_TEXTURE_FAIL");
        if (_textureFailInjected < want) { ++_textureFailInjected; return false; }
      }
      return this->nukeOpTexture(p, m, o);
    };
    // What is the op that an nkop: path names doing NOW?  Only this side can
    // answer - the loader sees a USD stage, not the Nuke graph.  Empty means it
    // is not feeding this render any more; otherwise the op's LIVE hash, which
    // moves when the picture does even though the URI does not.
    opt.nukeOpIdentity = [this](const std::string& p) -> std::string {
      const std::string n = nukeOpNameFromPath(p);
      if (n.empty()) return std::string("?");   // not a name we can check: assume live
      DD::Image::Iop* iop = nullptr;
      for (int i = 0; i < 3 && !iop; ++i) iop = findIopByName(input(i), n, 0);
      if (!iop) {
        irlog("nukeOpIdentity: " + n + " is no longer upstream - the texture is disconnected");
        return std::string();
      }
      std::ostringstream os;
      os << std::hex << iop->hash().value();
      return os.str();
    };
    opt.pointDetail = _pointDetail;
    opt.curveSides = _curveSides;
    opt.curveSegments = _curveSegments;
    // the keys come from the per-key builds above, so the main load just reads
    // the shutter-open pose and does no motion sampling of its own
    opt.timeCode = frame + shutterOpen;
    opt.shutterOpen = 0.0;
    opt.shutterClose = 0.0;
    opt.motionKeys = 2;
    opt.maxExpandedTriangles = _maxTriangles * 1e6;
    const bool ok = ir::loadStage(*pxrStage, opt, _scene);
    // The temperature -> colour table, built once per load rather than per sample -
    // an integral against the colour matching functions is not something to do
    // inside a march.  Outside the motion block on purpose: it is needed whenever
    // a volume is, and a still scene has volumes too.
    ir::buildBlackbodyLut(_scene, ir::kBlackbodyMinK, ir::kBlackbodyMaxK, _spectralBlackbody);
    {
      std::vector<std::string> names;
      for (size_t i = 0; i < _scene.textureNames.size(); ++i) {
        const std::string& tp = _scene.textureNames[i];
        if (tp.compare(0, 5, "nkop:") != 0) continue;
        const std::string n = nukeOpNameFromPath(tp);
        if (!n.empty()) names.push_back(n);
      }
      std::lock_guard<std::mutex> lock(_nkopMutex);
      _nkopOps.swap(names);
    }
    irlog(std::string("loadScene: loadStage ok=") + (ok ? "1" : "0") + " " + _scene.info);

    // Nothing arrived?  Then something upstream really did go wrong, and the
    // quiet build swallowed the reason - do it again with errors observed so it
    // reaches the user the usual way, and put it in this node's report too.
    // A VOLUME IS A WHOLE SCENE. Judging "nothing arrived" on geometry alone put
    // "the scene came back empty" in the report of every .vdb that rendered
    // perfectly well, which is a misleading thing to read while hunting a
    // different bug.
    const bool nothingArrived = _scene.protos.empty() && _scene.instances.empty()
                             && _scene.volumes.empty();
    if (!ok || nothingArrived) {
      const std::string buildErr = upstreamError(in0Main);
      if (!buildErr.empty()) {
        _stageDiag += "upstream error while building the stage (frame " + std::to_string(frame) + "): " + buildErr + "\n";
      }
      else if (!_reportStageErrors) {
        // the message, if there was one, was suppressed on the way past
        _stageDiag += "the scene came back empty, and errors raised while the stage is built are hidden "
                      "(see the 'report stage-build errors' knob) - turn that on to hear what the nodes "
                      "upstream have to say.\n";
      }
      irlog("loadScene: empty stage");
    }
    if (!ok) { err = _scene.warnings.empty() ? "stage load failed" : _scene.warnings; return false; }

    // IR_MOTION_FORCE_FALLBACK=1 throws away the stage's own motion so the
    // fallback below runs even on a graph that authors time samples - it is the
    // only way to exercise that path deterministically.
    if (_shutter > 0.0 && _scene.hasMotion && envFlag("IR_MOTION_FORCE_FALLBACK")) {
      for (size_t i = 0; i < _scene.instances.size(); ++i) _scene.instances[i].xf1 = _scene.instances[i].xf;
      _scene.hasMotion = false;
      _scene.info += " [forced fallback]";
    }

    // Motion blur: build a SECOND stage that carries every shutter sample and
    // read the instance transforms out of it.  BuildStage's multi-sample-time
    // overload is how Nuke means motion to be requested - the engine authors
    // real time samples, sub-frame ones included, in a single build.
    //
    // But it cannot be asked for that when Nuke's own material ops are in the
    // graph: they author their shader inputs once and then fail on the second
    // sample with
    //   Can't set time sample on .../<node>_UVTexture.inputs:scale to
    //   (0.18, 0.18, 0.18, 1): expected a value of type "TfToken"
    // which turns the upstream node red and aborts the render.  The message
    // cannot be cleared afterwards, so the only sound thing to do is not
    // provoke it: render sharp and say why.
    // Motion blur: the stage above was asked for every shutter sample, so the
    // keys are just reads of it at those time codes - a traversal each
    // (transformsOnly), no second build and nothing re-authored.
    std::vector<std::vector<ir::Xform> > motionSets;
    std::vector<ir::Vec3> closeVerticesOut;
    if (motionKeys >= 2) {
      motionSets.resize(size_t(motionKeys));
      for (int k = 0; k < motionKeys; ++k) {
        ir::LoaderOptions optK;
        optK.timeCode = keyTimes[size_t(k)];
        // every key extrapolates from the frame being rendered, so a
        // PointInstancer with velocities gives the same particles at each key
        optK.baseTimeCode = frame;
        optK.hasBaseTime = true;
        optK.includeGuides = _includeGuides;
        optK.textures = false;
        optK.mipFilter = false;
        optK.subdivLevels = 0;
        optK.transformsOnly = true;
        ir::Scene atK;
        if (ir::loadStage(*pxrStage, optK, atK)) {
          motionSets[size_t(k)].resize(atK.instances.size());
          for (size_t i = 0; i < atK.instances.size(); ++i) motionSets[size_t(k)][i] = atK.instances[i].xf;
          if (std::getenv("IR_MOTION_PROBE") && !atK.instances.empty()) {
            std::cerr << "IR_MOTION: key " << k << " t=" << keyTimes[size_t(k)]
                      << " base=" << frame << " -> " << atK.instances.size()
                      << " instance(s), first at x=" << atK.instances[0].xf.m[3]
                      << " y=" << atK.instances[0].xf.m[7] << std::endl;
          }
        }
      }
      if (_deformationBlur) {
        ir::LoaderOptions optD = opt;
        optD.timeCode = keyTimes.back();
        optD.shutterOpen = optD.shutterClose = 0.0;
        optD.textures = false;
        optD.mipFilter = false;
        optD.maxExpandedTriangles = 0.0;
        ir::Scene atClose;
        if (ir::loadStage(*pxrStage, optD, atClose)) closeVerticesOut.swap(atClose.vertices);
      }
    }

    if (motionKeys >= 2 && !_scene.instances.empty() && motionSets.size() == size_t(motionKeys)) {
      const size_t n = _scene.instances.size();
      const std::vector<std::vector<ir::Xform> >& keys = motionSets;
      bool complete = true;
      for (int k = 0; k < motionKeys && complete; ++k)
        if (keys[size_t(k)].size() != n) complete = false;
      if (complete) {
        int moved = 0;
        double travel = 0.0;
        std::vector<ir::Xform> flat;
        flat.resize(n * size_t(motionKeys));
        for (size_t i = 0; i < n; ++i) {
          ir::Instance& in = _scene.instances[i];
          const ir::Xform key0 = keys[0][i];
          bool moves = false;
          for (int k = 0; k < motionKeys; ++k) {
            const ir::Xform x = keys[size_t(k)][i];
            flat[i * size_t(motionKeys) + size_t(k)] = x;
            for (int c = 0; c < 12 && !moves; ++c) moves = (x.m[c] != key0.m[c]);
          }
          in.xf = key0;
          in.xf1 = keys[size_t(motionKeys - 1)][i];
          in.firstKey = int(i) * motionKeys;
          if (moves) {
            ++moved;
            const double dx = in.xf1.m[3] - key0.m[3], dy = in.xf1.m[7] - key0.m[7], dz = in.xf1.m[11] - key0.m[11];
            travel = std::max(travel, std::sqrt(dx * dx + dy * dy + dz * dz));
          }
        }
        if (moved > 0) {
          _scene.motionKeys.swap(flat);
          _scene.motionKeyCount = motionKeys;
          _scene.hasMotion = true;
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.4f", travel);
          _scene.info += ", motion blur on " + std::to_string(moved) + " instance(s), max travel " + buf
                       + ", " + std::to_string(motionKeys) + " key(s)";
        }
        else {
          _scene.motionKeys.clear();
          _scene.motionKeyCount = 0;
          _scene.hasMotion = false;
          for (size_t i = 0; i < n; ++i) _scene.instances[i].xf1 = _scene.instances[i].xf;
        }
      }
      else {
        _scene.warnings += "motion blur: the instance count changes across the shutter, rendering without it\n";
        _scene.motionKeys.clear();
        _scene.motionKeyCount = 0;
        _scene.hasMotion = false;
      }
            irlog(std::string("loadScene: motion pass hasMotion=") + (_scene.hasMotion ? "1" : "0"));
    }
    // deformation blur: read the geometry again at shutter close, out of the
    // same stage, and keep it only if the two ends describe the same mesh
    std::vector<ir::Vec3>& closeVertices = closeVerticesOut;
    if (_deformationBlur && motionWindow && !closeVertices.empty()) {
      if (closeVertices.size() == _scene.vertices.size()) {
        bool deforms = false;
        for (size_t i = 0; i < closeVertices.size() && !deforms; ++i) {
          const ir::Vec3& a = _scene.vertices[i];
          const ir::Vec3& b = closeVertices[i];
          deforms = (a.x != b.x || a.y != b.y || a.z != b.z);
        }
        if (deforms) {
          _scene.vertices1.swap(closeVertices);
          _scene.info += ", deformation blur";
        }
      }
      else {
        _scene.warnings += "deformation blur: the vertex count changes across the shutter ("
                         + std::to_string(_scene.vertices.size()) + " vs " + std::to_string(closeVertices.size())
                         + "), rendering the geometry at shutter open\n";
      }
    }
    return true;
  }
  catch (const std::exception& e) { err = std::string("exception while loading the stage: ") + e.what(); return false; }
  catch (...) { err = "unknown exception while loading the stage"; return false; }
}
#endif // IR_HAVE_USD

// The camera as it stands at another frame, for the motion vectors.  Nothing
// else uses it: the beauty is rendered with one camera, so nothing here can
// change what the image looks like.
bool InstanceRender::cameraAtFrame(double frame, int W, int H, double pixelAspect, ir::Camera& out)
{
  if (!input(1)) return false;
  OutputContext ctx = outputContext();
  ctx.setFrame(frame);
  Op* at = ir::inputAtContext(this, 1, ctx);
  CameraOp* cam = dynamic_cast<CameraOp*>(at);
  if (!cam) return false;
  try { cam->validate(true); } catch (...) { return false; }
  const fdk::Mat4d& cw = cam->worldTransform();
  ir::Xform x;
  x.m[0] = float(cw[0][0]); x.m[1] = float(cw[1][0]); x.m[2] = float(cw[2][0]); x.m[3] = float(cw[3][0]);
  x.m[4] = float(cw[0][1]); x.m[5] = float(cw[1][1]); x.m[6] = float(cw[2][1]); x.m[7] = float(cw[3][1]);
  x.m[8] = float(cw[0][2]); x.m[9] = float(cw[1][2]); x.m[10] = float(cw[2][2]); x.m[11] = float(cw[3][2]);
  out.camToWorld = x;
  const double focal = cam->focalLength();
  const double hap = cam->horizontalAperture();
  const float tanX = (focal > 1e-9) ? float(0.5 * hap / focal) : 0.5f;
  const double aspect = double(H) / (double(W) * (pixelAspect > 0.0 ? pixelAspect : 1.0));
  out.tanHalfFovX = tanX;
  out.tanHalfFovY = tanX * float(aspect);
  out.orthographic = ir::isOrthographic(cam) ? 1 : 0;
  if (out.orthographic) {
    const float halfW = float(0.5 * hap);
    out.orthoHalfW = halfW;
    out.orthoHalfH = halfW * float(aspect);
  }
  out.width = W; out.height = H;
  return true;
}

void InstanceRender::cameraFromInput(int W, int H, double pixelAspect)
{
  Op* camOp = input(1);
  CameraOp* cam = dynamic_cast<CameraOp*>(camOp);
  irlog(std::string("cameraFromInput: in1=") + (camOp ? camOp->Class() : "null") + " cast=" + (cam ? "ok" : "FAILED"));
  if (!cam) return;
  try { cam->validate(true); } catch (...) { irlog("cameraFromInput: validate threw"); return; }
  // camera -> world (fdk::Mat4d, row-vector convention: p' = p * M) -> our rows = columns of M
  const fdk::Mat4d& cw = cam->worldTransform();
  ir::Xform x;
  x.m[0] = float(cw[0][0]); x.m[1] = float(cw[1][0]); x.m[2] = float(cw[2][0]); x.m[3] = float(cw[3][0]);
  x.m[4] = float(cw[0][1]); x.m[5] = float(cw[1][1]); x.m[6] = float(cw[2][1]); x.m[7] = float(cw[3][1]);
  x.m[8] = float(cw[0][2]); x.m[9] = float(cw[1][2]); x.m[10] = float(cw[2][2]); x.m[11] = float(cw[3][2]);
  _scene.camera.camToWorld = x;
  // Nuke fits the horizontal aperture to the image width; the vertical fov follows the format aspect
  const double focal = cam->focalLength();
  const double hap = cam->horizontalAperture();
  const float tanX = (focal > 1e-9) ? float(0.5 * hap / focal) : 0.5f;
  const double aspect = double(H) / (double(W) * (pixelAspect > 0.0 ? pixelAspect : 1.0));
  _scene.camera.tanHalfFovX = tanX;
  _scene.camera.tanHalfFovY = tanX * float(aspect);
  _scene.camera.orthographic = ir::isOrthographic(cam) ? 1 : 0;
  if (_scene.camera.orthographic) {
    const float halfW = float(0.5 * hap);   // ortho: aperture in scene units across the image
    _scene.camera.orthoHalfW = halfW;
    _scene.camera.orthoHalfH = halfW * float(aspect);
  }
  _scene.camera.nearClip = float(cam->nearPlaneDistance());
  _scene.camera.farClip = float(cam->farPlaneDistance());
  _scene.hasCamera = true;
  {
    std::ostringstream os;
    os << "cameraFromInput: pos(" << x.m[3] << "," << x.m[7] << "," << x.m[11] << ") focal " << focal << " hap " << hap
       << " tanX " << _scene.camera.tanHalfFovX << " near " << _scene.camera.nearClip << " far " << _scene.camera.farClip;
    irlog(os.str());
  }
}

std::string InstanceRender::deviceReport() const
{
  std::string s = "CPU: " + ir::CpuRenderer::version();
#if IR_HAVE_OPTIX
  s += "; GPU: " + ir::GpuRenderer::deviceName();
#else
  s += "; GPU: not built";
#endif
  return s;
}

bool InstanceRender::_progressiveActive() const
{
  if (!_progressive) return false;
  // IR_PROGRESSIVE_FORCE lets the headless test drive the refinement loop by
  // executing repeatedly (each pass changes the hash, so the next pull renders
  // the next chunk); everywhere else the viewer is the only client.
  if (!envFlag("IR_PROGRESSIVE_FORCE")) {
    if (!DD::Image::Application::gui) return false;    // terminal / batch renders are always full quality
    if (envFlag("IR_EXECUTING")) return false;         // a Write is running
  }
  return _samples > std::max(1, _previewSamples);
}

// Nuke asks a render to stop by setting aborted() on the Op; the back-ends
// watch a plain atomic, because they know nothing about Nuke.  This bridges the
// two for the length of one render.  Without it a scrub queues up one full
// render per frame it crossed and every one of them runs to completion, which
// is the difference between a viewer that lags and one that has stopped.
namespace {
class AbortWatcher {
public:
  AbortWatcher(const DD::Image::Op* op, std::atomic<bool>& flag)
    : _op(op), _flag(flag), _stop(false)
  {
    _flag.store(false);
    _thread = std::thread([this] {
      while (!_stop.load()) {
        if (_op->aborted()) { _flag.store(true); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
  ~AbortWatcher()
  {
    _stop.store(true);
    if (_thread.joinable()) _thread.join();
  }
private:
  const DD::Image::Op* _op;
  std::atomic<bool>& _flag;
  std::atomic<bool> _stop;
  std::thread _thread;
};
} // namespace

// Everything that touches Nuke's node graph, done at VALIDATE time.
//
// It used to be the first half of renderFrame(), and that is a deadlock: a
// render runs on a pooled render thread (measured - _open() came in on 16688,
// 43732, 47488 ... while _validate() was always 44784), and asking the graph
// for an input at another frame takes a lock inside Nuke.  When the main
// thread is inside a viewer paint waiting for that render to finish - which is
// most of the time in a GUI - the two wait on each other for ever.  Caught in
// the act: main in QGLWidget::paintEvent -> DD::Image::Thread::wait, the render
// thread in loadClassicScene -> RtlEnterCriticalSection, twenty-four more
// queued behind it.
//
// Resolving the input POINTERS ahead of time does not fix it and was tried:
// node_input() hands back the same Op* for every context, so the frame it
// speaks for is a side effect of the call itself, and deferring the call
// defers the effect - motion blur simply stops (measured: travel 1.000 -> 
// 0.000).  The work has to move, not the lookup.
//
// So: the scene, the camera and the lights are built here, and renderFrame()
// below may touch nothing but Embree, OptiX and its own buffers.
void InstanceRender::prepareScene()
{
  Hash want;
  appendContent(want);
  if (_scenePrepared && want == _sceneHash) return;    // nothing has moved
  _sceneHash = want;
  // Self-test hook: IR_CUT_SHORT=<n> makes the first n loads behave as though
  // Nuke had cut them short - the scene is NOT built, exactly as when a load
  // does not finish.  Without it this can only be exercised by dragging a handle
  // and hoping the abort lands inside the load, which is the kind of
  // verification this project has been burned by; and the width of that window
  // is what subdivision changes, so it is not even the same test twice.  See
  // IR_STALL for the same idea applied to the watchdog.
  _injectedCutShort = false;
  {
    const int want = ir::envInt("IR_CUT_SHORT");
    if (_cutShortInjected < want) { ++_cutShortInjected; _injectedCutShort = true; }
  }

  // NOT true yet.  The load below can be cut short - Nuke aborts constantly
  // while a handle is being dragged - and claiming the hash first was the bug:
  // the scene for the new handle position was never loaded, every later validate
  // said "nothing has moved", and the image simply stopped updating.  Worse with
  // subdivision on, which widens the window to be interrupted in.
  // settlePreparedScene() decides at the end, when the answer is known.
  _scenePrepared = false;
  _sceneErr.clear();
  if (_injectedCutShort) {
    _sceneOk = false;
    _sceneErr = "render cancelled";
    settlePreparedScene();
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  _lastRenderAborted = false;
  irlog("renderFrame: start frame=" + std::to_string(outputContext().frame()));
  ir::watchdogMark("renderFrame: loading the scene");
  {
    // Self-test hook: IR_STALL=<seconds> makes a render hang ON PURPOSE, so the
    // watchdog's phase clock can be shown to fire without waiting for a real
    // freeze to turn up.  A freeze detector that has never been seen to detect
    // a freeze is not evidence of anything, and the alternative - tuning the
    // limit down until a genuine phase happens to overrun - is flaky, because
    // whether it trips depends on where the poll lands.
    const std::string stallStr = ir::envString("IR_STALL");
    if (!stallStr.empty()) {
      const double secs = std::atof(stallStr.c_str());
      if (secs > 0.0) {
        irlog("renderFrame: IR_STALL - hanging on purpose for " + std::to_string(secs) + "s");
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(secs * 1000.0)));
        irlog("renderFrame: IR_STALL - released");
      }
    }
  }
  const Format& fmt = info_.format();
  const int W = fmt.width(), H = fmt.height();
  std::string& err = _sceneErr;
  std::string prefix;                 // device fallback note, if any
  _status.clear();
  _stageDiag.clear();
  _sceneOk = loadScene(err);
  if (!_sceneOk) {
    // Only RECORD it here.  Announcing a failure - the status, the warning that
    // turns the node red - belongs to the render, because that is what a person
    // is waiting on; validate runs far more often and would say it repeatedly.
    settlePreparedScene();
    ir::watchdogMark(nullptr);
    return;
  }
  // Deliberately NOT an early return on aborted().  The report - what was in
  // the scene, what device ran, what went wrong - is composed by the build and
  // the render themselves, so giving up here leaves the info knob empty, and a
  // tree that was aborted because an upstream op ERRORED is exactly when
  // somebody wants to read it.  The render is cut short from inside instead:
  // the back-ends watch the cancel flag.
  // camera: Camera input overrides the stage camera; neither -> default looking at the origin
  irlog("renderFrame: scene loaded, camera next");
  ir::watchdogMark("renderFrame: camera");
  cameraFromInput(W, H, fmt.pixel_aspect());
  irlog("renderFrame: camera done");
  if (!_scene.hasCamera) {
    ir::Xform x = ir::Xform::identity();
    x.m[3] = 0.0f; x.m[7] = 1.0f; x.m[11] = 5.0f;
    _scene.camera.camToWorld = x;
    _scene.camera.tanHalfFovX = 0.5f;
    _scene.camera.tanHalfFovY = 0.5f * float(double(H) / double(W));
  }
  else if (!input(1)) {
    // stage camera: the aperture ratio decides the vertical fov, override with the format aspect like Nuke does
    _scene.camera.tanHalfFovY = _scene.camera.tanHalfFovX * float(double(H) / (double(W) * fmt.pixel_aspect()));
  }
  _scene.camera.width = W; _scene.camera.height = H;

  // ---- the camera at shutter close -------------------------------------------------
  // A still scene shot by a moving camera still moves on screen, and NOTHING
  // else in the pipeline can express that: instance blur describes geometry
  // moving past a fixed camera. Without this a pan rendered identically with the
  // shutter shut and wide open - measured, a moving cube covered 3857 pixels
  // sharp and 10116 blurred, while the same move given to the camera covered
  // 3856 either way.
  _scene.cameraClose = _scene.camera;
  _scene.cameraMoves = false;
  if (_cameraBlur && _shutter > 0.0 && input(1)) {
    const double frameNow = outputContext().frame();
    double open = -0.5 * _shutter;
    if (_shutterOffset == kShutterStart) open = 0.0;
    else if (_shutterOffset == kShutterEnd) open = -_shutter;
    ir::Camera cOpen = _scene.camera, cClose = _scene.camera;
    const bool gotOpen = cameraAtFrame(frameNow + open, W, H, fmt.pixel_aspect(), cOpen);
    const bool gotClose = cameraAtFrame(frameNow + open + _shutter, W, H, fmt.pixel_aspect(), cClose);
    if (gotOpen && gotClose) {
      // The render's own camera becomes the one at shutter OPEN, because that is
      // what t=0 has to mean for the lerp to land on the right instants.
      _scene.camera = cOpen;
      _scene.camera.width = W; _scene.camera.height = H;
      _scene.cameraClose = cClose;
      _scene.cameraClose.width = W; _scene.cameraClose.height = H;
      float moved = 0.0f;
      for (int i = 0; i < 12; ++i)
        moved = std::max(moved, std::fabs(cClose.camToWorld.m[i] - cOpen.camToWorld.m[i]));
      moved = std::max(moved, std::fabs(cClose.tanHalfFovX - cOpen.tanHalfFovX));
      _scene.cameraMoves = (moved > 1e-7f);
      if (_scene.cameraMoves) {
        char cb[96];
        std::snprintf(cb, sizeof(cb), ", camera motion blur over frames %.3f..%.3f",
                      frameNow + open, frameNow + open + _shutter);
        _scene.info += cb;
      }
    }
    // the camera input was validated at other frames above; leave it on this one
    if (!aborted())
      if (Op* back = ir::inputAtContext(this, 1, outputContext())) back->validate(true);
  }
  // motion vectors measure the travel between the ends of the motion window, so
  // a camera move counts too: both ends are projected through the camera of
  // their own moment
  _scene.cameraMv0 = _scene.camera;
  _scene.cameraMv1 = _scene.camera;
  if (_outMotion && _motionFrames > 0.0) {
    const double frameNow = outputContext().frame();
    double open = 0.0;
    if (!_mvOnly) {
      if (_shutterOffset == kShutterStart) open = 0.0;
      else if (_shutterOffset == kShutterEnd) open = -_shutter;
      else open = -0.5 * _shutter;
    }
    ir::Camera c0 = _scene.camera, c1 = _scene.camera;
    if (cameraAtFrame(frameNow + open, W, H, fmt.pixel_aspect(), c0)) _scene.cameraMv0 = c0;
    if (cameraAtFrame(frameNow + open + _motionFrames, W, H, fmt.pixel_aspect(), c1)) _scene.cameraMv1 = c1;
    // the render itself is left on the camera at this frame
    // see the note in loadScene: validating an input after Nuke has cancelled
    // the render deadlocks against the main thread
    if (!aborted())
      if (Op* back = ir::inputAtContext(this, 1, outputContext())) back->validate(true);
  }
  // headlight when the stage has no light
  if (_scene.lights.empty() && _headlight > 0.0) {
    ir::Light L;
    L.type = ir::kLightDistant;
    L.direction = ir::normalize(_scene.camera.camToWorld.vector(ir::Vec3(0.0f, 0.0f, -1.0f)));
    L.color = ir::Vec3(float(_headlight));
    L.angle = 5.0f;
    _scene.lights.push_back(L);
    _scene.lightNames.push_back("headlight");
  }

  // A provisional report, which the render will overwrite with the full one.
  // It matters because an upstream op that has errored can stop Nuke asking
  // for a render at all - and then the render is not there to say what went
  // wrong, which is exactly the moment somebody opens the info knob.  This
  // used to come out right by accident, when the loading happened inside the
  // render itself.
  _sceneInfo = _scene.info;
  std::string provisional = _sceneInfo;
  const std::string nl(1, '\n');
  // The shutter line belongs here as well as in the render's own summary: when
  // the stage failed to build, how many stages were being built is half of what
  // the reader is trying to work out.
  if (_shutter > 0.0) {
    std::ostringstream shut;
    shut << "shutter " << _shutter << " x " << _motionSamples << " key(s) (2 stage builds)";
    provisional += (provisional.empty() ? std::string() : nl) + shut.str();
  }
  if (!_sceneOk && !_sceneErr.empty())
    provisional += (provisional.empty() ? std::string() : nl) + "ERROR: " + _sceneErr;
  if (!_stageDiag.empty())
    provisional += (provisional.empty() ? std::string() : nl) + _stageDiag;
  if (!provisional.empty()) publishReport(node(), provisional);
  settlePreparedScene();
}

// May the scene that was just built be REMEMBERED as the one this hash asked
// for?  Only if nothing interrupted the building of it.
//
// This is the same lesson as caching a render (see _open): the question has to
// be answered by what happened WHILE the work ran, never by asking aborted()
// afterwards - by then Nuke has usually cleared the abort for the next frame.
// The load has its own version of it, and the USD path makes it sharper: being
// aborted there does not FAIL, it only skips the refetch, so loadScene hands
// back success with geometry that may be from the wrong frame.  There is no
// error to notice; the picture just stops following the handles.
void InstanceRender::settlePreparedScene()
{
  // A texture wired from a Nuke node is read by BAKING that node, and a bake can
  // fail while the timeline is moving.  The scene still loads - just without that
  // texture - so nothing looked wrong here, the render was remembered, and the
  // surface stayed untextured for that frame until something else changed it.
  // That is the node's version of the fault the Hydra delegate had, and it gets
  // the same answer as a load that was cut short: do not remember it, come back
  // once more.  The retry is bounded by _retriedCutShort below, so a texture
  // that genuinely cannot be read settles after one extra pass.
  const bool lostATexture = (_scene.nukeTextureFailures > 0);
  if (lostATexture)
    irlog("prepareScene: " + std::to_string(_scene.nukeTextureFailures)
          + " Nuke-node texture(s) would not bake - asking for another pass");

  // NOTHING RENDERABLE CAME BACK.
  //
  // A scene with no instances AND no volumes is either genuinely empty - fine,
  // it settles after the one extra pass below - or it is a load that ran before
  // the ops upstream were ready to answer, which is what a freshly opened script
  // does. That second case cached a black frame whose hash never changed again,
  // so it stayed black until the timebar was scrubbed.
  //
  // Volumes are counted here on purpose: judging this on geometry alone would
  // ask for a pointless extra pass on every .vdb-only scene, which is a whole
  // second load of a half-gigabyte grid.
  const bool nothingRenderable = _scene.instances.empty() && _scene.volumes.empty();
  if (nothingRenderable)
    irlog("prepareScene: nothing renderable came back - asking for another pass");

  if (!_injectedCutShort && !lostATexture && !nothingRenderable
      && !(aborted() || _cancel.load())) {
    _scenePrepared = true;             // it is the scene this hash asked for
    _retriedCutShort = false;
    return;
  }

  // Cut short.  _scenePrepared stays false, so the next validate rebuilds - but
  // on its own that changes nothing, because nothing would ASK for another one:
  // the frame that just rendered caches normally and its hash never changes
  // again, so the viewer keeps it for ever.
  //
  // So press "refresh render", automatically.  The epoch is part of the content
  // hash, so bumping it gives the next pass a hash of its own, which re-renders
  // AND resets the progressive accumulator instead of mixing stale samples with
  // fresh ones.  Nothing about the cache rules changes: a completed render is
  // still cached, which matters because engine() runs once per ROW and an
  // uncacheable render is re-run for every row of the frame.
  //
  // ONCE.  aborted() is also true when an upstream op has ERRORED, and that
  // does not go away, so an unguarded retry would loop for ever.  One pass is
  // enough for the case this exists for - the retry carries no incoming abort,
  // and a drag still in progress is generating new hashes anyway - and a real
  // error settles after one extra pass with its report intact.
  if (_retriedCutShort) return;
  _retriedCutShort = true;
  irlog("prepareScene: cut short, asking for another pass");
  bumpRenderEpoch(node());
  asapUpdate();
}

bool InstanceRender::renderFrame()
{
  const auto t0 = std::chrono::steady_clock::now();
  _lastRenderAborted = false;
  irlog("renderFrame: start frame=" + std::to_string(outputContext().frame()));
  const Format& fmt = info_.format();
  const int W = fmt.width(), H = fmt.height();
  std::string prefix;                 // device fallback note, if any
  // The scene was built by prepareScene() during _validate().  Nothing below
  // here may go near Nuke's node graph - see the note on prepareScene().
  if (!_sceneOk) {
    _fb.allocate(W, H);
    if (aborted() || _cancel.load()) _lastRenderAborted = true;
    _status = "ERROR: " + _sceneErr;
    if (!aborted()) warning("InstanceRender: %s", _sceneErr.c_str());
    return false;
  }
  irlog("renderFrame: settings");
  ir::watchdogMark("renderFrame: settings");
  // progressive: keep accumulating into the same buffer while the content hash
  // holds still, and render only the next chunk of samples this pass
  const bool prog = _progressiveActive();
  Hash contentHash;
  appendContent(contentHash);
  const size_t nRgba = size_t(W) * size_t(H) * 4;
  if (!prog || contentHash != _accumHash || _accumRgba.size() != nRgba) {
    _accumRgba.clear(); _accDepth.clear(); _accNormal.clear(); _accInstance.clear(); _accAlbedo.clear(); _accExtra.clear();
    _accumSamples = 0;
    _accumHash = contentHash;
  }
  int chunk = _samples;
  const int offset = prog ? _accumSamples : 0;
  if (prog) {
    const int preview = std::max(1, _previewSamples);
    chunk = (offset == 0) ? preview : std::max(preview, offset);   // preview, then double
    chunk = std::min(chunk, _samples - offset);
    if (chunk <= 0) chunk = _samples;                              // already complete: re-render it whole
  }

  ir::RenderSettings st;
  st.width = W; st.height = H; st.samples = chunk; st.sampleOffset = offset; st.maxBounces = _maxBounces; st.seed = _seed;
  st.clampRadiance = float(_clamp); st.background = ir::Vec3(_bgColor[0], _bgColor[1], _bgColor[2]);
  st.backgroundVisible = _bgVisible ? 1 : 0;
  st.motionBlur = _scene.hasMotion ? 1 : 0;
  st.mipFilter = _mipFilter ? 1 : 0;
  st.motionSteps = _motionSteps;
  st.volumeSteps = _volumeSteps;
  st.volumeShadowSteps = _volumeShadowSteps;
  st.volumeAlbedo = float(_volumeAlbedo);
  st.volumeOctaves = _volumeOctaves;
  st.volumeDeepSegments = _volumeDeepSegments;
  st.occlusionSamples = _occlusionSamples;
  st.occlusionDistance = float(_occlusionDistance);
  st.deformationBlur = (_scene.vertices1.size() == _scene.vertices.size() && !_scene.vertices1.empty()) ? 1 : 0;
  st.aov = aovLayout();
  _aov = st.aov;
  // motion vectors: the keys span _motionFrames frames, so dividing by it gives
  // pixels per frame - ScanlineRender's "velocity".  Its "distance" is half the
  // travel across the open shutter, which is what VectorBlur blurs by default.
  //   per frame: the travel divided by how many frames the keys span (velocity)
  //   shutter:   half the travel across the open shutter (distance)
  st.shutterFrames = (_motionUnits == 1) ? 2.0f : float(_motionFrames);
  if (_mvOnly) st.fixedShutterTime = 0.0f;           // vectors without blur: every ray at the open
  {
    // IR_SHUTTER_TIME=<0..1> pins the shutter for debugging both devices at the
    // ends of the motion
    const std::string sv = ir::envString("IR_SHUTTER_TIME");
    if (!sv.empty()) st.fixedShutterTime = float(std::atof(sv.c_str()));
  }

  int device = _device;
#if IR_HAVE_OPTIX
  if (device == kDeviceAuto) device = ir::GpuRenderer::available() ? kDeviceGpu : kDeviceCpu;
  if (device == kDeviceGpu && !ir::GpuRenderer::available()) { device = kDeviceCpu; prefix = "GPU not available, rendered on the CPU. "; }
#else
  if (device != kDeviceCpu) { device = kDeviceCpu; prefix = "GPU not built into this plugin, rendered on the CPU. "; }
#endif
  _usedDevice = device;
  std::string berr, stats;
  bool ok = false;
  AbortWatcher watching(this, _cancel);
#if IR_HAVE_OPTIX
  if (device == kDeviceGpu) {
    irlog("renderFrame: gpu build");
    ir::watchdogMark("renderFrame: OptiX build");
    ok = _gpu.build(_scene, berr);
    irlog(std::string("renderFrame: gpu build ok=") + (ok ? "1" : "0") + " " + _gpu.stats());
    if (ok) { _gpu.render(_scene, st, _fb, &_cancel); stats = _gpu.stats(); }
    irlog("renderFrame: gpu render done");
  }
#endif
  if (device == kDeviceCpu) {
    irlog("renderFrame: cpu build");
    ir::watchdogMark("renderFrame: Embree build");
    ok = _cpu.build(_scene, berr);
    irlog(std::string("renderFrame: cpu build ok=") + (ok ? "1" : "0") + " " + _cpu.stats());
    if (ok) { _cpu.render(_scene, st, _fb, &_cancel); stats = _cpu.stats(); }
    irlog("renderFrame: cpu render done");
  }
  // The denoiser runs on the finished frame, so it does not care which back-end
  // produced it - and it wants the albedo, which is only rendered when asked
  // for, so it is switched on here for the duration.
  if (ok && _denoise) {
#if IR_HAVE_OPTIX
    std::string derr;
    if (_denoiser.run(W, H, _fb.rgba, _fb.albedo, _fb.normal, derr)) {
      stats += "; " + _denoiser.stats();
    }
    else {
      prefix += "Denoise: " + derr + ". ";
      irlog("denoise failed: " + derr);
    }
#else
    prefix += "Denoise needs the GPU build. ";
#endif
  }
  if (!ok) {
    _fb.allocate(W, H);
    _status = prefix + "ERROR: " + berr;
    warning("InstanceRender: %s", berr.c_str());
    return false;
  }
  if (prog) {
    if (_accumSamples <= 0 || _accumRgba.size() != nRgba) {
      // first pass: it also owns the AOVs, which must not be blended (an
      // averaged instance.id is meaningless) or re-jittered by later passes
      _accumRgba.assign(_fb.rgba.begin(), _fb.rgba.end());
      _accDepth = _fb.depth; _accNormal = _fb.normal; _accInstance = _fb.instanceId; _accAlbedo = _fb.albedo;
      _accExtra = _fb.extra;
      _accumSamples = chunk;
    }
    else {
      const float total = float(_accumSamples + chunk);
      const float wOld = float(_accumSamples) / total, wNew = float(chunk) / total;
      for (size_t i = 0; i < nRgba; ++i) _accumRgba[i] = _accumRgba[i] * wOld + _fb.rgba[i] * wNew;
      std::copy(_accumRgba.begin(), _accumRgba.end(), _fb.rgba.begin());
      if (_accDepth.size() == _fb.depth.size()) _fb.depth = _accDepth;
      if (_accNormal.size() == _fb.normal.size()) _fb.normal = _accNormal;
      if (_accInstance.size() == _fb.instanceId.size()) _fb.instanceId = _accInstance;
      if (_accAlbedo.size() == _fb.albedo.size()) _fb.albedo = _accAlbedo;
      if (_accExtra.size() == _fb.extra.size() && _fb.aovStride > 0) {
        // Which of these refine and which are held is decided by
        // ir::aovSlotIsAveraged, next to the layout itself.  It used to be a
        // hand-written range covering the five light layers, which was right
        // when they were the only averages here and quietly wrong the moment
        // shadow, occlusion and the per-light groups arrived - those are
        // averages too, and were being frozen at the first pass.
        const int stride = _fb.aovStride;
        for (size_t i = 0; i < _accExtra.size(); ++i) {
          const int k = int(i % size_t(stride));
          if (ir::aovSlotIsAveraged(st.aov, k)) _accExtra[i] = _accExtra[i] * wOld + _fb.extra[i] * wNew;
        }
        // The cryptomatte tables are neither averaged nor held: they are merged
        // by id, and it has to happen HERE, into _accExtra, before the copy
        // below - anything written into _fb.extra instead is thrown away by it.
        mergeCryptoTables(_accExtra, _fb.extra, st.aov, wOld, wNew);
        _fb.extra = _accExtra;
      }
      _accumSamples += chunk;
    }
  }

  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::ostringstream os;
  os << prefix << (device == kDeviceGpu ? "GPU" : "CPU") << " render " << W << "x" << H << " x ";
  if (prog) os << _accumSamples << "/" << _samples << " spp (progressive, this pass " << chunk << "), ";
  else os << _samples << " spp, ";
  os << _maxBounces << " bounces";
  if (_shutter > 0.0) {
    os << ", shutter " << _shutter << " x " << _motionSamples << " key(s) (2 stage builds)";
    if (_motionSteps > 0) os << ", " << _motionSteps << " shutter step(s)";
  }
  os << ": " << int(ms) << " ms (" << stats << ")";
  if (!_stageDiag.empty()) os << "\n" << _stageDiag;
  if (!_scene.warnings.empty()) os << "\nwarnings: " << _scene.warnings;
  _status = os.str();

  // ask for another pass: the counter is in the hash, so the viewer re-pulls
  if (prog && _accumSamples < _samples) {
    ++_progressCounter;
    asapUpdate();
  }
  ir::watchdogMark(nullptr);            // idle again: nothing to time out
  // Cut short counts as unfinished however far it got: the buffer has holes
  // in it and must not be handed out as this frame's answer.
  if (_cancel.load() || aborted()) {
    irlog("renderFrame: cut short");
    _lastRenderAborted = true;
    return false;
  }
  return true;
}

// The metadata a Cryptomatte node reads.
//
// Nuke sees these under an "exr/" prefix on everything it reads from a real
// file, so that is the shape they take here; without a prefix Nuke rewrites the
// keys under "nuke/" on the way through a Write and the names stop being found.
const DD::Image::MetaData::Bundle& InstanceRender::_fetchMetaData(const char* key)
{
  std::string manifest;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    manifest = _cryptoManifest;
  }
  _meta = Iop::_fetchMetaData(key);
  if (_outCrypto && !manifest.empty()) {
    const char* const type = cryptoTypeName(_cryptoId);
    const std::string k = "exr/cryptomatte/" + ir::cryptoTypeKey(type) + "/";
    _meta.setData(k + "name", type);
    _meta.setData(k + "hash", "MurmurHash3_32");
    _meta.setData(k + "conversion", "uint32_to_float32");
    _meta.setData(k + "manifest", manifest);
  }
  return _meta;
}

void InstanceRender::_open()
{
  irlog("_open tid=" + std::to_string(currentThreadId()));
  std::lock_guard<std::mutex> lock(_mutex);
  const Hash h = hash();
  if (_rendered && h == _renderedHash && _fb.width == info_.format().width() && _fb.height == info_.format().height()) return;
  const bool finished = renderFrame();
  // A CANCELLED render must not be remembered as a rendered one: the hash would
  // match next time round and Nuke would keep handing out the blank buffer it
  // was interrupted in the middle of filling, so the viewer would stay empty
  // until something else in the scene changed.
  //
  // A FAILED one must be remembered, though, and the difference matters:
  // engine() is called once per ROW, so a failure that is not cached re-runs
  // the whole render - scene load and all - for every row in the frame.  On a
  // heavy scene that is its own freeze, and on exactly the symptom this release
  // is about.  Errors are also what the report is for, so it is published
  // either way.
  _sceneInfo = _scene.info;
  if (finished || !_lastRenderAborted) {
    _renderedHash = h;
    _rendered = true;
  }
  publishReport(node(), _sceneInfo + (_sceneInfo.empty() ? "" : "\n") + _status);
}

// Nothing upstream is deep, so nothing is asked of it.
void InstanceRender::getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet&,
                                     int count, std::vector<DD::Image::RequestData>& reqData)
{
  reqData.clear();
}

// One deep sample per pixel, out of the render that already happened.
//
// A hard surface has no thickness, so front and back are the same distance, and
// the colour is the beauty's - which is already premultiplied by its own alpha,
// because that is what a render IS. Front-to-back compositing of one sample is
// then just that sample, so flattening this reproduces the 2D image exactly;
// deep_test measures that against Nuke's own DeepToImage rather than trusting it.
//
// Empty pixels get a HOLE rather than a transparent sample: a sample that covers
// nothing still costs memory and still has to be composited.
bool InstanceRender::doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels,
                                  DD::Image::DeepOutputPlane& plane)
{
  {
    // The same gate engine() uses.  Deep tiles arrive on render threads too, and
    // both faces of this node must render once between them, not once each.
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_rendered) {
      const bool finished = renderFrame();
      _sceneInfo = _scene.info;
      if (finished || !_lastRenderAborted) {
        _renderedHash = hash();
        _rendered = true;
      }
      publishReport(node(), _sceneInfo + (_sceneInfo.empty() ? "" : "\n") + _status);
    }
  }

  plane = DD::Image::DeepOutputPlane(channels, box);
  const int W = _fb.width, H = _fb.height;

  // bottom left first, in raster order: DeepPlane.h is explicit about it, and
  // getting it wrong scrambles the picture rather than failing
  for (int y = box.y(); y < box.t(); ++y) {
    for (int x = box.x(); x < box.r(); ++x) {
      if (x < 0 || x >= W || y < 0 || y >= H) { plane.addHole(); continue; }
      const size_t pi = size_t(y) * size_t(W) + size_t(x);
      const float a = _fb.rgba[pi * 4 + 3];
      if (a <= 0.0f) { plane.addHole(); continue; }

      DD::Image::DeepOutPixel out;
      const float z = _fb.depth.empty() ? 0.0f : _fb.depth[pi];

      const ir::AovLayout& a2 = _aov;
      const float* table = (a2.deep >= 0 && _fb.aovStride > 0 && !_fb.extra.empty())
                         ? &_fb.extra[pi * size_t(_fb.aovStride) + size_t(a2.deep)]
                         : nullptr;
      if (!table) {
        // one sample: the whole pixel as a single surface
        foreach (ch, channels) {
          switch (ch) {
            case DD::Image::Chan_DeepFront:
            case DD::Image::Chan_DeepBack:  out.push_back(z); break;
            case DD::Image::Chan_Red:       out.push_back(_fb.rgba[pi * 4]); break;
            case DD::Image::Chan_Green:     out.push_back(_fb.rgba[pi * 4 + 1]); break;
            case DD::Image::Chan_Blue:      out.push_back(_fb.rgba[pi * 4 + 2]); break;
            case DD::Image::Chan_Alpha:     out.push_back(a); break;
            default:                        out.push_back(0.0f); break;
          }
        }
        plane.addPixel(out);
        continue;
      }

      // One sample per surface, front first - and each one divided by what is
      // still transmitting in front of it.
      //
      // THE ARITHMETIC THAT MATTERS.  The renderer's numbers are ADDITIVE: each
      // surface carries the share of the pixel it covered, and they sum to the
      // beauty.  Deep is composited with OVER, which multiplies each sample by
      // what got past the ones in front.  So a sample whose additive share is c
      // has to be stored as c / T, where T is one minus the coverage already
      // accounted for - otherwise every surface behind the first is attenuated
      // twice and the flatten comes out dark.  deep_test measures exactly that.
      float transmit = 1.0f;
      int written = 0;
      for (int sIdx = 0; sIdx < a2.deepSlots; ++sIdx) {
        const float* e = table + sIdx * ir::kDeepSlotFloats;
        const float cov = e[1];
        if (cov <= 0.0f) continue;                 // an empty slot
        if (transmit <= 1e-6f) break;              // nothing would get through anyway
        const float inv = 1.0f / transmit;
        // straight into the pixel: a DeepOutPixel is a stream of samples, one
        // channel value after another, not a container to index
        foreach (ch, channels) {
          switch (ch) {
            // front and back are the nearest and farthest this surface came
            // during the shutter - the same number twice when it did not move
            case DD::Image::Chan_DeepFront: out.push_back(e[2]); break;
            case DD::Image::Chan_DeepBack:  out.push_back(e[3]); break;
            case DD::Image::Chan_Red:       out.push_back(e[4] * inv); break;
            case DD::Image::Chan_Green:     out.push_back(e[5] * inv); break;
            case DD::Image::Chan_Blue:      out.push_back(e[6] * inv); break;
            case DD::Image::Chan_Alpha:     out.push_back(cov * inv); break;
            default:                        out.push_back(0.0f); break;
          }
        }
        transmit -= cov;
        ++written;
      }
      if (written == 0) { plane.addHole(); continue; }
      plane.addPixel(out);
    }
  }
  return !aborted();
}

void InstanceRender::engine(int y, int x, int r, ChannelMask channels, Row& row)
{
  irlog("engine y=" + std::to_string(y));
  if (!_rendered) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_rendered) {
      const bool finished = renderFrame();
      _sceneInfo = _scene.info;
      // cache it unless it was CANCELLED - see the note in _open().  A failed
      // render that is not cached is re-run once per row.
      if (finished || !_lastRenderAborted) {
        _renderedHash = hash();
        _rendered = true;
      }
      publishReport(node(), _sceneInfo + (_sceneInfo.empty() ? "" : "\n") + _status);
    }
  }
  const int W = _fb.width, H = _fb.height;
  // one reference to the snapshot for this whole row; _validate may replace it
  // underneath us at any moment, and this is what makes that harmless
  std::shared_ptr<const ChannelSourceMap> sources;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    sources = _chanSource;
  }
  foreach (z, channels) {
    float* out = row.writable(z);
    if (!sources) { for (int X = x; X < r; ++X) out[X] = 0.0f; continue; }
    // one lookup per CHANNEL, not per pixel
    ChannelSourceMap::const_iterator it = sources->find(int(z));
    if (it == sources->end()) {
      for (int X = x; X < r; ++X) out[X] = 0.0f;
      continue;
    }
    const ChannelSource src = it->second;
    const int stride = (src.buffer == ChannelSource::kExtra) ? _fb.aovStride : 0;
    if (src.buffer == ChannelSource::kExtra && stride <= 0) {
      for (int X = x; X < r; ++X) out[X] = 0.0f;
      continue;
    }
    for (int X = x; X < r; ++X) {
      float v = 0.0f;
      if (X >= 0 && X < W && y >= 0 && y < H) {
        const size_t pi = size_t(y) * size_t(W) + size_t(X);
        switch (src.buffer) {
          case ChannelSource::kRgba:     v = _fb.rgba[pi * 4 + src.offset]; break;
          case ChannelSource::kDepth:    v = _fb.depth[pi]; break;
          case ChannelSource::kNormal:   v = _fb.normal[pi * 3 + src.offset]; break;
          case ChannelSource::kInstance: v = _fb.instanceId[pi]; break;
          case ChannelSource::kAlbedo:   v = _fb.albedo[pi * 3 + src.offset]; break;
          case ChannelSource::kExtra:    v = _fb.extra[pi * size_t(stride) + src.offset]; break;
        }
      }
      out[X] = v;
    }
  }
}

static Op* build(Node* node) { return new InstanceRender(node); }
const Op::Description InstanceRender::description(kClass, "3D/InstanceRender", build);
