// InstanceRender - HdIrInstancer.cpp
// The instance-rate primvars Hydra gives a PointInstancer, turned into one
// matrix per copy.  This is the same shape hdEmbree uses, and it is what keeps
// a million copies costing a million transforms rather than a million meshes.
// Strict ASCII.
#include "HdIrMesh.h"

#include <pxr/base/gf/quaternion.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/tokens.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
  _instanceTokens,
  ((translate, "hydra:instanceTranslations"))
  ((rotate,    "hydra:instanceRotations"))
  ((scale,     "hydra:instanceScales"))
  ((transform, "hydra:instanceTransforms"))
);

void HdIrInstancer::_SyncPrimvars(HdSceneDelegate* delegate, HdDirtyBits dirtyBits)
{
  const SdfPath& id = GetId();
  const HdPrimvarDescriptorVector primvars =
    delegate->GetPrimvarDescriptors(id, HdInterpolationInstance);
  for (size_t i = 0; i < primvars.size(); ++i) {
    if (!HdChangeTracker::IsPrimvarDirty(dirtyBits, id, primvars[i].name)) continue;
    const VtValue value = delegate->Get(id, primvars[i].name);
    if (!value.IsEmpty()) _primvarMap[primvars[i].name] = value;
  }
}

VtVec3fArray HdIrInstancer::ComputeInstanceColors(SdfPath const& prototypeId)
{
  HdSceneDelegate* delegate = GetDelegate();
  const SdfPath& id = GetId();
  TfHashMap<TfToken, VtValue, TfToken::HashFunctor>::const_iterator it =
    _primvarMap.find(HdTokens->displayColor);
  if (it == _primvarMap.end() || !it->second.IsHolding<VtVec3fArray>()) return VtVec3fArray();
  const VtVec3fArray& c = it->second.UncheckedGet<VtVec3fArray>();
  if (c.empty()) return VtVec3fArray();

  // Indexed exactly as the transforms are, so colour i belongs to copy i.
  const VtIntArray instanceIndices = delegate->GetInstanceIndices(id, prototypeId);
  VtVec3fArray out(instanceIndices.size());
  for (size_t i = 0; i < instanceIndices.size(); ++i) {
    const int idx = instanceIndices[i];
    // One authored colour covers every copy - that is what a constant
    // displayColor on the instancer means.
    out[i] = (c.size() == 1) ? c[0]
           : ((idx >= 0 && size_t(idx) < c.size()) ? c[size_t(idx)] : c[0]);
  }
  return out;
}

VtMatrix4dArray HdIrInstancer::ComputeInstanceTransforms(SdfPath const& prototypeId)
{
  HdSceneDelegate* delegate = GetDelegate();
  const SdfPath& id = GetId();

  // this instancer's own place in the world
  const GfMatrix4d instancerTransform = delegate->GetInstancerTransform(id);
  const VtIntArray instanceIndices = delegate->GetInstanceIndices(id, prototypeId);

  VtMatrix4dArray transforms(instanceIndices.size());
  for (size_t i = 0; i < instanceIndices.size(); ++i) transforms[i] = instancerTransform;

  // "hydra:instanceTranslations" etc are the instance-rate primvars a
  // PointInstancer carries; each one is applied to every copy that indexes it.
  TfHashMap<TfToken, VtValue, TfToken::HashFunctor>::const_iterator it;

  it = _primvarMap.find(_instanceTokens->translate);
  if (it != _primvarMap.end() && it->second.IsHolding<VtVec3fArray>()) {
    const VtVec3fArray& v = it->second.UncheckedGet<VtVec3fArray>();
    for (size_t i = 0; i < instanceIndices.size(); ++i) {
      const int idx = instanceIndices[i];
      if (idx < 0 || size_t(idx) >= v.size()) continue;
      GfMatrix4d t(1.0);
      t.SetTranslate(GfVec3d(v[idx]));
      transforms[i] = t * transforms[i];
    }
  }

  it = _primvarMap.find(_instanceTokens->rotate);
  if (it != _primvarMap.end()) {
    if (it->second.IsHolding<VtQuathArray>()) {
      const VtQuathArray& q = it->second.UncheckedGet<VtQuathArray>();
      for (size_t i = 0; i < instanceIndices.size(); ++i) {
        const int idx = instanceIndices[i];
        if (idx < 0 || size_t(idx) >= q.size()) continue;
        GfMatrix4d r(1.0);
        r.SetRotate(GfQuatd(float(q[idx].GetReal()),
                            GfVec3d(q[idx].GetImaginary()[0], q[idx].GetImaginary()[1], q[idx].GetImaginary()[2])));
        transforms[i] = r * transforms[i];
      }
    }
    else if (it->second.IsHolding<VtVec4fArray>()) {
      const VtVec4fArray& q = it->second.UncheckedGet<VtVec4fArray>();
      for (size_t i = 0; i < instanceIndices.size(); ++i) {
        const int idx = instanceIndices[i];
        if (idx < 0 || size_t(idx) >= q.size()) continue;
        GfMatrix4d r(1.0);
        r.SetRotate(GfQuatd(q[idx][0], GfVec3d(q[idx][1], q[idx][2], q[idx][3])));
        transforms[i] = r * transforms[i];
      }
    }
  }

  it = _primvarMap.find(_instanceTokens->scale);
  if (it != _primvarMap.end() && it->second.IsHolding<VtVec3fArray>()) {
    const VtVec3fArray& v = it->second.UncheckedGet<VtVec3fArray>();
    for (size_t i = 0; i < instanceIndices.size(); ++i) {
      const int idx = instanceIndices[i];
      if (idx < 0 || size_t(idx) >= v.size()) continue;
      GfMatrix4d s(1.0);
      s.SetScale(GfVec3d(v[idx]));
      transforms[i] = s * transforms[i];
    }
  }

  it = _primvarMap.find(_instanceTokens->transform);
  if (it != _primvarMap.end() && it->second.IsHolding<VtMatrix4dArray>()) {
    const VtMatrix4dArray& m = it->second.UncheckedGet<VtMatrix4dArray>();
    for (size_t i = 0; i < instanceIndices.size(); ++i) {
      const int idx = instanceIndices[i];
      if (idx < 0 || size_t(idx) >= m.size()) continue;
      transforms[i] = m[idx] * transforms[i];
    }
  }

  // instancers can be nested: fold in the parent's transforms
  if (GetParentId().IsEmpty()) return transforms;

  HdInstancer* parentInstancer = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
  HdIrInstancer* parent = dynamic_cast<HdIrInstancer*>(parentInstancer);
  if (!parent) return transforms;

  const VtMatrix4dArray parentTransforms = parent->ComputeInstanceTransforms(id);
  VtMatrix4dArray final(parentTransforms.size() * transforms.size());
  for (size_t i = 0; i < parentTransforms.size(); ++i)
    for (size_t j = 0; j < transforms.size(); ++j)
      final[i * transforms.size() + j] = transforms[j] * parentTransforms[i];
  return final;
}

PXR_NAMESPACE_CLOSE_SCOPE
