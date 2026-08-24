// InstanceRender - HdIrPoints.h
// The two rprim types that are not meshes:
//
//   HdPoints       -> one sphere prototype, one instance per point, so a
//                     million particles cost a transform each rather than a
//                     sphere each.  That is the whole point of this renderer.
//   HdBasisCurves  -> a tube per curve, swept along the evaluated basis.
//
// Both fill the same HdIrMeshData the mesh path fills, so materials, textures,
// instancing and the AOVs all work without a special case anywhere downstream.
// Strict ASCII.
#pragma once

#include "HdIrScene.h"

#include <pxr/imaging/hd/points.h>
#include <pxr/imaging/hd/basisCurves.h>
#include <pxr/imaging/hd/sceneDelegate.h>

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

class HdIrPoints final : public HdPoints
{
public:
  HdIrPoints(SdfPath const& id, HdIrScene* scene) : HdPoints(id), _scene(scene) {}
  ~HdIrPoints() override = default;

  HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits, TfToken const& reprToken) override;
  void Finalize(HdRenderParam* renderParam) override;

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits) override;

private:
  HdIrScene* _scene = nullptr;

  HdIrPoints(const HdIrPoints&) = delete;
  HdIrPoints& operator=(const HdIrPoints&) = delete;
};

class HdIrCurves final : public HdBasisCurves
{
public:
  HdIrCurves(SdfPath const& id, HdIrScene* scene) : HdBasisCurves(id), _scene(scene) {}
  ~HdIrCurves() override = default;

  HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits, TfToken const& reprToken) override;
  void Finalize(HdRenderParam* renderParam) override;

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits) override;

private:
  HdIrScene* _scene = nullptr;

  HdIrCurves(const HdIrCurves&) = delete;
  HdIrCurves& operator=(const HdIrCurves&) = delete;
};

PXR_NAMESPACE_CLOSE_SCOPE
