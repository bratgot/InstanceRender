// InstanceRender - StageLoader.cpp  (strict ASCII)
#include "StageLoader.h"

#include <iostream>
#include <fstream>
#include <mutex>

#include "Dome.h"
#include "Texture.h"
#include "Subdivide.h"
#include "Tessellate.h"
#include "Gprims.h"
#include "Ies.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#ifdef IR_HAS_VOLUMES
#include <pxr/usd/usdVol/volume.h>
#include <pxr/imaging/hio/types.h>
#include <pxr/usd/usdVol/openVDBAsset.h>
#include <pxr/imaging/hio/fieldTextureData.h>
#include "VolumeRead.h"
#endif
#include <pxr/usd/sdf/assetPath.h>

#include "NukeOpImage.h"
#include "Env.h"
#include "Trace.h"

#include <chrono>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/tokens.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace ir {

namespace {

Xform toXform(const GfMatrix4d& m)
{
  // GfMatrix4d is row-vector convention: p' = p * M, translation in row 3.
  // Our Xform is p' = R * p + t with rows; so rows of Xform = columns of M (3x3 transposed).
  Xform x;
  x.m[0] = float(m[0][0]); x.m[1] = float(m[1][0]); x.m[2] = float(m[2][0]); x.m[3] = float(m[3][0]);
  x.m[4] = float(m[0][1]); x.m[5] = float(m[1][1]); x.m[6] = float(m[2][1]); x.m[7] = float(m[3][1]);
  x.m[8] = float(m[0][2]); x.m[9] = float(m[1][2]); x.m[10] = float(m[2][2]); x.m[11] = float(m[3][2]);
  return x;
}

Vec3 toV(const GfVec3f& v) { return Vec3(v[0], v[1], v[2]); }


// The path an asset attribute is really pointing at.
//
// GetResolvedPath() first is right for files and WRONG for Nuke's own texture
// paths.  Those are deliberately FAKE - "nkop:/NkRoot/Read1:1:main:...nkiop"
// carries an Op pointer through USD and is meant to be handed to
// MaterialOpI::retrieveOpFromAssetPath(), not resolved against a filesystem -
// and USD's resolver does not leave them alone.  It resolved that one to "1",
// which is not empty, so the raw path was never reached, and the loader then
// tried to open a file called "1", failed, and the card rendered grey with a
// material that had been found perfectly well.
//
// So: if the raw path is one of Nuke's, use it untouched.
inline std::string assetPathFor(const SdfAssetPath& asset)
{
  const std::string raw = asset.GetAssetPath();
  if (ir::isNukeOpPath(raw)) return raw;
  const std::string resolved = asset.GetResolvedPath();
  return resolved.empty() ? raw : resolved;
}

bool envFlag(const char* name)
{
  return envOn(name);
}

// IR_MOTION_PROBE=1 appends what the stage knows about time to the info knob:
// motion blur is only possible if the stage Nuke builds carries time samples at
// all (BuildStage bakes at the op's current frame, so this needs answering
// before any motion design).
std::string motionProbe(const UsdStageRefPtr& stage, double t)
{
  std::ostringstream o;
  o << "\nmotion probe: timeCodeRange " << stage->GetStartTimeCode() << ".." << stage->GetEndTimeCode()
    << " authored=" << (stage->HasAuthoredTimeCodeRange() ? "yes" : "no") << "; sampled at " << t;
  UsdGeomXformCache c0((UsdTimeCode(t))), c1((UsdTimeCode(t + 1.0)));
  int reported = 0;
  UsdPrimRange range = stage->Traverse();
  for (UsdPrimRange::iterator it = range.begin(); it != range.end() && reported < 6; ++it) {
    const UsdPrim prim = *it;
    const bool isMesh = prim.IsA<UsdGeomMesh>();
    const bool isPI = prim.IsA<UsdGeomPointInstancer>();
    if (!isMesh && !isPI && !prim.IsInstance()) continue;
    ++reported;
    const GfMatrix4d m0 = c0.GetLocalToWorldTransform(prim);
    const GfMatrix4d m1 = c1.GetLocalToWorldTransform(prim);
    o << "\n  " << prim.GetPath().GetString() << (isPI ? " [PointInstancer]" : (isMesh ? " [Mesh]" : " [instance]"))
      << " xform moves=" << ((m0 == m1) ? "no" : "YES");
    int xformSamples = 0;
    UsdGeomXformable xf(prim);
    if (xf) {
      bool reset = false;
      const std::vector<UsdGeomXformOp> ops = xf.GetOrderedXformOps(&reset);
      for (size_t i = 0; i < ops.size(); ++i)
        if (ops[i].GetAttr()) xformSamples += int(ops[i].GetAttr().GetNumTimeSamples());
    }
    o << " xformOp samples=" << xformSamples;
    if (isMesh) {
      UsdAttribute pts = UsdGeomMesh(prim).GetPointsAttr();
      o << " points samples=" << (pts ? int(pts.GetNumTimeSamples()) : -1);
    }
    if (isPI) {
      UsdGeomPointInstancer pi(prim);
      UsdAttribute pos = pi.GetPositionsAttr(), vel = pi.GetVelocitiesAttr();
      o << " positions samples=" << (pos ? int(pos.GetNumTimeSamples()) : -1)
        << " velocities=" << ((vel && vel.HasAuthoredValue()) ? "authored" : "none");
    }
  }
  return o.str();
}

struct Builder {
  Scene& scene;
  const LoaderOptions& opt;
  UsdTimeCode time, timeClose;
  UsdGeomXformCache xf;      // at shutter open (or just the frame)
  UsdGeomXformCache xfClose; // at shutter close; only consulted when motion is on
  bool motion;
  int  numKeys;                              // transforms sampled across the shutter
  std::vector<UsdTimeCode> keyTimes;
  std::vector<UsdGeomXformCache*> keyCaches; // one per key, xf and xfClose are the ends
  std::map<SdfPath, int> protoByPath;     // prototype root prim -> proto id
  std::vector<int> protoHasMaterial;      // per proto: did any of its meshes bind a material?
  std::map<SdfPath, int> materialByPath;
  std::map<std::string, int> iesByPath;   // one table per file, however many lights use it
  std::map<std::string, int> textureByKey;
  int defaultMaterial;
  int textureCount, textureFailures;
  int nukeTextureDisconnected;
  int udimSets = 0, udimTilesLoaded = 0, mipLevels = 0;
  std::ostringstream warn;
  int meshCount, instancerCount, nativeInstanceCount, lightCount, skippedInvisible, skippedGuide;
  int domeTexture = -1;
  int subdividedMeshes = 0;
  int displacedMeshes = 0;
  int pointCount = 0, curveCount = 0;
  size_t volumeVoxels = 0;
  int creasedMeshes = 0;
  int movingInstances = 0;

  ~Builder()
  {
    for (size_t k = 0; k < keyCaches.size(); ++k)
      if (keyCaches[k] && keyCaches[k] != &xf && keyCaches[k] != &xfClose) delete keyCaches[k];
  }

  // remember the second transform and note whether it differs
  void setMotion(Instance& inst, const GfMatrix4d& m1)
  {
    inst.xf1 = toXform(m1);
    bool moves = false;
    for (int i = 0; i < 12 && !moves; ++i) moves = (inst.xf1.m[i] != inst.xf.m[i]);
    if (moves) { ++movingInstances; scene.hasMotion = true; }
  }

  // store one transform per motion key; 'keys' must hold numKeys matrices
  void setMotionKeys(Instance& inst, const std::vector<GfMatrix4d>& keys)
  {
    if (int(keys.size()) != numKeys || numKeys < 2) return;
    inst.firstKey = int(scene.motionKeys.size());
    bool moves = false;
    for (int k = 0; k < numKeys; ++k) {
      const Xform x = toXform(keys[size_t(k)]);
      scene.motionKeys.push_back(x);
      if (k == 0) inst.xf = x;
      if (k == numKeys - 1) inst.xf1 = x;
      if (!moves) for (int i = 0; i < 12 && !moves; ++i) moves = (x.m[i] != scene.motionKeys[size_t(inst.firstKey)].m[i]);
    }
    if (moves) { ++movingInstances; scene.hasMotion = true; }
  }

  Builder(Scene& s, const LoaderOptions& o)
    : scene(s), opt(o), time(o.timeCode + o.shutterOpen), timeClose(o.timeCode + o.shutterClose),
      xf(UsdTimeCode(o.timeCode + o.shutterOpen)), xfClose(UsdTimeCode(o.timeCode + o.shutterClose)),
      motion(o.shutterClose > o.shutterOpen), numKeys(1), defaultMaterial(0),
      textureCount(0), textureFailures(0), nukeTextureDisconnected(0),
      meshCount(0), instancerCount(0), nativeInstanceCount(0), lightCount(0), skippedInvisible(0), skippedGuide(0)
  {
    Material m; m.useDisplayColor = 1;
    scene.materials.push_back(m);
    scene.materialNames.push_back("<displayColor>");
    if (motion) {
      numKeys = (o.motionKeys < 2) ? 2 : ((o.motionKeys > 16) ? 16 : o.motionKeys);
      keyTimes.resize(size_t(numKeys));
      keyCaches.resize(size_t(numKeys), nullptr);
      for (int k = 0; k < numKeys; ++k) {
        const double t = o.timeCode + o.shutterOpen
                       + (o.shutterClose - o.shutterOpen) * (double(k) / double(numKeys - 1));
        keyTimes[size_t(k)] = UsdTimeCode(t);
        keyCaches[size_t(k)] = (k == 0) ? &xf : ((k == numKeys - 1) ? &xfClose : new UsdGeomXformCache(UsdTimeCode(t)));
      }
      scene.motionKeyCount = numKeys;
    }
  }

  // ---- materials ----------------------------------------------------------------
  template <class T> static bool inputValue(const UsdShadeShader& sh, const char* name, const UsdTimeCode& t, T& out)
  {
    UsdShadeInput in = sh.GetInput(TfToken(name));
    if (!in) return false;
    // connected inputs (textures, graphs) are not resolved yet -> fall back to the constant if any
    VtValue v;
    if (!in.Get(&v, t)) {
      UsdAttribute a = in.GetAttr();
      if (!a || !a.Get(&v, t)) return false;
    }
    if (!v.IsHolding<T>()) return false;
    out = v.UncheckedGet<T>();
    return true;
  }

  // ---- textures -----------------------------------------------------------------
  static int wrapMode(const TfToken& t)
  {
    if (t == TfToken("clamp")) return kWrapClamp;
    if (t == TfToken("mirror")) return kWrapMirror;
    if (t == TfToken("black")) return kWrapBlack;
    return kWrapRepeat;                 // "repeat" and "useMetadata"
  }

  // <UDIM> in a path: load every tile that exists on disk and build the
  // 1001..1100 lookup the kernel indexes with floor(u), floor(v).
  int loadUdimSet(const std::string& pattern, int colorSpace, int wrapS, int wrapT)
  {
    const size_t tag = pattern.find("<UDIM>");
    if (tag == std::string::npos) return -1;
    std::map<std::string, int>::iterator it = textureByKey.find("udim|" + pattern);
    if (it != textureByKey.end()) return it->second;
    const int base = int(scene.udimTiles.size());
    scene.udimTiles.resize(size_t(base) + 100, -1);
    int found = 0;
    for (int tile = 0; tile < 100; ++tile) {
      char num[16];
      std::snprintf(num, sizeof(num), "%d", 1001 + tile);
      std::string file = pattern;
      file.replace(tag, 6, num);
      if (FILE* f = std::fopen(file.c_str(), "rb")) {
        std::fclose(f);
        const int id = loadTexture(file, colorSpace, wrapS, wrapT);
        if (id >= 0) { scene.udimTiles[size_t(base) + size_t(tile)] = id; ++found; }
      }
    }
    if (found == 0) {
      warn << "texture: no <UDIM> tiles found for " << pattern << "\n";
      scene.udimTiles.resize(size_t(base));
      textureByKey["udim|" + pattern] = -1;
      return -1;
    }
    udimSets += 1;
    udimTilesLoaded += found;
    textureByKey["udim|" + pattern] = base;
    return base;
  }

  int loadTexture(const std::string& path, int colorSpace, int wrapS, int wrapT)
  {
    std::ostringstream k;
    k << path << '|' << colorSpace << '|' << wrapS << '|' << wrapT;
    const std::string key = k.str();
    // An "nkop:" path names a Nuke op, and BOTH what the op is doing and whether
    // it is connected at all can change without the path changing - Nuke stops
    // maintaining the URI the moment the input is made. So the op is asked what
    // it is doing now, and the answer joins the key: no answer means it is no
    // longer feeding this render, and a different answer means a different
    // picture under the same URI.
    std::string liveKey = key;
    if (path.compare(0, 5, "nkop:") == 0 && opt.nukeOpIdentity) {
      const std::string ident = opt.nukeOpIdentity(path);
      if (ident.empty()) { ++nukeTextureDisconnected; return -1; }
      liveKey += '|';
      liveKey += ident;
    }
    const std::string& key2 = liveKey;
    std::map<std::string, int>::iterator it = textureByKey.find(key2);
    if (it != textureByKey.end()) return it->second;
    // Kept from a previous load?  The key is everything that decides the content:
    // the path, the colour space, the wrap modes and the size cap - plus, for an
    // nkop: path, what that op is doing NOW, because the URI alone stops
    // describing the picture as soon as the input is made.
    std::ostringstream ck;
    ck << liveKey << '|' << opt.maxTextureSize << '|' << (opt.mipFilter ? 1 : 0);
    const std::string cacheKey = ck.str();
    if (opt.textureCache) {
      if (const CachedTexture* hit = opt.textureCache->find(cacheKey)) {
        TextureDesc td;
        td.firstTexel = int(scene.texels.size() / 4);
        td.width = hit->width; td.height = hit->height;
        td.wrapS = wrapS; td.wrapT = wrapT;
        td.mipCount = hit->mipCount;
        for (int l = 0; l < hit->mipCount && l < kMaxMipLevels; ++l) {
          td.mipOffset[l] = td.firstTexel + hit->mipOffset[l];
          td.mipW[l] = hit->mipW[l];
          td.mipH[l] = hit->mipH[l];
        }
        scene.texels.insert(scene.texels.end(), hit->texels.begin(), hit->texels.end());
        const int cachedId = int(scene.textures.size());
        scene.textures.push_back(td);
        scene.textureNames.push_back(path);
        scene.textureKeys.push_back(cacheKey);
        textureByKey[key2] = cachedId;
        ++textureCount;
        if (td.mipCount > 1) mipLevels += td.mipCount - 1;
        return cachedId;
      }
    }

    ImageData img;
    std::string err;
    // A texture fed by a Nuke node is not a file: Nuke authors an "nkop:" path
    // naming the op, and reading one through Hio only works while Nuke happens
    // to be holding a texture image for that op at that frame - otherwise its
    // plugin hands back "a default 1x1 grey texture" (its own words), which is
    // why such a texture came and went along the timeline.  So the host reads
    // the op directly instead, which it can do at any frame.
    const bool nukeOp = (path.size() > 6 && path.compare(path.size() - 6, 6, ".nkiop") == 0);
    if (nukeOp && opt.nukeOpImage) {
      // The bake is the ANSWER for one of these, not the first thing to try.
      //
      // Falling through to the image readers when it failed looked harmless and
      // was the whole bug: Nuke's own Hio plugin will happily open an "nkop:"
      // path and hand back a ONE PIXEL GREY when it is not holding an image for
      // that op.  That is a perfectly good image as far as everything here is
      // concerned - so nothing was reported, the surface rendered flat, and the
      // frame was remembered that way.  A bake that fails is a failure.
      if (!opt.nukeOpImage(path, opt.maxTextureSize, img) || !img.valid()) {
        err = "cannot read the Nuke node feeding this texture (" + path + ")";
        warn << "texture: " << err << "\n";
        ++textureFailures;
        ++scene.nukeTextureFailures;    // transient: worth coming back for
        textureByKey[key2] = -1;
        return -1;
      }
    }
    else if (!loadImageFile(path, colorSpace, opt.maxTextureSize, img, err) || !img.valid()) {
      warn << "texture: " << err << "\n";
      ++textureFailures;
      textureByKey[key2] = -1;
      return -1;
    }
    // ---- scrub the pixels ---------------------------------------------------------
    // A texture is allowed to arrive holding NaN, infinity, negatives, or numbers
    // that are not a picture at all, and the arithmetic downstream cannot survive
    // any of them.  A NaN multiplied by anything is still a NaN, it survives every
    // average, and one texel is enough to put holes through a frame.
    //
    // MEASURED, same file, same renderer, only the colour management changed:
    //   Nuke  : the dome function peaked at 46368, integral 0.59
    //   OCIO  : it peaked at 1.96e37, integral 2.76e33
    // An importance distribution built from 1e37 overflows the moment anything is
    // squared or summed, and what comes out is NaN rather than a bright frame -
    // which is why this only ever appeared with OCIO on.
    //
    // The ceiling is not a look choice: real HDRIs peak in the thousands, and
    // nothing above a billion is an image.  It is said out loud, with the likely
    // cause, rather than quietly clamped.
    {
      const float kMaxTexel = 1.0e9f;
      size_t bad = 0, huge = 0;
      float mx = 0.0f;
      for (size_t i = 0; i < img.rgba.size(); ++i) {
        float& v = img.rgba[i];
        if (!(v == v) || v > 3.0e38f || v < 0.0f) { v = 0.0f; ++bad; continue; }
        if (v > mx) mx = v;
        if (v > kMaxTexel) { v = kMaxTexel; ++huge; }
      }
      if (bad)
        warn << "texture: " << path << " held " << bad
             << " value(s) that were NaN, infinite or negative; they have been zeroed. "
                "One of them is enough to put holes through the frame." << "\n";
      if (huge)
        warn << "texture: " << path << " peaks at " << mx << ", which is not a picture - "
             << huge << " value(s) clamped to " << kMaxTexel << ". This is almost always a "
                "colourspace set wrongly on the node reading it." << "\n";
    }

    TextureDesc td;
    td.firstTexel = int(scene.texels.size() / 4);
    td.width = img.width; td.height = img.height;
    td.wrapS = wrapS; td.wrapT = wrapT;
    // the whole mip chain lives in the same pool, level 0 first
    std::vector<ImageData> levels;
    const int mips = opt.mipFilter ? buildMipChain(img, levels) : 0;
    if (mips >= 1) {
      td.mipCount = (mips > kMaxMipLevels) ? kMaxMipLevels : mips;
      for (int l = 0; l < td.mipCount; ++l) {
        td.mipOffset[l] = int(scene.texels.size() / 4);
        td.mipW[l] = levels[size_t(l)].width;
        td.mipH[l] = levels[size_t(l)].height;
        scene.texels.insert(scene.texels.end(), levels[size_t(l)].rgba.begin(), levels[size_t(l)].rgba.end());
      }
      mipLevels += td.mipCount - 1;
    }
    else {
      td.mipCount = 1;
      td.mipOffset[0] = td.firstTexel;
      td.mipW[0] = img.width; td.mipH[0] = img.height;
      scene.texels.insert(scene.texels.end(), img.rgba.begin(), img.rgba.end());
    }
    if (opt.textureCache) {
      CachedTexture keep;
      keep.width = td.width; keep.height = td.height; keep.mipCount = td.mipCount;
      for (int l = 0; l < td.mipCount && l < kMaxMipLevels; ++l) {
        keep.mipOffset[l] = td.mipOffset[l] - td.firstTexel;
        keep.mipW[l] = td.mipW[l];
        keep.mipH[l] = td.mipH[l];
      }
      keep.texels.assign(scene.texels.begin() + size_t(td.firstTexel) * 4, scene.texels.end());
      opt.textureCache->trim();
      opt.textureCache->put(cacheKey, keep);
    }
    const int id = int(scene.textures.size());
    scene.textures.push_back(td);
    scene.textureNames.push_back(path);
    scene.textureKeys.push_back(cacheKey);
    textureByKey[key2] = id;
    ++textureCount;
    return id;
  }

  // Follow an input's connection to the UsdUVTexture that feeds it (through any
  // NodeGraph indirection) and fill 'ref'.
  bool resolveTexture(const UsdShadeInput& in, TexRef& ref, const SdfPath& matPath)
  {
    if (!in || !opt.textures || opt.transformsOnly) return false;
    UsdShadeConnectableAPI src;
    TfToken srcName;
    UsdShadeAttributeType srcType;
    if (!in.GetConnectedSource(&src, &srcName, &srcType)) return false;
    UsdPrim prim = src.GetPrim();
    TfToken outName = srcName;
    for (int hop = 0; hop < 8 && prim; ++hop) {
      UsdShadeShader sh(prim);
      TfToken id;
      if (sh) sh.GetShaderId(&id);
      if (id == TfToken("UsdUVTexture")) break;
      // a NodeGraph (or an unknown shader): follow its output's connection
      UsdShadeConnectableAPI conn(prim);
      if (!conn) return false;
      UsdShadeOutput out = conn.GetOutput(outName);
      UsdShadeConnectableAPI next;
      TfToken nextName;
      UsdShadeAttributeType nextType;
      if (!out || !out.GetConnectedSource(&next, &nextName, &nextType)) return false;
      prim = next.GetPrim();
      outName = nextName;
    }
    UsdShadeShader tex(prim);
    if (!tex) return false;
    TfToken id;
    tex.GetShaderId(&id);
    if (id != TfToken("UsdUVTexture")) {
      warn << "material " << matPath.GetString() << ": " << in.GetBaseName().GetString()
           << " is driven by an unsupported shader (" << id.GetString() << "), using the constant\n";
      return false;
    }
    // UsdTransform2d between the primvar reader and the texture
    {
      UsdShadeInput st = tex.GetInput(TfToken("st"));
      UsdShadeConnectableAPI stSrc;
      TfToken stName;
      UsdShadeAttributeType stType;
      if (st && st.GetConnectedSource(&stSrc, &stName, &stType)) {
        UsdShadeShader stShader(stSrc.GetPrim());
        TfToken stId;
        if (stShader) stShader.GetShaderId(&stId);
        if (stId == TfToken("UsdTransform2d")) {
          GfVec2f sc(1.0f, 1.0f), tl(0.0f, 0.0f);
          float rot = 0.0f;
          inputValue(stShader, "scale", time, sc);
          inputValue(stShader, "translation", time, tl);
          inputValue(stShader, "rotation", time, rot);
          const float r = rot * (3.14159265f / 180.0f);
          const float c = std::cos(r), sn = std::sin(r);
          // scale, then rotate, then translate - the UsdPreviewSurface order
          ref.uv[0] = c * sc[0];  ref.uv[1] = -sn * sc[1]; ref.uv[2] = tl[0];
          ref.uv[3] = sn * sc[0]; ref.uv[4] = c * sc[1];   ref.uv[5] = tl[1];
        }
      }
    }

    SdfAssetPath asset;
    UsdShadeInput file = tex.GetInput(TfToken("file"));
    if (!file || !file.Get(&asset, time)) return false;
    std::string path = assetPathFor(asset);
    if (path.empty()) return false;
    TfToken ws, wt, cs;
    inputValue(tex, "wrapS", time, ws);
    inputValue(tex, "wrapT", time, wt);
    inputValue(tex, "sourceColorSpace", time, cs);
    int colorSpace = kColorAuto;
    if (cs == TfToken("raw")) colorSpace = kColorRaw;
    else if (cs == TfToken("sRGB")) colorSpace = kColorSRGB;
    if (path.find("<UDIM>") != std::string::npos) {
      const int set = loadUdimSet(path, colorSpace, wrapMode(ws), wrapMode(wt));
      if (set < 0) return false;
      ref.udimSet = set;
      ref.index = -1;
    }
    else {
      const int texId = loadTexture(path, colorSpace, wrapMode(ws), wrapMode(wt));
      if (texId < 0) return false;
      ref.index = texId;
    }
    // which component of the texture feeds this input (ORM maps share one image)
    const std::string on = outName.GetString();
    ref.channel = (on == "g") ? 1 : (on == "b") ? 2 : (on == "a") ? 3 : 0;
    GfVec4f sc(1.0f, 1.0f, 1.0f, 1.0f), bi(0.0f, 0.0f, 0.0f, 0.0f);
    inputValue(tex, "scale", time, sc);
    inputValue(tex, "bias", time, bi);
    ref.scale = Vec4(sc[0], sc[1], sc[2], sc[3]);
    ref.bias = Vec4(bi[0], bi[1], bi[2], bi[3]);
    return true;
  }

  // material bound to a prim, or -1 when it has none (used for the per-instance
  // override: a binding authored on an instanceable prim cannot reach inside the
  // shared prototype, so the renderer has to apply it per instance)
  int materialIfBound(const UsdPrim& prim)
  {
    UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    if (!mat) return -1;
    return materialFor(prim);
  }

  int materialFor(const UsdPrim& gprim)
  {
    UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(gprim).ComputeBoundMaterial();
    if (!mat) return defaultMaterial;
    const SdfPath mp = mat.GetPath();
    std::map<SdfPath, int>::iterator it = materialByPath.find(mp);
    if (it != materialByPath.end()) return it->second;
    Material m;
    m.useDisplayColor = 0;
    UsdShadeShader surf = mat.ComputeSurfaceSource();
    bool any = false;
    if (surf) {
      GfVec3f c3; float f1; int i1;
      if (inputValue(surf, "diffuseColor", time, c3)) { m.diffuse = toV(c3); any = true; }
      if (inputValue(surf, "emissiveColor", time, c3)) { m.emissive = toV(c3); any = true; }
      if (inputValue(surf, "specularColor", time, c3)) { m.specularColor = toV(c3); any = true; }
      if (inputValue(surf, "metallic", time, f1)) { m.metallic = f1; any = true; }
      if (inputValue(surf, "roughness", time, f1)) { m.roughness = f1; any = true; }
      if (inputValue(surf, "opacity", time, f1)) { m.opacity = f1; any = true; }
      if (inputValue(surf, "ior", time, f1)) { m.ior = f1; any = true; }
      if (inputValue(surf, "useSpecularWorkflow", time, i1)) { m.useSpecularWorkflow = i1; any = true; }
      if (inputValue(surf, "clearcoat", time, f1)) { m.clearcoat = f1; any = true; }
      if (inputValue(surf, "clearcoatRoughness", time, f1)) { m.clearcoatRoughness = f1; any = true; }
      if (inputValue(surf, "occlusion", time, f1)) { m.occlusion = f1; any = true; }
      if (inputValue(surf, "opacityThreshold", time, f1)) { m.opacityThreshold = f1; any = true; }
      if (inputValue(surf, "displacement", time, f1)) { m.displacement = f1; any = true; }
      // A displacement MAP is a different job - it needs the texture sampled per
      // vertex at load, and the vertex uv to exist - so say so rather than
      // silently using the constant and leaving the surface flat.
      if (surf.GetInput(TfToken("displacement"))
          && surf.GetInput(TfToken("displacement")).HasConnectedSource())
        warn << "material " << mp.GetString() << ": a displacement MAP is not read - only "
             << "the constant displacement value is.\n";
      // textures (UsdUVTexture, possibly through a NodeGraph)
      if (resolveTexture(surf.GetInput(TfToken("diffuseColor")), m.diffuseTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("emissiveColor")), m.emissiveTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("roughness")), m.roughnessTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("metallic")), m.metallicTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("opacity")), m.opacityTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("normal")), m.normalTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("clearcoat")), m.clearcoatTex, mp)) any = true;
      if (resolveTexture(surf.GetInput(TfToken("occlusion")), m.occlusionTex, mp)) any = true;

      // NukeDefaultSurface: what a Nuke material op turns into.
      //
      // A GeoCard with an image wired into it does NOT author a
      // UsdPreviewSurface.  It authors a material whose surface output is a
      // shader with info:id "NukeDefaultSurface", carrying two inputs -
      // tex_color and tex_opacity - both connected to an ordinary UsdUVTexture
      // whose file is one of Nuke's nkop: paths.  Looking only for
      // UsdPreviewSurface finds a material with nothing in it, which is why
      // such a card rendered flat grey while ScanlineRender, which knows Nuke's
      // own shaders, showed the picture.
      //
      // The texture underneath is completely ordinary, so only the two input
      // names need translating; everything below this point already handles the
      // rest, including baking the Nuke node the texture points at.
      TfToken surfId;
      surf.GetShaderId(&surfId);
      if (surfId == TfToken("NukeDefaultSurface")) {
        if (inputValue(surf, "tex_color", time, c3)) { m.diffuse = toV(c3); any = true; }
        if (inputValue(surf, "tex_opacity", time, f1)) { m.opacity = f1; any = true; }
        if (resolveTexture(surf.GetInput(TfToken("tex_color")), m.diffuseTex, mp)) any = true;
        if (resolveTexture(surf.GetInput(TfToken("tex_opacity")), m.opacityTex, mp)) any = true;
        // it is an unlit card's worth of shading: no specular lobe to speak of
        m.roughness = 1.0f;
      }

      if (m.diffuseTex.valid()) m.diffuse = Vec3(1.0f, 1.0f, 1.0f);
      else if (surf.GetInput(TfToken("diffuseColor")) && surf.GetInput(TfToken("diffuseColor")).HasConnectedSource()) {
        m.diffuse = Vec3(0.8f, 0.8f, 0.8f);      // unresolved network: mid grey rather than black
      }
    }
    if (!any && !surf) { m.useDisplayColor = 1; }
    const int id = int(scene.materials.size());
    scene.materials.push_back(m);
    scene.materialNames.push_back(mp.GetString());
    materialByPath[mp] = id;
    return id;
  }

  // ---- visibility / purpose -----------------------------------------------------
  bool renderable(const UsdPrim& prim)
  {
    UsdGeomImageable img(prim);
    if (!img) return true;
    if (opt.skipInvisible && img.ComputeVisibility(time) == UsdGeomTokens->invisible) { ++skippedInvisible; return false; }
    if (!opt.includeGuides) {
      const TfToken purpose = img.ComputePurposeInfo().purpose;
      if (purpose == UsdGeomTokens->guide || purpose == UsdGeomTokens->proxy) { ++skippedGuide; return false; }
    }
    return true;
  }

  // ---- meshes -------------------------------------------------------------------
  // How an attribute array maps onto the mesh.  Deduced from the array size
  // (robust), falling back to the declared interpolation when sizes clash.
  enum AttrInterp { kAttrNone, kAttrConstant, kAttrUniform, kAttrVertex, kAttrFaceVarying };

  static AttrInterp classifyAttr(size_t n, size_t npts, size_t nfaces, size_t ncorners, const TfToken& declared)
  {
    if (n == 0) return kAttrNone;
    if (declared == UsdGeomTokens->faceVarying && n == ncorners) return kAttrFaceVarying;
    if (declared == UsdGeomTokens->uniform && n == nfaces) return kAttrUniform;
    if (declared == UsdGeomTokens->constant && n >= 1) return kAttrConstant;
    if (n == npts) return kAttrVertex;
    if (n == ncorners) return kAttrFaceVarying;
    if (n == nfaces) return kAttrUniform;
    if (n == 1) return kAttrConstant;
    return kAttrNone;
  }

  // One output vertex = a point plus the attribute values seen at that corner.
  // faceVarying UVs (every DCC writes them, for the seams) must NOT be averaged
  // per point - 0.9 and 0.1 across a seam would become 0.5 - so corners whose
  // values differ become separate vertices.
  struct CornerAttrs {
    Vec3  n, c;
    float u, v;
    bool same(const CornerAttrs& o) const
    {
      return n.x == o.n.x && n.y == o.n.y && n.z == o.n.z && c.x == o.c.x && c.y == o.c.y && c.z == o.c.z
             && u == o.u && v == o.v;
    }
  };

  struct CornerLookup {
    const VtVec3fArray* nrm; AttrInterp nMode;
    const VtVec3fArray* col; AttrInterp cMode;
    const VtVec2fArray* st;  AttrInterp sMode;
    static size_t pick(AttrInterp m, size_t corner, size_t point, size_t face)
    {
      if (m == kAttrFaceVarying) return corner;
      if (m == kAttrVertex) return point;
      if (m == kAttrUniform) return face;
      return 0;
    }
    CornerAttrs at(size_t corner, size_t point, size_t face) const
    {
      CornerAttrs a; a.n = Vec3(0.0f); a.c = Vec3(1.0f); a.u = 0.0f; a.v = 0.0f;
      if (nMode != kAttrNone) { const size_t i = pick(nMode, corner, point, face); if (i < nrm->size()) { const GfVec3f& x = (*nrm)[i]; a.n = Vec3(x[0], x[1], x[2]); } }
      if (cMode != kAttrNone) { const size_t i = pick(cMode, corner, point, face); if (i < col->size()) { const GfVec3f& x = (*col)[i]; a.c = Vec3(x[0], x[1], x[2]); } }
      if (sMode != kAttrNone) { const size_t i = pick(sMode, corner, point, face); if (i < st->size()) { const GfVec2f& x = (*st)[i]; a.u = x[0]; a.v = x[1]; } }
      return a;
    }
  };

  // append one vertex (transformed into the prototype's space); returns its index
  uint32_t emitVertex(const VtVec3fArray& pts, size_t point, const CornerAttrs& a, const GfMatrix4d& local, bool ident)
  {
    const GfVec3f p = pts[point];
    const GfVec3d pw = ident ? GfVec3d(p) : local.Transform(GfVec3d(p));
    const uint32_t id = uint32_t(scene.vertices.size());
    scene.vertices.push_back(Vec3(float(pw[0]), float(pw[1]), float(pw[2])));
    Vec3 n = a.n;
    if (dot(n, n) > 1e-20f) {
      if (!ident) { const GfVec3d nw = local.TransformDir(GfVec3d(n.x, n.y, n.z)); n = Vec3(float(nw[0]), float(nw[1]), float(nw[2])); }
      n = normalize(n);
    }
    else n = Vec3(0.0f);
    scene.normals.push_back(n);
    scene.colors.push_back(a.c);
    scene.uvs.push_back(a.u); scene.uvs.push_back(a.v);
    return id;
  }

  // append one mesh (in the space given by 'local') to the soup being built
  // Append an already-tessellated soup into a prototype under a local transform.
  // The gprim path needs exactly what appendMesh does after it has turned a
  // polygon list into triangles, so this is that half of it and no more.
  void appendSoup(const TriangleSoup& soup, const GfMatrix4d& local, int materialId, ProtoRange& pr,
                  bool& anyNormals, bool& anyColors, bool& anyUVs)
  {
    if (soup.indices.empty()) return;
    if (opt.transformsOnly) { ++meshCount; return; }
    const bool ident = (local == GfMatrix4d(1.0));
    const uint32_t base = uint32_t(scene.vertices.size()) - uint32_t(pr.firstVertex);
    for (size_t i = 0; i < soup.points.size(); ++i) {
      const Vec3& v = soup.points[i];
      if (ident) {
        scene.vertices.push_back(v);
        scene.normals.push_back(soup.normals.empty() ? Vec3(0.0f) : soup.normals[i]);
      }
      else {
        const GfVec3d w = local.Transform(GfVec3d(v.x, v.y, v.z));
        scene.vertices.push_back(Vec3(float(w[0]), float(w[1]), float(w[2])));
        if (soup.normals.empty()) scene.normals.push_back(Vec3(0.0f));
        else {
          const Vec3& n = soup.normals[i];
          const GfVec3d wn = local.GetInverse().GetTranspose().TransformDir(GfVec3d(n.x, n.y, n.z));
          // static_cast, not float(...): float(wn[0]) parses as a declaration
          Vec3 nn(static_cast<float>(wn[0]), static_cast<float>(wn[1]), static_cast<float>(wn[2]));
          const float l = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
          scene.normals.push_back(l > 0.0f ? nn * (1.0f / l) : Vec3(0.0f));
        }
      }
      scene.colors.push_back(Vec3(1.0f, 1.0f, 1.0f));
      if (soup.uvs.size() == soup.points.size() * 2) {
        scene.uvs.push_back(soup.uvs[i * 2]);
        scene.uvs.push_back(soup.uvs[i * 2 + 1]);
      }
      else { scene.uvs.push_back(0.0f); scene.uvs.push_back(0.0f); }
    }
    for (size_t i = 0; i < soup.indices.size(); ++i)
      scene.indices.push_back(uint32_t(pr.firstVertex) + base + soup.indices[i]);
    const int tris = int(soup.indices.size() / 3);
    for (int t = 0; t < tris; ++t) scene.triMaterial.push_back(materialId);
    pr.numVertices += int(soup.points.size());
    pr.numTris += tris;
    if (!soup.normals.empty()) anyNormals = true;
    if (soup.uvs.size() == soup.points.size() * 2) anyUVs = true;
    (void)anyColors;
    ++meshCount;
  }

  void appendMesh(const UsdGeomMesh& mesh, const GfMatrix4d& local, int materialId, ProtoRange& pr,
                  bool& anyNormals, bool& anyColors, bool& anyUVs)
  {
    VtVec3fArray pts; mesh.GetPointsAttr().Get(&pts, time);
    VtIntArray fvc, fvi;
    mesh.GetFaceVertexCountsAttr().Get(&fvc, time);
    mesh.GetFaceVertexIndicesAttr().Get(&fvi, time);
    if (pts.empty() || fvc.empty() || fvi.empty()) return;
    if (opt.transformsOnly) { ++meshCount; return; }   // second motion pass: instances only
    TfToken orient; mesh.GetOrientationAttr().Get(&orient, time);
    const bool leftHanded = (orient == UsdGeomTokens->leftHanded);
    const bool ident = (local == GfMatrix4d(1.0));
    size_t npts = pts.size(), nfaces = fvc.size(), ncorners = fvi.size();

    // normals (attribute, else a 'normals' primvar)
    VtVec3fArray nrm; mesh.GetNormalsAttr().Get(&nrm, time);
    TfToken nInterp = mesh.GetNormalsInterpolation();
    if (nrm.empty()) {
      UsdGeomPrimvar pvn = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("normals"));
      if (pvn) { pvn.ComputeFlattened(&nrm, time); nInterp = pvn.GetInterpolation(); }
    }
    AttrInterp nMode = classifyAttr(nrm.size(), npts, nfaces, ncorners, nInterp);

    // displayColor
    VtVec3fArray col; TfToken cInterp;
    {
      UsdGeomPrimvar pvc = mesh.GetDisplayColorPrimvar();
      if (pvc) { pvc.ComputeFlattened(&col, time); cInterp = pvc.GetInterpolation(); }
    }
    AttrInterp cMode = classifyAttr(col.size(), npts, nfaces, ncorners, cInterp);

    // uvs: 'st' by convention, then the first float2 / texCoord2f primvar
    VtVec2fArray st; TfToken sInterp;
    {
      UsdGeomPrimvarsAPI api(mesh);
      UsdGeomPrimvar pvs = api.GetPrimvar(TfToken("st"));
      if (!pvs) pvs = api.GetPrimvar(TfToken("UVMap"));
      if (!pvs) pvs = api.GetPrimvar(TfToken("uv"));
      if (!pvs) {
        const std::vector<UsdGeomPrimvar> all = api.GetPrimvarsWithValues();
        for (size_t i = 0; i < all.size(); ++i) {
          const SdfValueTypeName tn = all[i].GetTypeName();
          if (tn == SdfValueTypeNames->TexCoord2fArray || tn == SdfValueTypeNames->Float2Array) { pvs = all[i]; break; }
        }
      }
      if (pvs) { pvs.ComputeFlattened(&st, time); sInterp = pvs.GetInterpolation(); }
    }
    AttrInterp sMode = classifyAttr(st.size(), npts, nfaces, ncorners, sInterp);

    bool needSplit = (nMode == kAttrFaceVarying || cMode == kAttrFaceVarying || sMode == kAttrFaceVarying
                      || nMode == kAttrUniform || cMode == kAttrUniform || sMode == kAttrUniform);
    CornerLookup look;
    look.nrm = &nrm; look.nMode = nMode;
    look.col = &col; look.cMode = cMode;
    look.st = &st;   look.sMode = sMode;

    // ---- subdivision surfaces -----------------------------------------------
    // Refine the POLYGON mesh here, before the per-corner splitting below:
    // subdividing the split soup would give the wrong limit surface and weld the
    // uv seams shut.  Attributes travel as per-corner arrays and refine linearly.
    if (opt.subdivLevels > 0) {
      TfToken scheme;
      mesh.GetSubdivisionSchemeAttr().Get(&scheme, time);
      if (scheme.IsEmpty()) scheme = UsdGeomTokens->catmullClark;      // USD's fallback value
      int levels = 0;
      bool bilinear = false;
      if (scheme == UsdGeomTokens->none) levels = 0;
      else if (scheme == UsdGeomTokens->bilinear) { levels = opt.subdivLevels; bilinear = true; }
      else if (scheme == UsdGeomTokens->catmullClark) levels = opt.subdivLevels;
      else {
        warn << "mesh " << mesh.GetPath().GetString() << ": subdivision scheme " << scheme.GetString()
             << " is not supported, rendering the control cage\n";
      }

      // keep the refinement from exploding: corners x 4^levels
      while (levels > 0 && double(ncorners) * std::pow(4.0, double(levels)) > 8.0e6) --levels;
      if (levels > 0 && levels < opt.subdivLevels)
        warn << "mesh " << mesh.GetPath().GetString() << ": subdivision reduced to " << levels
             << " level(s) to stay under 8M corners\n";

      if (levels > 0) {
        SubdivMesh sm;
        sm.points.resize(npts);
        for (size_t i = 0; i < npts; ++i) sm.points[i] = Vec3(pts[i][0], pts[i][1], pts[i][2]);
        sm.faceVertexCounts.assign(fvc.begin(), fvc.end());
        sm.faceVertexIndices.assign(fvi.begin(), fvi.end());
        if (cMode != kAttrNone) {
          sm.cornerColor.resize(ncorners);
          for (size_t c = 0; c < ncorners; ++c) sm.cornerColor[c] = Vec3(0.0f);
        }
        if (sMode != kAttrNone) sm.cornerUV.assign(ncorners * 2, 0.0f);
        if (cMode != kAttrNone || sMode != kAttrNone) {
          size_t c = 0;
          for (size_t f = 0; f < nfaces; ++f) {
            const int k = fvc[f];
            for (int i = 0; i < k && c < ncorners; ++i, ++c) {
              const CornerAttrs a = look.at(c, size_t(fvi[c]), f);
              if (cMode != kAttrNone) sm.cornerColor[c] = a.c;
              if (sMode != kAttrNone) { sm.cornerUV[c * 2] = a.u; sm.cornerUV[c * 2 + 1] = a.v; }
            }
          }
        }
        // semi-sharp creases and pinned corners
        {
          VtIntArray creaseIndices, creaseLengths, cornerIndices;
          VtFloatArray creaseSharpness, cornerSharpness;
          mesh.GetCreaseIndicesAttr().Get(&creaseIndices, time);
          mesh.GetCreaseLengthsAttr().Get(&creaseLengths, time);
          mesh.GetCreaseSharpnessesAttr().Get(&creaseSharpness, time);
          mesh.GetCornerIndicesAttr().Get(&cornerIndices, time);
          mesh.GetCornerSharpnessesAttr().Get(&cornerSharpness, time);
          size_t at = 0, run = 0;
          for (size_t g = 0; g < creaseLengths.size(); ++g) {
            const int len = creaseLengths[g];
            for (int e = 0; e + 1 < len; ++e) {
              if (at + size_t(e) + 1 >= creaseIndices.size()) break;
              // one sharpness per run, or one per edge
              float sh = 1e6f;
              if (creaseSharpness.size() == creaseLengths.size()) sh = creaseSharpness[g];
              else if (run < creaseSharpness.size()) sh = creaseSharpness[run];
              sm.creaseEdges.push_back(creaseIndices[at + size_t(e)]);
              sm.creaseEdges.push_back(creaseIndices[at + size_t(e) + 1]);
              sm.creaseSharpness.push_back(sh);
              ++run;
            }
            at += size_t(len > 0 ? len : 0);
          }
          for (size_t c = 0; c < cornerIndices.size(); ++c) {
            sm.cornerPoints.push_back(cornerIndices[c]);
            sm.cornerSharpness.push_back(c < cornerSharpness.size() ? cornerSharpness[c] : 1e6f);
          }
          if (!sm.creaseEdges.empty() || !sm.cornerPoints.empty()) ++creasedMeshes;
        }
        subdivide(sm, levels, bilinear);
        std::vector<Vec3> smoothN;
        computeSmoothNormals(sm, smoothN);

        // back into the arrays the emit loop below reads
        VtVec3fArray np(sm.points.size());
        for (size_t i = 0; i < sm.points.size(); ++i) np[i] = GfVec3f(sm.points[i].x, sm.points[i].y, sm.points[i].z);
        pts = np;
        fvc = VtIntArray(sm.faceVertexCounts.begin(), sm.faceVertexCounts.end());
        fvi = VtIntArray(sm.faceVertexIndices.begin(), sm.faceVertexIndices.end());
        // authored normals mean nothing on a subdivision surface (USD: only for
        // scheme "none"), so use the ones computed from the refined mesh
        nrm = VtVec3fArray(smoothN.size());
        for (size_t i = 0; i < smoothN.size(); ++i) nrm[i] = GfVec3f(smoothN[i].x, smoothN[i].y, smoothN[i].z);
        nMode = kAttrVertex;
        if (cMode != kAttrNone) {
          col = VtVec3fArray(sm.cornerColor.size());
          for (size_t i = 0; i < sm.cornerColor.size(); ++i) col[i] = GfVec3f(sm.cornerColor[i].x, sm.cornerColor[i].y, sm.cornerColor[i].z);
          cMode = kAttrFaceVarying;
        }
        if (sMode != kAttrNone) {
          st = VtVec2fArray(sm.cornerUV.size() / 2);
          for (size_t i = 0; i < st.size(); ++i) st[i] = GfVec2f(sm.cornerUV[i * 2], sm.cornerUV[i * 2 + 1]);
          sMode = kAttrFaceVarying;
        }
        npts = pts.size(); nfaces = fvc.size(); ncorners = fvi.size();
        look.nrm = &nrm; look.nMode = nMode;
        look.col = &col; look.cMode = cMode;
        look.st = &st;   look.sMode = sMode;
        needSplit = (cMode == kAttrFaceVarying || sMode == kAttrFaceVarying);
        ++subdividedMeshes;
      }
    }

    // ---- displacement ------------------------------------------------------------
    // UsdPreviewSurface's displacement moves the surface ALONG ITS NORMAL, and it
    // is done here by moving vertices rather than in shading - so the silhouette
    // and the shadow move with it, which is the whole difference between a
    // displacement and a bump.
    //
    // It happens AFTER subdivision on purpose: displacing a control cage moves
    // the cage, and the limit surface barely notices.
    if (materialId >= 0 && size_t(materialId) < scene.materials.size()
        && scene.materials[size_t(materialId)].displacement != 0.0f && !opt.transformsOnly) {
      const float disp = scene.materials[size_t(materialId)].displacement;
      // per-vertex normals to push along: the authored ones when they are per
      // point, otherwise smooth ones worked out from the topology - a mesh with
      // face normals or none has no direction to displace in.
      std::vector<Vec3> dn;
      if (nMode == kAttrVertex && nrm.size() == npts) {
        dn.resize(npts);
        for (size_t i = 0; i < npts; ++i) dn[i] = Vec3(nrm[i][0], nrm[i][1], nrm[i][2]);
      }
      else {
        SubdivMesh dm;
        dm.points.resize(npts);
        for (size_t i = 0; i < npts; ++i) dm.points[i] = Vec3(pts[i][0], pts[i][1], pts[i][2]);
        dm.faceVertexCounts.assign(fvc.begin(), fvc.end());
        dm.faceVertexIndices.assign(fvi.begin(), fvi.end());
        computeSmoothNormals(dm, dn);
      }
      if (dn.size() == npts) {
        VtVec3fArray moved(npts);
        for (size_t i = 0; i < npts; ++i)
          moved[i] = GfVec3f(pts[i][0] + dn[i].x * disp,
                             pts[i][1] + dn[i].y * disp,
                             pts[i][2] + dn[i].z * disp);
        pts = moved;
        ++displacedMeshes;
      }
    }

    const size_t firstOut = scene.vertices.size();
    std::vector<std::vector<std::pair<CornerAttrs, uint32_t> > > perPoint;   // split vertices, per point
    std::vector<uint32_t> simple;                                           // fast path: one vertex per point
    if (needSplit) perPoint.resize(npts);
    else {
      simple.resize(npts);
      for (size_t i = 0; i < npts; ++i) simple[i] = emitVertex(pts, i, look.at(0, i, 0), local, ident);
    }

    // triangulate (fan), winding flipped for leftHanded
    std::vector<uint32_t> corner;
    size_t v = 0;
    for (size_t f = 0; f < nfaces; ++f) {
      const int nv = fvc[f];
      if (nv >= 3 && v + size_t(nv) <= ncorners) {
        bool ok = true;
        for (int k = 0; k < nv; ++k) if (fvi[v + k] < 0 || size_t(fvi[v + k]) >= npts) { ok = false; break; }
        if (ok) {
          corner.resize(size_t(nv));
          for (int k = 0; k < nv; ++k) {
            const size_t ci = v + size_t(k), point = size_t(fvi[ci]);
            if (!needSplit) { corner[size_t(k)] = simple[point]; continue; }
            const CornerAttrs a = look.at(ci, point, f);
            std::vector<std::pair<CornerAttrs, uint32_t> >& bucket = perPoint[point];
            uint32_t id = 0xffffffffu;
            for (size_t q = 0; q < bucket.size(); ++q) if (bucket[q].first.same(a)) { id = bucket[q].second; break; }
            if (id == 0xffffffffu) { id = emitVertex(pts, point, a, local, ident); bucket.push_back(std::make_pair(a, id)); }
            corner[size_t(k)] = id;
          }
          for (int k = 1; k + 1 < nv; ++k) {
            uint32_t a = corner[0], b = corner[size_t(k)], c = corner[size_t(k) + 1];
            if (leftHanded) { const uint32_t t = b; b = c; c = t; }
            scene.indices.push_back(a); scene.indices.push_back(b); scene.indices.push_back(c);
            scene.triMaterial.push_back(materialId);
            ++pr.numTris;
          }
        }
      }
      v += size_t(nv > 0 ? nv : 0);
    }
    pr.numVertices += int(scene.vertices.size() - firstOut);
    anyNormals = anyNormals || (nMode != kAttrNone);
    anyColors = anyColors || (cMode != kAttrNone);
    anyUVs = anyUVs || (sMode != kAttrNone);
    ++meshCount;
  }

  // build a prototype soup from 'root' and everything below it, in root-relative space
  // (root's own transform excluded - the instance transforms carry it)
  int protoFor(const UsdPrim& root, bool includeRootXform)
  {
    const SdfPath key = root.GetPath();
    std::map<SdfPath, int>::iterator it = protoByPath.find(key);
    if (it != protoByPath.end()) return it->second;
    ProtoRange pr;
    pr.firstVertex = int(scene.vertices.size()); pr.numVertices = 0;
    pr.firstTri = int(scene.indices.size() / 3); pr.numTris = 0;
    pr.hasNormals = pr.hasUVs = pr.hasColors = 0; pr.cryptoId = 0.0f;
    bool anyN = false, anyC = false, anyUV = false, ownMaterial = false;
    UsdPrimRange range(root, UsdTraverseInstanceProxies());
    for (UsdPrimRange::iterator pit = range.begin(); pit != range.end(); ++pit) {
      const UsdPrim p = *pit;
      if (!renderable(p)) { pit.PruneChildren(); continue; }
      if (p.IsA<UsdGeomMesh>()) {
        bool reset = false;
        GfMatrix4d local = (p == root) ? GfMatrix4d(1.0) : xf.ComputeRelativeTransform(p, root, &reset);
        if (includeRootXform) local = local * xf.GetLocalTransformation(root, &reset);
        const int mid = materialFor(p);
        if (mid != defaultMaterial) ownMaterial = true;
        appendMesh(UsdGeomMesh(p), local, mid, pr, anyN, anyC, anyUV);
      }
      else if (isGprim(p)) {
        // a prototype built out of primitives rather than meshes - a instanced
        // "rock" that is really a Sphere is a perfectly ordinary thing to author
        bool reset = false;
        GfMatrix4d local = (p == root) ? GfMatrix4d(1.0) : xf.ComputeRelativeTransform(p, root, &reset);
        if (includeRootXform) local = local * xf.GetLocalTransformation(root, &reset);
        const int mid = materialFor(p);
        if (mid != defaultMaterial) ownMaterial = true;
        TriangleSoup soup;
        if (gprimSoup(p, soup)) { displaceSoup(soup, mid); appendSoup(soup, local, mid, pr, anyN, anyC, anyUV); }
      }
    }
    pr.hasNormals = anyN ? 1 : 0; pr.hasColors = anyC ? 1 : 0; pr.hasUVs = anyUV ? 1 : 0;
    if (pr.numTris == 0 && !opt.transformsOnly)
      warn << "prototype " << key.GetString() << " has no renderable geometry (a native instance shares the "
              "subtree BELOW the instanceable prim, so the mesh must be a child of it)\n";
    const int id = int(scene.protos.size());
    scene.protos.push_back(pr);
    scene.protoNames.push_back(key.GetString());
    protoHasMaterial.resize(size_t(id) + 1, 0);
    protoHasMaterial[size_t(id)] = ownMaterial ? 1 : 0;
    protoByPath[key] = id;
    return id;
  }

  // ---- the analytic gprims ---------------------------------------------------
  // A stage can say "a sphere of radius 2" with no points at all, and one written
  // by hand or by a DCC's primitives is full of them.  Worth knowing what this
  // avoids: a usda holding one made Nuke's own GeoImport return nothing at all,
  // taking the meshes in the same file down with it.
  //
  // True means the prim WAS a gprim, whether or not it produced triangles, so the
  // caller knows not to fall through and treat it as something else.
  bool gprimSoup(const UsdPrim& p, TriangleSoup& soup)
  {
    const int sides = std::max(6, 4 * std::max(1, opt.pointDetail));
    std::string axis = "Z";
    if (p.IsA<UsdGeomSphere>()) {
      double r = 1.0;
      UsdGeomSphere(p).GetRadiusAttr().Get(&r, time);
      gprimSphere(r, std::max(1, opt.pointDetail), soup);
      return true;
    }
    if (p.IsA<UsdGeomCube>()) {
      double sz = 2.0;
      UsdGeomCube(p).GetSizeAttr().Get(&sz, time);
      gprimCube(sz, soup);
      return true;
    }
    if (p.IsA<UsdGeomCylinder>()) {
      double r = 1.0, hgt = 2.0;
      TfToken ax;
      UsdGeomCylinder c(p);
      c.GetRadiusAttr().Get(&r, time);
      c.GetHeightAttr().Get(&hgt, time);
      if (c.GetAxisAttr().Get(&ax, time)) axis = ax.GetString();
      gprimCylinder(r, hgt, sides, soup);
      gprimAxis(soup, axis);
      return true;
    }
    if (p.IsA<UsdGeomCone>()) {
      double r = 1.0, hgt = 2.0;
      TfToken ax;
      UsdGeomCone c(p);
      c.GetRadiusAttr().Get(&r, time);
      c.GetHeightAttr().Get(&hgt, time);
      if (c.GetAxisAttr().Get(&ax, time)) axis = ax.GetString();
      gprimCone(r, hgt, sides, soup);
      gprimAxis(soup, axis);
      return true;
    }
    if (p.IsA<UsdGeomCapsule>()) {
      double r = 0.5, hgt = 1.0;
      TfToken ax;
      UsdGeomCapsule c(p);
      c.GetRadiusAttr().Get(&r, time);
      c.GetHeightAttr().Get(&hgt, time);
      if (c.GetAxisAttr().Get(&ax, time)) axis = ax.GetString();
      gprimCapsule(r, hgt, sides, std::max(2, opt.pointDetail * 2), soup);
      gprimAxis(soup, axis);
      return true;
    }
    return false;
  }

  // A tessellated gprim displaces the same way a mesh does - it arrives as a
  // soup rather than through appendMesh, and leaving it out would mean a Sphere
  // quietly ignored a displacement its neighbouring Mesh honoured.
  void displaceSoup(TriangleSoup& soup, int materialId)
  {
    if (materialId < 0 || size_t(materialId) >= scene.materials.size()) return;
    const float disp = scene.materials[size_t(materialId)].displacement;
    if (disp == 0.0f || soup.normals.size() != soup.points.size()) return;
    for (size_t i = 0; i < soup.points.size(); ++i)
      soup.points[i] = soup.points[i] + soup.normals[i] * disp;
    ++displacedMeshes;
  }

  static bool isGprim(const UsdPrim& p)
  {
    return p.IsA<UsdGeomSphere>() || p.IsA<UsdGeomCube>() || p.IsA<UsdGeomCylinder>()
        || p.IsA<UsdGeomCone>() || p.IsA<UsdGeomCapsule>();
  }

  // ---- points and curves -----------------------------------------------------
  // Append a triangle soup as a new prototype and return its id.
  int protoFromSoup(const TriangleSoup& soup, const std::string& name, int materialId, bool hasUVs)
  {
    if (soup.indices.empty()) return -1;
    ProtoRange pr;
    pr.firstVertex = int(scene.vertices.size());
    pr.numVertices = int(soup.points.size());
    pr.firstTri = int(scene.indices.size() / 3);
    pr.numTris = int(soup.indices.size() / 3);
    pr.hasNormals = 1; pr.hasUVs = hasUVs ? 1 : 0; pr.hasColors = 1; pr.cryptoId = 0.0f;
    const uint32_t base = uint32_t(scene.vertices.size());
    for (size_t i = 0; i < soup.points.size(); ++i) {
      scene.vertices.push_back(soup.points[i]);
      scene.normals.push_back(i < soup.normals.size() ? soup.normals[i] : Vec3(0.0f));
      scene.colors.push_back(Vec3(1.0f));
      scene.uvs.push_back(i * 2 < soup.uvs.size() ? soup.uvs[i * 2] : 0.0f);
      scene.uvs.push_back(i * 2 + 1 < soup.uvs.size() ? soup.uvs[i * 2 + 1] : 0.0f);
    }
    for (size_t i = 0; i < soup.indices.size(); ++i) scene.indices.push_back(base + soup.indices[i]);
    for (int t = 0; t < pr.numTris; ++t) scene.triMaterial.push_back(materialId);
    const int id = int(scene.protos.size());
    scene.protos.push_back(pr);
    scene.protoNames.push_back(name);
    protoHasMaterial.resize(size_t(id) + 1, 0);
    protoHasMaterial[size_t(id)] = (materialId != defaultMaterial) ? 1 : 0;
    return id;
  }

  // UsdGeomPoints: one sphere prototype shared by every point, one instance each,
  // so a million particles cost a transform each rather than a sphere each.
  void addPoints(const UsdGeomPoints& pts)
  {
    VtVec3fArray positions;
    // ComputePointsAtTime, not a plain Get: it applies the prim's VELOCITIES the
    // way UsdGeomPointInstancer does for its instances, which is the only way a
    // Points prim blurs when the points themselves are authored at one time and
    // the motion lives in the velocities. Falls back to the raw samples when
    // there are none, which is the ordinary case.
    const UsdTimeCode ptBase = opt.hasBaseTime ? UsdTimeCode(opt.baseTimeCode) : time;
    if (!UsdGeomPointBased(pts).ComputePointsAtTime(&positions, time, ptBase)
        || positions.empty()) {
      if (!pts.GetPointsAttr().Get(&positions, time) || positions.empty()) return;
    }
    VtFloatArray widths;
    pts.GetWidthsAttr().Get(&widths, time);
    VtVec3fArray colors;
    {
      UsdGeomPrimvar pvc = pts.GetDisplayColorPrimvar();
      if (pvc) pvc.ComputeFlattened(&colors, time);
    }
    VtInt64Array ids;
    pts.GetIdsAttr().Get(&ids, time);

    // ONE PROTOTYPE PER POINTS PRIM, named after it.  A shared "<point sphere>"
    // costs one small soup less but gives every Points prim in the scene the
    // same cryptomatte name, so a matte cannot pick one of them out - and, with
    // per-copy ids, two prims' particle 42 would hash to the same thing.  A
    // sphere is a few hundred triangles and scenes have one or two of these.
    int sphereProto = -1;
    {
      // The motion pass wants where the points ARE at this key, not what they
      // look like - so no sphere is built for it, and the id is a placeholder
      // nothing dereferences.
      if (opt.transformsOnly) sphereProto = 0;
      else {
        TriangleSoup sphere;
        buildUnitSphere(opt.pointDetail, sphere);
        sphereProto = protoFromSoup(sphere, pts.GetPrim().GetPath().GetString(), defaultMaterial, true);
        if (sphereProto < 0) return;
      }
    }
    const int matId = materialFor(pts.GetPrim());
    const GfMatrix4d m = xf.GetLocalToWorldTransform(pts.GetPrim());
    const GfVec3d sx = m.TransformDir(GfVec3d(1.0, 0.0, 0.0));
    const float scale = float(sx.GetLength());
    for (size_t i = 0; i < positions.size(); ++i) {
      float w = 1.0f;
      if (widths.size() == positions.size()) w = widths[i];
      else if (widths.size() == 1) w = widths[0];
      const float r = 0.5f * w * scale;
      if (!(r > 0.0f)) continue;
      const GfVec3d wp = m.Transform(GfVec3d(positions[i]));
      Instance inst;
      inst.xf = Xform::identity();
      inst.xf.m[0] = r; inst.xf.m[5] = r; inst.xf.m[10] = r;
      inst.xf.m[3] = float(wp[0]); inst.xf.m[7] = float(wp[1]); inst.xf.m[11] = float(wp[2]);
      inst.xf1 = inst.xf;
      inst.protoId = sphereProto;
      inst.instanceId = (i < ids.size()) ? int(ids[i]) : int(scene.instances.size());
      inst.materialOverride = (matId != defaultMaterial) ? matId : -1;
      if (colors.size() == positions.size()) { inst.hasColor = 1; inst.color = toV(colors[i]); }
      else if (colors.size() == 1) { inst.hasColor = 1; inst.color = toV(colors[0]); }
      scene.instances.push_back(inst);
    }
    pointCount += int(positions.size());
  }

  // UsdGeomBasisCurves: each curve becomes a tube, swept along the evaluated basis.
  void addCurves(const UsdGeomBasisCurves& curves)
  {
    VtVec3fArray cvs;
    VtIntArray counts;
    if (!curves.GetPointsAttr().Get(&cvs, time) || cvs.empty()) return;
    if (!curves.GetCurveVertexCountsAttr().Get(&counts, time) || counts.empty()) return;
    VtFloatArray widths;
    curves.GetWidthsAttr().Get(&widths, time);
    TfToken type, basisTok, wrap;
    curves.GetTypeAttr().Get(&type, time);
    curves.GetBasisAttr().Get(&basisTok, time);
    curves.GetWrapAttr().Get(&wrap, time);
    const bool cubic = (type != UsdGeomTokens->linear);
    int basisId = 0;                                        // bspline
    if (basisTok == UsdGeomTokens->catmullRom) basisId = 1;
    else if (basisTok == UsdGeomTokens->bezier) basisId = 2;
    if (wrap == UsdGeomTokens->periodic)
      warn << "BasisCurves " << curves.GetPath().GetString() << ": periodic wrap is treated as open\n";

    const GfMatrix4d m = xf.GetLocalToWorldTransform(curves.GetPrim());
    const GfVec3d sx = m.TransformDir(GfVec3d(1.0, 0.0, 0.0));
    const float scale = float(sx.GetLength());
    const int samples = (opt.curveSegments < 1) ? 1 : ((opt.curveSegments > 16) ? 16 : opt.curveSegments);

    TriangleSoup soup;
    size_t at = 0;
    int built = 0;
    for (size_t c = 0; c < counts.size(); ++c) {
      const int nCv = counts[c];
      if (nCv < 2 || at + size_t(nCv) > cvs.size()) { at += size_t(nCv > 0 ? nCv : 0); continue; }
      std::vector<Vec3> centres;
      std::vector<float> radii;
      const size_t first = at;
      // width lookup for a control vertex
      struct W {
        const VtFloatArray& widths; size_t total; size_t curveFirst; int nCv;
        float at(size_t cv) const
        {
          if (widths.size() == total) return widths[curveFirst + cv];
          if (widths.size() == 1) return widths[0];
          return 0.1f;
        }
      };
      const W w = { widths, cvs.size(), first, nCv };
      if (!cubic) {
        for (int i = 0; i < nCv; ++i) {
          const GfVec3d p = m.Transform(GfVec3d(cvs[first + size_t(i)]));
          centres.push_back(Vec3(float(p[0]), float(p[1]), float(p[2])));
          radii.push_back(0.5f * w.at(size_t(i)) * scale);
        }
      }
      else {
        // walk the spans the basis defines; four control points at a time
        const int step = (basisId == 2) ? 3 : 1;            // bezier spans step by three
        for (int i = 0; i + 3 < nCv; i += step) {
          for (int sIdx = 0; sIdx < samples; ++sIdx) {
            const float t = float(sIdx) / float(samples);
            const Vec3 p0(cvs[first + size_t(i)][0], cvs[first + size_t(i)][1], cvs[first + size_t(i)][2]);
            const Vec3 p1(cvs[first + size_t(i + 1)][0], cvs[first + size_t(i + 1)][1], cvs[first + size_t(i + 1)][2]);
            const Vec3 p2(cvs[first + size_t(i + 2)][0], cvs[first + size_t(i + 2)][1], cvs[first + size_t(i + 2)][2]);
            const Vec3 p3(cvs[first + size_t(i + 3)][0], cvs[first + size_t(i + 3)][1], cvs[first + size_t(i + 3)][2]);
            const Vec3 local = evalCubic(p0, p1, p2, p3, t, basisId);
            const GfVec3d wp = m.Transform(GfVec3d(local.x, local.y, local.z));
            centres.push_back(Vec3(float(wp[0]), float(wp[1]), float(wp[2])));
            const float wa = w.at(size_t(i + 1)), wb = w.at(size_t(i + 2));
            radii.push_back(0.5f * (wa + (wb - wa) * t) * scale);
          }
        }
        // close the last span
        if (!centres.empty()) {
          const int last = nCv - 1;
          const Vec3 p0(cvs[first + size_t(last - 3)][0], cvs[first + size_t(last - 3)][1], cvs[first + size_t(last - 3)][2]);
          const Vec3 p1(cvs[first + size_t(last - 2)][0], cvs[first + size_t(last - 2)][1], cvs[first + size_t(last - 2)][2]);
          const Vec3 p2(cvs[first + size_t(last - 1)][0], cvs[first + size_t(last - 1)][1], cvs[first + size_t(last - 1)][2]);
          const Vec3 p3(cvs[first + size_t(last)][0], cvs[first + size_t(last)][1], cvs[first + size_t(last)][2]);
          const Vec3 local = evalCubic(p0, p1, p2, p3, 1.0f, basisId);
          const GfVec3d wp = m.Transform(GfVec3d(local.x, local.y, local.z));
          centres.push_back(Vec3(float(wp[0]), float(wp[1]), float(wp[2])));
          radii.push_back(0.5f * w.at(size_t(last)) * scale);
        }
      }
      if (centres.size() >= 2) {
        if (!opt.transformsOnly) buildTube(centres, radii, opt.curveSides, soup);
        ++built;
      }
      at += size_t(nCv);
    }
    if (built == 0) return;
    const int matId = materialFor(curves.GetPrim());
    int pid = 0;
    if (!opt.transformsOnly) {
      pid = protoFromSoup(soup, curves.GetPath().GetString(), matId, true);
      if (pid < 0) return;
    }
    Instance inst;                                            // the tube is already in world space
    inst.protoId = pid;
    inst.instanceId = int(scene.instances.size());
    VtVec3fArray colors;
    {
      UsdGeomPrimvar pvc = curves.GetDisplayColorPrimvar();
      if (pvc && pvc.ComputeFlattened(&colors, time) && !colors.empty()) { inst.hasColor = 1; inst.color = toV(colors[0]); }
    }
    scene.instances.push_back(inst);
    curveCount += built;
  }

  // ---- volumes -------------------------------------------------------------------
#ifdef IR_HAS_VOLUMES
  // The last few grids read, kept so the same frame does not pay for itself
  // twice.
  //
  // A simulation frame is 100-250 MB and Hio takes a second or two over it, and
  // a viewer re-renders the same frame for every knob that moves - a light, a
  // sample count, a camera nudge. Without this each of those paid the full read
  // again, which is what "it holds a frame for a long time" was.
  //
  // Keyed by the RESOLVED path and grid, so a sequence naturally rolls through
  // it, and bounded by the same memory the loader is allowed for one grid.
  void addVolume(const UsdVolVolume& vol)
  {
    if (opt.transformsOnly) return;    // the motion pass wants transforms, not grids

    // Which grid?  UsdVolVolume names its fields through RELATIONSHIPS - one per
    // grid, "field:density" and friends - so this is a rel lookup, not an attr.
    UsdRelationship rel = vol.GetPrim().GetRelationship(TfToken("field:density"));
    if (!rel) {
      warn << "Volume " << vol.GetPath().GetString() << ": no field:density relationship\n";
      return;
    }
    SdfPathVector targets;
    rel.GetTargets(&targets);
    if (targets.empty()) return;
    UsdPrim fieldPrim = vol.GetPrim().GetStage()->GetPrimAtPath(targets[0]);
    if (!fieldPrim) return;
    UsdVolOpenVDBAsset asset(fieldPrim);
    if (!asset) {
      warn << "Volume " << vol.GetPath().GetString() << ": field:density is not an OpenVDBAsset\n";
      return;
    }
    SdfAssetPath file;
    asset.GetFilePathAttr().Get(&file, time);
    TfToken fieldName;
    asset.GetFieldNameAttr().Get(&fieldName, time);
    std::string path = file.GetAssetPath();
    if (path.find('%') == std::string::npos && path.find('#') == std::string::npos)
      { const std::string r = file.GetResolvedPath(); if (!r.empty()) path = r; }
    if (path.empty()) return;
    const std::string rawPath = path;
    path = resolveVdbFrame(path, int(std::floor(double(time.GetValue()) + 0.5)));
    if (fieldName.IsEmpty()) fieldName = TfToken("density");
    if (std::getenv("IR_VOL_PROBE")) {
      std::cerr << "IR_VOL: asked at time " << time << " -> " << path
                << " grid " << fieldName.GetString() << std::endl;
    }

    // ONE READ, cached, and turned into a GridRef.
    auto readGrid = [&](const std::string& gpath, const std::string& gname, GridRef& out) -> bool {
      return readVdbGrid(scene, gpath, gname, opt.maxVolumeMemoryMB, out, &volumeVoxels);
    };

    VolumeGrid g;
    // the node's own knob, folded in here so the kernel multiplies once
    {
      float ds = 1.0f;
      if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken("ir:densityScale"))) a.Get(&ds, time);
      g.densityScale = ds;
    }
    if (!readGrid(path, fieldName.GetString(), g.density[0])) {
      warn << "Volume " << vol.GetPath().GetString() << ": could not read grid '"
           << fieldName.GetString() << "' from " << path << "\n";
      return;
    }

    // ---- the shutter-close frame, which is the motion blur -----------------------
    // A simulation carries no velocity to advect by, so the only way to blur it is
    // to read the NEXT frame of the sequence and cross-fade.  Skipped when the
    // path does not change across the shutter - a single .vdb, or a shutter of
    // zero - so a still volume costs nothing.
    if (opt.volumeBlur) {
      const int f0 = int(std::floor(double(time.GetValue()) + 0.5));
      const int f1 = opt.volumeCloseFrame;
      if (f1 != f0) {
        const std::string pathClose = resolveVdbFrame(rawPath, f1);
        if (pathClose != path) readGrid(pathClose, fieldName.GetString(), g.density[1]);
      }
    }
    // ---- the emissive grids ------------------------------------------------------
    // Two slots, summed by the kernel: heat and flames read differently and a
    // simulation usually carries both.  Each keeps its own resolution and extent -
    // a temperature field is often coarser than the density it sits inside, and
    // borrowing the density grid's numbers would stretch it across the wrong box.
    {
      static const char* const kRel[kVolumeEmissive]   = { "field:temperature", "field:emission" };
      static const char* const kScale[kVolumeEmissive] = { "ir:temperatureScale", "ir:emissionScale" };
      static const char* const kColor[kVolumeEmissive] = { "ir:temperatureColor", "ir:emissionColor" };
      static const char* const kMode[kVolumeEmissive]  = { "ir:temperatureMode", "ir:emissionMode" };
      static const char* const kKmin[kVolumeEmissive]  = { "ir:temperatureKmin", "ir:emissionKmin" };
      static const char* const kKmax[kVolumeEmissive]  = { "ir:temperatureKmax", "ir:emissionKmax" };
      for (int si = 0; si < kVolumeEmissive; ++si) {
        float es = 0.0f;
        if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken(kScale[si]))) a.Get(&es, time);
        GfVec3f ec(1.0f, 1.0f, 1.0f);
        if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken(kColor[si]))) a.Get(&ec, time);
        g.emissionColor[si] = Vec3(ec[0], ec[1], ec[2]);
        g.emissionScale[si] = es;
        {
          int md = 0;
          if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken(kMode[si]))) a.Get(&md, time);
          g.emissionMode[si] = (md == 1) ? kEmitBlackbody : kEmitIntensity;
          float k0 = 0.0f, k1 = 0.0f;
          if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken(kKmin[si]))) a.Get(&k0, time);
          if (UsdAttribute a = vol.GetPrim().GetAttribute(TfToken(kKmax[si]))) a.Get(&k1, time);
          g.emitKmin[si] = k0; g.emitKmax[si] = k1;
        }
        if (es <= 0.0f) continue;
        UsdRelationship erel = vol.GetPrim().GetRelationship(TfToken(kRel[si]));
        if (!erel) continue;
        SdfPathVector etargets;
        erel.GetTargets(&etargets);
        if (etargets.empty()) continue;
        UsdVolOpenVDBAsset easset(vol.GetPrim().GetStage()->GetPrimAtPath(etargets[0]));
        if (!easset) continue;
        SdfAssetPath ef;
        easset.GetFilePathAttr().Get(&ef, time);
        TfToken en;
        easset.GetFieldNameAttr().Get(&en, time);
        std::string eraw = ef.GetAssetPath();
        if (eraw.find('%') == std::string::npos && eraw.find('#') == std::string::npos)
          { const std::string r = ef.GetResolvedPath(); if (!r.empty()) eraw = r; }
        if (eraw.empty()) { g.emissionScale[si] = 0.0f; continue; }
        const int ef0 = int(std::floor(double(time.GetValue()) + 0.5));
        const std::string epath = resolveVdbFrame(eraw, ef0);
        if (!readGrid(epath, en.GetString(), g.emissive[si][0])) {
          warn << "Volume " << vol.GetPath().GetString() << ": could not read '"
               << en.GetString() << "' from " << epath << "\n";
          g.emissionScale[si] = 0.0f;
          continue;
        }
        // A BLACKBODY GRID THAT IS NOT IN KELVIN LOSES ALL ITS VARIATION.
        // The lookup clamps to the ends of the table, so a 0..1 normalised flames
        // grid left in blackbody mode with no Kelvin range puts every voxel on the
        // same coolest colour - a flat red glow with none of the grid's shape in
        // it, which reads as a shading bug rather than as a wrong knob. The voxels
        // are already here, so the peak costs one pass and names the knob to reach
        // for.
        if (g.emissionMode[si] == kEmitBlackbody && g.emitKmax[si] <= g.emitKmin[si]) {
          const float mx = gridPeak(scene, g.emissive[si][0]);
          if (mx < kBlackbodyMinK)
            warn << "Volume " << vol.GetPath().GetString() << ": '" << en.GetString()
                 << "' is set to blackbody but peaks at " << mx << ", below the "
                 << kBlackbodyMinK << " K the blackbody table starts at, so every voxel "
                 << "clamps to the same coolest colour and the grid's variation is lost. "
                 << "Either set the Kelvin range on the node to remap it, or switch that "
                 << "slot to intensity.\n";
        }
        if (opt.volumeBlur) {
          const int ef1 = opt.volumeCloseFrame;
          if (ef1 != ef0) {
            const std::string eclose = resolveVdbFrame(eraw, ef1);
            if (eclose != epath) readGrid(eclose, en.GetString(), g.emissive[si][1]);
          }
        }
      }
    }

    scene.volumeNames.push_back(vol.GetPath().GetString());
    scene.volumes.push_back(g);
  }
#endif

  // ---- point instancers ----------------------------------------------------------
  void addPointInstancer(const UsdGeomPointInstancer& pi)
  {
    SdfPathVector targets;
    pi.GetPrototypesRel().GetTargets(&targets);
    VtIntArray protoIndices; pi.GetProtoIndicesAttr().Get(&protoIndices, time);
    VtMatrix4dArray xforms;
    const UsdTimeCode baseTime = opt.hasBaseTime ? UsdTimeCode(opt.baseTimeCode) : time;
    if (!pi.ComputeInstanceTransformsAtTime(&xforms, time, baseTime, UsdGeomPointInstancer::IncludeProtoXform, UsdGeomPointInstancer::ApplyMask)) {
      warn << "PointInstancer " << pi.GetPath().GetString() << ": could not compute instance transforms\n";
      return;
    }
    // one set of instance transforms per motion key
    std::vector<VtMatrix4dArray> keyXforms;
    std::vector<GfMatrix4d> keyInstancerXf;
    bool instancerMotion = false;
    if (std::getenv("IR_MOTION_PROBE")) {
      std::cerr << "IR_MOTION: instancer " << pi.GetPrim().GetPath().GetString()
                << " motion=" << (motion ? 1 : 0) << " numKeys=" << numKeys
                << " time=" << time << " baseTime=" << (opt.hasBaseTime ? opt.baseTimeCode : opt.timeCode)
                << " keyTimes";
      for (int k = 0; k < numKeys && size_t(k) < keyTimes.size(); ++k) std::cerr << " " << keyTimes[size_t(k)];
      std::cerr << " instances=" << xforms.size() << " velocities="
                << ((pi.GetVelocitiesAttr() && pi.GetVelocitiesAttr().HasAuthoredValue()) ? "authored" : "none")
                << std::endl;
    }
    if (motion) {
      keyXforms.resize(size_t(numKeys));
      keyInstancerXf.resize(size_t(numKeys));
      instancerMotion = true;
      for (int k = 0; k < numKeys && instancerMotion; ++k) {
        if (k == 0) { keyXforms[0] = xforms; }
        // baseTime is the frame being rendered, NOT the key's own time.  That is
        // what makes USD use the instancer's VELOCITIES: "we facilitate motion
        // blur for varying-topology particle streams by optionally allowing
        // per-instance velocities", and they only apply when the requested time
        // differs from the base.  Asking for each key at its own time instead
        // interpolates positions between samples, which is the thing that
        // cannot work when particles are born and die - the two samples do not
        // hold the same particles.  With velocities every key comes from the
        // SAME base sample, so the instance list is identical across the
        // shutter and there is nothing to match up.  Without them authored, USD
        // interpolates positions exactly as before.
        else if (!pi.ComputeInstanceTransformsAtTime(&keyXforms[size_t(k)], keyTimes[size_t(k)], time,
                                                     UsdGeomPointInstancer::IncludeProtoXform,
                                                     UsdGeomPointInstancer::ApplyMask)
                 || keyXforms[size_t(k)].size() != xforms.size()) {
          if (std::getenv("IR_MOTION_PROBE")) {
            std::cerr << "IR_MOTION: instancer " << pi.GetPrim().GetPath().GetString()
                      << " key " << k << " at t=" << keyTimes[size_t(k)]
                      << " base=" << time << " gave " << keyXforms[size_t(k)].size()
                      << " transforms, base has " << xforms.size()
                      << "; velocities "
                      << ((pi.GetVelocitiesAttr() && pi.GetVelocitiesAttr().HasAuthoredValue()) ? "authored" : "none")
                      << " -> no instancer motion" << std::endl;
          }
          instancerMotion = false;
          break;
        }
        keyInstancerXf[size_t(k)] = keyCaches[size_t(k)]->GetLocalToWorldTransform(pi.GetPrim());
      }
      if (!instancerMotion)
        warn << "PointInstancer " << pi.GetPath().GetString()
             << ": instance count changes across the shutter, motion blur skipped for it\n";
    }
    VtInt64Array ids; pi.GetIdsAttr().Get(&ids, time);
    // per-instance displayColor / displayOpacity (instance-rate primvars)
    VtVec3fArray icol;
    VtFloatArray iopa;
    {
      UsdGeomPrimvarsAPI api(pi.GetPrim());
      UsdGeomPrimvar pvc = api.GetPrimvar(TfToken("displayColor"));
      if (pvc) pvc.ComputeFlattened(&icol, time);
      UsdGeomPrimvar pvo = api.GetPrimvar(TfToken("displayOpacity"));
      if (pvo) pvo.ComputeFlattened(&iopa, time);
    }
    // mask may have removed instances: map back through ComputeMaskAtTime
    std::vector<bool> mask = pi.ComputeMaskAtTime(time);
    const GfMatrix4d instancerXf = xf.GetLocalToWorldTransform(pi.GetPrim());
    const GfMatrix4d instancerXfClose = motion ? xfClose.GetLocalToWorldTransform(pi.GetPrim()) : instancerXf;
    std::vector<int> protoIds(targets.size(), -1);
    for (size_t t = 0; t < targets.size(); ++t) {
      UsdPrim pp = pi.GetPrim().GetStage()->GetPrimAtPath(targets[t]);
      if (pp) protoIds[t] = protoFor(pp, false);
    }
    size_t xi = 0;
    for (size_t i = 0; i < protoIndices.size(); ++i) {
      if (!mask.empty() && i < mask.size() && !mask[i]) continue;
      if (xi >= xforms.size()) break;
      const GfMatrix4d m = xforms[xi] * instancerXf;
      const size_t xiThis = xi;
      ++xi;
      const int pidx = protoIndices[i];
      if (pidx < 0 || size_t(pidx) >= protoIds.size() || protoIds[size_t(pidx)] < 0) continue;
      Instance inst;
      inst.xf = toXform(m);
      inst.xf1 = inst.xf;
      if (instancerMotion) {
        std::vector<GfMatrix4d> keys;
        keys.resize(size_t(numKeys));
        for (int k = 0; k < numKeys; ++k) keys[size_t(k)] = keyXforms[size_t(k)][xiThis] * keyInstancerXf[size_t(k)];
        setMotionKeys(inst, keys);
      }
      inst.protoId = protoIds[size_t(pidx)];
      inst.instanceId = (i < ids.size()) ? int(ids[i]) : int(scene.instances.size());
      if (i < icol.size()) { inst.hasColor = 1; inst.color = toV(icol[i]); }
      else if (icol.size() == 1) { inst.hasColor = 1; inst.color = toV(icol[0]); }
      if (i < iopa.size()) inst.opacity = iopa[i];
      else if (iopa.size() == 1) inst.opacity = iopa[0];
      scene.instances.push_back(inst);
    }
    ++instancerCount;
  }

  // ---- dome importance sampling ---------------------------------------------------
  // the distribution itself lives in Dome.h, so the Hydra delegate samples a
  // dome light exactly the way this loader does
  void buildDomeDistribution() { ir::buildDomeDistribution(scene, domeTexture); }

  // ---- lights -------------------------------------------------------------------
  void addLight(const UsdPrim& prim)
  {
    UsdLuxLightAPI api(prim);
    Light L;
    float intensity = 1.0f, exposure = 0.0f; GfVec3f color(1.0f); bool normalizeFlag = false;
    api.GetIntensityAttr().Get(&intensity, time);
    api.GetExposureAttr().Get(&exposure, time);
    api.GetColorAttr().Get(&color, time);
    api.GetNormalizeAttr().Get(&normalizeFlag, time);
    float diffuseMul = 1.0f, specularMul = 1.0f;
    api.GetDiffuseAttr().Get(&diffuseMul, time);
    api.GetSpecularAttr().Get(&specularMul, time);
    const float scale = intensity * std::pow(2.0f, exposure);
    L.color = toV(color) * scale;
    L.intensity = scale;
    L.normalizePower = normalizeFlag ? 1 : 0;
    L.diffuseMul = diffuseMul;
    L.specularMul = specularMul;
    L.visibleToCamera = opt.lightsVisible ? 1 : 0;
    const GfMatrix4d m = xf.GetLocalToWorldTransform(prim);
    const GfVec3d pos = m.ExtractTranslation();
    L.position = Vec3(float(pos[0]), float(pos[1]), float(pos[2]));
    // lights emit along their local -Z
    const GfVec3d dir = m.TransformDir(GfVec3d(0.0, 0.0, -1.0));
    L.direction = normalize(Vec3(float(dir[0]), float(dir[1]), float(dir[2])));
    const GfVec3d ax = m.TransformDir(GfVec3d(1.0, 0.0, 0.0));
    const GfVec3d ay = m.TransformDir(GfVec3d(0.0, 1.0, 0.0));
    const float sx = float(ax.GetLength()), sy = float(ay.GetLength());

    // UsdLuxShapingAPI: cone (a spot light), focus
    if (prim.HasAPI<UsdLuxShapingAPI>()) {
      UsdLuxShapingAPI shaping(prim);
      float coneAngle = 90.0f, coneSoftness = 0.0f, focus = 0.0f;
      GfVec3f focusTint(0.0f);
      if (shaping.GetShapingConeAngleAttr() && shaping.GetShapingConeAngleAttr().HasAuthoredValue()) {
        shaping.GetShapingConeAngleAttr().Get(&coneAngle, time);
        shaping.GetShapingConeSoftnessAttr().Get(&coneSoftness, time);
        const float outer = coneAngle * (3.14159265f / 180.0f);
        const float inner = outer * (1.0f - irClamp(coneSoftness, 0.0f, 1.0f));
        L.coneCos = std::cos(irMin(outer, 3.14159265f));
        L.coneCosInner = std::cos(irMin(inner, 3.14159265f));
      }
      shaping.GetShapingFocusAttr().Get(&focus, time);
      shaping.GetShapingFocusTintAttr().Get(&focusTint, time);
      L.focus = irMax(focus, 0.0f);
      L.focusTint = toV(focusTint);
      if (shaping.GetShapingIesFileAttr()) {
        SdfAssetPath ies;
        if (shaping.GetShapingIesFileAttr().Get(&ies, time) && !ies.GetAssetPath().empty()) {
          std::string ip = ies.GetResolvedPath();
          if (ip.empty()) ip = ies.GetAssetPath();
          // one table per FILE: a corridor of forty downlights is one profile
          std::map<std::string, int>::iterator hit = iesByPath.find(ip);
          if (hit != iesByPath.end()) L.iesProfile = hit->second;
          else {
            IesProfile prof;
            if (iesLoad(ip, prof)) {
              L.iesProfile = int(scene.ies.size() / (size_t(kIesVRes) * size_t(kIesHRes)));
              scene.ies.insert(scene.ies.end(), prof.table.begin(), prof.table.end());
              iesByPath[ip] = L.iesProfile;
            }
            else {
              iesByPath[ip] = -1;
              warn << "light " << prim.GetPath().GetString() << ": could not read the IES "
                   << "profile " << ip << " - it is read as IESNA LM-63, and one carrying a "
                   << "TILT table is not handled.\n";
            }
          }
        }
      }
    }
    // UsdLuxShadowAPI
    if (prim.HasAPI<UsdLuxShadowAPI>()) {
      UsdLuxShadowAPI shadow(prim);
      bool enable = true; GfVec3f shadowColor(0.0f);
      shadow.GetShadowEnableAttr().Get(&enable, time);
      shadow.GetShadowColorAttr().Get(&shadowColor, time);
      L.shadowEnable = enable ? 1 : 0;
      L.shadowColor = toV(shadowColor);
    }

    if (prim.IsA<UsdLuxDistantLight>()) {
      L.type = kLightDistant;
      float ang = 0.53f; UsdLuxDistantLight(prim).GetAngleAttr().Get(&ang, time); L.angle = ang;
      L.area = 1.0f;
    }
    else if (prim.IsA<UsdLuxSphereLight>()) {
      L.type = kLightSphere;
      float r = 0.5f; UsdLuxSphereLight(prim).GetRadiusAttr().Get(&r, time);
      L.radius = r * sx;
      bool treatAsPoint = false; UsdLuxSphereLight(prim).GetTreatAsPointAttr().Get(&treatAsPoint, time);
      L.area = 4.0f * 3.14159265f * L.radius * L.radius;
      if (treatAsPoint || L.radius <= 1e-5f) { L.type = kLightPoint; L.area = 1.0f; }
    }
    else if (prim.IsA<UsdLuxRectLight>()) {
      L.type = kLightRect;
      float w = 1.0f, h = 1.0f;
      UsdLuxRectLight(prim).GetWidthAttr().Get(&w, time); UsdLuxRectLight(prim).GetHeightAttr().Get(&h, time);
      const GfVec3d ux = m.TransformDir(GfVec3d(0.5 * w, 0.0, 0.0));
      const GfVec3d vy = m.TransformDir(GfVec3d(0.0, 0.5 * h, 0.0));
      L.u = Vec3(float(ux[0]), float(ux[1]), float(ux[2]));
      L.v = Vec3(float(vy[0]), float(vy[1]), float(vy[2]));
      L.area = 4.0f * length(L.u) * length(L.v);
      SdfAssetPath asset;
      UsdLuxRectLight rect(prim);
      if (opt.textures && rect.GetTextureFileAttr() && rect.GetTextureFileAttr().Get(&asset, time)) {
        std::string path = assetPathFor(asset);
        if (!path.empty()) L.texture = loadTexture(path, kColorAuto, kWrapClamp, kWrapClamp);
      }
    }
    else if (prim.IsA<UsdLuxDiskLight>()) {
      L.type = kLightDisk;
      float r = 0.5f; UsdLuxDiskLight(prim).GetRadiusAttr().Get(&r, time);
      L.radius = r * sx;
      L.u = normalize(Vec3(float(ax[0]), float(ax[1]), float(ax[2])));
      L.v = normalize(Vec3(float(ay[0]), float(ay[1]), float(ay[2])));
      L.area = 3.14159265f * L.radius * L.radius;
    }
    else if (prim.IsA<UsdLuxCylinderLight>()) {
      L.type = kLightCylinder;
      float r = 0.5f, len = 1.0f;
      UsdLuxCylinderLight(prim).GetRadiusAttr().Get(&r, time);
      UsdLuxCylinderLight(prim).GetLengthAttr().Get(&len, time);
      L.radius = r * sy;
      L.length = 0.5f * len * sx;                        // the cylinder runs along its local X
      L.u = normalize(Vec3(float(ax[0]), float(ax[1]), float(ax[2])));
      L.v = normalize(Vec3(float(ay[0]), float(ay[1]), float(ay[2])));
      L.area = 2.0f * 3.14159265f * L.radius * (2.0f * L.length);
    }
    else if (prim.IsA<UsdLuxDomeLight>()) {
      L.type = kLightDome;
      L.area = 1.0f;
      // the lat-long lookup needs the light's own frame
      const GfVec3d az = m.TransformDir(GfVec3d(0.0, 0.0, 1.0));
      L.lx = normalize(Vec3(float(ax[0]), float(ax[1]), float(ax[2])));
      L.ly = normalize(Vec3(float(ay[0]), float(ay[1]), float(ay[2])));
      L.lz = normalize(Vec3(float(az[0]), float(az[1]), float(az[2])));
      SdfAssetPath asset;
      UsdLuxDomeLight dome(prim);
      if (!opt.transformsOnly && dome.GetTextureFileAttr() && dome.GetTextureFileAttr().Get(&asset, time)) {
        std::string path = assetPathFor(asset);
        if (!path.empty()) {
          TfToken fmt;
          if (dome.GetTextureFormatAttr()) dome.GetTextureFormatAttr().Get(&fmt, time);
          if (fmt != TfToken() && fmt != TfToken("latlong") && fmt != TfToken("automatic"))
            warn << "dome light " << prim.GetPath().GetString() << ": texture:format " << fmt.GetString()
                 << " is not supported, treating the image as latlong\n";
          L.texture = loadTexture(path, kColorAuto, kWrapRepeat, kWrapClamp);
          domeTexture = L.texture;
        }
      }
    }
    else {
      return;   // unsupported light type
    }
    // normalize: the light's power stays put when its size changes, so the
    // radiance is the intensity spread over the emitting area
    if (L.normalizePower && L.area > 1e-9f && L.type != kLightDistant && L.type != kLightDome && L.type != kLightPoint)
      L.color = L.color * (1.0f / L.area);
    scene.lights.push_back(L);
    scene.lightNames.push_back(prim.GetPath().GetString());
    ++lightCount;
  }

  // ---- camera -------------------------------------------------------------------
  void setCamera(const UsdGeomCamera& cam)
  {
    const GfCamera gc = cam.GetCamera(time);
    Camera& c = scene.camera;
    c.camToWorld = toXform(gc.GetTransform());
    const float focal = gc.GetFocalLength();          // mm
    const float hap = gc.GetHorizontalAperture();     // mm
    const float vap = gc.GetVerticalAperture();
    c.orthographic = (gc.GetProjection() == GfCamera::Orthographic) ? 1 : 0;
    if (c.orthographic) { c.orthoHalfW = hap * 0.5f * 0.1f; c.orthoHalfH = vap * 0.5f * 0.1f; }   // apertures in tenths of scene units for ortho (GfCamera convention)
    c.tanHalfFovX = (focal > 1e-6f) ? (0.5f * hap / focal) : 0.5f;
    c.tanHalfFovY = (focal > 1e-6f) ? (0.5f * vap / focal) : 0.5f;
    const GfRange1f clip = gc.GetClippingRange();
    c.nearClip = clip.GetMin(); c.farClip = clip.GetMax();
    scene.hasCamera = true;
  }
};

} // namespace

bool loadStage(const UsdStageRefPtr& stage, const LoaderOptions& opt, Scene& scene)
{
  scene = Scene();
  if (!stage) { scene.warnings = "no stage"; return false; }
  Builder b(scene, opt);
  UsdPrim cameraPrim;
  if (!opt.cameraPath.empty()) {
    UsdPrim p = stage->GetPrimAtPath(SdfPath(opt.cameraPath));
    if (p && p.IsA<UsdGeomCamera>()) cameraPrim = p;
  }
  UsdPrimRange range = stage->Traverse();   // no instance proxies: native instances are handled explicitly
  for (UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
    const UsdPrim prim = *it;
    if (!cameraPrim && prim.IsA<UsdGeomCamera>()) cameraPrim = prim;
    if (prim.HasAPI<UsdLuxLightAPI>()) { if (b.renderable(prim)) b.addLight(prim); continue; }
    if (prim.IsA<UsdGeomCamera>()) continue;
    if (!b.renderable(prim)) { it.PruneChildren(); continue; }

    // A viewport preview for a volume - a box, or points standing in for fog.
    // It exists to be LOOKED at, not rendered, and the guide purpose cannot
    // express that on its own because the Viewer hides guides.
    //
    // BEFORE EVERY OTHER TEST.  Placed lower down it sat below the Points
    // dispatch, which had already turned 1728 fog points into 1728 spheres in
    // the render.
    if (prim.HasAttribute(TfToken("ir:preview"))) { it.PruneChildren(); continue; }
    if (prim.IsInstance()) {
      // native (scenegraph) instance: the prototype is shared, the instance prim carries the transform
      const UsdPrim proto = prim.GetPrototype();
      if (proto) {
        const int pid = b.protoFor(proto, false);
        Instance inst;
        inst.xf = toXform(b.xf.GetLocalToWorldTransform(prim));
        inst.xf1 = inst.xf;
        if (b.motion) {
          std::vector<GfMatrix4d> keys;
          keys.resize(size_t(b.numKeys));
          for (int k = 0; k < b.numKeys; ++k) keys[size_t(k)] = b.keyCaches[size_t(k)]->GetLocalToWorldTransform(prim);
          b.setMotionKeys(inst, keys);
        }
        inst.protoId = pid;
        inst.instanceId = int(scene.instances.size());
        // per-instance displayColor / displayOpacity authored on the instance root
        UsdGeomPrimvarsAPI api(prim);
        VtVec3fArray icol;
        UsdGeomPrimvar pvc = api.GetPrimvar(TfToken("displayColor"));
        if (pvc && pvc.ComputeFlattened(&icol, b.time) && !icol.empty()) { inst.hasColor = 1; inst.color = toV(icol[0]); }
        VtFloatArray iopa;
        UsdGeomPrimvar pvo = api.GetPrimvar(TfToken("displayOpacity"));
        if (pvo && pvo.ComputeFlattened(&iopa, b.time) && !iopa.empty()) inst.opacity = iopa[0];
        // a material bound on the instance prim cannot reach into the shared
        // prototype, so apply it here - but never over a binding the prototype's
        // own meshes carry (it would override every triangle of the instance)
        if (pid >= 0 && size_t(pid) < b.protoHasMaterial.size() && !b.protoHasMaterial[size_t(pid)]) {
          const int mid = b.materialIfBound(prim);
          if (mid >= 0) inst.materialOverride = mid;
        }
        scene.instances.push_back(inst);
        ++b.nativeInstanceCount;
      }
      it.PruneChildren();
      continue;
    }
    if (prim.IsA<UsdGeomPoints>()) {
      // Points and curves are visited by the MOTION pass too.  They were skipped
      // as "geometry", but each one puts INSTANCES in the scene - a sphere per
      // point, one tube per curve set - and the motion pass compares its instance
      // list against the scene's by length.  Skipping them made the key list
      // short, so a scene holding a Points prim next to anything else silently
      // rendered with no motion blur AT ALL: measured on a particle scene, 11550
      // instances at each key against 23100 in the scene, and the whole frame
      // came out sharp with the shutter wide open.
      b.addPoints(UsdGeomPoints(prim));
      continue;
    }
    if (prim.IsA<UsdGeomBasisCurves>()) {
      b.addCurves(UsdGeomBasisCurves(prim));
      continue;
    }
#ifdef IR_HAS_VOLUMES
    if (prim.IsA<UsdVolVolume>()) {
      b.addVolume(UsdVolVolume(prim));
      it.PruneChildren();        // the OpenVDBAsset below is not drawn on its own
      continue;
    }
#endif
    if (prim.IsA<UsdGeomPointInstancer>()) {
      b.addPointInstancer(UsdGeomPointInstancer(prim));
      it.PruneChildren();   // the prototypes below are only drawn through the instancer
      continue;
    }
    if (Builder::isGprim(prim)) {
      // an analytic gprim: tessellated into a prototype of its own, then placed
      // exactly the way a mesh is, so materials, motion and cryptomatte all
      // follow without another special case
      TriangleSoup soup;
      if (b.gprimSoup(prim, soup) && !soup.indices.empty()) {
        const int mid = b.materialFor(prim);
        b.displaceSoup(soup, mid);
        const int pid = b.protoFromSoup(soup, prim.GetPath().GetString(), mid, true);
        if (pid >= 0) {
          Instance inst;
          inst.xf = toXform(b.xf.GetLocalToWorldTransform(prim));
          inst.xf1 = inst.xf;
          if (b.motion) {
            std::vector<GfMatrix4d> keys;
            keys.resize(size_t(b.numKeys));
            for (int k = 0; k < b.numKeys; ++k)
              keys[size_t(k)] = b.keyCaches[size_t(k)]->GetLocalToWorldTransform(prim);
            b.setMotionKeys(inst, keys);
          }
          inst.protoId = pid;
          inst.instanceId = int(scene.instances.size());
          scene.instances.push_back(inst);
        }
      }
      continue;
    }
    if (prim.IsA<UsdGeomMesh>()) {
      // a plain mesh: prototype in its own local space + one world instance
      ProtoRange pr;
      pr.firstVertex = int(scene.vertices.size()); pr.numVertices = 0;
      pr.firstTri = int(scene.indices.size() / 3); pr.numTris = 0;
      pr.hasNormals = pr.hasUVs = pr.hasColors = 0; pr.cryptoId = 0.0f;
      bool anyN = false, anyC = false, anyUV = false;
      b.appendMesh(UsdGeomMesh(prim), GfMatrix4d(1.0), b.materialFor(prim), pr, anyN, anyC, anyUV);
      pr.hasNormals = anyN ? 1 : 0; pr.hasColors = anyC ? 1 : 0; pr.hasUVs = anyUV ? 1 : 0;
      if (pr.numTris > 0 || b.opt.transformsOnly) {
        const int pid = int(scene.protos.size());
        scene.protos.push_back(pr);
        scene.protoNames.push_back(prim.GetPath().GetString());
        Instance inst;
        inst.xf = toXform(b.xf.GetLocalToWorldTransform(prim));
        inst.xf1 = inst.xf;
        if (b.motion) {
          std::vector<GfMatrix4d> keys;
          keys.resize(size_t(b.numKeys));
          for (int k = 0; k < b.numKeys; ++k) keys[size_t(k)] = b.keyCaches[size_t(k)]->GetLocalToWorldTransform(prim);
          b.setMotionKeys(inst, keys);
        }
        inst.protoId = pid;
        inst.instanceId = int(scene.instances.size());
        scene.instances.push_back(inst);
      }
      continue;
    }
  }
  if (cameraPrim) b.setCamera(UsdGeomCamera(cameraPrim));

  // every instance owns motionKeyCount keys, including the ones that never moved
  if (scene.motionKeyCount >= 2) {
    for (size_t i = 0; i < scene.instances.size(); ++i) {
      Instance& in = scene.instances[i];
      if (in.firstKey < 0) {
        in.firstKey = int(scene.motionKeys.size());
        for (int k = 0; k < scene.motionKeyCount; ++k) scene.motionKeys.push_back(in.xf);
      }
    }
  }
  else {
    scene.motionKeyCount = 0;
    scene.motionKeys.clear();
    for (size_t i = 0; i < scene.instances.size(); ++i) scene.instances[i].firstKey = 0;
  }

  b.buildDomeDistribution();

  std::ostringstream info;
  info << "InstanceRender scene: " << b.meshCount << " mesh(es) in " << scene.protos.size() << " prototype(s), "
       << scene.numTriangles() << " unique triangles; " << scene.instances.size() << " instance(s) ("
       << b.instancerCount << " PointInstancer(s), " << b.nativeInstanceCount << " native instance(s)) = "
       << scene.expandedTriangles() / 1e6 << "M rendered triangles; " << scene.materials.size() - 1 << " material(s), "
       << b.lightCount << " light(s)" << (scene.hasCamera ? ", stage camera" : ", no stage camera");
  if (b.pointCount) info << ", " << b.pointCount << " point(s) as spheres";
  // A dense grid grows as the cube of its resolution, so the size is said out
  // loud rather than quietly eaten - the same reason the cryptomatte manifest
  // reports its own.
  if (!scene.volumes.empty()) {
    info << ", " << scene.volumes.size() << " volume(s) ";
    for (size_t i = 0; i < scene.volumes.size(); ++i) {
      const VolumeGrid& g = scene.volumes[i];
      if (i) info << " + ";
      info << g.density[0].nx << "x" << g.density[0].ny << "x" << g.density[0].nz;
      if (g.density[1].valid()) info << " (blurred)";
    }
    info << " = " << (b.volumeVoxels * sizeof(float)) / (1024 * 1024) << " MB";
    const GridRef& r0 = scene.volumes[0].density[0];
    info << ", extent (" << r0.bmin.x << ", " << r0.bmin.y << ", " << r0.bmin.z
         << ")..(" << r0.bmax.x << ", " << r0.bmax.y << ", " << r0.bmax.z << ")";
  }
  if (b.curveCount) info << ", " << b.curveCount << " curve(s) as tubes";
  if (b.movingInstances) {
    double travel = 0.0;
    for (size_t i = 0; i < scene.instances.size(); ++i) {
      const Instance& in = scene.instances[i];
      const double dx = in.xf1.m[3] - in.xf.m[3], dy = in.xf1.m[7] - in.xf.m[7], dz = in.xf1.m[11] - in.xf.m[11];
      const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d > travel) travel = d;
    }
    info << ", motion blur on " << b.movingInstances << " instance(s), max travel " << travel << " (stage time samples)";
  }
  if (b.subdividedMeshes) {
    info << ", " << b.subdividedMeshes << " subdivided mesh(es) x" << opt.subdivLevels;
    if (b.creasedMeshes) info << " (" << b.creasedMeshes << " creased)";
  }
  if (b.textureCount) {
    info << ", " << b.textureCount << " texture(s) (" << (scene.texels.size() * sizeof(float)) / (1024 * 1024) << " MB)";
    if (scene.domeW) info << ", dome importance map " << scene.domeW << "x" << scene.domeH;
  }
  if (b.mipLevels) info << ", " << b.mipLevels << " mip level(s)";
  if (b.udimSets) info << ", " << b.udimSets << " UDIM set(s) (" << b.udimTilesLoaded << " tiles)";
  if (b.textureFailures) info << ", " << b.textureFailures << " texture(s) failed to load";
  if (b.skippedInvisible) info << "; skipped " << b.skippedInvisible << " invisible";
  if (b.skippedGuide) info << ", " << b.skippedGuide << " guide/proxy";
  // Nuke's own material ops author their shaders under /materials/NukeMaterialOps
  // and cannot be asked for a second time sample (see loadScene), so the node
  // needs to know they are there before it asks.
  {
    UsdPrim mats = stage->GetPrimAtPath(SdfPath("/materials/NukeMaterialOps"));
    scene.hasNukeMaterialOps = bool(mats);
  }
  if (envFlag("IR_MOTION_PROBE")) info << motionProbe(stage, opt.timeCode);
#ifdef IR_HAS_VOLUMES
  // IR_VDB_PROBE=<file.vdb> answers the question the whole volume front end
  // rests on: can this process read a VDB grid at all?  Hio's OpenVDB reader is
  // a PLUGIN found through USD's registry, so the library existing on disk is not
  // enough - it has to be discoverable from inside Nuke.
  {
    const std::string path = envString("IR_VDB_PROBE");
    if (!path.empty()) {
      const std::string envGrid = envString("IR_VDB_GRID");
      const std::string probeGrid = envGrid.empty() ? std::string("density") : envGrid;
      std::ostringstream o;
      HioFieldTextureDataSharedPtr d =
        HioFieldTextureData::New(path, probeGrid, 0, std::string(), size_t(512) * 1024 * 1024);
      o << "vdb probe: " << path << "; New() " << (d ? "ok" : "NULL");
      if (d) {
        const bool read = d->Read();
        o << "; Read() " << (read ? "ok" : "FAILED")
          << "; dims " << d->ResizedWidth() << "x" << d->ResizedHeight() << "x" << d->ResizedDepth()
          << "; format " << int(d->GetFormat())
          << "; raw " << (d->HasRawBuffer() ? "yes" : "no");
        const GfRange3d r = d->GetBoundingBox().ComputeAlignedRange();
        const GfMatrix4d bm = d->GetBoundingBox().GetMatrix();
        o << "; bboxMatrix row3 (" << bm[3][0] << "," << bm[3][1] << "," << bm[3][2] << ")"
          << "; scale (" << bm[0][0] << "," << bm[1][1] << "," << bm[2][2] << ")";
        const GfRange3d rr = d->GetBoundingBox().GetRange();
        o << "; rawRange (" << rr.GetMin()[0] << "," << rr.GetMin()[1] << "," << rr.GetMin()[2] << ")..("
          << rr.GetMax()[0] << "," << rr.GetMax()[1] << "," << rr.GetMax()[2] << ")";
        o << "; bbox (" << r.GetMin()[0] << "," << r.GetMin()[1] << "," << r.GetMin()[2] << ")..("
          << r.GetMax()[0] << "," << r.GetMax()[1] << "," << r.GetMax()[2] << ")";
        if (read && d->HasRawBuffer()) {
          const float* fv = reinterpret_cast<const float*>(d->GetRawBuffer());
          const size_t nv = size_t(d->ResizedWidth()) * size_t(d->ResizedHeight()) * size_t(d->ResizedDepth());
          double sum = 0.0; float mx = 0.0f; size_t nz = 0;
          for (size_t i = 0; i < nv; ++i) { sum += fv[i]; if (fv[i] > mx) mx = fv[i]; if (fv[i] > 0.0f) ++nz; }
          o << "; voxels " << nv << ", non-zero " << nz << ", max " << mx
            << ", mean " << (nv ? sum / double(nv) : 0.0);
        }
      }
      ir::trace(o.str());
      std::cerr << o.str() << std::endl;
    }
  }
#endif

  // IR_STAGE_PROBE=1 lists what the stage actually contains, types and all -
  // the quickest way to find out whether a prim kind survives Nuke's engine
  if (envFlag("IR_STAGE_PROBE")) {
    std::ostringstream sp;
    sp << "\nstage probe:";
    int n = 0;
    UsdPrimRange all = stage->TraverseAll();
    for (UsdPrimRange::iterator it = all.begin(); it != all.end() && n < 40; ++it, ++n) {
      const UsdPrim p = *it;
      sp << "\n  " << p.GetPath().GetString() << " [" << p.GetTypeName().GetString() << "]";
      if (p.IsInstance()) sp << " (instance)";
      const std::vector<UsdAttribute> attrs = p.GetAttributes();
      int shown = 0;
      for (size_t a = 0; a < attrs.size() && shown < 4; ++a) {
        if (!attrs[a].HasAuthoredValue()) continue;
        sp << (shown ? ", " : " attrs: ") << attrs[a].GetName().GetString();
        ++shown;
      }
    }
    info << sp.str();
  }
  scene.info = info.str();
  scene.warnings = b.warn.str();
  if (opt.maxExpandedTriangles > 0.0 && scene.expandedTriangles() > opt.maxExpandedTriangles) {
    std::ostringstream w;
    w << "scene exceeds the triangle guard: " << scene.expandedTriangles() / 1e6 << "M rendered triangles > "
      << opt.maxExpandedTriangles / 1e6 << "M";
    scene.warnings += w.str();
    return false;
  }
  return true;
}

} // namespace ir
