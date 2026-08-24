// InstanceRender - HdIrMesh.h / HdIrInstancer
// The two rprim-side classes: a mesh that syncs itself into HdIrScene, and an
// instancer that computes where its copies go.  Strict ASCII.
#pragma once

#include "HdIrScene.h"

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

// Where the copies of a prototype go.  This is the class that keeps a
// PointInstancer instanced instead of flattening it: one prototype mesh, one
// transform per copy, exactly what the Embree and OptiX back-ends want.
class HdIrInstancer final : public HdInstancer
{
public:
  HdIrInstancer(HdSceneDelegate* delegate, SdfPath const& id) : HdInstancer(delegate, id) {}
  ~HdIrInstancer() override = default;

  void Sync(HdSceneDelegate* delegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits) override
  {
    _UpdateInstancer(delegate, dirtyBits);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId()))
      _SyncPrimvars(delegate, *dirtyBits);
  }

  // The transform of every copy of 'prototypeId', in the instancer's space.
  VtMatrix4dArray ComputeInstanceTransforms(SdfPath const& prototypeId);

  // The per-copy displayColor, in the same order, or empty when the instancer
  // carries none.  A PointInstancer's displayColor is an instance-rate primvar
  // and is the only place a per-copy colour can live - the prototype has one
  // colour for all of them.
  VtVec3fArray ComputeInstanceColors(SdfPath const& prototypeId);

private:
  void _SyncPrimvars(HdSceneDelegate* delegate, HdDirtyBits dirtyBits);

  // instance-rate primvars: translate / rotate / scale / instanceTransform
  TfHashMap<TfToken, VtValue, TfToken::HashFunctor> _primvarMap;
};

class HdIrMesh final : public HdMesh
{
public:
  HdIrMesh(SdfPath const& id, HdIrScene* scene);
  ~HdIrMesh() override = default;

  HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(HdSceneDelegate* sceneDelegate,
            HdRenderParam*   renderParam,
            HdDirtyBits*     dirtyBits,
            TfToken const&   reprToken) override;

  void Finalize(HdRenderParam* renderParam) override;

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;
  void _InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits) override;

private:
  HdIrScene* _scene = nullptr;

  HdIrMesh(const HdIrMesh&) = delete;
  HdIrMesh& operator=(const HdIrMesh&) = delete;
};

PXR_NAMESPACE_CLOSE_SCOPE
