// InstanceRender - a Hydra render delegate, step one.
//
// Nuke 17.1 (Hydra 2.0) picks its renderers straight out of USD's plugin
// registry: FnUsdShim's RenderInterfaceImpl.cpp calls
// HdRendererPluginRegistry::GetPluginDescs(), then GetOrCreateRendererPlugin(),
// IsSupported() and CreateDelegate() on every plugin it finds - so a delegate
// declared in a plugInfo.json on PXR_PLUGINPATH_NAME appears in the Viewer's
// renderer menu and in GeoRender, with no Foundry involvement.
//
// This is the smallest delegate that answers the question the research doc left
// open: will a delegate BUILT AGAINST NUKE'S OWN pxr load and run inside Nuke?
// It paints one colour into the colour AOV and nothing else.  Once that is
// proven, the geometry goes in - HdMesh and HdInstancer feeding the ir::Scene
// that both existing back-ends already render.
//
// Strict ASCII.
#include <pxr/pxr.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/gf/half.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderThread.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/instancer.h>
#include <cmath>
#include <map>

#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hf/pluginDesc.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

#include "HdIrMesh.h"
#include "HdIrPoints.h"
#include "HdIrVolume.h"
#include <pxr/imaging/hd/dataSourceLegacyPrim.h>

#include "ir/VolumeRead.h"
#include "ir/Blackbody.h"
#include "HdIrLight.h"
#include "HdIrScene.h"
#include "ir/Scene.h"
#include "ir/Crypto.h"
#include "ir/EmbreeBackend.h"
#include "ir/Dome.h"
#include "ir/Texture.h"
#include "ir/NukeOpImage.h"
#if IR_HAVE_OPTIX
#include "ir/OptixBackend.h"
#include "ir/Denoise.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Tracing, so "did Nuke load this, and how far did it get" is answerable from a
// GUI session with no console: set IR_HYDRA_LOG to a file.
namespace {
  void hlog(const std::string& m)
  {
    static std::string path = [] {
      char buf[1024];
      const char* e = std::getenv("IR_HYDRA_LOG");
      (void)buf;
      return e ? std::string(e) : std::string();
    }();
    if (path.empty()) return;
    std::ofstream f(path.c_str(), std::ios::app);
    f << m << std::endl;
  }
  // embree4.dll sits next to the InstanceRender plugin, one folder up from this
  // delegate, and neither is on the DLL search path - so it is delay-loaded and
  // fetched by hand, the same way the Nuke plugin does it.
  void loadSideBySide()
  {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&loadSideBySide), &self))
      return;
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash + 1);
    const wchar_t* names[] = { L"tbb12.dll", L"embree4.dll" };
    for (int i = 0; i < 2; ++i) {
      if (GetModuleHandleW(names[i])) continue;
      if (!LoadLibraryW((dir + names[i]).c_str()))
        LoadLibraryW((dir + L"../" + names[i]).c_str());
    }
    // usd_hio reads the texture images and is NOT one of the USD libraries Nuke
    // loads at startup; FnUSD/lib is not on the DLL search path either, so it is
    // delay-loaded and fetched from next to the running Nuke.
    if (!GetModuleHandleW(L"usd_hio.dll")) {
      wchar_t exe[MAX_PATH];
      const DWORD ne = GetModuleFileNameW(nullptr, exe, MAX_PATH);
      if (ne > 0 && ne < MAX_PATH) {
        std::wstring exeDir(exe);
        const size_t cut = exeDir.find_last_of(L"\\/");
        if (cut != std::wstring::npos) {
          exeDir.resize(cut + 1);
          LoadLibraryW((exeDir + L"FnUSD/lib/usd_hio.dll").c_str());
        }
      }
    }
#endif
  }

  struct LoadAnnounce {
    LoadAnnounce()
    {
      loadSideBySide();
      hlog("library loaded");
    }
  } g_loadAnnounce;
}

// ---------------------------------------------------------------------------
// A render buffer that just holds floats.  Hydra hands one of these to the
// render pass per AOV; the pass writes into it and Nuke reads it back.
// AOVs this renderer adds to Hydra's standard set.  GeoRender lays an AOV's
// components into an output plane's channels in order, so coverage has to be a
// one-component AOV of its own to land in rgba.alpha - mapping the four-channel
// colour AOV onto the "rgba" plane writes its red into alpha instead.
TF_DEFINE_PRIVATE_TOKENS(
  _irAovTokens,
  (alpha)
  (albedo)
  (position)
  (motion)
  (uv)
  (materialId)
  (objectId)
  (directDiffuse)
  (indirectDiffuse)
  (directSpecular)
  (indirectSpecular)
  (emission)
);

// The AOVs that come out of the renderer's packed "extra" buffer, and how wide
// each one is.  Which of them the kernel computes at all is decided per render
// from the bindings, so a render that wants none pays for none.
struct IrExtraAov {
  const TfToken& name;
  int components;
  int ir::AovLayout::* slot;
};

const IrExtraAov* irExtraAovs(size_t& count)
{
  static const IrExtraAov table[] = {
    { _irAovTokens->position,         3, &ir::AovLayout::position },
    { _irAovTokens->motion,           2, &ir::AovLayout::motion },
    { _irAovTokens->uv,               2, &ir::AovLayout::uv },
    { _irAovTokens->materialId,       1, &ir::AovLayout::materialId },
    { _irAovTokens->objectId,         1, &ir::AovLayout::objectId },
    { _irAovTokens->directDiffuse,    3, &ir::AovLayout::directDiffuse },
    { _irAovTokens->indirectDiffuse,  3, &ir::AovLayout::indirectDiffuse },
    { _irAovTokens->directSpecular,   3, &ir::AovLayout::directSpecular },
    { _irAovTokens->indirectSpecular, 3, &ir::AovLayout::indirectSpecular },
    { _irAovTokens->emission,         3, &ir::AovLayout::emission },
  };
  count = sizeof(table) / sizeof(table[0]);
  return table;
}

class HdIrRenderBuffer final : public HdRenderBuffer
{
public:
  HdIrRenderBuffer(SdfPath const& id) : HdRenderBuffer(id) {}

  bool Allocate(GfVec3i const& dimensions, HdFormat format, bool multiSampled) override
  {
    _width = dimensions[0];
    _height = dimensions[1];
    _format = format;
    _multiSampled = multiSampled;
    _buffer.assign(size_t(_width) * size_t(_height) * size_t(HdDataSizeOfFormat(format)), 0);
    hlog("buffer " + GetId().GetString() + " allocated " + std::to_string(_width) + "x" +
         std::to_string(_height) + " format " + std::to_string(int(format)));
    return true;
  }

  unsigned int GetWidth() const override { return _width; }
  unsigned int GetHeight() const override { return _height; }
  unsigned int GetDepth() const override { return 1; }
  HdFormat GetFormat() const override { return _format; }
  bool IsMultiSampled() const override { return _multiSampled; }

  void* Map() override { ++_mappers; return _buffer.empty() ? nullptr : _buffer.data(); }
  void Unmap() override { if (_mappers > 0) --_mappers; }
  bool IsMapped() const override { return _mappers > 0; }
  bool IsConverged() const override { return _converged; }
  void SetConverged(bool b) { _converged = b; }
  void Resolve() override {}

  // what the render pass paints into
  std::vector<uint8_t>& raw() { return _buffer; }

private:
  void _Deallocate() override
  {
    _buffer.clear();
    _width = _height = 0;
    _mappers = 0;
    _converged = false;
  }

  unsigned int _width = 0, _height = 0;
  HdFormat _format = HdFormatInvalid;
  bool _multiSampled = false;
  std::atomic<int> _mappers{0};
  bool _converged = false;
  std::vector<uint8_t> _buffer;
};

// ---------------------------------------------------------------------------
// The render pass.  Step one paints a flat colour so that "did it load and run"
// is visible in the viewer without any geometry handling at all.
class HdIrRenderPass final : public HdRenderPass
{
public:
  HdIrRenderPass(HdRenderIndex* index, HdRprimCollection const& collection, HdIrScene* scene)
    : HdRenderPass(index, collection), _scene(scene) {}

protected:
  void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                TfTokenVector const& renderTags) override
  {
    const HdRenderPassAovBindingVector& aovBindings = renderPassState->GetAovBindings();
    if (aovBindings.empty()) return;

    // the buffer this pass paints into, and how big the image is
    HdIrRenderBuffer* colorBuffer = nullptr;
    for (size_t i = 0; i < aovBindings.size() && !colorBuffer; ++i)
      if (aovBindings[i].aovName == HdAovTokens->color)
        colorBuffer = dynamic_cast<HdIrRenderBuffer*>(aovBindings[i].renderBuffer);
    if (!colorBuffer) colorBuffer = dynamic_cast<HdIrRenderBuffer*>(aovBindings[0].renderBuffer);
    if (!colorBuffer) return;
    const int w = int(colorBuffer->GetWidth()), h = int(colorBuffer->GetHeight());
    if (w <= 0 || h <= 0) return;

    // ---- Hydra's meshes -> the same ir::Scene both back-ends already render ----
    ir::Scene scene;
    uint64_t version = 0;
    const std::vector<HdIrMeshData> meshes = _scene ? _scene->meshes(&version) : std::vector<HdIrMeshData>();
    size_t instanceCount = 0;
    for (size_t m = 0; m < meshes.size(); ++m) {
      const HdIrMeshData& mesh = meshes[m];
      ir::ProtoRange pr;
      pr.firstVertex = int(scene.vertices.size());
      pr.numVertices = int(mesh.points.size());
      pr.firstTri = int(scene.indices.size() / 3);
      pr.numTris = int(mesh.indices.size() / 3);
      pr.hasNormals = mesh.normals.size() == mesh.points.size() ? 1 : 0;
      pr.hasUVs = (mesh.uvs.size() == mesh.points.size() * 2) ? 1 : 0;
      pr.hasColors = 1;
      // named after the prim, so a cryptomatte through GeoRender picks the same
      // objects by the same names the node would use
      pr.cryptoId = ir::cryptoIdOf(mesh.primId.GetString());
      const uint32_t base = uint32_t(scene.vertices.size());
      scene.vertices.insert(scene.vertices.end(), mesh.points.begin(), mesh.points.end());
      for (size_t i = 0; i < mesh.points.size(); ++i) {
        scene.normals.push_back(pr.hasNormals ? mesh.normals[i] : ir::Vec3(0.0f));
        scene.colors.push_back(mesh.color);
        if (pr.hasUVs) { scene.uvs.push_back(mesh.uvs[i * 2]); scene.uvs.push_back(mesh.uvs[i * 2 + 1]); }
        else { scene.uvs.push_back(0.0f); scene.uvs.push_back(0.0f); }
      }
      for (size_t i = 0; i < mesh.indices.size(); ++i) scene.indices.push_back(base + mesh.indices[i]);
      for (int t = 0; t < pr.numTris; ++t) scene.triMaterial.push_back(0);
      // the material this mesh is bound to, or a displayColor one, with any
      // images it carries appended to the scene's texel array
      const int materialId = int(scene.materials.size());
      HdIrMaterialData md = _scene ? _scene->material(mesh.materialId) : HdIrMaterialData();
      for (size_t t = 0; t < md.textures.size(); ++t) {
        const HdIrTextureData& tex = md.textures[t];
        if (!tex.image.valid()) continue;
        std::vector<ir::ImageData> levels;
        const int mips = ir::buildMipChain(tex.image, levels);
        ir::TextureDesc td;
        td.width = tex.image.width;
        td.height = tex.image.height;
        td.firstTexel = int(scene.texels.size() / 4);
        td.mipCount = (mips > 0) ? mips : 1;
        if (td.mipCount > 1) {
          for (int L = 0; L < td.mipCount && L < ir::kMaxMipLevels; ++L) {
            td.mipOffset[L] = int(scene.texels.size() / 4);
            td.mipW[L] = levels[L].width; td.mipH[L] = levels[L].height;
            scene.texels.insert(scene.texels.end(), levels[L].rgba.begin(), levels[L].rgba.end());
          }
        }
        else {
          td.mipOffset[0] = td.firstTexel; td.mipW[0] = tex.image.width; td.mipH[0] = tex.image.height;
          scene.texels.insert(scene.texels.end(), tex.image.rgba.begin(), tex.image.rgba.end());
        }
        const int texId = int(scene.textures.size());
        scene.textures.push_back(td);
        scene.textureNames.push_back(mesh.materialId.GetString());
        ir::TexRef* ref = nullptr;
        switch (tex.slot) {
          case HdIrTextureData::kDiffuse:   ref = &md.material.diffuseTex; break;
          case HdIrTextureData::kEmissive:  ref = &md.material.emissiveTex; break;
          case HdIrTextureData::kRoughness: ref = &md.material.roughnessTex; break;
          case HdIrTextureData::kMetallic:  ref = &md.material.metallicTex; break;
          case HdIrTextureData::kOpacity:   ref = &md.material.opacityTex; break;
          case HdIrTextureData::kNormal:    ref = &md.material.normalTex; break;
          default: break;
        }
        if (ref) {
          ref->index = texId;
          ref->channel = tex.channel;
          ref->scale = tex.scale;
          ref->bias = tex.bias;
        }
      }
      scene.materials.push_back(md.material);
      scene.materialNames.push_back(mesh.materialId.GetString());
      hlog("material for " + (mesh.materialId.IsEmpty() ? std::string("<none>") : mesh.materialId.GetString())
           + ": diffuse " + std::to_string(md.material.diffuse.x)
           + " displayColor " + std::to_string(mesh.color.x)
           + " useDisplayColor " + std::to_string(md.material.useDisplayColor)
           + " roughness " + std::to_string(md.material.roughness)
           + " metallic " + std::to_string(md.material.metallic)
           + " specularWorkflow " + std::to_string(md.material.useSpecularWorkflow));
      for (int t = 0; t < pr.numTris; ++t) scene.triMaterial[size_t(pr.firstTri) + size_t(t)] = materialId;
      const int protoId = int(scene.protos.size());
      scene.protos.push_back(pr);
      scene.protoNames.push_back(mesh.primId.IsEmpty() ? std::string("hydra mesh")
                                                      : mesh.primId.GetString());
      // ONE prototype, a transform per copy - the reason this renderer exists
      for (size_t i = 0; i < mesh.instances.size(); ++i) {
        ir::Instance in;
        in.xf = mesh.instances[i];
        in.xf1 = in.xf;
        in.protoId = protoId;
        in.instanceId = int(instanceCount++);
        in.materialOverride = -1;
        if (i < mesh.instanceColors.size()) { in.hasColor = 1; in.color = mesh.instanceColors[i]; }
        else { in.hasColor = 0; in.color = ir::Vec3(1.0f); }
        in.opacity = 1.0f;
        in.firstKey = 0;
        scene.instances.push_back(in);
      }
    }
    if (scene.materials.empty()) {
      ir::Material mat;
      mat.useDisplayColor = 1;
      scene.materials.push_back(mat);
      scene.materialNames.push_back("<hydra default>");
    }

    // ---- the camera Hydra is looking through ----------------------------------
    const GfMatrix4d view = renderPassState->GetWorldToViewMatrix();
    const GfMatrix4d proj = renderPassState->GetProjectionMatrix();
    scene.camera.camToWorld = irXformOf(view.GetInverse());
    scene.camera.tanHalfFovX = (proj[0][0] != 0.0) ? float(1.0 / proj[0][0]) : 0.5f;
    scene.camera.tanHalfFovY = (proj[1][1] != 0.0) ? float(1.0 / proj[1][1]) : 0.5f;

    // Nuke hands Hydra a camera whose filmback is SQUARE - the vertical aperture
    // is a copy of the horizontal - and leaves the window policy to widen it to
    // the image.  That is not how anything else in Nuke frames a shot:
    // ScanlineRender2 and the InstanceRender node both take the horizontal
    // aperture for the horizontal field of view and derive the vertical from the
    // image, which puts the same camera 1.32x wider through GeoRender than
    // through ScanlineRender2 on a 2048x1556 format.  So when the camera says
    // its filmback is square, its aperture is taken as the horizontal one and
    // the vertical follows the image - matching every other renderer in Nuke.
    if (const HdCamera* hdCam = renderPassState->GetCamera()) {
      const float focal = float(hdCam->GetFocalLength());
      const float hAperture = float(hdCam->GetHorizontalAperture());
      const float vAperture = float(hdCam->GetVerticalAperture());
      const bool squareFilmback = (hAperture > 0.0f) && (std::fabs(hAperture - vAperture) < 1e-6f);
      if (squareFilmback && focal > 1e-6f && h > 0
          && hdCam->GetProjection() == HdCamera::Perspective) {
        float pixelAspect = 1.0f;
        const CameraUtilFraming& framing = renderPassState->GetFraming();
        if (framing.IsValid() && framing.pixelAspectRatio > 1e-6f)
          pixelAspect = framing.pixelAspectRatio;
        scene.camera.tanHalfFovX = 0.5f * hAperture / focal;
        scene.camera.tanHalfFovY = scene.camera.tanHalfFovX
                                 * (float(h) / (float(w) * pixelAspect));
      }
    }
    scene.camera.width = w;
    scene.camera.height = h;
    scene.hasCamera = true;
    if (const HdCamera* hdCam = renderPassState->GetCamera()) {
      const CameraUtilFraming& framing = renderPassState->GetFraming();
      hlog("shutter: open " + std::to_string(hdCam->GetShutterOpen())
           + " close " + std::to_string(hdCam->GetShutterClose()));
      hlog("hdCamera: focal " + std::to_string(hdCam->GetFocalLength())
           + " hAperture " + std::to_string(hdCam->GetHorizontalAperture())
           + " vAperture " + std::to_string(hdCam->GetVerticalAperture())
           + " hOffset " + std::to_string(hdCam->GetHorizontalApertureOffset())
           + " vOffset " + std::to_string(hdCam->GetVerticalApertureOffset())
           + " projection " + std::to_string(int(hdCam->GetProjection()))
           + " windowPolicy " + std::to_string(int(renderPassState->GetWindowPolicy()))
           + " framingValid " + std::to_string(int(framing.IsValid()))
           + " pixelAspect " + std::to_string(framing.pixelAspectRatio)
           + " display " + std::to_string(framing.displayWindow.GetSize()[0]) + "x"
           + std::to_string(framing.displayWindow.GetSize()[1])
           + " data " + std::to_string(framing.dataWindow.GetWidth()) + "x"
           + std::to_string(framing.dataWindow.GetHeight()));
    }
    hlog("camera: proj[0][0] " + std::to_string(proj[0][0]) + " proj[1][1] " + std::to_string(proj[1][1])
         + " -> tanHalfFovX " + std::to_string(scene.camera.tanHalfFovX)
         + " tanHalfFovY " + std::to_string(scene.camera.tanHalfFovY)
         + " for " + std::to_string(w) + "x" + std::to_string(h));

    // Same budget the node's loader allows one grid, so a sequence rolls
    // through the shared cache here at the same rate it does there.
    const int kVolumeMemoryMB = 512;
    // ---- volumes ----------------------------------------------------------------
    // The grids arrive as field bprims naming a .vdb and a grid, which is a
    // different route from everything above, but they land in the same
    // ir::VolumeGrid the node's loader fills - so the march, the blackbody, the
    // shadows and the deep output are all the ones already tested.
    const std::vector<HdIrVolumeData> volData =
        _scene ? _scene->volumes() : std::vector<HdIrVolumeData>();
    hlog("scene build: " + std::to_string(volData.size()) + " volume(s)");
    for (size_t vi = 0; vi < volData.size(); ++vi) {
      const HdIrVolumeData& v = volData[vi];
      hlog("volume " + v.primId.GetString() + " density='" + v.density.filePath + "' grid='"
           + v.density.fieldName + "' frame=" + std::to_string(v.frame));
      if (v.density.filePath.empty() || v.density.fieldName.empty()) continue;
      ir::VolumeGrid g;
      g.densityScale = v.densityScale;
      const std::string dpath = ir::resolveVdbFrame(v.density.filePath, v.frame);
      if (!ir::readVdbGrid(scene, dpath, v.density.fieldName, kVolumeMemoryMB, g.density[0])) {
        hlog("  could not read " + dpath);
        continue;
      }
      hlog("  read ok, voxels now " + std::to_string(scene.voxels.size()));
      for (int si = 0; si < ir::kVolumeEmissive; ++si) {
        g.emissionScale[si] = v.emissionScale[si];
        g.emissionColor[si] = v.emissionColor[si];
        g.emissionMode[si]  = v.emissionMode[si];
        g.emitKmin[si]      = v.emitKmin[si];
        g.emitKmax[si]      = v.emitKmax[si];
        if (v.emissionScale[si] <= 0.0f) continue;
        if (v.emissive[si].filePath.empty() || v.emissive[si].fieldName.empty()) continue;
        const std::string epath = ir::resolveVdbFrame(v.emissive[si].filePath, v.frame);
        if (!ir::readVdbGrid(scene, epath, v.emissive[si].fieldName, kVolumeMemoryMB,
                             g.emissive[si][0]))
          g.emissionScale[si] = 0.0f;
      }
      g.worldToLocal = v.worldToLocal;
      g.cryptoId = ir::cryptoIdOf(v.primId.GetString());
      scene.volumeNames.push_back(v.primId.GetString());
      scene.volumes.push_back(g);
    }
    // The temperature -> colour table.  Without this every blackbody volume
    // through a Hydra host renders the wrong colour and nothing says why:
    // an empty table makes the lookup return white.
    ir::buildBlackbodyLut(scene, ir::kBlackbodyMinK, ir::kBlackbodyMaxK, true);

    // the scene's own lights; a headlight only when it has none, which is the
    // same fallback the Nuke node uses.  A dome light also brings an image, and
    // the image can only be loaded here, where the scene's textures live.
    const std::vector<HdIrLightData> sceneLightData =
        _scene ? _scene->lights() : std::vector<HdIrLightData>();
    int domeTexture = -1;
    for (size_t i = 0; i < sceneLightData.size(); ++i) {
      ir::Light L = sceneLightData[i].light;
      if (L.type == ir::kLightDome && !sceneLightData[i].domeTexture.empty()) {
        L.texture = _loadTexture(sceneLightData[i].domeTexture, scene);
        if (L.texture >= 0 && domeTexture < 0) domeTexture = L.texture;
      }
      scene.lights.push_back(L);
    }
    const size_t sceneLights = scene.lights.size();
    if (scene.lights.empty()) {
      ir::Light L;
      L.type = ir::kLightDistant;
      L.direction = ir::normalize(scene.camera.camToWorld.vector(ir::Vec3(0.0f, 0.0f, -1.0f)));
      L.color = ir::Vec3(1.0f);
      L.angle = 5.0f;
      scene.lights.push_back(L);
    }

    // a small bright sun in an HDRI is pure noise without this
    ir::buildDomeDistribution(scene, domeTexture);

    ir::RenderSettings st;
    st.width = w; st.height = h;
    st.samples = 4;
    st.maxBounces = 1;
    if (HdRenderDelegate* rd = GetRenderIndex() ? GetRenderIndex()->GetRenderDelegate() : nullptr) {
      st.samples = rd->GetRenderSetting<int>(TfToken("samples"), st.samples);
      st.maxBounces = rd->GetRenderSetting<int>(TfToken("maxBounces"), st.maxBounces);
    }

    // The packed AOVs cost memory and kernel time per pixel, so only the ones
    // something asked for are laid out at all.  The five lighting layers are
    // contiguous by construction and the kernel relies on that.
    {
      size_t n = 0;
      const IrExtraAov* extras = irExtraAovs(n);
      int offset = 0;
      for (size_t e = 0; e < n; ++e) {
        for (size_t i = 0; i < aovBindings.size(); ++i) {
          if (aovBindings[i].aovName != extras[e].name) continue;
          st.aov.*(extras[e].slot) = offset;
          offset += extras[e].components;
          break;
        }
      }
      st.aov.stride = offset;
    }
    st.seed = 0;
    st.backgroundVisible = 0;

    // what Hydra holds, which is not the same question as what this delegate was
    // asked to create: an empty index means the geometry never left Nuke
    if (std::getenv("IR_HYDRA_LOG") && GetRenderIndex())
      hlog("index holds " + std::to_string(GetRenderIndex()->GetRprimIds().size()) + " rprim(s)");

    hlog("render pass: " + std::to_string(meshes.size()) + " mesh(es), " +
         std::to_string(scene.instances.size()) + " instance(s), " +
         std::to_string(scene.indices.size() / 3) + " triangle(s), " +
         std::to_string(sceneLights) + " light(s), " +
         std::to_string(scene.materials.size()) + " material(s), " +
         std::to_string(scene.textures.size()) + " texture(s), " +
         std::to_string(w) + "x" + std::to_string(h));

    // Which back-end: the "Use GPU" render setting, honoured only if this
    // machine actually has a device.  Both run the same kernel, so the choice
    // changes the speed and nothing else.
    bool useGpu = false;
#if IR_HAVE_OPTIX
    if (HdRenderDelegate* rd = GetRenderIndex() ? GetRenderIndex()->GetRenderDelegate() : nullptr)
      useGpu = rd->GetRenderSetting<bool>(TfToken("useGpu"), true);
    if (useGpu && !ir::GpuRenderer::available()) {
      hlog("no GPU available, rendering on the CPU");
      useGpu = false;
    }
#endif

    ir::FrameBuffers fb;
    // A volume needs no instances - a .vdb on its own is a whole scene - so
    // gating the render on geometry alone rendered an empty frame for exactly
    // the case volumes were added for.
    if (!scene.instances.empty() || !scene.volumes.empty()) {
      std::string err;
#if IR_HAVE_OPTIX
      if (useGpu) {
        if (_gpu.build(scene, err)) {
          _gpu.render(scene, st, fb, nullptr);
          hlog("rendered on the GPU: " + _gpu.stats());
        }
        else {
          hlog("optix build failed, falling back to the CPU: " + err);
          useGpu = false;
        }
      }
#endif
      if (!useGpu) {
        if (_cpu.build(scene, err)) _cpu.render(scene, st, fb, nullptr);
        else hlog("embree build failed: " + err);
      }
    }

#if IR_HAVE_OPTIX
    // The denoiser is a post-process on the finished frame, so it does not care
    // which back-end produced it, and the albedo and normal passes guide it.
    {
      bool denoise = false;
      if (HdRenderDelegate* rd = GetRenderIndex() ? GetRenderIndex()->GetRenderDelegate() : nullptr)
        denoise = rd->GetRenderSetting<bool>(TfToken("denoise"), false);
      if (denoise && !fb.rgba.empty()) {
        std::string derr;
        if (_denoiser.run(w, h, fb.rgba, fb.albedo, fb.normal, derr)) hlog(_denoiser.stats());
        else hlog("denoise failed: " + derr);
      }
    }
#endif

    // ---- hand the pixels back to Hydra ----------------------------------------
    // Every bound AOV is filled with what the renderer produced for it, not just
    // the colour: depth, the normal and the two ids all come out of the same
    // frame buffers the Nuke node writes to its channels.
    const bool haveImage = (fb.width == w && fb.height == h && !fb.rgba.empty());
    for (size_t i = 0; i < aovBindings.size(); ++i) {
      HdIrRenderBuffer* buffer = dynamic_cast<HdIrRenderBuffer*>(aovBindings[i].renderBuffer);
      if (!buffer) continue;
      const int bw = int(buffer->GetWidth()), bh = int(buffer->GetHeight());
      if (bw <= 0 || bh <= 0) continue;
      const TfToken& aov = aovBindings[i].aovName;
      hlog("binding " + std::to_string(i) + " aov '" + aov.GetString() + "' format " +
           std::to_string(int(buffer->GetFormat())));
      const bool isColor = (aov == HdAovTokens->color) || (buffer == colorBuffer);
      const bool isDepth = (aov == HdAovTokens->depth) || (aov == HdAovTokens->cameraDepth);
      const bool isNormal = (aov == HdAovTokens->normal) || (aov == HdAovTokens->Neye);
      const bool isAlpha = (aov == _irAovTokens->alpha);
      const bool isAlbedo = (aov == _irAovTokens->albedo);
      // one of the packed AOVs?  its offset in the layout says where
      int extraAt = -1, extraComponents = 0;
      {
        size_t n = 0;
        const IrExtraAov* extras = irExtraAovs(n);
        for (size_t e = 0; e < n; ++e) {
          if (aov != extras[e].name) continue;
          extraAt = st.aov.*(extras[e].slot);
          extraComponents = extras[e].components;
          break;
        }
      }
      const bool isPrimId = (aov == HdAovTokens->primId);
      const bool isInstanceId = (aov == HdAovTokens->instanceId);
      for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
          float rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
          const bool inside = haveImage && x < w && y < h;
          const size_t p1 = inside ? (size_t(y) * size_t(w) + size_t(x)) : 0;
          if (isColor && inside) {
            // Hydra's buffers count rows from the bottom, like ours
            const size_t pi = p1 * 4;
            rgba[0] = fb.rgba[pi]; rgba[1] = fb.rgba[pi + 1];
            rgba[2] = fb.rgba[pi + 2]; rgba[3] = fb.rgba[pi + 3];
          }
          else if (isDepth && inside && !fb.depth.empty()) {
            rgba[0] = rgba[1] = rgba[2] = fb.depth[p1];
            rgba[3] = 1.0f;
          }
          else if (isAlpha && inside) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = fb.rgba[p1 * 4 + 3];
          }
          else if (isAlbedo && inside && !fb.albedo.empty()) {
            rgba[0] = fb.albedo[p1 * 3]; rgba[1] = fb.albedo[p1 * 3 + 1]; rgba[2] = fb.albedo[p1 * 3 + 2];
            rgba[3] = 1.0f;
          }
          else if (extraAt >= 0 && inside && fb.aovStride > 0
                   && (p1 + 1) * size_t(fb.aovStride) <= fb.extra.size()) {
            const float* px = &fb.extra[p1 * size_t(fb.aovStride) + size_t(extraAt)];
            for (int c = 0; c < extraComponents && c < 3; ++c) rgba[c] = px[c];
            rgba[3] = 1.0f;
          }
          else if (isNormal && inside && !fb.normal.empty()) {
            rgba[0] = fb.normal[p1 * 3]; rgba[1] = fb.normal[p1 * 3 + 1]; rgba[2] = fb.normal[p1 * 3 + 2];
            rgba[3] = 1.0f;
          }
          else if ((isPrimId || isInstanceId) && inside && !fb.instanceId.empty()) {
            rgba[0] = rgba[1] = rgba[2] = fb.instanceId[p1];
            rgba[3] = 1.0f;
          }
          const size_t p = size_t(y) * size_t(bw) + size_t(x);
          switch (buffer->GetFormat()) {
            case HdFormatFloat32Vec4: {
              float* px = reinterpret_cast<float*>(buffer->raw().data());
              for (int c = 0; c < 4; ++c) px[p * 4 + c] = rgba[c];
              break;
            }
            case HdFormatFloat16Vec4: {
              GfHalf* px = reinterpret_cast<GfHalf*>(buffer->raw().data());
              for (int c = 0; c < 4; ++c) px[p * 4 + c] = GfHalf(rgba[c]);
              break;
            }
            case HdFormatUNorm8Vec4: {
              uint8_t* px = buffer->raw().data();
              for (int c = 0; c < 4; ++c) {
                const float v = rgba[c] < 0.0f ? 0.0f : (rgba[c] > 1.0f ? 1.0f : rgba[c]);
                px[p * 4 + c] = uint8_t(v * 255.0f + 0.5f);
              }
              break;
            }
            case HdFormatFloat32: {
              float* px = reinterpret_cast<float*>(buffer->raw().data());
              px[p] = rgba[0];
              break;
            }
            case HdFormatFloat32Vec3: {
              float* px = reinterpret_cast<float*>(buffer->raw().data());
              for (int c = 0; c < 3; ++c) px[p * 3 + c] = rgba[c];
              break;
            }
            case HdFormatInt32: {
              int32_t* px = reinterpret_cast<int32_t*>(buffer->raw().data());
              px[p] = int32_t(rgba[0]);
              break;
            }
            default: break;
          }
        }
      }
      buffer->SetConverged(true);
    }
  }

  bool IsConverged() const override { return true; }

private:
  // Reading a 4k HDRI is far too slow to do on every render, and Hydra rebuilds
  // the scene each time, so decoded images are kept by path.  Nuke's own "nkop:"
  // paths carry a hash of the upstream op, so they change when its output does.
  //
  // WHICH IS ALSO THE PROBLEM.  Scrubbing mints a new path every frame, and this
  // held every one of them, uncapped, at full resolution - a 4096x2048 texture is
  // 128 MB, so a hundred frames of timeline is more memory than the machine has.
  // What that looked like was the texture disappearing part way through a scrub
  // and never coming back, because once an allocation failed the failure was
  // remembered for good.  So: a size cap, a bound on the cache, and a failure to
  // read a NUKE OP is not remembered at all - the op may simply have been
  // mid-evaluation, and it will be there on the next render.  A missing FILE is
  // worth remembering, since it will still be missing.
  int _loadTexture(const std::string& path, ir::Scene& scene)
  {
    std::map<std::string, ir::ImageData>::iterator it = _images.find(path);
    if (it == _images.end()) {
      ir::ImageData img;
      std::string err;
      // a texture wired from a Nuke node is the node, not a file
      const bool isOp = ir::isNukeOpPath(path);
      const bool fromNuke = isOp
                         && ir::loadNukeOpImage(path, kMaxTextureSize, img, err) && img.valid();
      if (!fromNuke && (!ir::loadImageFile(path, ir::kColorAuto, kMaxTextureSize, img, err) || !img.valid())) {
        hlog("could not read " + path + ": " + err);
        if (!isOp) _images[path] = ir::ImageData();   // a missing file stays missing
        return -1;
      }
      hlog("read " + path + " (" + std::to_string(img.width) + "x" + std::to_string(img.height) + ")");
      // Cleared wholesale rather than aged: what this exists for is the same few
      // textures being asked for over and over, not a working set.
      if (_images.size() >= kMaxCachedImages) {
        hlog("texture cache full, clearing");
        _images.clear();
      }
      it = _images.insert(std::make_pair(path, img)).first;
    }
    if (!it->second.valid()) return -1;
    const ir::ImageData& img = it->second;
    ir::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.firstTexel = int(scene.texels.size() / 4);
    td.mipCount = 1;
    td.mipOffset[0] = td.firstTexel;
    td.mipW[0] = img.width;
    td.mipH[0] = img.height;
    scene.texels.insert(scene.texels.end(), img.rgba.begin(), img.rgba.end());
    const int id = int(scene.textures.size());
    scene.textures.push_back(td);
    scene.textureNames.push_back(path);
    return id;
  }

  // The same cap the node uses, so a texture does not look different through
  // GeoRender than it does through the node.
  static const int kMaxTextureSize = 4096;
  // A texture at that cap is 128 MB of float RGBA; a timeline's worth is not
  // something to hold.
  static const size_t kMaxCachedImages = 8;

  HdIrScene* _scene = nullptr;
  ir::CpuRenderer _cpu;
  std::map<std::string, ir::ImageData> _images;
#if IR_HAVE_OPTIX
  ir::GpuRenderer _gpu;
  ir::Denoiser _denoiser;
#endif
};

// ---------------------------------------------------------------------------
class HdIrRenderDelegate final : public HdRenderDelegate
{
public:
  HdIrRenderDelegate() { _Init(); }
  HdIrRenderDelegate(HdRenderSettingsMap const& settingsMap) : HdRenderDelegate(settingsMap) { _Init(); }
  ~HdIrRenderDelegate() override = default;

  const TfTokenVector& GetSupportedRprimTypes() const override { return _rprims; }
  const TfTokenVector& GetSupportedSprimTypes() const override { return _sprims; }
  const TfTokenVector& GetSupportedBprimTypes() const override { return _bprims; }

  HdResourceRegistrySharedPtr GetResourceRegistry() const override
  {
    hlog("GetResourceRegistry()");
    return _resourceRegistry;
  }

  HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex* index,
                                         HdRprimCollection const& collection) override
  {
    hlog("CreateRenderPass()");
    return HdRenderPassSharedPtr(new HdIrRenderPass(index, collection, &_irScene));
  }

  HdInstancer* CreateInstancer(HdSceneDelegate* delegate, SdfPath const& id) override
  {
    hlog("CreateInstancer(" + id.GetString() + ")");
    return new HdIrInstancer(delegate, id);
  }
  void DestroyInstancer(HdInstancer* instancer) override { delete instancer; }

  HdRprim* CreateRprim(TfToken const& typeId, SdfPath const& rprimId) override
  {
    hlog("CreateRprim(" + typeId.GetString() + ", " + rprimId.GetString() + ")");
    if (typeId == HdPrimTypeTokens->mesh) return new HdIrMesh(rprimId, &_irScene);
    if (typeId == HdPrimTypeTokens->points) return new HdIrPoints(rprimId, &_irScene);
    if (typeId == HdPrimTypeTokens->basisCurves) return new HdIrCurves(rprimId, &_irScene);
    if (typeId == HdPrimTypeTokens->volume) return new HdIrVolume(rprimId, &_irScene);
    return nullptr;
  }
  void DestroyRprim(HdRprim* rPrim) override { delete rPrim; }

  static bool _isLight(TfToken const& typeId)
  {
    return typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->sphereLight
        || typeId == HdPrimTypeTokens->rectLight || typeId == HdPrimTypeTokens->diskLight
        || typeId == HdPrimTypeTokens->cylinderLight || typeId == HdPrimTypeTokens->domeLight
        || typeId == HdPrimTypeTokens->simpleLight;
  }

  HdSprim* CreateSprim(TfToken const& typeId, SdfPath const& sprimId) override
  {
    if (typeId == HdPrimTypeTokens->camera) return new HdCamera(sprimId);
    if (typeId == HdPrimTypeTokens->material) return new HdIrMaterial(sprimId, &_irScene);
    if (_isLight(typeId)) return new HdIrLight(sprimId, typeId, &_irScene);
    return nullptr;
  }
  HdSprim* CreateFallbackSprim(TfToken const& typeId) override
  {
    if (typeId == HdPrimTypeTokens->camera) return new HdCamera(SdfPath::EmptyPath());
    if (typeId == HdPrimTypeTokens->material) return new HdIrMaterial(SdfPath::EmptyPath(), &_irScene);
    if (_isLight(typeId)) return new HdIrLight(SdfPath::EmptyPath(), typeId, &_irScene);
    return nullptr;
  }
  void DestroySprim(HdSprim* sprim) override { delete sprim; }

  HdRenderParam* GetRenderParam() const override
  {
    hlog("GetRenderParam()");
    return nullptr;
  }

  HdBprim* CreateBprim(TfToken const& typeId, SdfPath const& bprimId) override
  {
    if (typeId == HdPrimTypeTokens->renderBuffer) return new HdIrRenderBuffer(bprimId);
    // A volume's grids arrive as these, one per grid.  Without them a volume
    // syncs with an empty field list and renders nothing at all.
    if (typeId == HdLegacyPrimTypeTokens->openvdbAsset
        || typeId == HdLegacyPrimTypeTokens->field3dAsset)
      return new HdIrField(bprimId, &_irScene);
    return nullptr;
  }
  HdBprim* CreateFallbackBprim(TfToken const& typeId) override
  {
    if (typeId == HdPrimTypeTokens->renderBuffer) return new HdIrRenderBuffer(SdfPath::EmptyPath());
    if (typeId == HdLegacyPrimTypeTokens->openvdbAsset
        || typeId == HdLegacyPrimTypeTokens->field3dAsset)
      return new HdIrField(SdfPath::EmptyPath(), &_irScene);
    return nullptr;
  }
  void DestroyBprim(HdBprim* bprim) override { delete bprim; }

  void CommitResources(HdChangeTracker* tracker) override { hlog("CommitResources()"); }

  // What Nuke reads off the delegate (see FnUsdShim/nuke/NukeRenderInterfaceImpl.cpp):
  //   GetMaterialRenderContexts()   -> the trailing token in the renderer menu
  //   GetRenderSettingDescriptors() -> GeoRender's "renderer settings" table
  //   GetDefaultAovDescriptor()     -> which AOVs exist, and the viewer's display aov
  TfTokenVector GetMaterialRenderContexts() const override
  {
    return { TfToken("nuke"), TfToken("glslfx") };
  }

  HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override { return _settings; }

  // A setting changed: points and curves are tessellated at Sync time, so the
  // values they read have to be up to date before the next one.
  void SetRenderSetting(TfToken const& key, VtValue const& value) override
  {
    HdRenderDelegate::SetRenderSetting(key, value);
    HdIrTessellation t = _irScene.tessellation();
    if (key == TfToken("pointDetail") && value.IsHolding<int>()) t.pointDetail = value.UncheckedGet<int>();
    else if (key == TfToken("curveSides") && value.IsHolding<int>()) t.curveSides = value.UncheckedGet<int>();
    else if (key == TfToken("curveSegments") && value.IsHolding<int>()) t.curveSegments = value.UncheckedGet<int>();
    else if (key == TfToken("subdivLevels") && value.IsHolding<int>()) t.subdivLevels = value.UncheckedGet<int>();
    else return;
    _irScene.setTessellation(t);
  }

  HdAovDescriptor GetDefaultAovDescriptor(TfToken const& name) const override
  {
    if (name == HdAovTokens->color)
      return HdAovDescriptor(HdFormatFloat32Vec4, true, VtValue(GfVec4f(0.0f)));
    if (name == _irAovTokens->alpha)
      return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    if (name == _irAovTokens->albedo)
      return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    {
      size_t n = 0;
      const IrExtraAov* extras = irExtraAovs(n);
      for (size_t i = 0; i < n; ++i) {
        if (name != extras[i].name) continue;
        if (extras[i].components == 1)
          return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
        return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
      }
    }
    if (name == HdAovTokens->depth)
      return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    if (name == HdAovTokens->normal)
      return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId)
      return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    return HdAovDescriptor();
  }

private:
  void _Init()
  {
    hlog("delegate constructed");
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
    // what this delegate will consume once the geometry goes in
    _rprims = { HdPrimTypeTokens->mesh, HdPrimTypeTokens->points, HdPrimTypeTokens->basisCurves,
                HdPrimTypeTokens->volume };
    _sprims = { HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
                HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight,
                HdPrimTypeTokens->rectLight, HdPrimTypeTokens->diskLight,
                HdPrimTypeTokens->cylinderLight, HdPrimTypeTokens->domeLight,
                HdPrimTypeTokens->simpleLight };
    _bprims = { HdPrimTypeTokens->renderBuffer,
                HdLegacyPrimTypeTokens->openvdbAsset,
                HdLegacyPrimTypeTokens->field3dAsset };
    // the knobs this renderer has always had, offered to GeoRender
    _settings.push_back(HdRenderSettingDescriptor{ "Samples", TfToken("samples"), VtValue(int(16)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Max bounces", TfToken("maxBounces"), VtValue(int(2)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Use GPU", TfToken("useGpu"), VtValue(bool(true)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Point detail", TfToken("pointDetail"), VtValue(int(2)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Curve sides", TfToken("curveSides"), VtValue(int(6)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Curve segments", TfToken("curveSegments"), VtValue(int(4)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Subdivision levels", TfToken("subdivLevels"), VtValue(int(0)) });
    _settings.push_back(HdRenderSettingDescriptor{ "Denoise", TfToken("denoise"), VtValue(bool(false)) });
  }

  HdIrScene _irScene;
  HdResourceRegistrySharedPtr _resourceRegistry;
  TfTokenVector _rprims, _sprims, _bprims;
  HdRenderSettingDescriptorList _settings;

  HdIrRenderDelegate(const HdIrRenderDelegate&) = delete;
  HdIrRenderDelegate& operator=(const HdIrRenderDelegate&) = delete;
};

// ---------------------------------------------------------------------------
class HdIrRendererPlugin final : public HdRendererPlugin
{
public:
  ~HdIrRendererPlugin() override = default;

  HdIrRendererPlugin() : _announced((hlog("plugin constructed"), true)) {}
  HdRenderDelegate* CreateRenderDelegate() override
  {
    hlog("CreateRenderDelegate()");
    return new HdIrRenderDelegate();
  }
  HdRenderDelegate* CreateRenderDelegate(HdRenderSettingsMap const& settingsMap) override
  {
    return new HdIrRenderDelegate(settingsMap);
  }
  void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override { delete renderDelegate; }
  bool IsSupported(bool gpuEnabled = true) const override
  {
    hlog(std::string("IsSupported(gpu=") + (gpuEnabled ? "1" : "0") + ") -> true");
    return true;
  }

private:
  bool _announced = false;
  HdIrRendererPlugin(const HdIrRendererPlugin&) = delete;
  HdIrRendererPlugin& operator=(const HdIrRendererPlugin&) = delete;
};

// the name here must match the type name in plugInfo.json
// TF_REGISTRY_FUNCTION alone is not enough here.  On Windows it emits its entry
// as an unreferenced const in an anonymous namespace inside a .pxrctor section,
// and MSVC drops it from the object file entirely - the library then loads and
// registers NOTHING, so Nuke lists the renderer and then refuses it with
// "Render interface is not available for the selected renderer".  Registering
// from an ordinary static initialiser, which does run, is the reliable route;
// the registry function below stays for hosts that do dispatch it.
namespace {
  struct RegisterPluginType {
    RegisterPluginType()
    {
      hlog("static registration running");
      HdRendererPluginRegistry::Define<HdIrRendererPlugin>();
      const TfType t = TfType::Find<HdIrRendererPlugin>();
      hlog(std::string("registered as: ") + (t.IsUnknown() ? "UNKNOWN" : t.GetTypeName()));
    }
  };
  static RegisterPluginType g_registerPluginType;
}

TF_REGISTRY_FUNCTION(TfType)
{
  hlog("TF_REGISTRY_FUNCTION(TfType) running");
  HdRendererPluginRegistry::Define<HdIrRendererPlugin>();
  const TfType t = TfType::Find<HdIrRendererPlugin>();
  hlog(std::string("registered as: ") + (t.IsUnknown() ? "UNKNOWN" : t.GetTypeName()));
  const TfType base = TfType::Find<HdRendererPlugin>();
  hlog(std::string("base HdRendererPlugin: ") + (base.IsUnknown() ? "UNKNOWN" : base.GetTypeName()));
}

PXR_NAMESPACE_CLOSE_SCOPE
