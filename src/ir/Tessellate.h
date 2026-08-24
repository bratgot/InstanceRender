// InstanceRender - Tessellate.h
// Turning the two USD primitive types that are not meshes into meshes:
//
//   UsdGeomPoints      -> one sphere prototype, one instance per point, so a
//                         million particles cost a transform each rather than a
//                         sphere each.  That is the whole point of this renderer.
//   UsdGeomBasisCurves -> a tube per curve, swept along the evaluated basis.
//
// Both produce plain triangle soups in the same layout the mesh path uses, so
// materials, textures, motion blur and the AOVs all work without special cases.
//
// Strict ASCII.
#pragma once

#include "Math.h"

#include <vector>

namespace ir {

struct TriangleSoup {
  std::vector<Vec3>     points;
  std::vector<Vec3>     normals;
  std::vector<float>    uvs;        // 2 per point
  std::vector<uint32_t> indices;    // 3 per triangle, local to this soup
};

// A unit sphere centred on the origin.  'detail' picks the ring count: the
// silhouette of a point is what gives it away, so this is a knob, not a constant.
inline void buildUnitSphere(int detail, TriangleSoup& out)
{
  const int rings = (detail < 1) ? 1 : ((detail > 6) ? 6 : detail);
  const int nv = 4 + rings * 4;                 // 8, 12, 16 ... around
  const int nu = 2 + rings * 2;                 // rows of latitude
  out.points.clear(); out.normals.clear(); out.uvs.clear(); out.indices.clear();
  for (int j = 0; j <= nu; ++j) {
    const float v = float(j) / float(nu);
    const float theta = v * 3.14159265f;
    const float st = sinf(theta), ct = cosf(theta);
    for (int i = 0; i <= nv; ++i) {
      const float u = float(i) / float(nv);
      const float phi = u * 6.28318530f;
      const Vec3 n(st * cosf(phi), ct, st * sinf(phi));
      out.points.push_back(n);
      out.normals.push_back(n);
      out.uvs.push_back(u);
      out.uvs.push_back(1.0f - v);
    }
  }
  const int stride = nv + 1;
  for (int j = 0; j < nu; ++j) {
    for (int i = 0; i < nv; ++i) {
      const uint32_t a = uint32_t(j * stride + i), b = uint32_t(a + 1);
      const uint32_t c = uint32_t((j + 1) * stride + i), d = uint32_t(c + 1);
      if (j != 0) { out.indices.push_back(a); out.indices.push_back(c); out.indices.push_back(b); }
      if (j != nu - 1) { out.indices.push_back(b); out.indices.push_back(c); out.indices.push_back(d); }
    }
  }
}

// ---- curve bases ---------------------------------------------------------------
// Evaluate one span of four control points.  Bezier and the two cubic bases USD
// allows; 'linear' is handled by the caller, which just walks the points.
inline Vec3 evalCubic(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t, int basis)
{
  const float t2 = t * t, t3 = t2 * t;
  if (basis == 0) {           // bspline
    const float b0 = (-t3 + 3.0f * t2 - 3.0f * t + 1.0f) / 6.0f;
    const float b1 = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
    const float b2 = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
    const float b3 = t3 / 6.0f;
    return p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3;
  }
  if (basis == 1) {           // catmullRom
    const float b0 = 0.5f * (-t3 + 2.0f * t2 - t);
    const float b1 = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    const float b2 = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    const float b3 = 0.5f * (t3 - t2);
    return p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3;
  }
  const float mt = 1.0f - t;  // bezier
  return p0 * (mt * mt * mt) + p1 * (3.0f * mt * mt * t) + p2 * (3.0f * mt * t * t) + p3 * t3;
}

// A tube swept along 'centres' with 'radii', 'sides' vertices around.  The frame
// is carried along the curve (parallel transport) so the tube does not spin.
inline void buildTube(const std::vector<Vec3>& centres, const std::vector<float>& radii, int sides, TriangleSoup& out)
{
  const size_t n = centres.size();
  if (n < 2 || radii.size() != n) return;
  const int ns = (sides < 3) ? 3 : ((sides > 32) ? 32 : sides);
  const uint32_t base = uint32_t(out.points.size());

  Vec3 prevT = normalize(centres[1] - centres[0]);
  Vec3 ref, up;
  basis(prevT, ref, up);
  for (size_t i = 0; i < n; ++i) {
    Vec3 t = (i + 1 < n) ? (centres[i + 1] - centres[i]) : (centres[i] - centres[i - 1]);
    if (dot(t, t) < 1e-20f) t = prevT; else t = normalize(t);
    // rotate the reference frame onto the new tangent instead of rebuilding it
    const Vec3 axis = cross(prevT, t);
    if (dot(axis, axis) > 1e-16f) {
      const Vec3 a = normalize(axis);
      const float c = irClamp(dot(prevT, t), -1.0f, 1.0f);
      const float s = irSqrt(irMax(0.0f, 1.0f - c * c));
      // Rodrigues on the reference vector
      ref = ref * c + cross(a, ref) * s + a * (dot(a, ref) * (1.0f - c));
      ref = normalize(ref - t * dot(t, ref));
    }
    up = cross(t, ref);
    prevT = t;
    const float r = radii[i];
    const float v = float(i) / float(n - 1);
    for (int k = 0; k <= ns; ++k) {
      const float a = 6.28318530f * float(k) / float(ns);
      const Vec3 nrm = ref * cosf(a) + up * sinf(a);
      out.points.push_back(centres[i] + nrm * r);
      out.normals.push_back(nrm);
      out.uvs.push_back(float(k) / float(ns));
      out.uvs.push_back(v);
    }
  }
  const uint32_t stride = uint32_t(ns + 1);
  for (size_t i = 0; i + 1 < n; ++i) {
    for (int k = 0; k < ns; ++k) {
      const uint32_t a = base + uint32_t(i) * stride + uint32_t(k);
      const uint32_t b = a + 1;
      const uint32_t c = a + stride;
      const uint32_t d = c + 1;
      out.indices.push_back(a); out.indices.push_back(c); out.indices.push_back(b);
      out.indices.push_back(b); out.indices.push_back(c); out.indices.push_back(d);
    }
  }
}

} // namespace ir
