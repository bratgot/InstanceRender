// InstanceRender - HdIrScene.h
//
// What the delegate's rprims fill in, and what the render pass turns into an
// ir::Scene.  Hydra syncs prims one at a time and from several threads, so each
// prim keeps its own record here and the render pass assembles them; nothing in
// this file knows anything about rendering.
//
// Strict ASCII.
#pragma once

#include "ir/Scene.h"
#include "ir/Image.h"

#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/path.h>

#include <map>
#include <mutex>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// One synced mesh: triangles in object space, plus where its copies go.
struct HdIrMeshData {
  std::vector<ir::Vec3>     points;
  std::vector<ir::Vec3>     normals;     // empty = flat shading from the triangle
  std::vector<float>        uvs;         // 2 per point, empty = untextured
  std::vector<uint32_t>     indices;     // 3 per triangle
  std::vector<ir::Xform>    instances;   // at least one: the prim's own transform
  std::vector<ir::Vec3>     instanceColors;  // empty, or one per instance (points)
  // Hydra's fallback surface, and UsdPreviewSurface's own default diffuseColor
  ir::Vec3                  color = ir::Vec3(0.18f, 0.18f, 0.18f);
  bool                      visible = true;
  SdfPath                   materialId;  // empty = no material bound
  // The prim's own path.  The map is keyed by it, but meshes() hands back the
  // values alone - and a cryptomatte is named after this, so it has to travel
  // with the data rather than being left behind in the key.
  SdfPath                   primId;
};

// One synced volume: which .vdb grids it wants and how they are shaded.
//
// The VOXELS are deliberately not here.  A field bprim knows only a path and a
// grid name, the same grid is often shared by several volumes, and the render
// pass owns the scene the voxels have to land in - so this carries the names and
// the render pass reads them through the shared cache.
struct HdIrFieldRef {
  std::string filePath;      // may still hold %04d - resolved per frame
  std::string fieldName;
};

struct HdIrVolumeData {
  HdIrFieldRef density;
  HdIrFieldRef emissive[ir::kVolumeEmissive];
  float        densityScale = 1.0f;
  float        emissionScale[ir::kVolumeEmissive] = { 0.0f, 0.0f };
  ir::Vec3     emissionColor[ir::kVolumeEmissive] = { ir::Vec3(1.0f), ir::Vec3(1.0f) };
  int          emissionMode[ir::kVolumeEmissive] = { ir::kEmitIntensity, ir::kEmitIntensity };
  float        emitKmin[ir::kVolumeEmissive] = { 0.0f, 0.0f };
  float        emitKmax[ir::kVolumeEmissive] = { 0.0f, 0.0f };
  // world -> the grid's own space, which is what the kernel marches in
  ir::Xform    worldToLocal;
  int          frame = 0;   // which frame to resolve a %04d path against
  bool         visible = true;
  SdfPath      primId;
};

// One image feeding a UsdPreviewSurface input.  The render pass turns these into
// the scene's texel array and points the material's TexRefs at them.
struct HdIrTextureData {
  enum Slot { kDiffuse = 0, kEmissive, kRoughness, kMetallic, kOpacity, kNormal, kSlotCount };
  int          slot = kDiffuse;
  int          channel = 0;      // which channel a scalar input reads
  // UsdUVTexture multiplies by scale and adds bias, and Nuke uses that scale to
  // carry the shader's own colour - a PreviewSurface with diffuseColor 0.18 and
  // a texture authors scale (0.18, 0.18, 0.18, 1).  Ignoring it renders every
  // Nuke-authored texture far too bright.
  ir::Vec4     scale = ir::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
  ir::Vec4     bias = ir::Vec4(0.0f, 0.0f, 0.0f, 0.0f);
  ir::ImageData image;
};

// A light, plus the HDRI a dome light lights the scene with.  The image cannot
// be loaded here: textures belong to the scene the render pass assembles, and
// this is filled from Sync() on whichever thread Hydra chose.
struct HdIrLightData {
  ir::Light   light;
  std::string domeTexture;    // empty unless this is a dome light with an image
};

struct HdIrMaterialData {
  ir::Material material;
  std::vector<HdIrTextureData> textures;
};

// How finely the two non-mesh prim types are turned into triangles.  These are
// render settings, so GeoRender offers them in its renderer settings table, and
// they default to what the Nuke node defaults to.
struct HdIrTessellation {
  int pointDetail = 2;      // rings on a point's sphere
  int curveSides = 6;       // sides of a curve's tube
  int curveSegments = 4;    // samples along each span of a cubic curve
  int subdivLevels = 0;     // Catmull-Clark refinement (0 = the control cage)
};

// Everything the delegate has synced.  The render pass reads it under the lock.
class HdIrScene
{
public:
  void setTessellation(const HdIrTessellation& t)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _tess = t;
    ++_version;
  }

  HdIrTessellation tessellation() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _tess;
  }

  void setMesh(const SdfPath& id, const HdIrMeshData& meshIn)
  {
    HdIrMeshData mesh = meshIn;
    mesh.primId = id;
    std::lock_guard<std::mutex> lock(_mutex);
    _meshes[id] = mesh;
    ++_version;
  }

  void removeMesh(const SdfPath& id)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _meshes.erase(id);
    ++_version;
  }

  void setVolume(const SdfPath& id, const HdIrVolumeData& volIn)
  {
    HdIrVolumeData v = volIn;
    v.primId = id;
    std::lock_guard<std::mutex> lock(_mutex);
    _volumes[id] = v;
    ++_version;
  }

  void removeVolume(const SdfPath& id)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _volumes.erase(id);
    ++_version;
  }

  std::vector<HdIrVolumeData> volumes() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<HdIrVolumeData> out;
    out.reserve(_volumes.size());
    for (std::map<SdfPath, HdIrVolumeData>::const_iterator it = _volumes.begin();
         it != _volumes.end(); ++it)
      if (it->second.visible) out.push_back(it->second);
    return out;
  }

  // A field bprim is a path and a grid name; the volumes that reference it look
  // it up here at Sync, so the two can be synced in either order.
  void setField(const SdfPath& id, const HdIrFieldRef& f)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _fields[id] = f;
    ++_version;
  }

  void removeField(const SdfPath& id)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _fields.erase(id);
    ++_version;
  }

  HdIrFieldRef field(const SdfPath& id) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<SdfPath, HdIrFieldRef>::const_iterator it = _fields.find(id);
    return (it == _fields.end()) ? HdIrFieldRef() : it->second;
  }

  void setLight(const SdfPath& id, const HdIrLightData& light)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _lights[id] = light;
    ++_version;
  }

  void removeLight(const SdfPath& id)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _lights.erase(id);
    ++_version;
  }

  void setMaterial(const SdfPath& id, const HdIrMaterialData& material)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _materials[id] = material;
    ++_version;
  }

  void removeMaterial(const SdfPath& id)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _materials.erase(id);
    ++_version;
  }

  std::vector<HdIrLightData> lights() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<HdIrLightData> out;
    out.reserve(_lights.size());
    for (std::map<SdfPath, HdIrLightData>::const_iterator it = _lights.begin(); it != _lights.end(); ++it)
      out.push_back(it->second);
    return out;
  }

  // The material bound to a mesh, or a displayColor one when there is none.
  HdIrMaterialData material(const SdfPath& id) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<SdfPath, HdIrMaterialData>::const_iterator it = _materials.find(id);
    if (it != _materials.end()) return it->second;
    HdIrMaterialData fallback;
    fallback.material.useDisplayColor = 1;
    return fallback;
  }

  // A snapshot the render pass can walk without holding the lock.
  std::vector<HdIrMeshData> meshes(uint64_t* version = nullptr) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<HdIrMeshData> out;
    out.reserve(_meshes.size());
    for (std::map<SdfPath, HdIrMeshData>::const_iterator it = _meshes.begin(); it != _meshes.end(); ++it)
      if (it->second.visible && !it->second.indices.empty()) out.push_back(it->second);
    if (version) *version = _version;
    return out;
  }

  uint64_t version() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _version;
  }

private:
  mutable std::mutex _mutex;
  std::map<SdfPath, HdIrMeshData> _meshes;
  std::map<SdfPath, HdIrLightData> _lights;
  std::map<SdfPath, HdIrMaterialData> _materials;
  std::map<SdfPath, HdIrVolumeData> _volumes;
  std::map<SdfPath, HdIrFieldRef> _fields;
  uint64_t _version = 0;
  HdIrTessellation _tess;
};

// Hydra's matrices are row-vector (p' = p * M), like Nuke's fdk::Mat4d, so our
// rows are its columns.
inline ir::Xform irXformOf(const GfMatrix4d& m)
{
  ir::Xform x;
  x.m[0] = float(m[0][0]); x.m[1] = float(m[1][0]); x.m[2] = float(m[2][0]); x.m[3] = float(m[3][0]);
  x.m[4] = float(m[0][1]); x.m[5] = float(m[1][1]); x.m[6] = float(m[2][1]); x.m[7] = float(m[3][1]);
  x.m[8] = float(m[0][2]); x.m[9] = float(m[1][2]); x.m[10] = float(m[2][2]); x.m[11] = float(m[3][2]);
  return x;
}

PXR_NAMESPACE_CLOSE_SCOPE
