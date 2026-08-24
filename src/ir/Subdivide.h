// InstanceRender - Subdivide.h
// Catmull-Clark (and bilinear) subdivision of a polygon mesh.
//
// Nuke ships pxOsd but not the OpenSubdiv headers it includes, so this is a
// self-contained implementation.  It runs on POLYGON topology, before the
// per-corner vertex splitting in the stage loader: subdividing an already
// triangulated, already split soup gives the wrong limit surface and welds uv
// seams back together.
//
// Attributes travel as per-corner (faceVarying) arrays and are refined
// linearly - which is what keeps a uv seam a seam, since each face refines its
// own corners.  Positions use the Catmull-Clark rules (or midpoints for the
// bilinear scheme).  Authored normals are meaningless on a subdivision surface
// (the USD spec says they only apply to scheme "none"), so the caller drops
// them and recomputes from the refined mesh.
//
// Strict ASCII.
#pragma once

#include "Math.h"

#include <map>
#include <utility>
#include <vector>

namespace ir {

struct SubdivMesh {
  std::vector<Vec3>  points;
  std::vector<int>   faceVertexCounts;
  std::vector<int>   faceVertexIndices;   // one entry per corner
  std::vector<Vec3>  cornerColor;         // empty, or one per corner
  std::vector<float> cornerUV;            // empty, or two per corner
  // semi-sharp creases: pairs of point indices with a sharpness each, and
  // single points pinned by a corner sharpness.  Sharpness drops by one per
  // level, so 1.0 is sharp for exactly one refinement and 10 is effectively
  // infinite.
  std::vector<int>   creaseEdges;         // 2 point indices per crease edge
  std::vector<float> creaseSharpness;     // one per crease edge
  std::vector<int>   cornerPoints;
  std::vector<float> cornerSharpness;
};

namespace subdiv_detail {

struct EdgeInfo {
  int index;          // edge id
  int faces[2];       // adjacent faces, -1 when absent
  int a, b;           // endpoints
};

inline uint64_t edgeKey(int a, int b)
{
  const uint32_t lo = uint32_t(a < b ? a : b), hi = uint32_t(a < b ? b : a);
  return (uint64_t(hi) << 32) | uint64_t(lo);
}

} // namespace subdiv_detail

// One level of refinement.  Every face becomes one quad per corner.
inline void subdivideOnce(SubdivMesh& m, bool bilinear)
{
  using namespace subdiv_detail;
  // sharpness per edge and per point, looked up while the rules are applied
  std::map<uint64_t, float> creaseMap;
  for (size_t i = 0; i * 2 + 1 < m.creaseEdges.size() && i < m.creaseSharpness.size(); ++i) {
    const float sh = m.creaseSharpness[i];
    if (sh <= 0.0f) continue;
    const uint64_t key = edgeKey(m.creaseEdges[i * 2], m.creaseEdges[i * 2 + 1]);
    std::map<uint64_t, float>::iterator it = creaseMap.find(key);
    if (it == creaseMap.end() || it->second < sh) creaseMap[key] = sh;
  }
  std::map<int, float> cornerMap;
  for (size_t i = 0; i < m.cornerPoints.size() && i < m.cornerSharpness.size(); ++i)
    if (m.cornerSharpness[i] > 0.0f) cornerMap[m.cornerPoints[i]] = m.cornerSharpness[i];
  const size_t nPoints = m.points.size();
  const size_t nFaces = m.faceVertexCounts.size();
  if (nPoints == 0 || nFaces == 0) return;
  const bool hasColor = !m.cornerColor.empty();
  const bool hasUV = !m.cornerUV.empty();

  // ---- corner offsets + edges ---------------------------------------------------
  std::vector<int> faceStart(nFaces + 1, 0);
  for (size_t f = 0; f < nFaces; ++f) faceStart[f + 1] = faceStart[f] + m.faceVertexCounts[f];
  const size_t nCorners = size_t(faceStart[nFaces]);
  if (m.faceVertexIndices.size() < nCorners) return;

  std::map<uint64_t, EdgeInfo> edges;
  std::vector<int> cornerEdge(nCorners, -1);          // edge of corner i -> corner i+1
  for (size_t f = 0; f < nFaces; ++f) {
    const int k = m.faceVertexCounts[f];
    for (int i = 0; i < k; ++i) {
      const int c = faceStart[f] + i;
      const int a = m.faceVertexIndices[size_t(c)];
      const int b = m.faceVertexIndices[size_t(faceStart[f] + (i + 1) % k)];
      const uint64_t key = edgeKey(a, b);
      std::map<uint64_t, EdgeInfo>::iterator it = edges.find(key);
      if (it == edges.end()) {
        EdgeInfo e;
        e.index = int(edges.size());
        e.faces[0] = int(f); e.faces[1] = -1;
        e.a = a; e.b = b;
        it = edges.insert(std::make_pair(key, e)).first;
      }
      else if (it->second.faces[1] < 0 && it->second.faces[0] != int(f)) {
        it->second.faces[1] = int(f);
      }
      cornerEdge[size_t(c)] = it->second.index;
    }
  }
  const size_t nEdges = edges.size();

  // ---- face points --------------------------------------------------------------
  std::vector<Vec3> facePoint(nFaces, Vec3(0.0f));
  for (size_t f = 0; f < nFaces; ++f) {
    const int k = m.faceVertexCounts[f];
    Vec3 sum(0.0f);
    for (int i = 0; i < k; ++i) sum += m.points[size_t(m.faceVertexIndices[size_t(faceStart[f] + i)])];
    facePoint[f] = (k > 0) ? sum * (1.0f / float(k)) : sum;
  }

  // ---- edge points --------------------------------------------------------------
  std::vector<Vec3> edgePoint(nEdges, Vec3(0.0f));
  std::vector<float> edgeSharp(nEdges, 0.0f);
  std::vector<Vec3> edgeMid(nEdges, Vec3(0.0f));
  std::vector<int>  edgeBoundary(nEdges, 0);
  std::vector<int>  edgeA(nEdges, 0), edgeB(nEdges, 0);
  for (std::map<uint64_t, EdgeInfo>::const_iterator it = edges.begin(); it != edges.end(); ++it) {
    const EdgeInfo& e = it->second;
    const Vec3 pa = m.points[size_t(e.a)], pb = m.points[size_t(e.b)];
    const Vec3 mid = (pa + pb) * 0.5f;
    edgeMid[size_t(e.index)] = mid;
    edgeA[size_t(e.index)] = e.a; edgeB[size_t(e.index)] = e.b;
    const bool boundary = (e.faces[1] < 0);
    edgeBoundary[size_t(e.index)] = boundary ? 1 : 0;
    float sharp = 0.0f;
    {
      std::map<uint64_t, float>::const_iterator cit = creaseMap.find(it->first);
      if (cit != creaseMap.end()) sharp = cit->second;
    }
    edgeSharp[size_t(e.index)] = sharp;
    if (bilinear || boundary) edgePoint[size_t(e.index)] = mid;
    else {
      const Vec3 smooth = (pa + pb + facePoint[size_t(e.faces[0])] + facePoint[size_t(e.faces[1])]) * 0.25f;
      // a sharpness of 1 or more keeps the edge crisp, in between blends
      const float w = (sharp >= 1.0f) ? 1.0f : ((sharp > 0.0f) ? sharp : 0.0f);
      edgePoint[size_t(e.index)] = (w > 0.0f) ? vlerp(smooth, mid, w) : smooth;
    }
  }

  // ---- vertex points ------------------------------------------------------------
  std::vector<Vec3> vertQ(nPoints, Vec3(0.0f)), vertR(nPoints, Vec3(0.0f)), boundarySum(nPoints, Vec3(0.0f));
  std::vector<int>  valence(nPoints, 0), edgeCount(nPoints, 0), boundaryCount(nPoints, 0);
  std::vector<Vec3> creaseSum(nPoints, Vec3(0.0f));
  std::vector<int>  creaseCount(nPoints, 0);
  std::vector<float> creaseSharpSum(nPoints, 0.0f);
  for (size_t f = 0; f < nFaces; ++f) {
    const int k = m.faceVertexCounts[f];
    for (int i = 0; i < k; ++i) {
      const int v = m.faceVertexIndices[size_t(faceStart[f] + i)];
      vertQ[size_t(v)] += facePoint[f];
      ++valence[size_t(v)];
    }
  }
  for (size_t e = 0; e < nEdges; ++e) {
    const int a = edgeA[e], b = edgeB[e];
    vertR[size_t(a)] += edgeMid[e]; ++edgeCount[size_t(a)];
    vertR[size_t(b)] += edgeMid[e]; ++edgeCount[size_t(b)];
    if (edgeBoundary[e]) {
      boundarySum[size_t(a)] += edgeMid[e]; ++boundaryCount[size_t(a)];
      boundarySum[size_t(b)] += edgeMid[e]; ++boundaryCount[size_t(b)];
    }
    if (edgeSharp[e] > 0.0f) {
      creaseSum[size_t(a)] += edgeMid[e]; ++creaseCount[size_t(a)]; creaseSharpSum[size_t(a)] += edgeSharp[e];
      creaseSum[size_t(b)] += edgeMid[e]; ++creaseCount[size_t(b)]; creaseSharpSum[size_t(b)] += edgeSharp[e];
    }
  }
  std::vector<Vec3> vertexPoint(nPoints, Vec3(0.0f));
  for (size_t v = 0; v < nPoints; ++v) {
    const Vec3 P = m.points[v];
    if (bilinear) { vertexPoint[v] = P; continue; }
    // pinned corner, or three creases meeting: the point does not move
    std::map<int, float>::const_iterator corner = cornerMap.find(int(v));
    if ((corner != cornerMap.end() && corner->second >= 1.0f) || creaseCount[v] >= 3) { vertexPoint[v] = P; continue; }
    if (creaseCount[v] == 2) {
      // crease rule: the point follows the crease, (m1 + m2 + 6P) / 8
      // (m1 + m2 + 6P) / 8 - creaseSum holds the SUM of the two midpoints, so it
      // is averaged first; scaling it by 2 instead pushes the point 25% outward
      const Vec3 crease = (creaseSum[v] * (2.0f / float(creaseCount[v])) + P * 6.0f) * 0.125f;
      const float sh = creaseSharpSum[v] * 0.5f;
      if (sh >= 1.0f) { vertexPoint[v] = crease; continue; }
      Vec3 smooth = P;
      if (valence[v] > 0 && edgeCount[v] > 0) {
        const float n = float(valence[v]);
        smooth = (vertQ[v] * (1.0f / n) + (vertR[v] * (1.0f / float(edgeCount[v]))) * 2.0f + P * (n - 3.0f)) * (1.0f / n);
      }
      vertexPoint[v] = vlerp(smooth, crease, sh);
      continue;
    }
    if (boundaryCount[v] >= 2) {
      // boundary: (m1 + m2 + 6P) / 8, the standard crease rule
      vertexPoint[v] = (boundarySum[v] * (2.0f / float(boundaryCount[v])) + P * 6.0f) * 0.125f;
    }
    else if (valence[v] > 0 && edgeCount[v] > 0) {
      const float n = float(valence[v]);
      const Vec3 Q = vertQ[v] * (1.0f / n);
      const Vec3 R = vertR[v] * (1.0f / float(edgeCount[v]));
      vertexPoint[v] = (Q + R * 2.0f + P * (n - 3.0f)) * (1.0f / n);
    }
    else vertexPoint[v] = P;
  }

  // ---- assemble -----------------------------------------------------------------
  SubdivMesh out;
  out.points.reserve(nPoints + nEdges + nFaces);
  out.points.insert(out.points.end(), vertexPoint.begin(), vertexPoint.end());
  out.points.insert(out.points.end(), edgePoint.begin(), edgePoint.end());
  out.points.insert(out.points.end(), facePoint.begin(), facePoint.end());
  const int edgeBase = int(nPoints), faceBase = int(nPoints + nEdges);

  out.faceVertexCounts.reserve(nCorners);
  out.faceVertexIndices.reserve(nCorners * 4);
  if (hasColor) out.cornerColor.reserve(nCorners * 4);
  if (hasUV) out.cornerUV.reserve(nCorners * 8);

  for (size_t f = 0; f < nFaces; ++f) {
    const int k = m.faceVertexCounts[f];
    if (k < 3) continue;
    // per-face attribute averages (linear refinement, per face, so seams stay)
    Vec3 faceCol(0.0f);
    float faceU = 0.0f, faceV = 0.0f;
    for (int i = 0; i < k; ++i) {
      const size_t c = size_t(faceStart[f] + i);
      if (hasColor) faceCol += m.cornerColor[c];
      if (hasUV) { faceU += m.cornerUV[c * 2]; faceV += m.cornerUV[c * 2 + 1]; }
    }
    const float inv = 1.0f / float(k);
    faceCol = faceCol * inv; faceU *= inv; faceV *= inv;

    for (int i = 0; i < k; ++i) {
      const size_t c = size_t(faceStart[f] + i);
      const size_t cNext = size_t(faceStart[f] + (i + 1) % k);
      const size_t cPrev = size_t(faceStart[f] + (i + k - 1) % k);
      const int eNext = cornerEdge[c];         // corner i -> i+1
      const int ePrev = cornerEdge[cPrev];     // corner i-1 -> i
      out.faceVertexCounts.push_back(4);
      out.faceVertexIndices.push_back(m.faceVertexIndices[c]);
      out.faceVertexIndices.push_back(edgeBase + eNext);
      out.faceVertexIndices.push_back(faceBase + int(f));
      out.faceVertexIndices.push_back(edgeBase + ePrev);
      if (hasColor) {
        const Vec3 ci = m.cornerColor[c];
        out.cornerColor.push_back(ci);
        out.cornerColor.push_back((ci + m.cornerColor[cNext]) * 0.5f);
        out.cornerColor.push_back(faceCol);
        out.cornerColor.push_back((ci + m.cornerColor[cPrev]) * 0.5f);
      }
      if (hasUV) {
        const float u = m.cornerUV[c * 2], v = m.cornerUV[c * 2 + 1];
        out.cornerUV.push_back(u); out.cornerUV.push_back(v);
        out.cornerUV.push_back(0.5f * (u + m.cornerUV[cNext * 2])); out.cornerUV.push_back(0.5f * (v + m.cornerUV[cNext * 2 + 1]));
        out.cornerUV.push_back(faceU); out.cornerUV.push_back(faceV);
        out.cornerUV.push_back(0.5f * (u + m.cornerUV[cPrev * 2])); out.cornerUV.push_back(0.5f * (v + m.cornerUV[cPrev * 2 + 1]));
      }
    }
  }
  // creases carry down a level, one sharpness weaker, split at the edge point
  {
    std::vector<int> childEdges;
    std::vector<float> childSharp;
    for (std::map<uint64_t, EdgeInfo>::const_iterator it = edges.begin(); it != edges.end(); ++it) {
      const EdgeInfo& e = it->second;
      const float sh = edgeSharp[size_t(e.index)] - 1.0f;
      if (sh <= 0.0f) continue;
      const int mid = edgeBase + e.index;
      childEdges.push_back(e.a); childEdges.push_back(mid); childSharp.push_back(sh);
      childEdges.push_back(mid); childEdges.push_back(e.b); childSharp.push_back(sh);
    }
    std::vector<int> childCorners;
    std::vector<float> childCornerSharp;
    for (std::map<int, float>::const_iterator it = cornerMap.begin(); it != cornerMap.end(); ++it) {
      const float sh = it->second - 1.0f;
      if (sh <= 0.0f) continue;
      childCorners.push_back(it->first);          // vertex points keep their index
      childCornerSharp.push_back(sh);
    }
    m.creaseEdges.swap(childEdges);
    m.creaseSharpness.swap(childSharp);
    m.cornerPoints.swap(childCorners);
    m.cornerSharpness.swap(childCornerSharp);
  }
  m.points.swap(out.points);
  m.faceVertexCounts.swap(out.faceVertexCounts);
  m.faceVertexIndices.swap(out.faceVertexIndices);
  m.cornerColor.swap(out.cornerColor);
  m.cornerUV.swap(out.cornerUV);
}

inline void subdivide(SubdivMesh& m, int levels, bool bilinear)
{
  for (int i = 0; i < levels; ++i) subdivideOnce(m, bilinear);
}

// smooth per-point normals of a polygon mesh (Newell per face, accumulated)
inline void computeSmoothNormals(const SubdivMesh& m, std::vector<Vec3>& normals)
{
  normals.assign(m.points.size(), Vec3(0.0f));
  size_t c = 0;
  for (size_t f = 0; f < m.faceVertexCounts.size(); ++f) {
    const int k = m.faceVertexCounts[f];
    if (k >= 3 && c + size_t(k) <= m.faceVertexIndices.size()) {
      Vec3 n(0.0f);
      for (int i = 0; i < k; ++i) {
        const Vec3& p0 = m.points[size_t(m.faceVertexIndices[c + size_t(i)])];
        const Vec3& p1 = m.points[size_t(m.faceVertexIndices[c + size_t((i + 1) % k)])];
        n.x += (p0.y - p1.y) * (p0.z + p1.z);
        n.y += (p0.z - p1.z) * (p0.x + p1.x);
        n.z += (p0.x - p1.x) * (p0.y + p1.y);
      }
      for (int i = 0; i < k; ++i) normals[size_t(m.faceVertexIndices[c + size_t(i)])] += n;
    }
    c += size_t(k > 0 ? k : 0);
  }
  for (size_t i = 0; i < normals.size(); ++i)
    if (dot(normals[i], normals[i]) > 1e-20f) normals[i] = normalize(normals[i]);
}

} // namespace ir
