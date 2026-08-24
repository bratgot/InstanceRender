// InstanceRender - HdIrLight.cpp
// Hydra's lights and materials, turned into the ir::Light and ir::Material the
// kernel already understands.  The mapping is nearly the same one the Nuke node
// does from UsdLux and UsdPreviewSurface, because Hydra is handing over the same
// data under the same names.
// Strict ASCII.
#include "HdIrLight.h"
#include <mutex>
#include <map>

#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/usd/sdf/assetPath.h>

#include "ir/Texture.h"
#include "ir/NukeOpImage.h"
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>

#include <cmath>
#include <cstdlib>
#include <fstream>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

void hlogLight(const std::string& m)
{
  if (const char* dbg = std::getenv("IR_HYDRA_LOG")) {
    std::ofstream f(dbg, std::ios::app);
    f << m << std::endl;
  }
}

} // namespace

namespace {

float paramFloat(HdSceneDelegate* d, SdfPath const& id, TfToken const& name, float fallback)
{
  const VtValue v = d->GetLightParamValue(id, name);
  if (v.IsHolding<float>()) return v.UncheckedGet<float>();
  if (v.IsHolding<double>()) return float(v.UncheckedGet<double>());
  if (v.IsHolding<int>()) return float(v.UncheckedGet<int>());
  return fallback;
}

bool paramBool(HdSceneDelegate* d, SdfPath const& id, TfToken const& name, bool fallback)
{
  const VtValue v = d->GetLightParamValue(id, name);
  if (v.IsHolding<bool>()) return v.UncheckedGet<bool>();
  if (v.IsHolding<int>()) return v.UncheckedGet<int>() != 0;
  return fallback;
}

ir::Vec3 paramColor(HdSceneDelegate* d, SdfPath const& id, TfToken const& name, const ir::Vec3& fallback)
{
  const VtValue v = d->GetLightParamValue(id, name);
  if (v.IsHolding<GfVec3f>()) { const GfVec3f c = v.UncheckedGet<GfVec3f>(); return ir::Vec3(c[0], c[1], c[2]); }
  return fallback;
}

} // namespace

void HdIrLight::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits)
{
  if (!sceneDelegate || !_scene) return;
  const SdfPath& id = GetId();

  HdIrLightData data;
  ir::Light L;
  const GfMatrix4d xf = sceneDelegate->GetTransform(id);
  const ir::Xform x = irXformOf(xf);
  L.position = ir::Vec3(x.m[3], x.m[7], x.m[11]);
  // every UsdLux light points down its own -Z
  L.direction = ir::normalize(ir::Vec3(-x.m[2], -x.m[6], -x.m[10]));

  const float intensity = paramFloat(sceneDelegate, id, HdLightTokens->intensity, 1.0f);
  const float exposure = paramFloat(sceneDelegate, id, HdLightTokens->exposure, 0.0f);
  const ir::Vec3 colour = paramColor(sceneDelegate, id, HdLightTokens->color, ir::Vec3(1.0f));
  L.color = colour * (intensity * std::pow(2.0f, exposure));
  L.normalizePower = paramBool(sceneDelegate, id, HdLightTokens->normalize, false) ? 1 : 0;
  L.shadowEnable = paramBool(sceneDelegate, id, HdLightTokens->shadowEnable, true) ? 1 : 0;
  L.shadowColor = paramColor(sceneDelegate, id, HdLightTokens->shadowColor, ir::Vec3(0.0f));
  L.diffuseMul = paramFloat(sceneDelegate, id, HdLightTokens->diffuse, 1.0f);
  L.specularMul = paramFloat(sceneDelegate, id, HdLightTokens->specular, 1.0f);

  // UsdLuxShapingAPI: a cone makes a spot light
  const float coneAngle = paramFloat(sceneDelegate, id, HdLightTokens->shapingConeAngle, -1.0f);
  if (coneAngle > 0.0f) {
    const float softness = paramFloat(sceneDelegate, id, HdLightTokens->shapingConeSoftness, 0.0f);
    const float outer = coneAngle * 3.14159265f / 180.0f;
    L.coneCos = std::cos(outer);
    L.coneCosInner = std::cos(outer * (1.0f - (softness > 0.0f ? softness : 0.0f)));
  }
  L.focus = paramFloat(sceneDelegate, id, HdLightTokens->shapingFocus, 0.0f);
  L.focusTint = paramColor(sceneDelegate, id, HdLightTokens->shapingFocusTint, ir::Vec3(1.0f));

  if (_typeId == HdPrimTypeTokens->distantLight) {
    L.type = ir::kLightDistant;
    L.angle = paramFloat(sceneDelegate, id, HdLightTokens->angle, 0.53f);
  }
  else if (_typeId == HdPrimTypeTokens->sphereLight) {
    L.type = ir::kLightSphere;
    L.radius = paramFloat(sceneDelegate, id, HdLightTokens->radius, 0.5f);
    L.area = 4.0f * 3.14159265f * L.radius * L.radius;
  }
  else if (_typeId == HdPrimTypeTokens->diskLight) {
    L.type = ir::kLightDisk;
    L.radius = paramFloat(sceneDelegate, id, HdLightTokens->radius, 0.5f);
    L.area = 3.14159265f * L.radius * L.radius;
    L.u = ir::normalize(ir::Vec3(x.m[0], x.m[4], x.m[8]));
    L.v = ir::normalize(ir::Vec3(x.m[1], x.m[5], x.m[9]));
  }
  else if (_typeId == HdPrimTypeTokens->rectLight) {
    L.type = ir::kLightRect;
    const float w = paramFloat(sceneDelegate, id, HdLightTokens->width, 1.0f);
    const float h = paramFloat(sceneDelegate, id, HdLightTokens->height, 1.0f);
    // half extents along the light's own axes, in world units
    L.u = ir::Vec3(x.m[0], x.m[4], x.m[8]) * (0.5f * w);
    L.v = ir::Vec3(x.m[1], x.m[5], x.m[9]) * (0.5f * h);
    L.area = w * h;
  }
  else if (_typeId == HdPrimTypeTokens->cylinderLight) {
    L.type = ir::kLightCylinder;
    L.radius = paramFloat(sceneDelegate, id, HdLightTokens->radius, 0.5f);
    L.length = 0.5f * paramFloat(sceneDelegate, id, HdLightTokens->length, 1.0f);
    L.u = ir::normalize(ir::Vec3(x.m[0], x.m[4], x.m[8]));
    L.area = 2.0f * 3.14159265f * L.radius * (2.0f * L.length);
  }
  else if (_typeId == HdPrimTypeTokens->domeLight) {
    L.type = ir::kLightDome;
    L.area = 1.0f;
    // the world -> light basis the lat-long lookup needs (rows)
    L.lx = ir::Vec3(x.m[0], x.m[1], x.m[2]);
    L.ly = ir::Vec3(x.m[4], x.m[5], x.m[6]);
    L.lz = ir::Vec3(x.m[8], x.m[9], x.m[10]);
    // the image itself is loaded by the render pass, which owns the scene's
    // textures; all that can be done here is say which one it wants.  Nuke
    // points a dome at a Read node with an "nkop:" path, which its own Hio
    // plugin resolves - so the string is passed through untouched.
    const VtValue tex = sceneDelegate->GetLightParamValue(id, HdLightTokens->textureFile);
    if (tex.IsHolding<SdfAssetPath>()) {
      const SdfAssetPath& asset = tex.UncheckedGet<SdfAssetPath>();
      hlogLight("dome asset: authored '" + asset.GetAssetPath()
                + "' resolved '" + asset.GetResolvedPath() + "'");
      // The resolved path is the one to use: Nuke points a dome at the node
      // feeding it with an "nkop:" path, and its resolver strips that scheme by
      // design (source/FnUsdShim/ImageInterfaceImpl.cpp).  Reading one only
      // works while Nuke is holding a texture image for that op, and hands back
      // a 1x1 grey when it is not - which the InstanceRender node avoids by
      // rendering the op itself.  A delegate cannot: it never sees Nuke's graph.
      data.domeTexture = asset.GetResolvedPath();
      if (data.domeTexture.empty()) data.domeTexture = asset.GetAssetPath();
    }
    else if (tex.IsHolding<std::string>()) {
      data.domeTexture = tex.UncheckedGet<std::string>();
    }
    const VtValue fmt = sceneDelegate->GetLightParamValue(id, HdLightTokens->textureFormat);
    if (fmt.IsHolding<TfToken>()) {
      const TfToken& f = fmt.UncheckedGet<TfToken>();
      if (!f.IsEmpty() && f != TfToken("latlong") && f != TfToken("automatic"))
        hlogLight("dome light " + id.GetString() + ": texture:format " + f.GetString()
                  + " is not supported, reading the image as latlong");
    }
  }
  else {
    // simpleLight and anything else: treat as a point
    L.type = ir::kLightPoint;
  }

  data.light = L;
  hlogLight("light " + id.GetString() + " type " + std::to_string(L.type)
            + (data.domeTexture.empty() ? std::string() : (" texture " + data.domeTexture)));
  _scene->setLight(id, data);
  *dirtyBits = HdLight::Clean;
}

void HdIrLight::Finalize(HdRenderParam* renderParam)
{
  if (_scene) _scene->removeLight(GetId());
}

// ---------------------------------------------------------------------------
// The UsdUVTexture nodes feeding a UsdPreviewSurface.  Hydra gives the network
// as nodes plus relationships, so each surface input is followed back to the
// node driving it, and if that is a texture its file is read with the same
// loader the Nuke node uses.
// Is anything wired into a surface input at all?  Distinct from whether its
// image loaded - which is exactly the difference between "this surface has no
// texture" and "this surface has lost its texture".
bool HdIrMaterial::_hasConnection(const HdMaterialNetwork& net,
                                  const HdMaterialNode& surface,
                                  const char* input)
{
  for (size_t r = 0; r < net.relationships.size(); ++r)
    if (net.relationships[r].outputId == surface.path
        && net.relationships[r].outputName == TfToken(input))
      return true;
  return false;
}

void HdIrMaterial::_readTextures(const HdMaterialNetwork& net,
                                 const HdMaterialNode& surface,
                                 HdIrMaterialData& data)
{
  struct SlotName { const char* input; int slot; int channel; };
  static const SlotName kSlots[] = {
    { "diffuseColor",  HdIrTextureData::kDiffuse,   0 },
    { "emissiveColor", HdIrTextureData::kEmissive,  0 },
    { "roughness",     HdIrTextureData::kRoughness, 0 },
    { "metallic",      HdIrTextureData::kMetallic,  0 },
    { "opacity",       HdIrTextureData::kOpacity,   0 },
    { "normal",        HdIrTextureData::kNormal,    0 },
  };

  // What Hydra actually handed over.  A texture that does not arrive is silent -
  // the surface renders in its base colour and there is nothing to read - so the
  // network itself has to be visible when it goes wrong.
  for (size_t n = 0; n < net.nodes.size(); ++n)
    hlogLight("  material node " + net.nodes[n].path.GetString()
              + " identifier=" + net.nodes[n].identifier.GetString());
  for (size_t r = 0; r < net.relationships.size(); ++r)
    hlogLight("  material link " + net.relationships[r].inputId.GetString()
              + "." + net.relationships[r].inputName.GetString() + " -> "
              + net.relationships[r].outputId.GetString()
              + "." + net.relationships[r].outputName.GetString());

  for (size_t r = 0; r < net.relationships.size(); ++r) {
    const HdMaterialRelationship& rel = net.relationships[r];
    if (rel.outputId != surface.path) continue;
    int slot = -1, channel = 0;
    for (size_t k = 0; k < sizeof(kSlots) / sizeof(kSlots[0]); ++k) {
      if (rel.outputName == TfToken(kSlots[k].input)) { slot = kSlots[k].slot; channel = kSlots[k].channel; break; }
    }
    if (slot < 0) continue;

    // which node drives that input, and is it a texture?
    for (size_t n = 0; n < net.nodes.size(); ++n) {
      const HdMaterialNode& tex = net.nodes[n];
      if (tex.path != rel.inputId) continue;
      if (tex.identifier != TfToken("UsdUVTexture")) {
        hlogLight("  driving node for " + rel.outputName.GetString() + " is "
                  + tex.identifier.GetString() + ", not UsdUVTexture - no texture read");
        break;
      }

      // a scalar input reads one channel of the image (r, g, b or a)
      const std::string out = rel.inputName.GetString();
      if (out == "g") channel = 1;
      else if (out == "b") channel = 2;
      else if (out == "a") channel = 3;

      std::string path;
      const auto fileIt = tex.parameters.find(TfToken("file"));
      if (fileIt != tex.parameters.end()) {
        if (fileIt->second.IsHolding<SdfAssetPath>()) {
          const SdfAssetPath asset = fileIt->second.UncheckedGet<SdfAssetPath>();
          path = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
        }
        else if (fileIt->second.IsHolding<std::string>()) {
          path = fileIt->second.UncheckedGet<std::string>();
        }
      }
      if (path.empty()) {
        hlogLight("  UsdUVTexture " + tex.path.GetString() + " has no usable file parameter");
        break;
      }
      hlogLight("  texture for " + rel.outputName.GetString() + ": " + path);

      HdIrTextureData t;
      t.slot = slot;
      t.channel = channel;
      {
        const auto sIt = tex.parameters.find(TfToken("scale"));
        if (sIt != tex.parameters.end() && sIt->second.IsHolding<GfVec4f>()) {
          const GfVec4f v = sIt->second.UncheckedGet<GfVec4f>();
          t.scale = ir::Vec4(v[0], v[1], v[2], v[3]);
        }
        const auto bIt = tex.parameters.find(TfToken("bias"));
        if (bIt != tex.parameters.end() && bIt->second.IsHolding<GfVec4f>()) {
          const GfVec4f v = bIt->second.UncheckedGet<GfVec4f>();
          t.bias = ir::Vec4(v[0], v[1], v[2], v[3]);
        }
      }
      std::string err;
      const int colourSpace = (slot == HdIrTextureData::kDiffuse || slot == HdIrTextureData::kEmissive)
                            ? ir::kColorAuto : ir::kColorRaw;
      // A UsdUVTexture fed by a Nuke node names the op, not a file - and for one
      // of those the BAKE is the answer, not the first thing to try.
      //
      // Falling through to the image readers when it failed is what made a card
      // render flat: Nuke's own Hio plugin will open an "nkop:" path and hand
      // back a ONE PIXEL GREY when it is not holding an image for that op. That
      // is a perfectly good image as far as this is concerned, so it was bound
      // as the texture and nothing was reported. A bake that fails is a failure,
      // and the material keeps its previous texture instead (see Sync).
      bool ok = false;
      if (ir::isNukeOpPath(path)) {
        ok = ir::loadNukeOpImage(path, 4096, t.image, err) && t.image.valid();
        if (!ok && err.empty())
          err = "the Nuke node feeding this texture would not render";
      }
      else {
        ok = ir::loadImageFile(path, colourSpace, 4096, t.image, err) && t.image.valid();
      }
      if (ok)
        data.textures.push_back(t);
      else
        hlogLight("material texture " + path + ": " + err);
      break;
    }
  }
}

void HdIrMaterial::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits)
{
  if (!sceneDelegate || !_scene) return;
  const SdfPath& id = GetId();

  HdIrMaterialData data;
  ir::Material& m = data.material;
  m.useDisplayColor = 1;      // until a UsdPreviewSurface says otherwise

  const VtValue res = sceneDelegate->GetMaterialResource(id);
  if (res.IsHolding<HdMaterialNetworkMap>()) {
    const HdMaterialNetworkMap& netMap = res.UncheckedGet<HdMaterialNetworkMap>();
    const auto it = netMap.map.find(HdMaterialTerminalTokens->surface);
    if (it != netMap.map.end()) {
      const HdMaterialNetwork& net = it->second;
      for (size_t n = 0; n < net.nodes.size(); ++n) {
        const HdMaterialNode& node = net.nodes[n];
        if (node.identifier != TfToken("UsdPreviewSurface")) continue;
        m.useDisplayColor = 0;
        const bool wantedDiffuseTex = _hasConnection(net, node, "diffuseColor");
        _readTextures(net, node, data);
        for (auto const& [name, value] : node.parameters) {
          if (name == TfToken("diffuseColor") && value.IsHolding<GfVec3f>()) {
            const GfVec3f c = value.UncheckedGet<GfVec3f>();
            m.diffuse = ir::Vec3(c[0], c[1], c[2]);
          }
          else if (name == TfToken("emissiveColor") && value.IsHolding<GfVec3f>()) {
            const GfVec3f c = value.UncheckedGet<GfVec3f>();
            m.emissive = ir::Vec3(c[0], c[1], c[2]);
          }
          else if (name == TfToken("specularColor") && value.IsHolding<GfVec3f>()) {
            const GfVec3f c = value.UncheckedGet<GfVec3f>();
            m.specularColor = ir::Vec3(c[0], c[1], c[2]);
          }
          else if (name == TfToken("roughness") && value.IsHolding<float>()) m.roughness = value.UncheckedGet<float>();
          else if (name == TfToken("metallic") && value.IsHolding<float>()) m.metallic = value.UncheckedGet<float>();
          else if (name == TfToken("opacity") && value.IsHolding<float>()) m.opacity = value.UncheckedGet<float>();
          else if (name == TfToken("ior") && value.IsHolding<float>()) m.ior = value.UncheckedGet<float>();
          else if (name == TfToken("useSpecularWorkflow") && value.IsHolding<int>())
            m.useSpecularWorkflow = value.UncheckedGet<int>();
        }
        // A texture on diffuseColor REPLACES the constant, and has to be applied
        // after the parameters: Nuke authors the knob value as well as the
        // connection, and carries that value in the texture's own scale.
        //
        // ONLY when the texture is actually there.  Nuke authors diffuseColor 1
        // beside the connection, so a texture that failed to load used to leave
        // the surface at a flat, blown-out WHITE - far more conspicuous than the
        // missing texture itself, and the reason a card with no checkerboard
        // stands out as a white shape rather than a grey one.
        bool haveDiffuseTex = false;
        for (size_t t = 0; t < data.textures.size(); ++t)
          if (data.textures[t].slot == HdIrTextureData::kDiffuse) haveDiffuseTex = true;
        if (haveDiffuseTex) m.diffuse = ir::Vec3(1.0f, 1.0f, 1.0f);
        else if (wantedDiffuseTex) {
          // a connection was there and the image was not: show the surface as
          // untextured rather than as a white card
          m.diffuse = ir::Vec3(0.18f, 0.18f, 0.18f);
          hlogLight("material " + id.GetString()
                    + ": diffuseColor is connected but no image loaded - "
                    "rendering it untextured rather than white");
        }
        break;      // the first UsdPreviewSurface is the surface
      }
    }
  }

  // A bake that failed must not take the texture with it.
  //
  // A texture wired from a Nuke node is read by BAKING that node, and a bake can
  // fail while the timeline is moving - the op is mid evaluation, or its render
  // was dropped for the next frame.  When that happened this stored a material
  // with no textures at all and the surface fell back to its base colour: the
  // reported "GeoRender lost the checkerboard when scrubbing".
  //
  // So the previous images are carried over instead.  Note what this does NOT do:
  // it does not keep a cache of decoded images.  ir::ImageData owns its pixels,
  // so caching one copies a 4k texture - 128 MB - on every sync, and doing that
  // for two materials while scrubbing lost BOTH textures rather than saving them.
  // The images here are ones the scene is already holding.
  if (data.textures.empty()) {
    const HdIrMaterialData previous = _scene->material(id);
    if (!previous.textures.empty()) {
      hlogLight("material " + id.GetString()
                + ": nothing readable this time, keeping the last good texture(s)");
      data.textures = previous.textures;
      for (size_t t = 0; t < data.textures.size(); ++t)
        if (data.textures[t].slot == HdIrTextureData::kDiffuse)
          data.material.diffuse = ir::Vec3(1.0f, 1.0f, 1.0f);
    }
  }

  _scene->setMaterial(id, data);
  *dirtyBits = HdMaterial::Clean;
}

void HdIrMaterial::Finalize(HdRenderParam* renderParam)
{
  if (_scene) _scene->removeMaterial(GetId());
}

PXR_NAMESPACE_CLOSE_SCOPE
