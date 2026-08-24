// InstanceRender - HdIrLight.h
// The two sprim-side classes: lights and materials, syncing themselves into
// HdIrScene.  Strict ASCII.
#pragma once

#include "HdIrScene.h"

#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdIrLight final : public HdLight
{
public:
  HdIrLight(SdfPath const& id, TfToken const& typeId, HdIrScene* scene)
    : HdLight(id), _typeId(typeId), _scene(scene) {}
  ~HdIrLight() override = default;

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits) override;
  void Finalize(HdRenderParam* renderParam) override;
  HdDirtyBits GetInitialDirtyBitsMask() const override { return HdLight::AllDirty; }

private:
  TfToken _typeId;
  HdIrScene* _scene = nullptr;
};

class HdIrMaterial final : public HdMaterial
{
public:
  HdIrMaterial(SdfPath const& id, HdIrScene* scene) : HdMaterial(id), _scene(scene) {}
  ~HdIrMaterial() override = default;

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits) override;
  void Finalize(HdRenderParam* renderParam) override;
  HdDirtyBits GetInitialDirtyBitsMask() const override { return HdMaterial::AllDirty; }

private:
  static bool _hasConnection(const HdMaterialNetwork& net,
                             const HdMaterialNode& surface,
                             const char* input);
  static void _readTextures(const HdMaterialNetwork& net,
                            const HdMaterialNode& surface,
                            HdIrMaterialData& data);

  HdIrScene* _scene = nullptr;
};

PXR_NAMESPACE_CLOSE_SCOPE
