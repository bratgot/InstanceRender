// Reading OpenVDB grids into the dense arrays the kernel marches through.
//
// WHY THIS IS ITS OWN FILE.  Two front ends need it and they arrive by
// completely different routes: the NODE walks a UsdVolVolume's field
// relationships in StageLoader, while the HYDRA DELEGATE is handed the same
// grids as HdField bprims by whatever host is driving it.  They share the
// reading, the frame resolution and - most importantly - the CACHE, because a
// 67 MB explosion re-read on every Sync is exactly the stall that "holds a
// frame for a long time" was.
//
// Hio's OpenVDB reader is a PLUGIN found through USD's registry, so a failure
// here is often nothing to do with the file; callers report rather than assume.
// Strict ASCII.
#pragma once

#include "Scene.h"

#include <pxr/imaging/hio/fieldTextureData.h>

#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace ir {

// One grid's voxels, kept whole so a sequence rolls through the cache and a
// frame revisited during playback is free.  Keyed by RESOLVED path plus grid.
struct VolCacheEntry {
  std::string key;
  std::vector<float> voxels;
  int nx, ny, nz;
  Vec3 bmin, bmax;
  VolCacheEntry() : nx(0), ny(0), nz(0), bmin(0.0f), bmax(0.0f) {}
};

inline std::mutex& volCacheMutex() { static std::mutex m; return m; }
inline std::vector<VolCacheEntry>& volCache() { static std::vector<VolCacheEntry> c; return c; }

inline bool volCacheGet(const std::string& key, VolCacheEntry& out)
{
  std::lock_guard<std::mutex> lock(volCacheMutex());
  std::vector<VolCacheEntry>& c = volCache();
  for (size_t i = 0; i < c.size(); ++i) {
    if (c[i].key != key) continue;
    out = c[i];
    // most recently used last, so the eviction below drops the coldest
    VolCacheEntry e = c[i];
    c.erase(c.begin() + i);
    c.push_back(e);
    return true;
  }
  return false;
}

inline void volCachePut(const VolCacheEntry& e, size_t budgetBytes)
{
  std::lock_guard<std::mutex> lock(volCacheMutex());
  std::vector<VolCacheEntry>& c = volCache();
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i].key == e.key) { c.erase(c.begin() + i); break; }
  c.push_back(e);
  size_t total = 0;
  for (size_t i = 0; i < c.size(); ++i) total += c[i].voxels.size() * sizeof(float);
  while (c.size() > 1 && total > budgetBytes) {
    total -= c.front().voxels.size() * sizeof(float);
    c.erase(c.begin());
  }
}

// A .vdb path with the frame filled in.
//
// The stage carries the path UNRESOLVED - AerialExplosion_%04d.vdb - because a
// resolved one goes stale: the layer persists and the authoring engine does not
// re-run for a frame it has already built, so a path baked at frame 60 was still
// there on the way back to frame 20.  Resolving here instead means the frame
// being RENDERED always decides which file is opened.
inline std::string resolveVdbFrame(const std::string& path, int frame)
{
  if (path.empty()) return path;
  const size_t pc = path.find('%');
  if (pc != std::string::npos) {
    size_t e = pc + 1;
    while (e < path.size() && isdigit((unsigned char)path[e])) ++e;
    if (e < path.size() && (path[e] == 'd' || path[e] == 'i')) {
      int width = 0;
      bool zero = false;
      size_t q = pc + 1;
      if (q < path.size() && path[q] == '0') { zero = true; ++q; }
      while (q < e) { width = width * 10 + (path[q] - '0'); ++q; }
      char num[64];
      if (zero && width > 0) std::snprintf(num, sizeof(num), "%0*d", width, frame);
      else                   std::snprintf(num, sizeof(num), "%d", frame);
      return path.substr(0, pc) + num + path.substr(e + 1);
    }
  }
  const size_t hs = path.find('#');
  if (hs != std::string::npos) {
    size_t he = hs;
    while (he < path.size() && path[he] == '#') ++he;
    char num[64];
    std::snprintf(num, sizeof(num), "%0*d", int(he - hs), frame);
    return path.substr(0, hs) + num + path.substr(he);
  }
  return path;
}

// Read one grid and append its voxels to the scene, filling in a GridRef.
// False means the grid is not there or Hio could not decode it - the caller
// knows which of its own fields that was and can say so usefully.
inline bool readVdbGrid(Scene& scene, const std::string& path, const std::string& gridName,
                        int budgetMB, GridRef& out, size_t* voxelsAdded = nullptr)
{
  if (path.empty() || gridName.empty()) return false;
  const size_t budget = size_t(budgetMB > 0 ? budgetMB : 1) * 1024u * 1024u;
  VolCacheEntry ce;
  const std::string ckey = path + "|" + gridName;
  if (!volCacheGet(ckey, ce)) {
    HioFieldTextureDataSharedPtr data =
      HioFieldTextureData::New(path, gridName, 0, std::string(), budget);
    if (!data || !data->Read() || !data->HasRawBuffer()) return false;
    if (data->GetFormat() != HioFormatFloat32) return false;
    ce.key = ckey;
    ce.nx = data->ResizedWidth(); ce.ny = data->ResizedHeight(); ce.nz = data->ResizedDepth();
    const size_t cnv = size_t(ce.nx) * size_t(ce.ny) * size_t(ce.nz);
    if (cnv == 0) return false;
    const GfRange3d cr = data->GetBoundingBox().ComputeAlignedRange();
    ce.bmin = Vec3(float(cr.GetMin()[0]), float(cr.GetMin()[1]), float(cr.GetMin()[2]));
    ce.bmax = Vec3(float(cr.GetMax()[0]), float(cr.GetMax()[1]), float(cr.GetMax()[2]));
    const float* csrc = reinterpret_cast<const float*>(data->GetRawBuffer());
    ce.voxels.assign(csrc, csrc + cnv);
    volCachePut(ce, budget);
  }
  out.nx = ce.nx; out.ny = ce.ny; out.nz = ce.nz;
  out.bmin = ce.bmin; out.bmax = ce.bmax;
  out.firstVoxel = int(scene.voxels.size());
  scene.voxels.insert(scene.voxels.end(), ce.voxels.begin(), ce.voxels.end());
  if (voxelsAdded) *voxelsAdded += ce.voxels.size();
  return true;
}

// The peak of a grid already read into the scene.  Used to tell an artist that a
// blackbody grid is not in Kelvin, which otherwise shows up only as a flat colour.
inline float gridPeak(const Scene& scene, const GridRef& g)
{
  const size_t nv = size_t(g.nx) * size_t(g.ny) * size_t(g.nz);
  float mx = 0.0f;
  for (size_t i = 0; i < nv; ++i) {
    const float v = scene.voxels[size_t(g.firstVoxel) + i];
    if (v > mx) mx = v;
  }
  return mx;
}

} // namespace ir
