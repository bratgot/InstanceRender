// InstanceRender - HdIrSubdiv.h
// Catmull-Clark (and bilinear) subdivision for the delegate.
//
// The refinement itself is ir::subdivide() in src/ir/Subdivide.h, the same one
// the Nuke node uses, so a mesh limits to the same surface whichever front end
// rendered it.  What lives here is the Hydra side: reading the POLYGON topology
// and its crease tags off HdMeshTopology, and turning the refined mesh back
// into the triangles and per-corner attributes the render pass wants.
//
// The refinement has to happen on the polygon mesh, before anything is
// triangulated or split per corner: subdividing a triangulated soup gives the
// wrong limit surface, and splitting first welds uv seams shut.
//
// Strict ASCII.
#pragma once

#include "HdIrScene.h"

#include "ir/Subdivide.h"

#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

// How many levels this mesh should be refined by, given what the scene asks for
// and what the topology says.  0 = render the control cage.
inline int irSubdivLevels(const HdMeshTopology& topology, int wanted, bool* bilinearOut)
{
  *bilinearOut = false;
  if (wanted <= 0) return 0;
  const TfToken scheme = topology.GetScheme();
  int levels = 0;
  if (scheme == PxOsdOpenSubdivTokens->catmullClark || scheme.IsEmpty()) levels = wanted;
  else if (scheme == PxOsdOpenSubdivTokens->bilinear) { levels = wanted; *bilinearOut = true; }
  else return 0;                       // "none", "loop" and anything else: as authored

  // keep the refinement from exploding: corners x 4^levels
  const double corners = double(topology.GetFaceVertexIndices().size());
  while (levels > 0 && corners * std::pow(4.0, double(levels)) > 8.0e6) --levels;
  return levels;
}

// Refines the mesh and fills 'out' with one vertex per refined corner: after
// Catmull-Clark every face is a quad, and a quad becomes two triangles.  Per
// corner is what keeps a uv seam a seam.  'cornerSt' is the faceVarying st of
// the UNREFINED mesh (empty if it has none).
inline bool irSubdivide(const HdMeshTopology& topology,
                        const VtVec3fArray& points,
                        const VtVec2fArray& cornerSt,
                        int levels,
                        bool bilinear,
                        HdIrMeshData& out)
{
  const VtIntArray& fvc = topology.GetFaceVertexCounts();
  const VtIntArray& fvi = topology.GetFaceVertexIndices();
  if (points.empty() || fvc.empty() || fvi.empty() || levels <= 0) return false;

  ir::SubdivMesh sm;
  sm.points.resize(points.size());
  for (size_t i = 0; i < points.size(); ++i)
    sm.points[i] = ir::Vec3(points[i][0], points[i][1], points[i][2]);
  sm.faceVertexCounts.assign(fvc.begin(), fvc.end());
  sm.faceVertexIndices.assign(fvi.begin(), fvi.end());
  if (cornerSt.size() == fvi.size()) {
    sm.cornerUV.assign(cornerSt.size() * 2, 0.0f);
    for (size_t c = 0; c < cornerSt.size(); ++c) {
      sm.cornerUV[c * 2] = cornerSt[c][0];
      sm.cornerUV[c * 2 + 1] = cornerSt[c][1];
    }
  }

  // semi-sharp creases and pinned corners, as OpenSubdiv tags them
  {
    const PxOsdSubdivTags& tags = topology.GetSubdivTags();
    const VtIntArray& creaseIndices = tags.GetCreaseIndices();
    const VtIntArray& creaseLengths = tags.GetCreaseLengths();
    const VtFloatArray& creaseWeights = tags.GetCreaseWeights();
    size_t at = 0, run = 0;
    for (size_t g = 0; g < creaseLengths.size(); ++g) {
      const int len = creaseLengths[g];
      for (int e = 0; e + 1 < len; ++e) {
        if (at + size_t(e) + 1 >= creaseIndices.size()) break;
        // one weight per run, or one per edge
        float sh = 1e6f;
        if (creaseWeights.size() == creaseLengths.size()) sh = creaseWeights[g];
        else if (run < creaseWeights.size()) sh = creaseWeights[run];
        sm.creaseEdges.push_back(creaseIndices[at + size_t(e)]);
        sm.creaseEdges.push_back(creaseIndices[at + size_t(e) + 1]);
        sm.creaseSharpness.push_back(sh);
        ++run;
      }
      at += size_t(len > 0 ? len : 0);
    }
    const VtIntArray& cornerIndices = tags.GetCornerIndices();
    const VtFloatArray& cornerWeights = tags.GetCornerWeights();
    for (size_t c = 0; c < cornerIndices.size(); ++c) {
      sm.cornerPoints.push_back(cornerIndices[c]);
      sm.cornerSharpness.push_back(c < cornerWeights.size() ? cornerWeights[c] : 1e6f);
    }
  }

  ir::subdivide(sm, levels, bilinear);

  // authored normals mean nothing on a subdivision surface (USD says they only
  // apply to scheme "none"), so they come from the refined mesh
  std::vector<ir::Vec3> smooth;
  ir::computeSmoothNormals(sm, smooth);

  out.points.clear(); out.normals.clear(); out.uvs.clear(); out.indices.clear();
  const bool haveUV = (sm.cornerUV.size() == sm.faceVertexIndices.size() * 2);
  size_t c = 0;
  for (size_t f = 0; f < sm.faceVertexCounts.size(); ++f) {
    const int k = sm.faceVertexCounts[f];
    if (k < 3 || c + size_t(k) > sm.faceVertexIndices.size()) { c += size_t(k > 0 ? k : 0); continue; }
    const uint32_t base = uint32_t(out.points.size());
    for (int i = 0; i < k; ++i) {
      const int p = sm.faceVertexIndices[c + size_t(i)];
      out.points.push_back(sm.points[size_t(p)]);
      out.normals.push_back(size_t(p) < smooth.size() ? smooth[size_t(p)] : ir::Vec3(0.0f));
      if (haveUV) {
        out.uvs.push_back(sm.cornerUV[(c + size_t(i)) * 2]);
        out.uvs.push_back(sm.cornerUV[(c + size_t(i)) * 2 + 1]);
      }
    }
    for (int i = 1; i + 1 < k; ++i) {            // fan: a quad becomes two triangles
      out.indices.push_back(base);
      out.indices.push_back(base + uint32_t(i));
      out.indices.push_back(base + uint32_t(i + 1));
    }
    c += size_t(k);
  }
  if (!haveUV) out.uvs.clear();
  return !out.indices.empty();
}

PXR_NAMESPACE_CLOSE_SCOPE
