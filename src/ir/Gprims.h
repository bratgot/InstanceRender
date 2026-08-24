// The analytic gprims: Sphere, Cube, Cylinder, Cone and Capsule.
//
// USD can say "a sphere of radius 2" without any points at all, and a stage
// written by hand or by a DCC's default primitives is full of them.  This
// renderer traces triangles, so they are tessellated at load - which is what
// every triangle-based renderer does with them, and is why the tessellation is
// a knob rather than a constant: the silhouette is what gives a coarse one away.
//
// The schemas are old and simple, but each has its own idea of what its
// parameters mean, and getting one wrong is the sort of thing that looks nearly
// right: a cone's height is its FULL height about the origin, a capsule's is the
// cylindrical part ONLY with the hemispheres added on top of it, and every one
// of them can be built along X, Y or Z.
// Strict ASCII.
#pragma once

#include "Tessellate.h"

#include <cmath>
#include <string>
#include <vector>

namespace ir {

// Turn a soup built along +Z into one built along the axis a prim asked for.
// The gprim schemas all default to Z, and a stage that says otherwise means it.
inline void gprimAxis(TriangleSoup& s, const std::string& axis)
{
  if (axis == "Z" || axis.empty()) return;
  for (size_t i = 0; i < s.points.size(); ++i) {
    const Vec3 p = s.points[i];
    const Vec3 n = s.normals.empty() ? Vec3(0.0f) : s.normals[i];
    if (axis == "X") {
      s.points[i] = Vec3(p.z, p.y, -p.x);
      if (!s.normals.empty()) s.normals[i] = Vec3(n.z, n.y, -n.x);
    }
    else if (axis == "Y") {
      s.points[i] = Vec3(p.x, p.z, -p.y);
      if (!s.normals.empty()) s.normals[i] = Vec3(n.x, n.z, -n.y);
    }
  }
}

inline void gprimSphere(double radius, int detail, TriangleSoup& out)
{
  buildUnitSphere(detail, out);
  for (size_t i = 0; i < out.points.size(); ++i) out.points[i] = out.points[i] * float(radius);
}

// A cube of the given SIZE - the full edge length, not a half extent.
inline void gprimCube(double size, TriangleSoup& out)
{
  const float h = float(size) * 0.5f;
  static const float fn[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
  for (int face = 0; face < 6; ++face) {
    const Vec3 n(fn[face][0], fn[face][1], fn[face][2]);
    // two vectors spanning the face, right handed about the normal
    Vec3 u, v;
    if (std::fabs(n.z) > 0.5f)      { u = Vec3(n.z, 0.0f, 0.0f); v = Vec3(0.0f, 1.0f, 0.0f); }
    else if (std::fabs(n.x) > 0.5f) { u = Vec3(0.0f, 0.0f, -n.x); v = Vec3(0.0f, 1.0f, 0.0f); }
    else                            { u = Vec3(1.0f, 0.0f, 0.0f); v = Vec3(0.0f, 0.0f, -n.y); }
    const uint32_t base = uint32_t(out.points.size());
    const float su[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float sv[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int c = 0; c < 4; ++c) {
      out.points.push_back(n * h + u * (su[c] * h) + v * (sv[c] * h));
      out.normals.push_back(n);
      out.uvs.push_back(su[c] * 0.5f + 0.5f);
      out.uvs.push_back(sv[c] * 0.5f + 0.5f);
    }
    out.indices.push_back(base); out.indices.push_back(base + 1); out.indices.push_back(base + 2);
    out.indices.push_back(base); out.indices.push_back(base + 2); out.indices.push_back(base + 3);
  }
}

// Shared by cylinder, cone and capsule.
namespace gp {

inline void ring(TriangleSoup& s, double r, double z, const Vec3& n, int sides, bool radialN)
{
  for (int i = 0; i <= sides; ++i) {
    const float t = float(i) / float(sides);
    const float a = t * 6.2831853f;
    const float c = std::cos(a), si = std::sin(a);
    s.points.push_back(Vec3(float(r) * c, float(r) * si, float(z)));
    s.normals.push_back(radialN ? Vec3(c, si, 0.0f) : n);
    s.uvs.push_back(t);
    s.uvs.push_back(float(z));
  }
}

inline void band(TriangleSoup& s, uint32_t a, uint32_t b, int sides)
{
  for (int i = 0; i < sides; ++i) {
    s.indices.push_back(a + uint32_t(i));
    s.indices.push_back(b + uint32_t(i));
    s.indices.push_back(b + uint32_t(i) + 1u);
    s.indices.push_back(a + uint32_t(i));
    s.indices.push_back(b + uint32_t(i) + 1u);
    s.indices.push_back(a + uint32_t(i) + 1u);
  }
}

inline void cap(TriangleSoup& s, double r, double z, const Vec3& n, int sides, bool flip)
{
  const uint32_t centre = uint32_t(s.points.size());
  s.points.push_back(Vec3(0.0f, 0.0f, float(z)));
  s.normals.push_back(n);
  s.uvs.push_back(0.5f); s.uvs.push_back(0.5f);
  const uint32_t first = uint32_t(s.points.size());
  ring(s, r, z, n, sides, false);
  for (int i = 0; i < sides; ++i) {
    if (flip) {
      s.indices.push_back(centre);
      s.indices.push_back(first + uint32_t(i) + 1u);
      s.indices.push_back(first + uint32_t(i));
    }
    else {
      s.indices.push_back(centre);
      s.indices.push_back(first + uint32_t(i));
      s.indices.push_back(first + uint32_t(i) + 1u);
    }
  }
}

} // namespace gp

// height is the FULL height, centred on the origin.
inline void gprimCylinder(double radius, double height, int sides, TriangleSoup& out)
{
  const double h = height * 0.5;
  const uint32_t a = uint32_t(out.points.size());
  gp::ring(out, radius, -h, Vec3(0.0f), sides, true);
  const uint32_t b = uint32_t(out.points.size());
  gp::ring(out, radius, h, Vec3(0.0f), sides, true);
  gp::band(out, a, b, sides);
  gp::cap(out, radius, h, Vec3(0.0f, 0.0f, 1.0f), sides, false);
  gp::cap(out, radius, -h, Vec3(0.0f, 0.0f, -1.0f), sides, true);
}

// A cone stands on its base at -height/2 and comes to a point at +height/2.
inline void gprimCone(double radius, double height, int sides, TriangleSoup& out)
{
  const double h = height * 0.5;
  // the side normal leans by the slope, so a cone does not shade like a cylinder
  const double slope = (height != 0.0) ? radius / height : 0.0;
  const uint32_t a = uint32_t(out.points.size());
  for (int i = 0; i <= sides; ++i) {
    const float t = float(i) / float(sides);
    const float ang = t * 6.2831853f;
    const float c = std::cos(ang), si = std::sin(ang);
    Vec3 n(c, si, float(slope));
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (l > 0.0f) n = n * (1.0f / l);
    out.points.push_back(Vec3(float(radius) * c, float(radius) * si, float(-h)));
    out.normals.push_back(n);
    out.uvs.push_back(t); out.uvs.push_back(0.0f);
  }
  const uint32_t b = uint32_t(out.points.size());
  for (int i = 0; i <= sides; ++i) {
    const float t = float(i) / float(sides);
    const float ang = t * 6.2831853f;
    Vec3 n(std::cos(ang), std::sin(ang), float(slope));
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (l > 0.0f) n = n * (1.0f / l);
    out.points.push_back(Vec3(0.0f, 0.0f, float(h)));
    out.normals.push_back(n);
    out.uvs.push_back(t); out.uvs.push_back(1.0f);
  }
  gp::band(out, a, b, sides);
  gp::cap(out, radius, -h, Vec3(0.0f, 0.0f, -1.0f), sides, true);
}

// A capsule's HEIGHT is the cylindrical part only; the two hemispheres are added
// to it, so the thing is height + 2 * radius long overall.  Reading it as the
// full length makes every capsule too short by a diameter.
inline void gprimCapsule(double radius, double height, int sides, int rings, TriangleSoup& out)
{
  const double h = height * 0.5;
  if (rings < 2) rings = 2;
  const uint32_t a = uint32_t(out.points.size());
  gp::ring(out, radius, -h, Vec3(0.0f), sides, true);
  const uint32_t b = uint32_t(out.points.size());
  gp::ring(out, radius, h, Vec3(0.0f), sides, true);
  gp::band(out, a, b, sides);
  // the two hemispheres, walked from the seam out to the pole
  for (int hemi = 0; hemi < 2; ++hemi) {
    const double zc = (hemi == 0) ? h : -h;
    const double dir = (hemi == 0) ? 1.0 : -1.0;
    uint32_t prev = (hemi == 0) ? b : a;
    for (int r = 1; r <= rings; ++r) {
      const double phi = (double(r) / double(rings)) * 1.5707963;
      const double rr = radius * std::cos(phi);
      const double zz = zc + dir * radius * std::sin(phi);
      const uint32_t cur = uint32_t(out.points.size());
      for (int i = 0; i <= sides; ++i) {
        const float t = float(i) / float(sides);
        const float ang = t * 6.2831853f;
        const float c = std::cos(ang), si = std::sin(ang);
        out.points.push_back(Vec3(float(rr) * c, float(rr) * si, float(zz)));
        Vec3 n(float(std::cos(phi)) * c, float(std::cos(phi)) * si, float(dir * std::sin(phi)));
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        out.normals.push_back(l > 0.0f ? n * (1.0f / l) : Vec3(0.0f, 0.0f, float(dir)));
        out.uvs.push_back(t);
        out.uvs.push_back(float(r) / float(rings));
      }
      if (hemi == 0) gp::band(out, prev, cur, sides);
      else           gp::band(out, cur, prev, sides);
      prev = cur;
    }
  }
}

} // namespace ir
