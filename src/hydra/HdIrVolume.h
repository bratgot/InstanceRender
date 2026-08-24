// InstanceRender - HdIrVolume.h
//
// Volumes through Hydra, which arrive by a route nothing else here uses.
//
//   HdVolume   the rprim.  It does NOT carry the grids: it carries field
//              DESCRIPTORS, fetched with GetVolumeFieldDescriptors, each naming
//              a separate bprim.
//   HdField    the bprim, one per grid, holding the .vdb path and the grid name.
//
// So a host has to be told this delegate supports the field bprim type as well
// as the volume rprim - advertise only the rprim and every volume arrives with
// zero fields and renders nothing, which is exactly what a delegate that had
// "volume support" but no grids would look like.
//
// The shading knobs travel as CONSTANT PRIMVARS rather than as the plain USD
// attributes the node reads, because an rprim can only ask for primvars.
// VolumeToUSD authors both.
// Strict ASCII.
#pragma once

#include "HdIrScene.h"

#include <pxr/imaging/hd/volume.h>
#include <pxr/imaging/hd/field.h>
#include <pxr/imaging/hd/sceneDelegate.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdIrVolume final : public HdVolume
{
public:
  HdIrVolume(SdfPath const& id, HdIrScene* scene) : HdVolume(id), _scene(scene) {}
  ~HdIrVolume() override = default;

  HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits, TfToken const& reprToken) override;
  void Finalize(HdRenderParam* renderParam) override;

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits) override;

private:
  HdIrScene* _scene = nullptr;

  HdIrVolume(const HdIrVolume&) = delete;
  HdIrVolume& operator=(const HdIrVolume&) = delete;
};

// The bprim behind one grid.  All it has to do is remember the path and the
// grid name; the render pass reads the voxels through the shared cache so that
// a grid shared by several volumes is read once.
class HdIrField final : public HdField
{
public:
  HdIrField(SdfPath const& id, HdIrScene* scene) : HdField(id), _scene(scene) {}
  ~HdIrField() override = default;

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override;
  HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
  void Finalize(HdRenderParam* renderParam) override;

private:
  HdIrScene* _scene = nullptr;

  HdIrField(const HdIrField&) = delete;
  HdIrField& operator=(const HdIrField&) = delete;
};

PXR_NAMESPACE_CLOSE_SCOPE
