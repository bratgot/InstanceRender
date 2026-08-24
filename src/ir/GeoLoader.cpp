// InstanceRender - GeoLoader.cpp
// Nuke's classic 3D system -> ir::Scene.  See GeoLoader.h.
// Strict ASCII.
#include "GeoLoader.h"
#include "Image.h"
#include "Tessellate.h"
#include "Trace.h"
#include "Watchdog.h"

#include "DDImage/GeoOp.h"
#include "DDImage/GeoInfo.h"
#include "DDImage/GeometryList.h"
#include "DDImage/Scene.h"
#include "DDImage/Primitive.h"
#include "DDImage/Attribute.h"
#include "DDImage/LightOp.h"
#include "DDImage/ParticleOp.h"
#include "DDImage/Iop.h"
#include "DDImage/Knob.h"
#include "DDImage/Row.h"
#include "DDImage/Pixel.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <map>
#include <sstream>
#include <vector>

using namespace DD::Image;

namespace ir {

namespace {

// ---- small helpers ---------------------------------------------------------------
Xform xformOf(const Matrix4& m)
{
  // Nuke's Matrix4 is aRC (row R, column C) and transforms column vectors, which
  // is exactly what ir::Xform's 3x4 row-major layout wants.
  Xform x;
  x.m[0] = m.a00; x.m[1] = m.a01; x.m[2] = m.a02; x.m[3] = m.a03;
  x.m[4] = m.a10; x.m[5] = m.a11; x.m[6] = m.a12; x.m[7] = m.a13;
  x.m[8] = m.a20; x.m[9] = m.a21; x.m[10] = m.a22; x.m[11] = m.a23;
  return x;
}

Xform xformOf(const fdk::Mat4d& m)
{
  // fdk::Mat4d is row-vector (p' = p * M), so our rows are its columns
  Xform x;
  x.m[0] = float(m[0][0]); x.m[1] = float(m[1][0]); x.m[2] = float(m[2][0]); x.m[3] = float(m[3][0]);
  x.m[4] = float(m[0][1]); x.m[5] = float(m[1][1]); x.m[6] = float(m[2][1]); x.m[7] = float(m[3][1]);
  x.m[8] = float(m[0][2]); x.m[9] = float(m[1][2]); x.m[10] = float(m[2][2]); x.m[11] = float(m[3][2]);
  return x;
}

// FNV-1a over raw bytes: what makes two GeoInfos "the same mesh"
struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void add(const void* p, size_t n)
  {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  }
  void add(int v) { add(&v, sizeof(v)); }
  void add(float v) { add(&v, sizeof(v)); }
};

// A triangulated object, before it is either matched to an existing prototype or
// added as a new one.
struct Mesh {
  std::vector<Vec3>     points;
  std::vector<Vec3>     normals;
  std::vector<float>    uvs;      // 2 per point
  std::vector<Vec3>     colors;
  std::vector<uint32_t> indices;
  bool hasNormals = false, hasUVs = false, hasColors = false;
  uint64_t key = 0;
  Vec3 centre = Vec3(0.0f);   // the points are stored RELATIVE to this
  // what the object looked like, for the report when nothing comes out of it
  size_t numPoints = 0, numPrims = 0, numFaces = 0, badFaces = 0;
};

// Where an attribute is indexed from.  Classic Nuke can attach the same name to
// points, to vertices (face-varying) or to the whole object.
enum AttrSource { kAttrNone = 0, kAttrPoint, kAttrVertex, kAttrObject };

struct AttrRef {
  const Attribute* attr = nullptr;
  AttrSource source = kAttrNone;
  bool valid() const { return attr != nullptr && source != kAttrNone; }
};

AttrRef findAttribute(const GeoInfo& info, const char* name)
{
  AttrRef ref;
  static const int kGroups[3] = { DD::Image::Group_Vertices, DD::Image::Group_Points, DD::Image::Group_Object };
  static const AttrSource kSources[3] = { kAttrVertex, kAttrPoint, kAttrObject };
  for (int i = 0; i < 3; ++i) {
    const Attribute* a = info.get_group_attribute(kGroups[i], name);
    if (a && a->size() > 0) { ref.attr = a; ref.source = kSources[i]; return ref; }
  }
  return ref;
}

// index into an attribute for one corner of one face
size_t attrIndex(const AttrRef& ref, size_t pointIndex, size_t vertexIndex)
{
  switch (ref.source) {
    case kAttrPoint:  return pointIndex;
    case kAttrVertex: return vertexIndex;
    case kAttrObject: return 0;
    default:          return 0;
  }
}

Vec3 attrVec3(const AttrRef& ref, size_t i)
{
  if (!ref.valid() || i >= ref.attr->size()) return Vec3(0.0f);
  switch (ref.attr->type()) {
    case NORMAL_ATTRIB:
    case VECTOR3_ATTRIB: { const Vector3& v = ref.attr->vector3(i); return Vec3(v.x, v.y, v.z); }
    case VECTOR4_ATTRIB: { const Vector4& v = ref.attr->vector4(i); return Vec3(v.x, v.y, v.z); }
    default: return Vec3(0.0f);
  }
}

void attrUV(const AttrRef& ref, size_t i, float& u, float& v)
{
  u = 0.0f; v = 0.0f;
  if (!ref.valid() || i >= ref.attr->size()) return;
  if (ref.attr->type() == VECTOR4_ATTRIB) {
    const Vector4& t = ref.attr->vector4(i);
    // classic uv is (u, v, w, q) with q the homogeneous divisor
    const float q = (t.w != 0.0f) ? t.w : 1.0f;
    u = t.x / q; v = t.y / q;
  }
  else if (ref.attr->type() == VECTOR2_ATTRIB) {
    const Vector2& t = ref.attr->vector2(i);
    u = t.x; v = t.y;
  }
  else if (ref.attr->type() == VECTOR3_ATTRIB) {
    const Vector3& t = ref.attr->vector3(i);
    u = t.x; v = t.y;
  }
}

// ---- one GeoInfo -> one triangulated mesh -----------------------------------------
// Attributes that vary per FACE VERTEX cannot be shared between the triangles
// that meet at a point, so in that case every corner becomes its own vertex -
// the same split the USD front end does for faceVarying primvars.
bool triangulate(const GeoInfo& info, Mesh& out, size_t triangleBudget)
{
  const PointList* pl = info.point_list();
  out.numPoints = pl ? pl->size() : 0;
  out.numPrims = info.primitives();
  if (!pl || pl->empty()) return false;
  const unsigned nprims = info.primitives();
  if (nprims == 0) return false;

  const AttrRef nrm = findAttribute(info, kNormalAttrName);
  const AttrRef uv = findAttribute(info, kUVAttrName);
  const AttrRef col = findAttribute(info, kColorAttrName);
  const bool perCorner = (nrm.source == kAttrVertex) || (uv.source == kAttrVertex) || (col.source == kAttrVertex);

  out.hasNormals = nrm.valid();
  out.hasUVs = uv.valid();
  out.hasColors = col.valid();

  // IR_GEO_PROBE=1 dumps what each object actually carries
  if (getenv("IR_GEO_PROBE")) {
    std::ostringstream o;
    o << "GEO_PROBE object: src " << std::hex << info.src_id().value()
      << " out " << info.out_id().value() << std::dec
      << " points " << pl->size() << " prims " << nprims;
    for (unsigned pi = 0; pi < nprims && pi < 3; ++pi) {
      const Primitive* pr = info.primitive(pi);
      o << " | [" << pi << "] ";
      if (!pr) { o << "NULL"; continue; }
      o << pr->Class() << " faces " << pr->faces() << " verts " << pr->vertices()
        << " voffset " << pr->vertex_offset();
      if (pr->faces() > 0) o << " fv0 " << pr->face_vertices(0);
    }
    // everything the object carries, not just the names this loader looks for -
    // Nuke keeps a dedicated velocity reference (VEL_ref) and it is worth
    // seeing whether the particle system fills it in
    o << " | attribs " << info.get_attribcontext_count() << ":";
    for (int ai = 0; ai < info.get_attribcontext_count(); ++ai) {
      const DD::Image::AttribContext* ac = info.get_attribcontext(ai);
      if (!ac || !ac->name) continue;
      o << " " << ac->name << "(g" << int(ac->group) << " t" << int(ac->type)
        << " n" << (ac->attribute ? ac->attribute->size() : 0) << ")";
    }
    o << " | VEL_ref " << (info.VEL_ref ? (info.VEL_ref->name ? info.VEL_ref->name : "unnamed") : "none");
    // the value too, not just its presence: whether it is per frame or per
    // second decides the whole blur, and the only way to know is to compare it
    // against how far the thing actually moves
    if (const Attribute* va = info.get_group_attribute(DD::Image::Group_Object, "vel")) {
      if (va->size() > 0 && va->type() == VECTOR3_ATTRIB) {
        const Vector3& v = va->vector3(0);
        o << " vel=(" << v.x << "," << v.y << "," << v.z << ") |vel|="
          << std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
      }
    }
    o << " PW_ref " << (info.PW_ref ? "yes" : "no");

    // which of the attributes this loader cares about are present, and where
    static const char* kNames[6] = { "size", "Cf", "vel", "id", "N", "uv" };
    static const int kGroups[4] = { DD::Image::Group_Primitives, DD::Image::Group_Vertices,
                                    DD::Image::Group_Points, DD::Image::Group_Object };
    static const char* kGroupName[4] = { "prim", "vertex", "point", "object" };
    for (int n = 0; n < 6; ++n) {
      for (int g = 0; g < 4; ++g) {
        const Attribute* at = info.get_group_attribute(kGroups[g], kNames[n]);
        if (at && at->size() > 0)
          o << " | " << kGroupName[g] << ":" << kNames[n] << " type " << int(at->type()) << " n " << at->size();
      }
    }
    std::cerr << o.str() << std::endl;
  }

  Fnv fnv;
  fnv.add(int(pl->size()));
  fnv.add(int(nprims));
  fnv.add(int(perCorner ? 1 : 0));

  if (!perCorner) {
    // one vertex per point: the cheap case, and the one that instances well
    out.points.reserve(pl->size());
    for (size_t i = 0; i < pl->size(); ++i) {
      const Vector3& p = (*pl)[i];
      out.points.push_back(Vec3(p.x, p.y, p.z));
      out.normals.push_back(nrm.valid() ? attrVec3(nrm, attrIndex(nrm, i, 0)) : Vec3(0.0f));
      float u = 0.0f, v = 0.0f;
      if (uv.valid()) attrUV(uv, attrIndex(uv, i, 0), u, v);
      out.uvs.push_back(u); out.uvs.push_back(v);
      out.colors.push_back(col.valid() ? attrVec3(col, attrIndex(col, i, 0)) : Vec3(1.0f));
      fnv.add(p.x); fnv.add(p.y); fnv.add(p.z);
    }
  }

  std::vector<unsigned> face;
  for (unsigned pi = 0; pi < nprims; ++pi) {
    const Primitive* prim = info.primitive(pi);
    if (!prim) continue;
    const unsigned nfaces = prim->faces();
    const unsigned voffset = prim->vertex_offset();
    for (unsigned f = 0; f < nfaces; ++f) {
      const unsigned nfv = prim->face_vertices(int(f));
      if (nfv < 3) continue;
      ++out.numFaces;
      face.resize(nfv);
      prim->get_face_vertices(int(f), face.data());
      // fan from the first corner
      for (unsigned k = 1; k + 1 < nfv; ++k) {
        const unsigned corner[3] = { face[0], face[k], face[k + 1] };
        if (out.indices.size() / 3 >= triangleBudget) return !out.indices.empty();
        // get_face_vertices gives vertex indices within the primitive, but some
        // primitive types hand back indices already offset into the object's
        // vertex list - accept either rather than dropping the face
        bool bad = false;
        const size_t before = out.indices.size();
        for (int c = 0; c < 3 && !bad; ++c) {
          unsigned localVertex = corner[c];
          if (localVertex >= prim->vertices() && localVertex >= voffset
              && (localVertex - voffset) < prim->vertices())
            localVertex -= voffset;
          if (localVertex >= prim->vertices()) { bad = true; break; }
          const unsigned point = prim->vertex(localVertex);
          if (point >= pl->size()) { bad = true; break; }
          if (!perCorner) {
            out.indices.push_back(point);
            fnv.add(int(point));
          }
          else {
            const size_t vtx = size_t(voffset) + localVertex;
            const Vector3& p = (*pl)[point];
            out.indices.push_back(uint32_t(out.points.size()));
            out.points.push_back(Vec3(p.x, p.y, p.z));
            out.normals.push_back(nrm.valid() ? attrVec3(nrm, attrIndex(nrm, point, vtx)) : Vec3(0.0f));
            float u = 0.0f, v = 0.0f;
            if (uv.valid()) attrUV(uv, attrIndex(uv, point, vtx), u, v);
            out.uvs.push_back(u); out.uvs.push_back(v);
            out.colors.push_back(col.valid() ? attrVec3(col, attrIndex(col, point, vtx)) : Vec3(1.0f));
            fnv.add(p.x); fnv.add(p.y); fnv.add(p.z); fnv.add(u); fnv.add(v);
          }
        }
        if (bad) {
          ++out.badFaces;
          out.indices.resize(before);
          if (perCorner) {
            const size_t keep = before;   // the corners pushed for this triangle
            out.points.resize(keep); out.normals.resize(keep); out.colors.resize(keep);
            out.uvs.resize(keep * 2);
          }
        }
      }
    }
  }
  // Centre the mesh.  Nuke's own generators bake their transform knobs into the
  // points, so two Spheres a metre apart are "different geometry" until the
  // translation is taken back out - and it is exactly that case this renderer
  // wants to instance.  The offset goes into the instance transform instead.
  if (!out.points.empty()) {
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (size_t i = 0; i < out.points.size(); ++i) { cx += out.points[i].x; cy += out.points[i].y; cz += out.points[i].z; }
    const double inv = 1.0 / double(out.points.size());
    out.centre = Vec3(float(cx * inv), float(cy * inv), float(cz * inv));
    for (size_t i = 0; i < out.points.size(); ++i) out.points[i] = out.points[i] - out.centre;
  }

  // The hash is quantised so that the same mesh in two places matches despite
  // the last bits of the subtraction; a hit is verified point by point below,
  // so a collision costs a comparison, never the wrong geometry.
  float extent = 1e-6f;
  for (size_t i = 0; i < out.points.size(); ++i) {
    extent = irMax(extent, irAbs(out.points[i].x));
    extent = irMax(extent, irAbs(out.points[i].y));
    extent = irMax(extent, irAbs(out.points[i].z));
  }
  const float quant = extent * 1e-5f;
  fnv.add(int(out.points.size()));
  fnv.add(int(out.indices.size()));
  for (size_t i = 0; i < out.points.size(); ++i) {
    fnv.add(int(out.points[i].x / quant));
    fnv.add(int(out.points[i].y / quant));
    fnv.add(int(out.points[i].z / quant));
  }
  for (size_t i = 0; i < out.indices.size(); ++i) fnv.add(int(out.indices[i]));
  out.key = fnv.h;
  return !out.indices.empty();
}

// ---- particles ------------------------------------------------------------------
// Nuke's particles arrive as a primitive with vertices but NO faces (Particles,
// ParticlesSprite, Point), carrying a size, a colour and an id per point.  They
// become what UsdGeomPoints becomes on the USD side: ONE sphere prototype and a
// transform per particle, which is the whole reason this renderer exists.  A
// sprite is really a camera-facing card - drawing it as a sphere is the
// approximation a path tracer wants, and it keeps them instanced.
const char* const kVelAttrName = "vel";

// Nuke's particle geometry reaches a renderer carrying id, Cf and size, and
// nothing else - no velocity, with GeoInfo::VEL_ref empty.  Measured, on a
// scene of nothing but particles: "425 point(s), vel absent, VEL_ref none,
// attribs: id(g2 t5) Cf(g2 t3) size(g2 t0)".
//
// The velocity does exist; it just never leaves the particle SYSTEM, which is
// what CopyToPoints reads when its "read particle system" knob is on.  So ask
// the system directly, the same way, and match it to the points by the id they
// do carry.  Without this, particles rendered as spheres can only be blurred by
// pairing them across the shutter, and Nuke RECYCLES particle ids, so the
// pairing is wrong exactly where it matters - as particles die.
//
// Safe to call from here because loading now happens at validate time; walking
// the graph from a render thread is the deadlock fixed in 0.31.0.
void collectParticleVelocities(DD::Image::Op* op, std::map<int, Vec3>& out, int depth = 0)
{
  if (!op || depth > 8) return;
  if (DD::Image::ParticleRender* pr = dynamic_cast<DD::Image::ParticleRender*>(op)) {
    float prevTime = 0.0f, outTime = 0.0f;
    if (DD::Image::ParticleSystem* ps = pr->getParticleSystem(prevTime, outTime)) {
      const unsigned n = ps->numParticles();
      const bool* active = ps->particleActive();
      const int* ids = ps->particleId();
      const Vector3* vel = ps->particleVelocity();
      if (ids && vel) {
        for (unsigned i = 0; i < n; ++i) {
          if (active && !active[i]) continue;
          out[ids[i]] = Vec3(vel[i].x, vel[i].y, vel[i].z);
        }
      }
    }
  }
  for (int i = 0; i < op->inputs(); ++i) collectParticleVelocities(op->input(i), out, depth + 1);
}

// The object's own velocity, in units per FRAME, if it was given one.
// CopyToPoints writes it as an object attribute when its "copy attributes"
// knob names "vel", and Nuke then also points GeoInfo::VEL_ref at it.  This is
// worth far more than any amount of guessing by proximity: a velocity is the
// object's OWN account of where it is going, so it needs no partner at the
// other end of the shutter and is right for things being born or dying inside
// it - which is exactly what proximity matching cannot do.
bool objectVelocity(const GeoInfo& info, Vec3& out)
{
  const Attribute* va = info.get_group_attribute(DD::Image::Group_Object, kVelAttrName);
  if (!va || va->size() == 0 || va->type() != VECTOR3_ATTRIB) return false;
  const Vector3& v = va->vector3(0);
  out = Vec3(v.x, v.y, v.z);
  return true;
}

struct Particle {
  Vec3     P;
  float    radius;
  Vec3     color;
  bool     hasColor;
  uint64_t id;
  Vec3     vel;        // units per frame
  bool     hasVel;
};

bool isPointPrimitive(const Primitive* prim)
{
  return prim && prim->faces() == 0 && prim->vertices() > 0;
}

// Returns true if the object IS particles (so it has no faces to triangulate).
bool collectParticles(const GeoInfo& info, std::vector<Particle>& out,
                      const std::map<int, Vec3>* systemVel)
{
  const PointList* pl = info.point_list();
  if (!pl || pl->empty()) return false;
  const AttrRef size = findAttribute(info, kSizeAttrName);
  const AttrRef col = findAttribute(info, kColorAttrName);
  const AttrRef ids = findAttribute(info, "id");
  const AttrRef vel = findAttribute(info, kVelAttrName);
  if (getenv("IR_GEO_PROBE")) {
    std::cerr << "GEO_PROBE particles: " << pl->size() << " point(s), vel "
              << (vel.valid() ? "FOUND" : "absent")
              << ", VEL_ref " << (info.VEL_ref ? "set" : "none")
              << ", attribs:";
    for (int ai = 0; ai < info.get_attribcontext_count(); ++ai) {
      const DD::Image::AttribContext* ac = info.get_attribcontext(ai);
      if (ac && ac->name)
        std::cerr << " " << ac->name << "(g" << int(ac->group) << " t" << int(ac->type) << ")";
    }
    std::cerr << std::endl;
  }
  bool any = false;
  for (unsigned pi = 0; pi < info.primitives(); ++pi) {
    const Primitive* prim = info.primitive(pi);
    if (!isPointPrimitive(prim)) continue;
    any = true;
    for (unsigned v = 0; v < prim->vertices(); ++v) {
      const unsigned point = prim->vertex(v);
      if (point >= pl->size()) continue;
      const size_t vtx = size_t(prim->vertex_offset()) + v;
      Particle p;
      const Vector3& q = (*pl)[point];
      p.P = Vec3(q.x, q.y, q.z);
      float w = 0.1f;                       // 'size' is a width, like UsdGeomPoints' widths
      if (size.valid()) {
        const size_t i = attrIndex(size, point, vtx);
        if (size.attr->type() == FLOAT_ATTRIB && i < size.attr->size()) w = size.attr->flt(i);
        else { const Vec3 vv = attrVec3(size, i); if (vv.x > 0.0f) w = vv.x; }
      }
      p.radius = 0.5f * w;
      p.hasColor = col.valid();
      p.color = p.hasColor ? attrVec3(col, attrIndex(col, point, vtx)) : Vec3(1.0f);
      p.id = uint64_t(point);
      if (ids.valid() && ids.attr->type() == INT_ATTRIB) {
        const size_t i = attrIndex(ids, point, vtx);
        if (i < ids.attr->size()) p.id = uint64_t(uint32_t(ids.attr->integer(i)));
      }
      // a velocity attribute if the geometry brought one, otherwise the
      // particle system's own, found by the id this point carries
      p.hasVel = vel.valid();
      p.vel = p.hasVel ? attrVec3(vel, attrIndex(vel, point, vtx)) : Vec3(0.0f);
      if (!p.hasVel && systemVel && !systemVel->empty()) {
        const std::map<int, Vec3>::const_iterator it = systemVel->find(int(uint32_t(p.id)));
        if (it != systemVel->end()) { p.vel = it->second; p.hasVel = true; }
      }
      if (p.radius > 0.0f) out.push_back(p);
    }
  }
  return any;
}

// ---- materials -------------------------------------------------------------------
// A classic material is an Iop.  What it paints is its image, so it is baked into
// a texture here and sampled with the geometry's uvs - which is what the classic
// renderers do too, only per shading point.  Shaders that compute lighting of
// their own (Phong and friends) pass their input image through, so the texture
// still comes out right; their lighting model does not, and this renderer's own
// takes over.
bool bakeIopImpl(Iop* mat, int maxSize, ImageData& out)
{
  if (!mat) return false;
  try {
    mat->validate(true);
    const Box& bx = mat->info();
    const int x0 = bx.x(), y0 = bx.y();
    int w = bx.w(), h = bx.h();
    if (w <= 0 || h <= 0) return false;
    int step = 1;
    if (maxSize > 0) while ((w / step) > maxSize || (h / step) > maxSize) step *= 2;
    const int ow = std::max(1, w / step), oh = std::max(1, h / step);
    mat->request(x0, y0, x0 + w, y0 + h, Mask_RGBA, 1);
    out.width = ow; out.height = oh;
    out.rgba.assign(size_t(ow) * size_t(oh) * 4, 0.0f);
    Row row(x0, x0 + w);
    for (int oy = 0; oy < oh; ++oy) {
      // Nuke's y counts up from the bottom, ImageData's row 0 is the top
      const int sy = y0 + std::min(h - 1, oy * step);
      row.get(*mat, sy, x0, x0 + w, Mask_RGBA);
      const float* r = row[Chan_Red] + x0;
      const float* g = row[Chan_Green] + x0;
      const float* b = row[Chan_Blue] + x0;
      const float* a = row[Chan_Alpha] + x0;
      float* dst = &out.rgba[size_t(oh - 1 - oy) * size_t(ow) * 4];
      for (int ox = 0; ox < ow; ++ox) {
        const int sx = std::min(w - 1, ox * step);
        dst[ox * 4 + 0] = r ? r[sx] : 0.0f;
        dst[ox * 4 + 1] = g ? g[sx] : 0.0f;
        dst[ox * 4 + 2] = b ? b[sx] : 0.0f;
        dst[ox * 4 + 3] = a ? a[sx] : 1.0f;
      }
    }
    if (getenv("IR_GEO_PROBE") && out.valid()) {
      std::ostringstream o;
      o << "GEO_PROBE bake: " << mat->node_name() << " box " << x0 << "," << y0 << " " << w << "x" << h
        << " step " << step << " -> " << ow << "x" << oh << " row" << (oh / 2) << ":";
      const float* mid = &out.rgba[size_t(oh / 2) * size_t(ow) * 4];
      for (int i = 0; i < ow && i < 8; ++i) o << " " << mid[size_t(i * (ow / 8 > 0 ? ow / 8 : 1)) * 4];
      std::cerr << o.str() << std::endl;
    }
    return out.valid();
  }
  catch (...) { return false; }
}

} // namespace

bool bakeIop(DD::Image::Iop* op, int maxSize, ImageData& out) { return bakeIopImpl(op, maxSize, out); }

namespace {

// ---- classic shaders -------------------------------------------------------------
// Nuke's classic materials are Iops: their 2D output is only the image they
// carry, and their shading knobs mean something solely inside ScanlineRender.
// A path tracer cannot run that lighting model, but it can carry the INTENT -
// the colour, how shiny it is, what it emits - which is what this reads.  A
// Phong will not match ScanlineRender's falloff, and the README says so.
struct ClassicShader {
  bool known = false;
  bool hasDiffuse = false;
  Vec3 diffuse = Vec3(1.0f);
  Vec3 specular = Vec3(0.0f);
  Vec3 emission = Vec3(0.0f);
  float shininess = 0.0f;        // Nuke's Phong exponent; 0 = not authored
};

bool knobVec3(Op* op, const char* name, Vec3& out)
{
  if (!op) return false;
  Knob* k = op->knob(name);
  if (!k) return false;
  const fdk::Vec3d v = k->get<fdk::Vec3d>();
  out = Vec3(float(v.x), float(v.y), float(v.z));
  return true;
}

bool knobFloat(Op* op, const char* name, float& out)
{
  if (!op) return false;
  Knob* k = op->knob(name);
  if (!k) return false;
  out = float(k->get<double>());
  return true;
}

ClassicShader readClassicShader(Iop* op)
{
  ClassicShader sh;
  if (!op) return sh;
  const std::string cls = op->Class();
  Vec3 v;
  if (cls == "Phong" || cls == "BasicMaterial") {
    sh.known = true;
    if (knobVec3(op, "diffuse", v)) { sh.diffuse = v; sh.hasDiffuse = true; }
    // Phong carries a colour on top of its diffuse; BasicMaterial has none
    if (knobVec3(op, "color", v)) { sh.diffuse = sh.diffuse * v; sh.hasDiffuse = true; }
    if (knobVec3(op, "specular", v)) sh.specular = v;
    if (knobVec3(op, "emission", v)) sh.emission = v;
    float mn = 0.0f, mx = 0.0f;
    knobFloat(op, "min_shininess", mn);
    knobFloat(op, "max_shininess", mx);
    sh.shininess = (mx > 0.0f) ? mx : mn;
  }
  else if (cls == "Diffuse") {
    sh.known = true;
    if (knobVec3(op, "white", v)) { sh.diffuse = v; sh.hasDiffuse = true; }
  }
  else if (cls == "Emission") {
    sh.known = true;
    if (knobVec3(op, "emission", v)) sh.emission = v;
  }
  else if (cls == "Specular") {
    sh.known = true;
    if (knobVec3(op, "white", v)) sh.specular = v;
    float mn = 0.0f, mx = 0.0f;
    knobFloat(op, "min_shininess", mn);
    knobFloat(op, "max_shininess", mx);
    sh.shininess = (mx > 0.0f) ? mx : mn;
  }
  return sh;
}

// A Phong exponent as a GGX roughness.  The usual mapping, and it puts a
// shininess of 10 at 0.41 and 100 at 0.14 - blurry and tight respectively.
float roughnessFromShininess(float shininess)
{
  if (!(shininess > 0.0f)) return 0.5f;
  const float r = std::sqrt(2.0f / (shininess + 2.0f));
  return r < 0.02f ? 0.02f : (r > 1.0f ? 1.0f : r);
}

// ---- lights ----------------------------------------------------------------------
void collectLights(Op* root, std::vector<LightOp*>& out, int depth = 0)
{
  if (!root || depth > 32) return;
  if (LightOp* l = dynamic_cast<LightOp*>(root)) {
    if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
    return;
  }
  for (int i = 0; i < root->inputs(); ++i) collectLights(root->input(i), out, depth + 1);
}

Vec3 lightColor(LightOp* l)
{
  Vec3 c(1.0f);
  try {
    const Pixel& p = l->color();
    c = Vec3(p[Chan_Red], p[Chan_Green], p[Chan_Blue]);
  }
  catch (...) {}
  const float i = l->intensity();
  return c * i;
}

// Adds a mesh to the scene as a new prototype and returns its index.
int addPrototype(Scene& out, const Mesh& mesh, int materialId, const std::string& name)
{
  ProtoRange pr;
  pr.firstVertex = int(out.vertices.size());
  pr.numVertices = int(mesh.points.size());
  pr.firstTri = int(out.indices.size() / 3);
  pr.numTris = int(mesh.indices.size() / 3);
  pr.hasNormals = mesh.hasNormals ? 1 : 0;
  pr.hasUVs = mesh.hasUVs ? 1 : 0;
  pr.hasColors = mesh.hasColors ? 1 : 0;
  pr.cryptoId = 0.0f;
  const uint32_t base = uint32_t(out.vertices.size());
  out.vertices.insert(out.vertices.end(), mesh.points.begin(), mesh.points.end());
  out.normals.insert(out.normals.end(), mesh.normals.begin(), mesh.normals.end());
  out.uvs.insert(out.uvs.end(), mesh.uvs.begin(), mesh.uvs.end());
  out.colors.insert(out.colors.end(), mesh.colors.begin(), mesh.colors.end());
  for (size_t i = 0; i < mesh.indices.size(); ++i) out.indices.push_back(base + mesh.indices[i]);
  for (int t = 0; t < pr.numTris; ++t) out.triMaterial.push_back(materialId);
  const int id = int(out.protos.size());
  out.protos.push_back(pr);
  out.protoNames.push_back(name);
  return id;
}

// Is this mesh the one already stored as prototype 'id'?  Called only when the
// hashes agree, so it is cheap in practice and makes a collision harmless.
bool sameAsPrototype(const Scene& scene, int id, const Mesh& mesh)
{
  const ProtoRange& pr = scene.protos[size_t(id)];
  if (size_t(pr.numVertices) != mesh.points.size()) return false;
  if (size_t(pr.numTris) * 3 != mesh.indices.size()) return false;
  const uint32_t base = uint32_t(pr.firstVertex);
  for (size_t i = 0; i < mesh.indices.size(); ++i)
    if (scene.indices[size_t(pr.firstTri) * 3 + i] != base + mesh.indices[i]) return false;
  float extent = 1e-6f;
  for (size_t i = 0; i < mesh.points.size(); ++i) {
    extent = irMax(extent, irAbs(mesh.points[i].x));
    extent = irMax(extent, irAbs(mesh.points[i].y));
    extent = irMax(extent, irAbs(mesh.points[i].z));
  }
  const float tol = extent * 1e-4f;
  for (size_t i = 0; i < mesh.points.size(); ++i) {
    const Vec3& a = scene.vertices[size_t(pr.firstVertex) + i];
    const Vec3& b = mesh.points[i];
    if (irAbs(a.x - b.x) > tol || irAbs(a.y - b.y) > tol || irAbs(a.z - b.z) > tol) return false;
  }
  return true;
}

} // namespace

// ------------------------------------------------------------------------------------
bool loadClassicGeometry(GeoOp* geo, Op* lightSearchRoot, const GeoLoaderOptions& opt, Scene& out, std::string& err)
{
  // Start from nothing.  This node keeps its Scene between renders, so appending
  // to whatever was there would add another whole copy of the geometry every
  // time - which in a viewer, where progressive refinement re-renders again and
  // again, grows the BVH until Nuke stops responding.  (loadStage() does the
  // same thing on the USD side.)
  out = Scene();
  if (!geo) { err = "no classic geometry connected to scn"; return false; }
  try {
    if (opt.aborted && opt.aborted()) { trace("classic: cancelled before validating the input"); err = "render cancelled"; return false; }
    trace("classic: validate input");
    WatchdogPhase wpValidate("classic load: validate the geometry input");
    geo->validate(true);
    trace("classic: build_scene");
    WatchdogPhase wpBuild("classic load: build_scene");
    DD::Image::Scene nukeScene;
    geo->build_scene(nukeScene);
    GeometryList* objects = nukeScene.object_list();
    const unsigned nobjects = objects ? unsigned(objects->size()) : 0u;
    trace("classic: build_scene done, " + std::to_string(nobjects) + " object(s)");

    // the particle systems feeding this scene, asked once
    std::map<int, Vec3> systemVel;
    collectParticleVelocities(lightSearchRoot ? lightSearchRoot : geo, systemVel);
    if (!systemVel.empty())
      trace("classic: " + std::to_string(systemVel.size()) + " particle velocities from the system");
    if (nobjects == 0) { err = "the classic scene has no objects"; return false; }

    // ---- materials, one per distinct material Iop ------------------------------
    std::map<Iop*, int> materialIds;
    const int defaultMaterial = int(out.materials.size());
    {
      Material m;
      m.useDisplayColor = 1;      // the geometry's own Cf, or white
      out.materials.push_back(m);
      out.materialNames.push_back("<default>");
    }

    size_t triangleBudget = size_t(opt.maxTriangles > 0.0 ? opt.maxTriangles : 5e8);
    std::map<uint64_t, std::vector<int> > protoByKey;
    int sphereProto = -1;            // the one prototype every particle shares
    size_t numParticles = 0;
    std::map<const PointList*, int> protoByPoints;   // shared caches: the exact case
    size_t instanced = 0, baked = 0, skipped = 0;

    for (unsigned o = 0; o < nobjects; ++o) {
      // a heartbeat through the object loop: a trace that simply stops here
      // cannot say whether the loop is stuck on one object or crawling through
      // all of them, and those have completely different causes
      if ((o % 256u) == 0u) {
        trace("classic: object " + std::to_string(o) + "/" + std::to_string(nobjects));
        if (opt.aborted && opt.aborted()) { trace("classic: cancelled"); err = "render cancelled"; return false; }
      }
      GeoInfo& info = objects->object(o);

      // ---- the material this object wears --------------------------------------
      int materialId = defaultMaterial;
      Iop* matOp = info.material;
      if (matOp) {
        std::map<Iop*, int>::iterator it = materialIds.find(matOp);
        if (it != materialIds.end()) materialId = it->second;
        else {
          Material m;
          m.useDisplayColor = 1;
          // what the shader itself says, before any image it carries
          const ClassicShader sh = readClassicShader(matOp);
          if (sh.known) {
            m.useDisplayColor = 0;
            if (sh.hasDiffuse) m.diffuse = sh.diffuse;
            m.emissive = sh.emission;
            // Nuke's "specular" is a multiplier on its own Phong term, not a
            // reflectance: handing it straight over as F0 turns a default
            // material into a mirror and blows the highlight out (measured at
            // 2.25 against 0.36 for the same card unshaded).  A dielectric
            // sits near 0.04, so that is what a full-strength specular means.
            m.useSpecularWorkflow = 1;
            m.specularColor = sh.specular * 0.04f;
            m.roughness = roughnessFromShininess(sh.shininess);
          }
          if (opt.textures) {
            ImageData img;
            // A shader with no image input has no pixels - its box comes back
            // 1x1 - and baking that would paint everything with one black
            // texel.  Its colour is in the knobs above instead.
            trace("classic: baking material " + std::string(matOp->node_name()));
            const bool bakedOk = bakeIopImpl(matOp, opt.maxTextureSize, img);
            trace(std::string("classic: baked ") + (bakedOk ? "ok" : "no") + " "
                  + std::to_string(img.width) + "x" + std::to_string(img.height));
            // A bake that FAILED is worth coming back for, the same as on the
            // USD path: the op may simply have been mid evaluation.  Coming back
            // 1x1 is not a failure - that is a shader with no image input, whose
            // colour is in the knobs.
            if (!bakedOk) ++out.nukeTextureFailures;
            if (bakedOk && (img.width > 1 || img.height > 1)) {
              std::vector<ImageData> levels;
              const int mips = opt.mipFilter ? buildMipChain(img, levels) : 1;
              TextureDesc td;
              td.width = img.width; td.height = img.height;
              td.firstTexel = int(out.texels.size() / 4);
              td.mipCount = std::max(1, mips);
              if (td.mipCount > 1) {
                for (int L = 0; L < td.mipCount && L < kMaxMipLevels; ++L) {
                  td.mipOffset[L] = int(out.texels.size() / 4);
                  td.mipW[L] = levels[L].width; td.mipH[L] = levels[L].height;
                  out.texels.insert(out.texels.end(), levels[L].rgba.begin(), levels[L].rgba.end());
                }
              }
              else {
                td.mipOffset[0] = td.firstTexel; td.mipW[0] = img.width; td.mipH[0] = img.height;
                out.texels.insert(out.texels.end(), img.rgba.begin(), img.rgba.end());
              }
              m.diffuseTex.index = int(out.textures.size());
              m.useDisplayColor = 0;
              // the texture IS the albedo; a shader's own colour tints it, the
              // same way a UsdUVTexture's scale does
              if (sh.hasDiffuse) m.diffuseTex.scale = Vec4(sh.diffuse.x, sh.diffuse.y, sh.diffuse.z, 1.0f);
              m.diffuse = Vec3(1.0f);
              out.textures.push_back(td);
              out.textureNames.push_back(matOp->node_name());
              ++baked;
            }
          }
          materialId = int(out.materials.size());
          out.materials.push_back(m);
          out.materialNames.push_back(matOp->node_name());
          materialIds[matOp] = materialId;
        }
      }

      // ---- particles -----------------------------------------------------------
      {
        std::vector<Particle> particles;
        if (collectParticles(info, particles, &systemVel)) {
          if (sphereProto < 0 && !particles.empty()) {
            TriangleSoup sphere;
            buildUnitSphere(opt.pointDetail, sphere);
            Mesh m;
            m.points = sphere.points;
            m.normals = sphere.normals;
            m.uvs = sphere.uvs;
            m.indices = sphere.indices;
            m.colors.assign(sphere.points.size(), Vec3(1.0f));
            m.hasNormals = true; m.hasUVs = true; m.hasColors = true;
            sphereProto = addPrototype(out, m, materialId, "<particle sphere>");
          }
          const Xform objXf = xformOf(info.matrix);
          for (size_t i = 0; i < particles.size(); ++i) {
            const Particle& pt = particles[i];
            Instance in;
            // the object's transform, scaled to the particle and moved to it
            const Vec3 world = objXf.point(pt.P);
            for (int r = 0; r < 3; ++r)
              for (int c = 0; c < 3; ++c) in.xf.m[r * 4 + c] = objXf.m[r * 4 + c] * pt.radius;
            in.xf.m[3] = world.x; in.xf.m[7] = world.y; in.xf.m[11] = world.z;
            in.xf1 = in.xf;
            in.firstKey = 0;
            in.protoId = sphereProto;
            in.hasColor = pt.hasColor ? 1 : 0;
            in.color = pt.color;
            in.instanceId = int(pt.id);
            in.materialOverride = -1;
            in.opacity = 1.0f;
            out.instances.push_back(in);
            out.instanceMatchKey.push_back((uint64_t(o) << 40) | (pt.id & 0xffffffffffull));
            out.instanceVel.push_back(pt.vel);
            out.instanceVelValid.push_back(pt.hasVel ? 1 : 0);
            if (pt.hasVel) out.hasVelocities = true;
            out.matchKeysAreIds = true;      // point particles carry Nuke's own id
          }
          numParticles += particles.size();
          continue;      // particles have no faces to triangulate
        }
      }

      // ---- geometry ------------------------------------------------------------
      Mesh mesh;
      if (!triangulate(info, mesh, triangleBudget)) {
        ++skipped;
        std::ostringstream w;
        w << "object " << o << " rendered nothing: " << mesh.numPoints << " point(s), "
          << mesh.numPrims << " primitive(s), " << mesh.numFaces << " face(s)";
        if (mesh.badFaces > 0) w << ", " << mesh.badFaces << " face(s) with out-of-range vertices";
        out.warnings += w.str() + "\n";
        continue;
      }
      if (mesh.badFaces > 0) {
        std::ostringstream w;
        w << "object " << o << ": " << mesh.badFaces << " face(s) skipped, out-of-range vertices\n";
        out.warnings += w.str();
      }
      // the material is part of what makes two objects the same prototype
      const uint64_t key = mesh.key ^ (uint64_t(materialId + 1) * 1099511628211ull);

      int protoId = -1;
      // Copies made by a node that references one cache (CopyToPoints does this)
      // share their point list outright - no hashing needed, and no doubt.
      const PointList* sharedPoints = info.point_list();
      {
        std::map<const PointList*, int>::iterator sit = protoByPoints.find(sharedPoints);
        if (sit != protoByPoints.end() && sameAsPrototype(out, sit->second, mesh)) {
          protoId = sit->second;
          ++instanced;
        }
      }
      if (protoId < 0) {
        std::vector<int>& candidates = protoByKey[key];
        for (size_t c = 0; c < candidates.size(); ++c) {
          if (sameAsPrototype(out, candidates[c], mesh)) { protoId = candidates[c]; ++instanced; break; }
        }
      }
      if (protoId < 0) {
        protoId = addPrototype(out, mesh, materialId, "object " + std::to_string(o));
        const size_t tris = mesh.indices.size() / 3;
        triangleBudget = (triangleBudget >= tris) ? (triangleBudget - tris) : 0;
        protoByKey[key].push_back(protoId);
        protoByPoints[sharedPoints] = protoId;
      }

      Instance in;
      in.xf = xformOf(info.matrix);
      if (mesh.centre.x != 0.0f || mesh.centre.y != 0.0f || mesh.centre.z != 0.0f) {
        // the prototype was centred, so put the offset back here
        const Vec3 t = in.xf.point(mesh.centre);
        in.xf.m[3] = t.x; in.xf.m[7] = t.y; in.xf.m[11] = t.z;
      }
      in.xf1 = in.xf;
      in.firstKey = 0;
      in.protoId = protoId;
      in.hasColor = 0;
      in.instanceId = int(o);
      in.materialOverride = -1;
      in.color = Vec3(1.0f);
      in.opacity = 1.0f;
      out.instances.push_back(in);
      out.instanceMatchKey.push_back(uint64_t(o) << 40);
      Vec3 ov(0.0f);
      const bool haveVel = objectVelocity(info, ov);
      out.instanceVel.push_back(ov);
      out.instanceVelValid.push_back(haveVel ? 1 : 0);
      if (haveVel) out.hasVelocities = true;
    }

    if (out.instances.empty()) { err = "no renderable geometry in the classic scene"; return false; }

    // ---- lights ----------------------------------------------------------------
    // validate() below is the deadlock the aborted() hook exists for
    if (opt.aborted && opt.aborted()) { trace("classic: cancelled before the lights"); err = "render cancelled"; return false; }
    if (opt.lights) {
      std::vector<LightOp*> lightOps;
      for (size_t i = 0; i < nukeScene.lights.size(); ++i) {
        if (nukeScene.lights[i] && nukeScene.lights[i]->light()) lightOps.push_back(nukeScene.lights[i]->light());
      }
      if (lightOps.empty()) collectLights(lightSearchRoot ? lightSearchRoot : geo, lightOps);
      for (size_t i = 0; i < lightOps.size(); ++i) {
        LightOp* lo = lightOps[i];
        if (!lo) continue;
        if (opt.aborted && opt.aborted()) { trace("classic: cancelled among the lights"); err = "render cancelled"; return false; }
        {
          WatchdogPhase wpLight("classic load: validate a light");
          try { lo->validate(true); } catch (...) { continue; }
        }
        const Xform x = xformOf(lo->worldTransform());
        Light L;
        L.color = lightColor(lo);
        L.position = Vec3(x.m[3], x.m[7], x.m[11]);
        // an Axis looks down its own -Z
        L.direction = normalize(Vec3(-x.m[2], -x.m[6], -x.m[10]));
        switch (lo->lightType()) {
          case LightOp::eDirectionalLight:
            L.type = kLightDistant;
            L.angle = 0.5f;
            break;
          case LightOp::eSpotLight: {
            L.type = kLightPoint;
            const float cone = float(lo->hfov()) * 0.5f;
            L.coneCos = std::cos(irClamp(cone, 0.0f, 179.0f) * 3.14159265f / 180.0f);
            L.coneCosInner = std::cos(irClamp(cone * 0.8f, 0.0f, 179.0f) * 3.14159265f / 180.0f);
            break;
          }
          default:
            // A classic point light has no size, and this renderer's point light
            // falls off with the square of the distance whether or not Nuke's
            // "falloff" knob is on - a path tracer has no way to express light
            // that does not, so intensities usually want raising relative to
            // ScanlineRender.
            L.type = kLightPoint;
            break;
        }
        L.shadowEnable = lo->cast_shadows() ? 1 : 0;
        L.shadowColor = Vec3(0.0f);
        L.diffuseMul = 1.0f; L.specularMul = 1.0f;
        L.visibleToCamera = 0;
        out.lights.push_back(L);
        out.lightNames.push_back(lo->node_name());
      }
    }

    std::ostringstream info;
    info << "classic 3D: " << out.instances.size() << " object(s) in " << out.protos.size() << " prototype(s)";
    if (numParticles > 0) info << ", " << numParticles << " particle(s) as spheres";
    if (instanced > 0) info << " (" << instanced << " instanced)";
    info << ", " << out.numTriangles() << " triangle(s)";
    if (baked > 0) info << ", " << baked << " material(s) baked to textures";
    if (!out.lights.empty()) info << ", " << out.lights.size() << " light(s)";
    if (skipped > 0) info << ", " << skipped << " object(s) with nothing to render";
    out.info += (out.info.empty() ? "" : "; ") + info.str();
    return true;
  }
  catch (const std::exception& e) { err = std::string("exception while reading classic geometry: ") + e.what(); return false; }
  catch (...) { err = "unknown exception while reading classic geometry"; return false; }
}

} // namespace ir
