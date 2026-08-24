// InstanceRender - HdIrVolume.cpp
// Volumes arriving through Hydra rather than through the node's stage loader.
// Strict ASCII.
#include "HdIrVolume.h"

#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/vt/value.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The knobs VolumeToUSD writes.  They are read as CONSTANT PRIMVARS - an rprim
// cannot ask a scene delegate for an arbitrary USD attribute, so the plain
// ir:... attributes the node's loader reads are invisible from here and the
// same values are authored a second time as primvars:ir:... for this path.
// NOTE THE NAMES.  VolumeToUSD authors primvars:ir:temperatureScale, but Hydra
// hands primvars back with that namespace already stripped - asking for the
// prefixed name returns nothing at all, silently, and every knob sits at its
// default while the volume itself renders perfectly.
float _floatPrimvar(HdSceneDelegate* sd, const SdfPath& id, const char* name, float dflt)
{
  const VtValue v = sd->Get(id, TfToken(name));
  if (v.IsHolding<float>())  return v.UncheckedGet<float>();
  if (v.IsHolding<double>()) return float(v.UncheckedGet<double>());
  if (v.IsHolding<VtArray<float> >()) {
    const VtArray<float>& a = v.UncheckedGet<VtArray<float> >();
    if (!a.empty()) return a[0];
  }
  return dflt;
}

int _intPrimvar(HdSceneDelegate* sd, const SdfPath& id, const char* name, int dflt)
{
  const VtValue v = sd->Get(id, TfToken(name));
  if (v.IsHolding<int>()) return v.UncheckedGet<int>();
  if (v.IsHolding<VtArray<int> >()) {
    const VtArray<int>& a = v.UncheckedGet<VtArray<int> >();
    if (!a.empty()) return a[0];
  }
  return dflt;
}

ir::Vec3 _colorPrimvar(HdSceneDelegate* sd, const SdfPath& id, const char* name,
                       const ir::Vec3& dflt)
{
  const VtValue v = sd->Get(id, TfToken(name));
  if (v.IsHolding<GfVec3f>()) {
    const GfVec3f& c = v.UncheckedGet<GfVec3f>();
    return ir::Vec3(c[0], c[1], c[2]);
  }
  if (v.IsHolding<VtVec3fArray>()) {
    const VtVec3fArray& a = v.UncheckedGet<VtVec3fArray>();
    if (!a.empty()) return ir::Vec3(a[0][0], a[0][1], a[0][2]);
  }
  return dflt;
}

} // namespace

HdDirtyBits HdIrVolume::GetInitialDirtyBitsMask() const
{
  return HdChangeTracker::Clean
       | HdChangeTracker::DirtyTransform
       | HdChangeTracker::DirtyVisibility
       | HdChangeTracker::DirtyPrimvar
       | HdChangeTracker::DirtyVolumeField;
}

void HdIrVolume::_InitRepr(TfToken const& reprToken, HdDirtyBits*)
{
  _ReprVector::iterator it = std::find_if(_reprs.begin(), _reprs.end(),
                                          _ReprComparator(reprToken));
  if (it == _reprs.end()) _reprs.push_back(std::make_pair(reprToken, HdReprSharedPtr()));
}

void HdIrVolume::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam*,
                      HdDirtyBits* dirtyBits, TfToken const&)
{
  if (!sceneDelegate || !_scene) { if (dirtyBits) *dirtyBits = HdChangeTracker::Clean; return; }
  const SdfPath& id = GetId();

  HdIrVolumeData vol;
  vol.visible = sceneDelegate->GetVisible(id);
  // inverted here, where it is still a GfMatrix4d and can be
  vol.worldToLocal = irXformOf(sceneDelegate->GetTransform(id).GetInverse());

  // The fields, by the names UsdVolVolume uses for its relationships.  Anything
  // else in the list is a grid this renderer has no slot for, and is skipped
  // rather than guessed at.
  const HdVolumeFieldDescriptorVector fields = sceneDelegate->GetVolumeFieldDescriptors(id);
  for (size_t i = 0; i < fields.size(); ++i) {
    const HdIrFieldRef ref = _scene->field(fields[i].fieldId);
    if (ref.fieldName.empty()) continue;
    const std::string& n = fields[i].fieldName.GetString();
    if      (n == "density")     vol.density = ref;
    else if (n == "temperature") vol.emissive[0] = ref;
    else if (n == "emission")    vol.emissive[1] = ref;
  }

  vol.densityScale = _floatPrimvar(sceneDelegate, id, "ir:densityScale", 1.0f);
  vol.frame = _intPrimvar(sceneDelegate, id, "ir:frame", 0);
  static const char* const kScale[2] = { "ir:temperatureScale", "ir:emissionScale" };
  static const char* const kColor[2] = { "ir:temperatureColor", "ir:emissionColor" };
  static const char* const kMode[2]  = { "ir:temperatureMode",  "ir:emissionMode" };
  static const char* const kKmin[2]  = { "ir:temperatureKmin",  "ir:emissionKmin" };
  static const char* const kKmax[2]  = { "ir:temperatureKmax",  "ir:emissionKmax" };
  for (int si = 0; si < ir::kVolumeEmissive; ++si) {
    vol.emissionScale[si] = _floatPrimvar(sceneDelegate, id, kScale[si], 0.0f);
    vol.emissionColor[si] = _colorPrimvar(sceneDelegate, id, kColor[si], ir::Vec3(1.0f));
    vol.emissionMode[si]  = (_intPrimvar(sceneDelegate, id, kMode[si], 0) == 1)
                          ? ir::kEmitBlackbody : ir::kEmitIntensity;
    vol.emitKmin[si]      = _floatPrimvar(sceneDelegate, id, kKmin[si], 0.0f);
    vol.emitKmax[si]      = _floatPrimvar(sceneDelegate, id, kKmax[si], 0.0f);
  }

  _scene->setVolume(id, vol);
  if (dirtyBits) *dirtyBits = HdChangeTracker::Clean;
}

void HdIrVolume::Finalize(HdRenderParam*)
{
  if (_scene) _scene->removeVolume(GetId());
}

// ---- the field bprim ---------------------------------------------------------

void HdIrField::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam*, HdDirtyBits* dirtyBits)
{
  if (!sceneDelegate || !_scene) { if (dirtyBits) *dirtyBits = HdChangeTracker::Clean; return; }
  const SdfPath& id = GetId();

  HdIrFieldRef ref;
  const VtValue fp = sceneDelegate->Get(id, HdFieldTokens->filePath);
  if (fp.IsHolding<SdfAssetPath>()) {
    const SdfAssetPath& ap = fp.UncheckedGet<SdfAssetPath>();
    // Prefer the UNRESOLVED path: a sequence is resolved per frame by the render
    // pass, and a path resolved once is what made a whole shot render frame one.
    ref.filePath = ap.GetAssetPath();
    if (ref.filePath.find('%') == std::string::npos
        && ref.filePath.find('#') == std::string::npos
        && !ap.GetResolvedPath().empty())
      ref.filePath = ap.GetResolvedPath();
  }
  else if (fp.IsHolding<std::string>()) {
    ref.filePath = fp.UncheckedGet<std::string>();
  }
  const VtValue fn = sceneDelegate->Get(id, HdFieldTokens->fieldName);
  if (fn.IsHolding<TfToken>()) ref.fieldName = fn.UncheckedGet<TfToken>().GetString();
  else if (fn.IsHolding<std::string>()) ref.fieldName = fn.UncheckedGet<std::string>();

  _scene->setField(id, ref);
  if (dirtyBits) *dirtyBits = HdChangeTracker::Clean;
}

void HdIrField::Finalize(HdRenderParam*)
{
  if (_scene) _scene->removeField(GetId());
}

PXR_NAMESPACE_CLOSE_SCOPE
