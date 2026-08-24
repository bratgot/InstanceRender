// InstanceRender - Kernel.h
// The renderer proper: camera rays, surface reconstruction from a hit,
// UsdPreviewSurface-style BSDF, next-event estimation and a small path
// tracer.  Header-only, host + device, templated on a Tracer that provides
//   bool closest(const Ray&, HitRecord&) and bool occluded(const Ray&)
// so the Embree and OptiX back-ends run the *same* code (CPU/GPU parity).
// Strict ASCII.
#pragma once

#include "Math.h"
#include "Scene.h"
#include "Crypto.h"

namespace ir {

// The transform an instance has at shutter time t: with more than two keys the
// shutter is split into equal segments and the pair around t is interpolated,
// which is exactly what Embree's time steps and OptiX's matrix motion do.
IR_HD Xform instanceXformAt(const SceneView& sv, const Instance& inst, float t)
{
  const int n = sv.motionKeyCount;
  if (n < 2 || !sv.motionKeys) return inst.xfAt(t);
  const float u = irClamp(t, 0.0f, 1.0f) * float(n - 1);
  int k = int(u);
  if (k > n - 2) k = n - 2;
  if (k < 0) k = 0;
  const float f = u - float(k);
  const Xform& a = sv.motionKeys[inst.firstKey + k];
  const Xform& b = sv.motionKeys[inst.firstKey + k + 1];
  Xform x;
  for (int i = 0; i < 12; ++i) x.m[i] = a.m[i] + (b.m[i] - a.m[i]) * f;
  return x;
}

struct HitRecord {
  int   instId;      // index into SceneView::instances
  int   primId;      // triangle index within the prototype (0-based)
  float t;
  float u, v;        // barycentrics of vertices 1 and 2 (Embree / OptiX convention)
  IR_HD HitRecord() : instId(-1), primId(-1), t(0.0f), u(0.0f), v(0.0f) {}
};

struct Surface {
  float lod;         // mip level for this hit, from the ray's footprint
  Vec3 P, Ng, Ns;    // world position, geometric normal, shading normal (both facing the incoming ray side)
  Vec3 albedo;       // diffuse colour after displayColor / instance colour
  Vec3 emissive;
  Vec3 F0;           // specular reflectance at normal incidence
  float roughness;
  float metallic;
  float opacity;
  float diffuseWeight;
  float clearcoat;          // strength of the second, smooth specular lobe
  float clearcoatRoughness;
  float occlusion;          // baked AO, applied to INDIRECT light only
  float u, v;        // surface uv (USD st)
  int instanceId;
  float cryptoId;    // this copy's cryptomatte id, already hashed on the host
  int materialId;
  int objectId;      // prototype index: every copy of one mesh shares it
  bool backface;
};

// ---- camera ------------------------------------------------------------------------
IR_HD Ray cameraRay(const Camera& cam, float px, float py)
{
  // px, py in pixels with (0,0) bottom-left of the image, +y up
  const float sx = (px / float(cam.width)) * 2.0f - 1.0f;
  const float sy = (py / float(cam.height)) * 2.0f - 1.0f;
  Vec3 o, d;
  if (cam.orthographic) {
    o = Vec3(sx * cam.orthoHalfW, sy * cam.orthoHalfH, 0.0f);
    d = Vec3(0.0f, 0.0f, -1.0f);
  }
  else {
    o = Vec3(0.0f, 0.0f, 0.0f);
    d = normalize(Vec3(sx * cam.tanHalfFovX, sy * cam.tanHalfFovY, -1.0f));
  }
  Ray r;
  r.o = cam.camToWorld.point(o);
  r.d = normalize(cam.camToWorld.vector(d));
  r.tmin = cam.nearClip;
  r.tmax = cam.farClip;
  return r;
}


// The camera at a moment of the open shutter.  Lerped the same way an instance
// transform is - the twelve numbers, straight - which is right for a pan or a
// dolly and close enough for the amount a camera rotates inside one shutter.
// The field of view is carried along too, so a zoom during the shutter blurs.
IR_HD Camera cameraAt(const SceneView& sv, float t)
{
  if (!sv.cameraMoves) return sv.camera;
  Camera c = sv.camera;
  const Camera& b = sv.cameraClose;
  for (int i = 0; i < 12; ++i) c.camToWorld.m[i] = sv.camera.camToWorld.m[i] + (b.camToWorld.m[i] - sv.camera.camToWorld.m[i]) * t;
  c.tanHalfFovX = sv.camera.tanHalfFovX + (b.tanHalfFovX - sv.camera.tanHalfFovX) * t;
  c.tanHalfFovY = sv.camera.tanHalfFovY + (b.tanHalfFovY - sv.camera.tanHalfFovY) * t;
  c.orthoHalfW  = sv.camera.orthoHalfW  + (b.orthoHalfW  - sv.camera.orthoHalfW)  * t;
  c.orthoHalfH  = sv.camera.orthoHalfH  + (b.orthoHalfH  - sv.camera.orthoHalfH)  * t;
  return c;
}

// ---- volumes -------------------------------------------------------------------------
// The ray is put into the GRID's own space, so the box test is exact whatever
// transform the prim carries and a rotated volume needs no special case.
IR_HD bool volumeSlab(const VolumeGrid& g, const Vec3& o, const Vec3& d,
                      float tmin, float tmax, float& t0, float& t1)
{
  // The union of the two frames' boxes: an explosion grows, so the shutter-close
  // frame reaches further than the open one and marching only the open box would
  // clip the leading edge of every streak.
  Vec3 bmin = g.density[0].bmin, bmax = g.density[0].bmax;
  if (g.density[1].valid()) {
    bmin = Vec3(irMin(bmin.x, g.density[1].bmin.x), irMin(bmin.y, g.density[1].bmin.y), irMin(bmin.z, g.density[1].bmin.z));
    bmax = Vec3(irMax(bmax.x, g.density[1].bmax.x), irMax(bmax.y, g.density[1].bmax.y), irMax(bmax.z, g.density[1].bmax.z));
  }
  const Vec3 lo = g.worldToLocal.point(o);
  const Vec3 ld = g.worldToLocal.vector(d);
  t0 = tmin; t1 = tmax;
  for (int a = 0; a < 3; ++a) {
    const float oa = (a == 0) ? lo.x : (a == 1) ? lo.y : lo.z;
    const float da = (a == 0) ? ld.x : (a == 1) ? ld.y : ld.z;
    const float mn = (a == 0) ? bmin.x : (a == 1) ? bmin.y : bmin.z;
    const float mx = (a == 0) ? bmax.x : (a == 1) ? bmax.y : bmax.z;
    if (irAbs(da) < 1e-12f) { if (oa < mn || oa > mx) return false; continue; }
    const float inv = 1.0f / da;
    float ta = (mn - oa) * inv, tb = (mx - oa) * inv;
    if (ta > tb) { const float tmp = ta; ta = tb; tb = tmp; }
    if (ta > t0) t0 = ta;
    if (tb < t1) t1 = tb;
    if (t0 > t1) return false;
  }
  return t1 > t0;
}

// Trilinear, on a flat array: no texture object, because the same code has to
// compile for the CPU and for the device and a cudaTextureObject would fork it.
IR_HD float volumeSampleGrid(const float* v, const GridRef& r, const Vec3& p)
{
  if (!r.valid()) return 0.0f;
  const Vec3 ext(r.bmax.x - r.bmin.x, r.bmax.y - r.bmin.y, r.bmax.z - r.bmin.z);
  if (ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f) return 0.0f;
  // voxel CENTRES: the grid samples the middle of each cell, so the -0.5 is what
  // stops the whole volume sitting half a voxel off its own bounds.
  const float fx = ((p.x - r.bmin.x) / ext.x) * float(r.nx) - 0.5f;
  const float fy = ((p.y - r.bmin.y) / ext.y) * float(r.ny) - 0.5f;
  const float fz = ((p.z - r.bmin.z) / ext.z) * float(r.nz) - 0.5f;
  const int x0 = int(floorf(fx)), y0 = int(floorf(fy)), z0 = int(floorf(fz));
  const float tx = fx - float(x0), ty = fy - float(y0), tz = fz - float(z0);
  const float* d = v + r.firstVoxel;
  float acc = 0.0f;
  for (int dz = 0; dz < 2; ++dz)
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const int xi = x0 + dx, yi = y0 + dy, zi = z0 + dz;
        if (xi < 0 || yi < 0 || zi < 0 || xi >= r.nx || yi >= r.ny || zi >= r.nz) continue;
        const float w = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty) * (dz ? tz : 1.0f - tz);
        acc += w * d[(size_t(zi) * size_t(r.ny) + size_t(yi)) * size_t(r.nx) + size_t(xi)];
      }
  return acc;
}

// One grid pair at a moment of the shutter: the two frames cross-faded.
//
// A simulation has no velocity to advect by, so this IS the motion blur - the
// next frame of the sequence read as well and mixed by where in the shutter the
// ray is looking.  With no second frame it costs one compare.
IR_HD float volumeSamplePair(const SceneView& sv, const GridRef* pair, const Vec3& p, float st)
{
  const float a = volumeSampleGrid(sv.voxels, pair[0], p);
  if (!pair[1].valid()) return a;
  const float b = volumeSampleGrid(sv.voxels, pair[1], p);
  return a + (b - a) * st;
}

// What the volume GIVES OFF at a point, before anything absorbs it.
// A temperature as a colour, from the table the host built.
IR_HD Vec3 blackbodyLookup(const SceneView& sv, float kelvin)
{
  if (!sv.blackbody || sv.bbMaxK <= sv.bbMinK) return Vec3(1.0f);
  float t = (kelvin - sv.bbMinK) / (sv.bbMaxK - sv.bbMinK);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const float f = t * float(kBlackbodyLutSize - 1);
  const int i0 = int(f);
  const int i1 = (i0 + 1 < kBlackbodyLutSize) ? i0 + 1 : i0;
  const float fr = f - float(i0);
  return sv.blackbody[i0] * (1.0f - fr) + sv.blackbody[i1] * fr;
}

IR_HD Vec3 volumeEmission(const SceneView& sv, const VolumeGrid& g, const Vec3& pw, float st)
{
  Vec3 out(0.0f);
  const Vec3 p = g.worldToLocal.point(pw);
  for (int i = 0; i < kVolumeEmissive; ++i) {
    if (g.emissionScale[i] <= 0.0f || !g.emissive[i][0].valid()) continue;
    const float v = volumeSamplePair(sv, g.emissive[i], p, st);
    if (v <= 0.0f) continue;
    if (g.emissionMode[i] == kEmitBlackbody) {
      // The grid decides the COLOUR and the scale decides the brightness.  Using
      // the value as brightness is what made a temperature grid of 8336 Kelvin
      // render at 46361.
      float kelvin = v;
      if (g.emitKmax[i] > g.emitKmin[i])          // a 0..1 grid remapped to Kelvin
        kelvin = g.emitKmin[i] + (g.emitKmax[i] - g.emitKmin[i]) * v;
      out += g.emissionColor[i] * blackbodyLookup(sv, kelvin) * g.emissionScale[i];
    }
    else {
      out += g.emissionColor[i] * (v * g.emissionScale[i]);
    }
  }
  return out;
}

IR_HD float volumeDensity(const SceneView& sv, const VolumeGrid& g, const Vec3& pw, float st)
{
  const Vec3 p = g.worldToLocal.point(pw);
  return volumeSamplePair(sv, g.density, p, st) * g.densityScale;
}

// How much of a ray survives [0, tmax] through every volume in the scene.
// The optical depth along a ray, which is what everything else is built from.
IR_HD float volumeOpticalDepth(const SceneView& sv, const Vec3& o, const Vec3& d, float tmax,
                               int steps, float st)
{
  if (sv.numVolumes <= 0 || !sv.voxels) return 0.0f;
  float tau = 0.0f;
  for (int i = 0; i < sv.numVolumes; ++i) {
    const VolumeGrid& g = sv.volumes[i];
    float t0, t1;
    if (!volumeSlab(g, o, d, 0.0f, tmax, t0, t1)) continue;
    const int n = steps > 0 ? steps : 1;
    const float dt = (t1 - t0) / float(n);
    for (int k = 0; k < n; ++k) {
      const float t = t0 + (float(k) + 0.5f) * dt;
      tau += volumeDensity(sv, g, o + d * t, st) * dt;
    }
  }
  return tau;
}

IR_HD float volumeTransmittance(const SceneView& sv, const Vec3& o, const Vec3& d, float tmax,
                                int steps, float st)
{
  return expf(-volumeOpticalDepth(sv, o, d, tmax, steps, st));
}

// MULTIPLE SCATTERING, approximated the way production cloud renderers do it.
//
// Light that has bounced around inside smoke arrives softer and from further
// away than light that came straight through, and single scattering has none of
// it - a thick cloud lit from behind comes out far too dark and far too contrasty.
//
// Rather than tracing those bounces, each successive "octave" reuses the SAME
// optical depth with the density scaled down and its contribution weighted down:
//
//   sum over i of  b^i * exp(-tau * a^i)
//
// which costs nothing extra, because tau has already been marched.  Octave 0 is
// exactly the single-scattering term, so turning this off changes nothing.
IR_HD float volumeMultiScatter(float tau, int octaves)
{
  float sum = 0.0f, w = 1.0f, a = 1.0f;
  const int n = octaves < 1 ? 1 : octaves;
  for (int i = 0; i < n; ++i) {
    sum += w * expf(-tau * a);
    w *= 0.5f;      // each bounce carries half the energy of the one before
    a *= 0.5f;      // and sees the volume as half as thick
  }
  return sum;
}

// ---- textures ----------------------------------------------------------------------
// Bilinear, in plain float - the SAME code on both devices.  Hardware texture
// units would filter at reduced precision and break CPU/GPU parity.
IR_HD int irWrapIndex(int i, int n, int mode)
{
  if (n <= 1) return 0;
  if (mode == kWrapClamp) return i < 0 ? 0 : (i >= n ? n - 1 : i);
  if (mode == kWrapMirror) {
    const int period = 2 * n;
    int k = i % period; if (k < 0) k += period;
    return (k < n) ? k : (period - 1 - k);
  }
  int k = i % n; if (k < 0) k += n;
  return k;
}

IR_HD Vec4 irTexel(const SceneView& sv, const TextureDesc& t, int level, int x, int y)
{
  const int w = t.mipW[level], h = t.mipH[level];
  if (t.wrapS == kWrapBlack && (x < 0 || x >= w)) return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
  if (t.wrapT == kWrapBlack && (y < 0 || y >= h)) return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
  const int xi = irWrapIndex(x, w, t.wrapS), yi = irWrapIndex(y, h, t.wrapT);
  const float* p = sv.texels + (size_t(t.mipOffset[level]) + size_t(yi) * size_t(w) + size_t(xi)) * 4;
  return Vec4(p[0], p[1], p[2], p[3]);
}

IR_HD Vec4 sampleTextureLevel(const SceneView& sv, const TextureDesc& t, int level, float u, float v)
{
  const int w = t.mipW[level], h = t.mipH[level];
  if (w <= 0 || h <= 0) return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
  const float fx = u * float(w) - 0.5f;
  const float fy = (1.0f - v) * float(h) - 0.5f;
  const float x0f = floorf(fx), y0f = floorf(fy);
  const int x0 = int(x0f), y0 = int(y0f);
  const float ax = fx - x0f, ay = fy - y0f;
  const Vec4 c00 = irTexel(sv, t, level, x0, y0), c10 = irTexel(sv, t, level, x0 + 1, y0);
  const Vec4 c01 = irTexel(sv, t, level, x0, y0 + 1), c11 = irTexel(sv, t, level, x0 + 1, y0 + 1);
  const float w00 = (1.0f - ax) * (1.0f - ay), w10 = ax * (1.0f - ay);
  const float w01 = (1.0f - ax) * ay, w11 = ax * ay;
  return Vec4(c00.x * w00 + c10.x * w10 + c01.x * w01 + c11.x * w11,
              c00.y * w00 + c10.y * w10 + c01.y * w01 + c11.y * w11,
              c00.z * w00 + c10.z * w10 + c01.z * w01 + c11.z * w11,
              c00.w * w00 + c10.w * w10 + c01.w * w01 + c11.w * w11);
}

// u,v are USD st coordinates: (0,0) is the BOTTOM-left of the image, while
// texel row 0 is the image's TOP row.  'lod' is the mip level to read, blended
// between the two nearest ones; 0 is the full resolution image.
IR_HD Vec4 sampleTexture(const SceneView& sv, int texId, float u, float v, float lod)
{
  if (texId < 0 || texId >= sv.numTextures || !sv.textures || !sv.texels) return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
  const TextureDesc t = sv.textures[texId];
  if (t.width <= 0 || t.height <= 0) return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
  if (t.mipCount <= 1 || !(lod > 0.0f)) return sampleTextureLevel(sv, t, 0, u, v);
  const float maxLevel = float(t.mipCount - 1);
  const float l = (lod > maxLevel) ? maxLevel : lod;
  const int l0 = int(l);
  const int l1 = (l0 + 1 < t.mipCount) ? l0 + 1 : l0;
  const float f = l - float(l0);
  const Vec4 a = sampleTextureLevel(sv, t, l0, u, v);
  if (l1 == l0 || f <= 0.0f) return a;
  const Vec4 b = sampleTextureLevel(sv, t, l1, u, v);
  return Vec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f);
}

IR_HD Vec4 sampleTexture(const SceneView& sv, int texId, float u, float v)
{
  return sampleTexture(sv, texId, u, v, 0.0f);
}

IR_HD Vec4 sampleTexRef(const SceneView& sv, const TexRef& tr, float u, float v, float lod)
{
  // UsdTransform2d on the st input
  const float tu = tr.uv[0] * u + tr.uv[1] * v + tr.uv[2];
  const float tv = tr.uv[3] * u + tr.uv[4] * v + tr.uv[5];
  int tex = tr.index;
  float su = tu, sv2 = tv;
  if (tr.udimSet >= 0 && sv.udimTiles) {
    // <UDIM>: the tile is 1001 + floor(u) + 10 * floor(v), and each tile owns
    // the unit square of uv space it sits in
    const float fu = floorf(tu), fv = floorf(tv);
    const int col = int(fu), row = int(fv);
    tex = -1;
    if (col >= 0 && col < 10 && row >= 0 && row < 10) tex = sv.udimTiles[tr.udimSet + row * 10 + col];
    su = tu - fu; sv2 = tv - fv;
  }
  if (tex < 0) return Vec4(tr.bias.x, tr.bias.y, tr.bias.z, tr.bias.w);
  const Vec4 c = sampleTexture(sv, tex, su, sv2, lod);
  return Vec4(c.x * tr.scale.x + tr.bias.x, c.y * tr.scale.y + tr.bias.y,
              c.z * tr.scale.z + tr.bias.z, c.w * tr.scale.w + tr.bias.w);
}

IR_HD Vec4 sampleTexRef(const SceneView& sv, const TexRef& tr, float u, float v)
{
  return sampleTexRef(sv, tr, u, v, 0.0f);
}

// s.lod is measured in uv units per pixel; a level is one octave of TEXELS, so
// the image resolution has to be folded in
IR_HD float texelLod(const SceneView& sv, const TexRef& tr)
{
  int id = tr.index;
  if (id < 0 && tr.udimSet >= 0 && sv.udimTiles) id = sv.udimTiles[tr.udimSet];
  if (id < 0 || id >= sv.numTextures || !sv.textures) return 0.0f;
  const TextureDesc& t = sv.textures[id];
  const int m = (t.width > t.height) ? t.width : t.height;
  return (m > 1) ? log2f(float(m)) : 0.0f;
}

IR_HD float texChannel(const Vec4& c, int ch)
{
  return (ch == 0) ? c.x : (ch == 1) ? c.y : (ch == 2) ? c.z : c.w;
}

// ---- dome light (lat-long, UsdLuxDomeLight convention) -------------------------------
// image column 0 = longitude +pi, last column = -pi; row 0 (top) = latitude
// +pi/2 (+Y); latitude 0 / longitude 0 points down +Z.
IR_HD void domeDirToUv(const Light& L, const Vec3& d, float& u, float& v)
{
  const Vec3 dl(dot(d, L.lx), dot(d, L.ly), dot(d, L.lz));
  const float lat = asinf(irClamp(dl.y, -1.0f, 1.0f));
  const float lon = atan2f(dl.x, dl.z);
  u = (3.14159265f - lon) * (1.0f / 6.28318530f);
  v = (lat + 1.57079633f) * (1.0f / 3.14159265f);      // 1 = up = top row
  if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
}

IR_HD Vec3 domeUvToDir(const Light& L, float u, float v)
{
  const float lon = 3.14159265f - u * 6.28318530f;
  const float lat = v * 3.14159265f - 1.57079633f;
  const float cl = cosf(lat);
  const Vec3 dl(cl * sinf(lon), sinf(lat), cl * cosf(lon));
  return L.lx * dl.x + L.ly * dl.y + L.lz * dl.z;
}

IR_HD Vec3 domeRadiance(const SceneView& sv, const Light& L, const Vec3& d)
{
  if (L.texture < 0) return L.color;
  float u, v; domeDirToUv(L, d, u, v);
  const Vec4 c = sampleTexture(sv, L.texture, u, v);
  return Vec3(c.x, c.y, c.z) * L.color;
}

IR_HD int irCdfSearch(const float* cdf, int n, float x)
{
  int lo = 0, hi = n;
  while (lo + 1 < hi) {
    const int mid = (lo + hi) >> 1;
    if (cdf[mid] <= x) lo = mid; else hi = mid;
  }
  return lo;
}

IR_HD float domeFuncAt(const SceneView& sv, int x, int y)
{
  int xi = x % sv.domeW; if (xi < 0) xi += sv.domeW;             // longitude wraps
  const int yi = (y < 0) ? 0 : ((y >= sv.domeH) ? sv.domeH - 1 : y);
  return sv.domeFunc[size_t(yi) * size_t(sv.domeW) + size_t(xi)];
}

// Solid-angle pdf of the dome distribution, BILINEARLY interpolated.
//
// It is used only for the MIS weights, never as an estimator denominator:
// domeSample returns the exact piecewise-constant pdf it sampled from.  The
// smoothing matters because radiance is read with a bilinear filter, so light
// leaks one texel beyond the cells the cdf can pick - with the exact cell pdf
// those directions get weight 1 from the bsdf side and turn into fireflies.
// Any weight function is legal as long as both techniques use the same one and
// the weights sum to 1, which they still do.
IR_HD float domePdf(const SceneView& sv, const Light& L, const Vec3& d)
{
  if (!sv.domeFunc || sv.domeW <= 0 || sv.domeH <= 0 || sv.domeFuncInt <= 0.0f) return 1.0f / (4.0f * 3.14159265f);
  float u, v; domeDirToUv(L, d, u, v);
  const float st = 1.0f - v;                       // 0 at the top row
  const float sinT = sinf(st * 3.14159265f);
  if (sinT <= 1e-6f) return 0.0f;
  const float fx = u * float(sv.domeW) - 0.5f, fy = st * float(sv.domeH) - 0.5f;
  const float x0f = floorf(fx), y0f = floorf(fy);
  const int x0 = int(x0f), y0 = int(y0f);
  const float ax = fx - x0f, ay = fy - y0f;
  const float f = domeFuncAt(sv, x0, y0) * (1.0f - ax) * (1.0f - ay)
                + domeFuncAt(sv, x0 + 1, y0) * ax * (1.0f - ay)
                + domeFuncAt(sv, x0, y0 + 1) * (1.0f - ax) * ay
                + domeFuncAt(sv, x0 + 1, y0 + 1) * ax * ay;
  return (f / sv.domeFuncInt) / (19.7392088f * sinT);
}

// importance-sample the dome: wi, its radiance and its solid-angle pdf
IR_HD bool domeSample(const SceneView& sv, const Light& L, float r1, float r2, Vec3& wi, Vec3& Li, float& pdf)
{
  if (!sv.domeMarginal || !sv.domeConditional || sv.domeW <= 0 || sv.domeH <= 0 || sv.domeFuncInt <= 0.0f) {
    // constant dome: uniform sphere
    const float z = 1.0f - 2.0f * r1;
    const float r = irSqrt(irMax(0.0f, 1.0f - z * z));
    const float phi = 6.28318530f * r2;
    wi = Vec3(r * cosf(phi), z, r * sinf(phi));
    pdf = 1.0f / (4.0f * 3.14159265f);
    Li = domeRadiance(sv, L, wi);
    return true;
  }
  const int y = irCdfSearch(sv.domeMarginal, sv.domeH, r1);
  const float dy = sv.domeMarginal[y + 1] - sv.domeMarginal[y];
  const float fy = (dy > 0.0f) ? (r1 - sv.domeMarginal[y]) / dy : 0.5f;
  const float* cond = sv.domeConditional + size_t(y) * size_t(sv.domeW + 1);
  const int x = irCdfSearch(cond, sv.domeW, r2);
  const float dx = cond[x + 1] - cond[x];
  const float fx = (dx > 0.0f) ? (r2 - cond[x]) / dx : 0.5f;
  const float su = (float(x) + fx) / float(sv.domeW);
  const float stt = (float(y) + fy) / float(sv.domeH);        // 0 at the top row
  const float sinT = sinf(stt * 3.14159265f);
  if (sinT <= 1e-6f) return false;
  wi = domeUvToDir(L, su, 1.0f - stt);
  const float func = sv.domeFunc[size_t(y) * size_t(sv.domeW) + size_t(x)];
  pdf = (func / sv.domeFuncInt) / (19.7392088f * sinT);       // 19.739 = 2*pi^2
  if (pdf <= 0.0f) return false;
  Li = domeRadiance(sv, L, wi);
  return true;
}

IR_HD float misWeight(float pdfA, float pdfB)
{
  const float a = pdfA * pdfA, b = pdfB * pdfB;
  return (a + b > 0.0f) ? a / (a + b) : 0.0f;
}

// ---- surface reconstruction --------------------------------------------------------------
IR_HD bool buildSurface(const SceneView& sv, const Ray& ray, const HitRecord& h, Surface& s)
{
  if (h.instId < 0 || h.instId >= sv.numInstances) return false;
  const Instance& inst = sv.instances[h.instId];
  if (inst.protoId < 0 || inst.protoId >= sv.numProtos) return false;
  const ProtoRange& pr = sv.protos[inst.protoId];
  if (h.primId < 0 || h.primId >= pr.numTris) return false;
  s.objectId = inst.protoId;
  const Xform xform = sv.settings.motionBlur ? instanceXformAt(sv, inst, ray.time) : inst.xf;
  const int tri = pr.firstTri + h.primId;
  const uint32_t i0 = sv.indices[tri * 3], i1 = sv.indices[tri * 3 + 1], i2 = sv.indices[tri * 3 + 2];
  Vec3 p0 = sv.vertices[i0], p1 = sv.vertices[i1], p2 = sv.vertices[i2];
  if (sv.settings.deformationBlur && sv.vertices1) {
    // the mesh itself moves: the same linear interpolation the two back-ends use
    // between their vertex time steps
    const float t = ray.time;
    p0 = p0 + (sv.vertices1[i0] - p0) * t;
    p1 = p1 + (sv.vertices1[i1] - p1) * t;
    p2 = p2 + (sv.vertices1[i2] - p2) * t;
  }
  const float w0 = 1.0f - h.u - h.v;
  const Vec3 pObj = p0 * w0 + p1 * h.u + p2 * h.v;
  s.P = xform.point(pObj);
  Vec3 ngObj = cross(p1 - p0, p2 - p0);
  s.Ng = xform.normal(ngObj);
  Vec3 ns = s.Ng;
  if (pr.hasNormals && sv.normals) {
    const Vec3 n = sv.normals[i0] * w0 + sv.normals[i1] * h.u + sv.normals[i2] * h.v;
    if (dot(n, n) > 1e-12f) ns = xform.normal(n);
  }
  s.backface = dot(s.Ng, ray.d) > 0.0f;
  if (s.backface) { s.Ng = -s.Ng; ns = -ns; }
  // keep the shading normal in the same hemisphere as the geometric one
  if (dot(ns, s.Ng) < 0.0f) ns = -ns;
  s.Ns = ns;

  // uv (USD st) for the textures
  float uu = 0.0f, vv = 0.0f;
  const bool hasUV = (pr.hasUVs && sv.uvs != nullptr);
  if (hasUV) {
    uu = sv.uvs[i0 * 2] * w0 + sv.uvs[i1 * 2] * h.u + sv.uvs[i2 * 2] * h.v;
    vv = sv.uvs[i0 * 2 + 1] * w0 + sv.uvs[i1 * 2 + 1] * h.u + sv.uvs[i2 * 2 + 1] * h.v;
  }
  s.u = uu; s.v = vv;

  // Ray cones (Akenine-Moller et al.): the cone's width at the hit, compared
  // with how much texture the triangle carries per unit of world area, gives
  // the mip level.  Grazing angles widen the footprint, hence the 1/|cos|.
  s.lod = 0.0f;
  if (sv.settings.mipFilter && hasUV && ray.spread > 0.0f) {
    const float du1 = sv.uvs[i1 * 2] - sv.uvs[i0 * 2], dv1 = sv.uvs[i1 * 2 + 1] - sv.uvs[i0 * 2 + 1];
    const float du2 = sv.uvs[i2 * 2] - sv.uvs[i0 * 2], dv2 = sv.uvs[i2 * 2 + 1] - sv.uvs[i0 * 2 + 1];
    const float uvArea = irAbs(du1 * dv2 - du2 * dv1);
    const Vec3 e1w = xform.vector(p1 - p0), e2w = xform.vector(p2 - p0);
    const float worldArea = length(cross(e1w, e2w));
    if (uvArea > 1e-16f && worldArea > 1e-16f) {
      const float cone = ray.spread * h.t;                     // footprint width at the hit
      const float cosT = irMax(irAbs(dot(normalize(ray.d), s.Ng)), 1e-3f);
      const float footprint = cone * cone * (uvArea / worldArea) / cosT;
      // in UV units for now - the texture's resolution is added per read, and
      // only then can it be clamped, or every hit lands on the smallest level
      s.lod = 0.5f * log2f(irMax(footprint, 1e-20f));
    }
  }

  const int matId = (inst.materialOverride >= 0) ? inst.materialOverride : (sv.triMaterial ? sv.triMaterial[tri] : 0);
  const Material& m = (matId >= 0 && matId < sv.numMaterials) ? sv.materials[matId] : Material();
  s.materialId = matId;
  Vec3 base = m.diffuse;
  if (m.useDisplayColor) {
    if (inst.hasColor) base = inst.color;
    else if (pr.hasColors && sv.colors) base = sv.colors[i0] * w0 + sv.colors[i1] * h.u + sv.colors[i2] * h.v;
    else base = Vec3(0.18f, 0.18f, 0.18f);
  }
  else if (inst.hasColor) {
    base = base * inst.color;   // per-instance tint on a material
  }
  if (hasUV && m.diffuseTex.valid()) {
    const Vec4 c = sampleTexRef(sv, m.diffuseTex, uu, vv, s.lod + texelLod(sv, m.diffuseTex));
    base = Vec3(c.x, c.y, c.z);
    if (inst.hasColor && !m.useDisplayColor) base = base * inst.color;
  }
  float metallic = m.metallic, roughness = m.roughness, opacity = m.opacity;
  Vec3 emissive = m.emissive;
  if (hasUV) {
    if (m.metallicTex.valid())  metallic = texChannel(sampleTexRef(sv, m.metallicTex, uu, vv, s.lod + texelLod(sv, m.metallicTex)), m.metallicTex.channel);
    if (m.roughnessTex.valid()) roughness = texChannel(sampleTexRef(sv, m.roughnessTex, uu, vv, s.lod + texelLod(sv, m.roughnessTex)), m.roughnessTex.channel);
    if (m.opacityTex.valid())   opacity = texChannel(sampleTexRef(sv, m.opacityTex, uu, vv, s.lod + texelLod(sv, m.opacityTex)), m.opacityTex.channel);
    if (m.emissiveTex.valid())  { const Vec4 c = sampleTexRef(sv, m.emissiveTex, uu, vv, s.lod + texelLod(sv, m.emissiveTex)); emissive = Vec3(c.x, c.y, c.z); }
    // tangent-space normal map: the tangent comes from the triangle's uv gradient
    if (m.normalTex.valid()) {
      const float du1 = sv.uvs[i1 * 2] - sv.uvs[i0 * 2], dv1 = sv.uvs[i1 * 2 + 1] - sv.uvs[i0 * 2 + 1];
      const float du2 = sv.uvs[i2 * 2] - sv.uvs[i0 * 2], dv2 = sv.uvs[i2 * 2 + 1] - sv.uvs[i0 * 2 + 1];
      const float det = du1 * dv2 - du2 * dv1;
      if (irAbs(det) > 1e-12f) {
        const Vec3 e1 = p1 - p0, e2 = p2 - p0;
        Vec3 T = xform.vector((e1 * dv2 - e2 * dv1) * (1.0f / det));
        T = T - s.Ns * dot(s.Ns, T);
        if (dot(T, T) > 1e-16f) {
          T = normalize(T);
          const Vec3 B = cross(s.Ns, T);
          Vec4 nm = sampleTexRef(sv, m.normalTex, uu, vv);
          // assets usually carry scale (2,2,2,1) / bias (-1,-1,-1,0); if they
          // do not, remap the raw [0,1] texel here
          if (m.normalTex.scale.x == 1.0f && m.normalTex.bias.x == 0.0f)
            nm = Vec4(nm.x * 2.0f - 1.0f, nm.y * 2.0f - 1.0f, nm.z * 2.0f - 1.0f, nm.w);
          const Vec3 nt(nm.x * m.normalScale, nm.y * m.normalScale, nm.z);
          const Vec3 mapped = T * nt.x + B * nt.y + s.Ns * nt.z;
          if (dot(mapped, mapped) > 1e-16f) {
            Vec3 mn = normalize(mapped);
            if (dot(mn, s.Ng) < 0.0f) mn = mn - s.Ng * (2.0f * dot(mn, s.Ng));   // keep it on the visible side
            s.Ns = mn;
          }
        }
      }
    }
  }
  float clearcoat = m.clearcoat, occlusion = m.occlusion;
  if (hasUV) {
    if (m.clearcoatTex.valid())
      clearcoat = texChannel(sampleTexRef(sv, m.clearcoatTex, uu, vv, s.lod + texelLod(sv, m.clearcoatTex)), m.clearcoatTex.channel);
    if (m.occlusionTex.valid())
      occlusion = texChannel(sampleTexRef(sv, m.occlusionTex, uu, vv, s.lod + texelLod(sv, m.occlusionTex)), m.occlusionTex.channel);
  }
  s.metallic = irClamp(metallic, 0.0f, 1.0f);
  s.roughness = irClamp(roughness, 0.02f, 1.0f);
  s.clearcoat = irClamp(clearcoat, 0.0f, 1.0f);
  s.clearcoatRoughness = irClamp(m.clearcoatRoughness, 0.01f, 1.0f);
  s.occlusion = irClamp(occlusion, 0.0f, 1.0f);
  // BELOW THE THRESHOLD IS A HOLE, NOT A TINT.  A leaf cutout wants the texel to
  // stop being surface at all - so it casts no shadow there either - and taking
  // a 0.1 as "mostly transparent" leaves a grey ghost of the whole quad.
  if (m.opacityThreshold > 0.0f) opacity = (opacity < m.opacityThreshold) ? 0.0f : 1.0f;
  s.opacity = irClamp(opacity * inst.opacity, 0.0f, 1.0f);
  if (m.useSpecularWorkflow) { s.F0 = m.specularColor; s.diffuseWeight = 1.0f; }
  else { s.F0 = vlerp(Vec3(0.04f), base, s.metallic); s.diffuseWeight = 1.0f - s.metallic; }
  s.albedo = base;
  s.emissive = emissive;
  s.instanceId = inst.instanceId;
  s.cryptoId = inst.cryptoId;
  return true;
}

// ---- BSDF: lambert + GGX (UsdPreviewSurface-ish) -------------------------------------------
IR_HD float ggxD(float NdotH, float a2)
{
  const float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
  return a2 / (3.14159265f * d * d + 1e-12f);
}
IR_HD float smithG1(float NdotV, float a2)
{
  const float k = NdotV * NdotV;
  return 2.0f * NdotV / (NdotV + irSqrt(a2 + (1.0f - a2) * k) + 1e-12f);
}
IR_HD Vec3 fresnelSchlick(const Vec3& F0, float VdotH)
{
  const float f = powf(1.0f - irClamp(VdotH, 0.0f, 1.0f), 5.0f);
  return F0 + (Vec3(1.0f) - F0) * f;
}

// evaluate f(wo, wi) * cos(theta_i) split into its diffuse and specular halves,
// The clearcoat lobe: a second, near-smooth specular layer over everything else.
// It is what a varnish, a car's lacquer or a wet surface adds - a sharp highlight
// that does NOT take the base colour, because the coat is clear.  Its Fresnel is
// the 0.04 of a dielectric whatever the base is made of, so a clearcoated metal
// still gets a white sheen rather than a gold one.
IR_HD Vec3 clearcoatLobe(const Surface& s, const Vec3& wo, const Vec3& wi)
{
  if (s.clearcoat <= 0.0f) return Vec3(0.0f);
  const float NdotL = dot(s.Ns, wi), NdotV = dot(s.Ns, wo);
  if (NdotL <= 0.0f || NdotV <= 0.0f) return Vec3(0.0f);
  const Vec3 H = normalize(wo + wi);
  const float NdotH = irMax(dot(s.Ns, H), 0.0f);
  const float VdotH = irMax(dot(wo, H), 0.0f);
  const float ac = s.clearcoatRoughness * s.clearcoatRoughness;
  const float ac2 = ac * ac;
  const float D = ggxD(NdotH, ac2);
  const float G = smithG1(NdotV, ac2) * smithG1(NdotL, ac2);
  const Vec3 F = fresnelSchlick(Vec3(0.04f), VdotH);
  return F * (D * G / (4.0f * NdotV * NdotL + 1e-12f)) * s.clearcoat;
}

// so a light's diffuse / specular multipliers can scale them separately
IR_HD void bsdfEvalSplit(const Surface& s, const Vec3& wo, const Vec3& wi, Vec3& diffuseOut, Vec3& specularOut)
{
  diffuseOut = Vec3(0.0f); specularOut = Vec3(0.0f);
  const float NdotL = dot(s.Ns, wi);
  const float NdotV = dot(s.Ns, wo);
  if (NdotL <= 0.0f || NdotV <= 0.0f) return;
  const Vec3 H = normalize(wo + wi);
  const float NdotH = irMax(dot(s.Ns, H), 0.0f);
  const float VdotH = irMax(dot(wo, H), 0.0f);
  const float a = s.roughness * s.roughness;
  const float a2 = a * a;
  const Vec3 F = fresnelSchlick(s.F0, VdotH);
  const float D = ggxD(NdotH, a2);
  const float G = smithG1(NdotV, a2) * smithG1(NdotL, a2);
  specularOut = F * (D * G / (4.0f * NdotV * NdotL + 1e-12f)) * NdotL;
  specularOut = specularOut + clearcoatLobe(s, wo, wi) * NdotL;
  const Vec3 kd = Vec3(1.0f) - F;
  diffuseOut = s.albedo * (s.diffuseWeight * (1.0f / 3.14159265f)) * kd * NdotL;
}

// evaluate f(wo, wi) * cos(theta_i); wo = towards viewer, wi = towards light (world space)
IR_HD Vec3 bsdfEval(const Surface& s, const Vec3& wo, const Vec3& wi)
{
  const float NdotL = dot(s.Ns, wi);
  const float NdotV = dot(s.Ns, wo);
  if (NdotL <= 0.0f || NdotV <= 0.0f) return Vec3(0.0f);
  const Vec3 H = normalize(wo + wi);
  const float NdotH = irMax(dot(s.Ns, H), 0.0f);
  const float VdotH = irMax(dot(wo, H), 0.0f);
  const float a = s.roughness * s.roughness;
  const float a2 = a * a;
  const Vec3 F = fresnelSchlick(s.F0, VdotH);
  const float D = ggxD(NdotH, a2);
  const float G = smithG1(NdotV, a2) * smithG1(NdotL, a2);
  const Vec3 spec = F * (D * G / (4.0f * NdotV * NdotL + 1e-12f));
  const Vec3 diff = s.albedo * (s.diffuseWeight * (1.0f / 3.14159265f));
  // energy: diffuse is attenuated by the specular Fresnel (roughly)
  const Vec3 kd = Vec3(1.0f) - F;
  return (diff * kd + spec + clearcoatLobe(s, wo, wi)) * NdotL;
}

// probability of picking the specular lobe when sampling
IR_HD float bsdfSpecProb(const Surface& s)
{
  const float specLum = luminance(s.F0);
  const float diffLum = luminance(s.albedo) * s.diffuseWeight;
  return irClamp(specLum / (specLum + diffLum + 1e-6f), 0.1f, 0.9f);
}

// pdf (solid angle) of the sampling scheme in bsdfSample for a given wi
IR_HD float bsdfPdf(const Surface& s, const Vec3& wo, const Vec3& wi)
{
  const float NdotL = dot(s.Ns, wi), NdotV = dot(s.Ns, wo);
  if (NdotL <= 0.0f || NdotV <= 0.0f) return 0.0f;
  const float a = s.roughness * s.roughness;
  const float a2 = a * a;
  const Vec3 H = normalize(wo + wi);
  const float NdotH = irMax(dot(s.Ns, H), 0.0f);
  const float VdotH = irMax(dot(wo, H), 1e-6f);
  const float pdfSpec = ggxD(NdotH, a2) * NdotH / (4.0f * VdotH + 1e-12f);
  const float pdfDiff = NdotL / 3.14159265f;
  const float pSpec = bsdfSpecProb(s);
  return pSpec * pdfSpec + (1.0f - pSpec) * pdfDiff;
}

// sample wi; returns weight = f*cos/pdf (and that pdf, for MIS)
IR_HD bool bsdfSample(const Surface& s, const Vec3& wo, Rng& rng, Vec3& wi, Vec3& weight, float& pdfOut, int* lobeOut = 0)
{
  Vec3 T, B;
  basis(s.Ns, T, B);
  const float NdotV = dot(s.Ns, wo);
  if (NdotV <= 0.0f) return false;
  const float pSpec = bsdfSpecProb(s);
  const float a = s.roughness * s.roughness;
  const float a2 = a * a;
  const float r1 = rng.uniform(), r2 = rng.uniform();
  const bool specLobe = (rng.uniform() < pSpec);
  if (lobeOut) *lobeOut = specLobe ? 1 : 0;
  if (specLobe) {
    // GGX half vector
    const float phi = 6.28318530f * r1;
    const float cosT = irSqrt((1.0f - r2) / (1.0f + (a2 - 1.0f) * r2));
    const float sinT = irSqrt(irMax(0.0f, 1.0f - cosT * cosT));
    const Vec3 Hl(sinT * cosf(phi), sinT * sinf(phi), cosT);
    const Vec3 H = normalize(T * Hl.x + B * Hl.y + s.Ns * Hl.z);
    wi = reflect(-wo, H);
    if (dot(wi, s.Ns) <= 0.0f) return false;
  }
  else {
    // cosine hemisphere
    const float phi = 6.28318530f * r1;
    const float rr = irSqrt(r2);
    const Vec3 dl(rr * cosf(phi), rr * sinf(phi), irSqrt(irMax(0.0f, 1.0f - r2)));
    wi = normalize(T * dl.x + B * dl.y + s.Ns * dl.z);
  }
  // combined pdf of the two strategies
  const float pdf = bsdfPdf(s, wo, wi);
  if (pdf <= 1e-8f) return false;
  weight = bsdfEval(s, wo, wi) / pdf;
  pdfOut = pdf;
  return true;
}

// ---- lights --------------------------------------------------------------------------------
// UsdLuxShapingAPI: the cone (a spot light) and the focus exponent, as a factor
// on the light's emission for a given direction FROM the light.
// An IES profile, looked up by the direction leaving the light.
//
// The angles are the profile's own: VERTICAL measured from the light's axis
// (0 straight down the beam, 180 straight back), HORIZONTAL around it. The
// light's lx/ly/lz basis is what turns a world direction into those, which is
// also what makes a rotated luminaire throw its pattern in the right place.
IR_HD float iesLookup(const SceneView& sv, const Light& L, const Vec3& wFromLight)
{
  if (L.iesProfile < 0 || !sv.ies) return 1.0f;
  const float ct = irClamp(dot(wFromLight, L.direction), -1.0f, 1.0f);
  const float vAng = acosf(ct) * (180.0f / 3.14159265f);
  // around the axis, from the light's own x towards its y
  const float x = dot(wFromLight, L.lx), y = dot(wFromLight, L.ly);
  float hAng = atan2f(y, x) * (180.0f / 3.14159265f);
  if (hAng < 0.0f) hAng += 360.0f;
  const float fv = irClamp(vAng / 180.0f, 0.0f, 1.0f) * float(kIesVRes - 1);
  const float fh = (hAng / 360.0f) * float(kIesHRes);
  const int v0 = int(fv);
  const int v1 = (v0 + 1 < kIesVRes) ? v0 + 1 : v0;
  const float vt = fv - float(v0);
  int h0 = int(fh) % kIesHRes;
  if (h0 < 0) h0 += kIesHRes;
  const int h1 = (h0 + 1) % kIesHRes;      // wraps: it is a full turn
  const float ht = fh - floorf(fh);
  const float* t = sv.ies + size_t(L.iesProfile) * size_t(kIesVRes) * size_t(kIesHRes);
  const float a00 = t[size_t(v0) * kIesHRes + size_t(h0)];
  const float a01 = t[size_t(v1) * kIesHRes + size_t(h0)];
  const float a10 = t[size_t(v0) * kIesHRes + size_t(h1)];
  const float a11 = t[size_t(v1) * kIesHRes + size_t(h1)];
  return (a00 * (1.0f - vt) + a01 * vt) * (1.0f - ht)
       + (a10 * (1.0f - vt) + a11 * vt) * ht;
}

IR_HD Vec3 lightShaping(const SceneView& sv, const Light& L, const Vec3& wFromLight)
{
  Vec3 f(1.0f);
  if (L.iesProfile >= 0) f *= iesLookup(sv, L, wFromLight);
  const float ct = dot(wFromLight, L.direction);      // direction is the light's own axis
  if (L.coneCos > -1.0f) {
    if (ct <= L.coneCos) return Vec3(0.0f);
    if (ct < L.coneCosInner && L.coneCosInner > L.coneCos) {
      const float x = (ct - L.coneCos) / (L.coneCosInner - L.coneCos);
      f *= x * x * (3.0f - 2.0f * x);                 // smoothstep across the soft edge
    }
  }
  if (L.focus > 0.0f) {
    const float c = irMax(ct, 0.0f);
    const float w = powf(c, L.focus);
    f = f * vlerp(L.focusTint, Vec3(1.0f), w);
    f = f * w;
  }
  return f;
}

// Sample a light: direction to it, distance, and the radiance arriving along wi
// already divided by the sampling pdf.
IR_HD bool sampleLight(const SceneView& sv, const Light& L, const Vec3& P, Rng& rng, Vec3& wi, float& dist, Vec3& Li)
{
  if (L.type == kLightDistant) {
    // cone of 'angle' degrees around -direction
    const float half = 0.5f * L.angle * (3.14159265f / 180.0f);
    Vec3 d = -L.direction;
    if (half > 1e-5f) {
      Vec3 T, B; basis(d, T, B);
      const float cosMax = cosf(half);
      const float r1 = rng.uniform(), r2 = rng.uniform();
      const float ct = 1.0f - r1 * (1.0f - cosMax);
      const float st = irSqrt(irMax(0.0f, 1.0f - ct * ct));
      const float phi = 6.28318530f * r2;
      d = normalize(T * (st * cosf(phi)) + B * (st * sinf(phi)) + d * ct);
    }
    wi = d; dist = 1e30f;
    Li = L.color * lightShaping(sv, L, -wi);
    return true;
  }
  if (L.type == kLightPoint) {
    Vec3 d = L.position - P;
    const float d2 = irMax(dot(d, d), 1e-8f);
    dist = irSqrt(d2);
    wi = d / dist;
    Li = L.color * lightShaping(sv, L, -wi) * (1.0f / d2);
    return true;
  }

  // area lights: pick a point on the shape, convert to solid angle with cos/d^2
  Vec3 target, nrm;
  Vec3 tint(1.0f);
  float areaSampled = L.area;
  if (L.type == kLightSphere) {
    // only the hemisphere facing the shading point can be seen from it, so
    // sample that half and account for half the area - sampling the whole
    // sphere throws away every second sample and shows up as noise
    const Vec3 toP = normalize(P - L.position);
    Vec3 T, B; basis(toP, T, B);
    const float r1 = rng.uniform(), r2 = rng.uniform();
    const float z = r1;                                  // cos of the polar angle, uniform on the hemisphere
    const float rr = irSqrt(irMax(0.0f, 1.0f - z * z));
    const float phi = 6.28318530f * r2;
    nrm = normalize(T * (rr * cosf(phi)) + B * (rr * sinf(phi)) + toP * z);
    target = L.position + nrm * L.radius;
    areaSampled = 0.5f * L.area;
  }
  else if (L.type == kLightRect) {
    const float r1 = rng.uniform() * 2.0f - 1.0f, r2 = rng.uniform() * 2.0f - 1.0f;
    target = L.position + L.u * r1 + L.v * r2;
    nrm = L.direction;
    if (L.texture >= 0) {
      const Vec4 c = sampleTexture(sv, L.texture, (r1 + 1.0f) * 0.5f, (r2 + 1.0f) * 0.5f);
      tint = Vec3(c.x, c.y, c.z);
    }
  }
  else if (L.type == kLightDisk) {
    // uniform on the disk, not on its bounding square
    const float r = L.radius * irSqrt(rng.uniform());
    const float phi = 6.28318530f * rng.uniform();
    target = L.position + normalize(L.u) * (r * cosf(phi)) + normalize(L.v) * (r * sinf(phi));
    nrm = L.direction;
  }
  else if (L.type == kLightCylinder) {
    const float t = (rng.uniform() * 2.0f - 1.0f) * L.length;
    const float phi = 6.28318530f * rng.uniform();
    const Vec3 axis = normalize(L.u);
    const Vec3 e1 = normalize(L.v), e2 = cross(axis, e1);
    nrm = e1 * cosf(phi) + e2 * sinf(phi);
    target = L.position + axis * t + nrm * L.radius;
  }
  else return false;

  Vec3 d = target - P;
  const float d2 = irMax(dot(d, d), 1e-8f);
  dist = irSqrt(d2);
  wi = d / dist;
  const float cosL = dot(-wi, nrm);                   // the shape's outward normal at the sample
  if (cosL <= 0.0f) return false;
  Li = L.color * tint * lightShaping(sv, L, -wi) * (cosL * areaSampled / d2);
  return true;
}

// ---- lights seen directly by a ray ---------------------------------------------------------
// Only used for camera rays: the paths that bounce find lights through next
// event estimation instead, and counting both would double the energy.
IR_HD bool intersectLight(const SceneView& sv, const Light& L, const Ray& r, float& tHit, Vec3& Le)
{
  if (!L.visibleToCamera) return false;
  if (L.type == kLightDistant || L.type == kLightDome || L.type == kLightPoint) return false;
  if (L.type == kLightSphere) {
    const Vec3 oc = r.o - L.position;
    const float b = dot(oc, r.d);
    const float c = dot(oc, oc) - L.radius * L.radius;
    const float disc = b * b - c;
    if (disc <= 0.0f) return false;
    const float sq = irSqrt(disc);
    float t = -b - sq;
    if (t < r.tmin) t = -b + sq;
    if (t < r.tmin || t > r.tmax || t > tHit) return false;
    tHit = t;
    const Vec3 n = normalize(r.o + r.d * t - L.position);
    Le = L.color * lightShaping(sv, L, n);
    return true;
  }
  // planar shapes: rect and disk
  if (L.type == kLightRect || L.type == kLightDisk) {
    const float denom = dot(r.d, L.direction);
    if (irAbs(denom) < 1e-9f) return false;
    const float t = dot(L.position - r.o, L.direction) / denom;
    if (t < r.tmin || t > r.tmax || t > tHit) return false;
    const Vec3 p = r.o + r.d * t - L.position;
    if (L.type == kLightRect) {
      const float lu = length(L.u), lv = length(L.v);
      if (lu < 1e-9f || lv < 1e-9f) return false;
      const float a = dot(p, L.u) / (lu * lu), b = dot(p, L.v) / (lv * lv);
      if (a < -1.0f || a > 1.0f || b < -1.0f || b > 1.0f) return false;
    }
    else if (dot(p, p) > L.radius * L.radius) return false;
    tHit = t;
    const Vec3 n = (denom < 0.0f) ? L.direction : -L.direction;
    Le = L.color * lightShaping(sv, L, n);
    if (L.type == kLightRect && L.texture >= 0) {
      const float lu = length(L.u), lv = length(L.v);
      const float a = dot(p, L.u) / (lu * lu), b = dot(p, L.v) / (lv * lv);
      const Vec4 c = sampleTexture(sv, L.texture, (a + 1.0f) * 0.5f, (b + 1.0f) * 0.5f);
      Le = Le * Vec3(c.x, c.y, c.z);
    }
    return true;
  }
  return false;
}

// nearest light along a ray, if any is closer than 'tHit'
IR_HD bool nearestLight(const SceneView& sv, const Ray& r, float& tHit, Vec3& Le, int& which)
{
  bool found = false;
  which = -1;
  for (int i = 0; i < sv.numLights; ++i) {
    Vec3 e;
    if (intersectLight(sv, sv.lights[i], r, tHit, e)) { Le = e; which = i; found = true; }
  }
  return found;
}

// Which per-light slot a contribution belongs in.
//
// Every contribution to the pixel goes in exactly one, which is the whole point:
// the slots then add up to the beauty, and a bounce attributed to the wrong
// light shows up as the sum not closing.  A light index past the last named slot
// - and anything that did not come from a Light at all, emission and background
// - lands in the last one.
IR_HD void addToGroup(float* groupOut, int slot, const Vec3& c)
{
  if (!groupOut || slot < 0) return;
  groupOut[slot * 3] += c.x;
  groupOut[slot * 3 + 1] += c.y;
  groupOut[slot * 3 + 2] += c.z;
}

IR_HD int lightGroupSlot(const AovLayout& a, int lightIndex)
{
  if (a.lightGroups < 0 || a.lightGroupCount <= 0) return -1;
  const int other = a.lightGroupCount - 1;
  return (lightIndex >= 0 && lightIndex < other) ? lightIndex : other;
}

// ---- integrator ------------------------------------------------------------------------
struct PixelResult {
  Vec3 color; float alpha;
  float depth; Vec3 normal; float instanceId; Vec3 albedo;
  // extra aovs, only filled when they were asked for
  Vec3 position;
  float motionX, motionY;
  float u, v;
  float materialId, objectId;
  Vec3 directDiffuse, directSpecular, indirectDiffuse, indirectSpecular, emission;
  float roughness, metallic, surfaceOpacity, facing;
  Vec3 specularColor, geoNormal;
  float occlusion;        // 1 = nothing above the hit, 0 = closed in
  Vec3 shadow;            // direct light that geometry stopped from arriving
  IR_HD PixelResult() : color(0.0f), alpha(0.0f), depth(0.0f), normal(0.0f), instanceId(-1.0f), albedo(0.0f),
                        position(0.0f), motionX(0.0f), motionY(0.0f), u(0.0f), v(0.0f),
                        materialId(-1.0f), objectId(-1.0f), directDiffuse(0.0f), directSpecular(0.0f),
                        indirectDiffuse(0.0f), indirectSpecular(0.0f), emission(0.0f),
                        roughness(0.0f), metallic(0.0f), surfaceOpacity(0.0f), facing(0.0f),
                        specularColor(0.0f), geoNormal(0.0f), occlusion(0.0f), shadow(0.0f) {}
};

// world point -> pixel coordinates, the inverse of cameraRay
IR_HD bool projectToPixel(const Camera& cam, const Vec3& P, float& px, float& py)
{
  const Vec3 rel = P - cam.camToWorld.point(Vec3(0.0f));
  const Vec3 cx = normalize(cam.camToWorld.vector(Vec3(1.0f, 0.0f, 0.0f)));
  const Vec3 cy = normalize(cam.camToWorld.vector(Vec3(0.0f, 1.0f, 0.0f)));
  const Vec3 cz = normalize(cam.camToWorld.vector(Vec3(0.0f, 0.0f, -1.0f)));
  const float z = dot(rel, cz);
  if (cam.orthographic) {
    px = (dot(rel, cx) / irMax(cam.orthoHalfW, 1e-6f) * 0.5f + 0.5f) * float(cam.width);
    py = (dot(rel, cy) / irMax(cam.orthoHalfH, 1e-6f) * 0.5f + 0.5f) * float(cam.height);
    return true;
  }
  if (z <= 1e-6f) return false;
  const float sx = dot(rel, cx) / (z * irMax(cam.tanHalfFovX, 1e-6f));
  const float sy = dot(rel, cy) / (z * irMax(cam.tanHalfFovY, 1e-6f));
  px = (sx * 0.5f + 0.5f) * float(cam.width);
  py = (sy * 0.5f + 0.5f) * float(cam.height);
  return true;
}

IR_HD Vec3 hitPositionAt(const SceneView& sv, const HitRecord& h, float time)
{
  const Instance& inst = sv.instances[h.instId];
  const ProtoRange& pr = sv.protos[inst.protoId];
  const int tri = pr.firstTri + h.primId;
  const uint32_t i0 = sv.indices[tri * 3], i1 = sv.indices[tri * 3 + 1], i2 = sv.indices[tri * 3 + 2];
  Vec3 p0 = sv.vertices[i0], p1 = sv.vertices[i1], p2 = sv.vertices[i2];
  if (sv.settings.deformationBlur && sv.vertices1) {
    p0 = p0 + (sv.vertices1[i0] - p0) * time;
    p1 = p1 + (sv.vertices1[i1] - p1) * time;
    p2 = p2 + (sv.vertices1[i2] - p2) * time;
  }
  const float w0 = 1.0f - h.u - h.v;
  const Vec3 pObj = p0 * w0 + p1 * h.u + p2 * h.v;
  const Xform xf = sv.settings.motionBlur ? instanceXformAt(sv, inst, time) : inst.xf;
  return xf.point(pObj);
}

// Screen-space movement of a hit, in pixels per frame - what VectorBlur wants.
IR_HD void hitMotion(const SceneView& sv, const HitRecord& h, float& mx, float& my)
{
  mx = 0.0f; my = 0.0f;
  const RenderSettings& st = sv.settings;
  // no test for motionBlur here: a still scene shot by a MOVING camera still
  // moves on screen, and hitPositionAt handles instances that do not move
  if (st.shutterFrames <= 0.0f) return;
  float x0, y0, x1, y1;
  if (!projectToPixel(sv.cameraMv0, hitPositionAt(sv, h, 0.0f), x0, y0)) return;
  if (!projectToPixel(sv.cameraMv1, hitPositionAt(sv, h, 1.0f), x1, y1)) return;
  const float inv = 1.0f / st.shutterFrames;
  mx = (x1 - x0) * inv;
  my = (y1 - y0) * inv;
}

template <class Tracer>
// groupOut, when it is not null, points at this pixel's per-light slice of the
// output buffer, and every contribution is ADDED to it as it happens.  Nothing
// per-thread holds the groups - see the note on kMaxLightGroups for what that
// costs when it does.  The caller clears the slice before the samples and scales
// it afterwards.
IR_HD void renderSample(const SceneView& sv, Tracer& tr, int px, int py, int sampleIndex, PixelResult& out,
                        float* groupOut = nullptr, float* cryptoOut = nullptr,
                        float* deepOut = nullptr)
{
  const RenderSettings& st = sv.settings;
  Rng rng;
  rng.seed(uint64_t(hashU32(uint32_t(px) * 73856093u ^ uint32_t(py) * 19349663u ^ uint32_t(st.seed) * 83492791u)) ^ (uint64_t(sampleIndex) << 32),
           uint64_t(sampleIndex) * 2u + 1u);
  const float jx = rng.uniform(), jy = rng.uniform();
  // The shutter matters whenever anything moves - the instances, the vertices,
  // OR THE CAMERA.  Leaving the camera out of this was the whole of "camera
  // motion blur does nothing": the ray still asked cameraAt() for its moment,
  // but every ray was given moment 0, so a pan rendered sharp at the shutter-open
  // pose - the right width, hard edges, shifted by half the move.
  // A VOLUME carrying a second frame counts too.  Nothing else in a smoke shot
  // need move - no instance, no camera - and then every ray was dealt shutter
  // moment 0 and the cross-fade never left the first frame: the second .vdb was
  // read, reported, and had no effect on a single pixel.
  const bool anyMotion = (st.motionBlur != 0) || (st.deformationBlur != 0)
                       || (sv.cameraMoves != 0) || (sv.volumeMoves != 0);

  // WHERE IN THE SHUTTER this ray looks, dealt out rather than drawn.
  //
  // A pixel along a streak only sees the object for a fraction of the shutter,
  // so with times drawn independently the number of samples that land on it is
  // binomial - and that variance IS the noise in a motion-blurred frame.  It is
  // the dominant noise here by a distance: this scene is lit by a point light,
  // which is a delta light and adds none at all.
  //
  // Dealing the times out instead - sample i takes the i'th slice of the
  // shutter, jittered inside it - makes the count exact and leaves only the
  // jitter, which is what stratified sampling is for.  ScanlineRender has no
  // such noise because its samples are fixed instants; this is how a stochastic
  // renderer gets the same benefit without giving up a continuous shutter.
  //
  // The stratum spans the samples in THIS pass.  Under progressive refinement
  // each pass re-strata its own chunk, which is still far better than drawing
  // blind, and the passes average to the same answer.
  const int strataCount = (st.samples > 0) ? st.samples : 1;
  const int myStratum = sampleIndex % strataCount;
  const float stratum = (float(myStratum) + rng.uniform()) / float(strataCount);
  float shutterTime = 0.0f;
  if (anyMotion) {
    if (st.fixedShutterTime >= 0.0f) shutterTime = st.fixedShutterTime;
    else if (st.motionSteps >= 1) {
      // ScanlineRender's blur: the shutter is cut into N slices and sampled at
      // the MIDDLE of each, not at the ends.  Measured, not assumed: with two
      // samples Scanline spans 171px of a travel this renderer streaks over
      // 315px, and 171 is what (i + 0.5) / N predicts while ends-included
      // predicts 315.  So N = 1 lands mid-shutter and blurs not at all, which
      // is also what Scanline does with one sample.
      //
      // Which step a ray gets is DEALT OUT, not drawn - see below.
      int step = int(stratum * float(st.motionSteps));
      if (step > st.motionSteps - 1) step = st.motionSteps - 1;
      shutterTime = (float(step) + 0.5f) / float(st.motionSteps);
    }
    else shutterTime = stratum;
  }
  Ray ray = cameraRay(cameraAt(sv, shutterTime), float(px) + jx, float(py) + jy);
  ray.time = shutterTime;
  // one pixel's angular size: the cone this ray traces out
  ray.spread = (sv.camera.height > 0) ? (2.0f * sv.camera.tanHalfFovY / float(sv.camera.height)) : 0.0f;

  Vec3 throughput(1.0f);
  Vec3 L(0.0f);
  float primaryT = 1e30f;      // how far the camera ray went before a surface stopped it
  const Vec3 primaryO = ray.o, primaryD = ray.d;
  // the light-path aovs.  Light reflected by the first surface the camera sees is
  // "direct", split by the lobe that reflected it; everything the path picks up
  // later is "indirect", filed under the lobe the path left that first surface on.
  Vec3 aovDD(0.0f), aovDS(0.0f), aovID(0.0f), aovIS(0.0f), aovEm(0.0f), aovShadow(0.0f);
  const bool wantGroups = (groupOut != nullptr) && (st.aov.lightGroups >= 0);
  const bool wantCrypto = (cryptoOut != nullptr) && (st.aov.crypto >= 0);
  const bool wantDeep = (deepOut != nullptr) && (st.aov.deep >= 0);
  int deepSlot = -1;               // which surface THIS sample started on
  int pathLobe = 0;              // 0 diffuse, 1 specular - set by the first bounce
  int shadeCount = 0;            // opaque surfaces the path has shaded
  bool first = true;
  float lastPdf = 0.0f;          // pdf of the bsdf sample that generated 'ray' (for MIS with the dome)
  for (int bounce = 0; bounce <= st.maxBounces; ++bounce) {
    HitRecord h;
    const bool hit = tr.closest(ray, h);
    if (first) {
      // an area light with visibleToCamera set is drawn where it actually is -
      // only for camera rays, since the bouncing paths find lights through next
      // event estimation and counting both would double their energy
      float tLight = hit ? h.t : 1e30f;
      Vec3 Le;
      int whichLight = -1;
      if (nearestLight(sv, ray, tLight, Le, whichLight)) {
        L += Le;
        aovEm += Le;
        if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, whichLight), Le);
        out.alpha = 1.0f;
        out.depth = tLight;
        out.instanceId = -1.0f;
        out.albedo = Le;
        break;
      }
    }
    if (!hit) {
      // miss: dome light / background
      if (sv.domeLight >= 0) {
        const Light& dome = sv.lights[sv.domeLight];
        const Vec3 Le = domeRadiance(sv, dome, ray.d);
        if (first) {
          if (st.backgroundVisible) {
            L += Le; aovEm += Le;
            if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, sv.domeLight), Le);
          }
        }
        else {
          // the dome is also sampled directly (NEE), so weight this hit against it
          const float pdfL = domePdf(sv, dome, ray.d) / float(sv.numLights > 0 ? sv.numLights : 1);
          const Vec3 c = throughput * Le * misWeight(lastPdf, pdfL);
          L += c;
          if (pathLobe) aovIS += c; else aovID += c;
          if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, sv.domeLight), c);
        }
      }
      else if (first && st.backgroundVisible) {
        L += st.background;
        aovEm += st.background;
        if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, -1), st.background);
      }
      break;
    }
    if (first) primaryT = h.t;
    Surface s;
    if (!buildSurface(sv, ray, h, s)) break;
    const Vec3 wo = -ray.d;
    if (first) {
      // the aovs describe the first surface the camera ray meets, transparent or not
      const Vec3 camZ = normalize(sv.camera.camToWorld.vector(Vec3(0.0f, 0.0f, -1.0f)));
      out.depth = dot(s.P - sv.camera.camToWorld.point(Vec3(0.0f)), camZ);
      out.normal = s.Ns;
      out.instanceId = float(s.instanceId);
      out.albedo = s.albedo;
      out.position = s.P;
      out.u = s.u; out.v = s.v;
      out.materialId = float(s.materialId);
      out.objectId = float(s.objectId);
      // what the surface IS.  Ns is the shading normal, so it carries any normal
      // map; Ng is the geometry's own, which is what a comp wants when it needs
      // the shape rather than the detail.  The facing ratio is against the
      // SHADING normal, because that is the one the lighting used.
      out.roughness = s.roughness;
      out.metallic = s.metallic;
      out.surfaceOpacity = s.opacity;
      out.specularColor = s.F0;
      out.geoNormal = s.Ng;
      out.facing = irAbs(dot(s.Ns, wo));

      // Cryptomatte coverage: this sample landed on this object, so add one
      // sample's worth to that object's entry.  Straight into the pixel's own
      // slice of the output buffer - see the note on kMaxLightGroups in Scene.h
      // for why nothing per-thread holds it.  Scaled to a fraction by the caller.
      if (wantCrypto && s.objectId >= 0 && s.objectId < sv.numProtos)
        cryptoAdd(cryptoOut, st.aov.cryptoSlots, s.cryptoId, 1.0f);

      // Deep: this sample belongs to the surface it MET FIRST, and carries the
      // whole path's radiance to it.  The slot is remembered rather than written
      // now, because that radiance is not known until the path is over.
      if (wantDeep) {
        deepSlot = deepSlotFor(deepOut, st.aov.deepSlots, float(s.objectId));
        if (deepSlot >= 0) {
          // the depth BEFORE the coverage, because "is this the first sample on
          // this surface" is what tells the range where to start
          deepAddDepth(deepOut, deepSlot, out.depth);
          deepOut[deepSlot * kDeepSlotFloats + 1] += 1.0f;
        }
      }

      // Ambient occlusion.  The only AOV here that costs rays of its own, so it
      // is traced only when it was asked for, and how far it looks is a knob -
      // an unbounded one on an interior says "closed" everywhere and tells you
      // nothing.
      //
      // ITS OWN RANDOM STREAM.  Drawing from the path's rng would shift every
      // number after it and change the beauty, so switching the pass on would
      // quietly re-render the image; the odd/even split keeps the two sequences
      // from ever meeting.  occlusion_test checks the beauty is untouched.
      if (st.aov.occlusion >= 0) {
        Rng ao;
        ao.seed(uint64_t(hashU32(uint32_t(px) * 2654435761u ^ uint32_t(py) * 40503u
                                 ^ uint32_t(st.seed) * 22695477u)) ^ (uint64_t(sampleIndex) << 32),
                uint64_t(sampleIndex) * 2u + 2u);
        const int n = irMax(1, st.occlusionSamples);
        const float far = (st.occlusionDistance > 0.0f) ? st.occlusionDistance : 1e30f;
        Vec3 T, B;
        basis(s.Ns, T, B);
        int open = 0;
        for (int i = 0; i < n; ++i) {
          // cosine weighted, so the average is the same cosine-weighted
          // visibility that ambient light would give
          const float r1 = ao.uniform(), r2 = ao.uniform();
          const float phi = 6.28318530f * r1;
          const float rr = irSqrt(r2);
          const Vec3 dl(rr * cosf(phi), rr * sinf(phi), irSqrt(irMax(0.0f, 1.0f - r2)));
          const Vec3 wi = normalize(T * dl.x + B * dl.y + s.Ns * dl.z);
          if (dot(wi, s.Ng) <= 0.0f) { ++open; continue; }   // under the surface: nothing to hit
          Ray probe(s.P + s.Ng * st.rayEpsilon, wi, st.rayEpsilon, far, shutterTime);
          if (!tr.occluded(probe)) ++open;
        }
        out.occlusion = float(open) / float(n);
      }
      if (st.aov.motion >= 0) hitMotion(sv, h, out.motionX, out.motionY);
    }
    // opacity: stochastic transparency.  Coverage is whether THIS sample was
    // stopped by a surface, so alpha is only written once the test passes -
    // averaged over the samples that is the opacity, and a sample that passes
    // through everything and escapes leaves alpha at 0.
    if (s.opacity < 1.0f && rng.uniform() > s.opacity) {
      ray.o = s.P + ray.d * st.rayEpsilon;
      ray.tmin = st.rayEpsilon; ray.tmax = 1e30f; ray.time = shutterTime;
      first = false;
      continue;
    }
    out.alpha = 1.0f;
    first = false;
    ++shadeCount;
    {
      const Vec3 c = throughput * s.emissive;
      L += c;
      if (shadeCount == 1) aovEm += c;
      else if (pathLobe) aovIS += c; else aovID += c;
      if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, -1), c);
    }

    // next event estimation
    if (sv.numLights > 0) {
      const int li = irMin(int(rng.uniform() * float(sv.numLights)), sv.numLights - 1);
      const Light& light = sv.lights[li];
      const float selProb = 1.0f / float(sv.numLights);
      if (light.type != kLightDome) {
        Vec3 wi; float dist; Vec3 Li;
        if (sampleLight(sv, light, s.P, rng, wi, dist, Li) && dot(wi, s.Ns) > 0.0f && dot(wi, s.Ng) > 0.0f) {
          Ray sh(s.P + s.Ng * st.rayEpsilon, wi, st.rayEpsilon, dist - 2.0f * st.rayEpsilon, shutterTime);
          // shadow:enable off means the light ignores geometry; shadow:color
          // tints what it does block instead of removing it
          Vec3 shadow(1.0f);
          if (light.shadowEnable && tr.occluded(sh)) shadow = light.shadowColor;
          // AND THROUGH ANY SMOKE IN THE WAY.  A volume that darkens itself but
          // not the floor under it reads as a hologram: the light passes through
          // it untouched and lands at full strength on everything behind.
          if (light.shadowEnable && sv.numVolumes > 0)
            shadow = shadow * volumeTransmittance(sv, s.P + s.Ng * st.rayEpsilon, wi, dist,
                                                  st.volumeShadowSteps, shutterTime);
          // The shadow pass is what the geometry STOPPED: the same contribution
          // worked out twice, once as it arrived and once as if nothing were in
          // the way.  It costs no extra ray - the shadow ray was already being
          // traced - only the arithmetic, and only at the first hit, which is
          // the shadow a comp means.  Note the test below has to let a fully
          // blocked sample through when the pass is on, or the thing being
          // measured is exactly the thing that gets skipped.
          const bool wantShadow = (st.aov.shadow >= 0) && (shadeCount == 1);
          if (maxComp(shadow) > 0.0f || wantShadow) {
            Vec3 fd, fs;
            bsdfEvalSplit(s, wo, wi, fd, fs);
            const Vec3 unshadowed = throughput * Li / selProb;
            const Vec3 cdFull = fd * light.diffuseMul * unshadowed;
            const Vec3 csFull = fs * light.specularMul * unshadowed;
            const Vec3 cd = cdFull * shadow;
            const Vec3 cs = csFull * shadow;
            L += cd + cs;
            if (wantShadow) aovShadow += (cdFull - cd) + (csFull - cs);
            if (shadeCount == 1) { aovDD += cd; aovDS += cs; }
            else if (pathLobe) aovIS += cd + cs; else aovID += cd + cs;
            if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, li), cd + cs);
          }
        }
      }
      else {
        // dome: importance-sample the lat-long image, MIS against the bsdf
        Vec3 wi, Li; float pdfL = 0.0f;
        const float r1 = rng.uniform(), r2 = rng.uniform();
        if (domeSample(sv, light, r1, r2, wi, Li, pdfL) && pdfL > 0.0f
            && dot(wi, s.Ns) > 0.0f && dot(wi, s.Ng) > 0.0f) {
          Ray sh(s.P + s.Ng * st.rayEpsilon, wi, st.rayEpsilon, 1e30f, shutterTime);
          Vec3 shadow(1.0f);
          if (light.shadowEnable && tr.occluded(sh)) shadow = light.shadowColor;
          // the dome shines through smoke too
          if (light.shadowEnable && sv.numVolumes > 0)
            shadow = shadow * volumeTransmittance(sv, s.P + s.Ng * st.rayEpsilon, wi, 1e30f,
                                                  st.volumeShadowSteps, shutterTime);
          const bool wantShadow = (st.aov.shadow >= 0) && (shadeCount == 1);
          if (maxComp(shadow) > 0.0f || wantShadow) {
            const float pdfLsel = pdfL * selProb;                          // what we actually sampled from
            const float w = misWeight(domePdf(sv, light, wi) * selProb, bsdfPdf(s, wo, wi));
            Vec3 fd, fs;
            bsdfEvalSplit(s, wo, wi, fd, fs);
            const Vec3 unshadowed = throughput * Li * (w / pdfLsel);
            // THE ONE PLACE A DOME CAN OVERFLOW.  The radiance comes from a full
            // resolution texel while the pdf comes from a COARSE average of its
            // neighbourhood, so a single enormous texel in an otherwise dark cell
            // is divided by a pdf that knows nothing about it and the ratio runs
            // away.  Measured on an HDRI that arrived a thousand-fold too bright
            // under an OCIO config: 5283 of 19200 pixels came back NaN, and none
            // under Nuke's own colour management from the same file.
            //
            // Dropped rather than clamped: the sample is meaningless either way,
            // and a NaN averages to NaN however many good samples surround it -
            // one texel can take a whole frame with it.
            if (!(unshadowed.x == unshadowed.x) || !(unshadowed.y == unshadowed.y)
                || !(unshadowed.z == unshadowed.z) || maxComp(unshadowed) > 3.0e38f) continue;
            const Vec3 cdFull = fd * light.diffuseMul * unshadowed;
            const Vec3 csFull = fs * light.specularMul * unshadowed;
            const Vec3 cd = cdFull * shadow;
            const Vec3 cs = csFull * shadow;
            L += cd + cs;
            if (wantShadow) aovShadow += (cdFull - cd) + (csFull - cs);
            if (shadeCount == 1) { aovDD += cd; aovDS += cs; }
            else if (pathLobe) aovIS += cd + cs; else aovID += cd + cs;
            if (wantGroups) addToGroup(groupOut, lightGroupSlot(st.aov, li), cd + cs);
          }
        }
      }
    }
    if (bounce == st.maxBounces) break;

    // continue the path
    Vec3 wi, w; float pdf = 0.0f;
    int lobe = 0;
    if (!bsdfSample(s, wo, rng, wi, w, pdf, &lobe)) break;
    if (shadeCount == 1) pathLobe = lobe;
    if (dot(wi, s.Ng) <= 0.0f) break;
    // Baked occlusion darkens what the surface GATHERS, not what a light puts on
    // it directly. Applying it to direct light too is the usual way an AO map
    // ends up looking like dirt painted on rather than like a crevice.
    if (s.occlusion < 1.0f) throughput *= s.occlusion;
    throughput *= w;
    lastPdf = pdf;
    // Russian roulette
    if (bounce >= 2) {
      const float q = irClamp(maxComp(throughput), 0.05f, 0.95f);
      if (rng.uniform() > q) break;
      throughput *= 1.0f / q;
    }
    ray.o = s.P + s.Ng * st.rayEpsilon;
    ray.d = wi; ray.tmin = st.rayEpsilon; ray.tmax = 1e30f; ray.time = shutterTime;
    // a rough bounce scatters wide, so its rays may read a blurrier level
    ray.spread = ray.spread + s.roughness * 0.5f;
  }
  if (st.clampRadiance > 0.0f) {
    const float m = maxComp(L);
    if (m > st.clampRadiance) {
      const float k = st.clampRadiance / m;
      L *= k; aovDD *= k; aovDS *= k; aovID *= k; aovIS *= k; aovEm *= k; aovShadow *= k;
      // The per-light groups are NOT clamped.  They went into the output
      // buffer as they happened, and this factor is only known once the path is
      // over, so there is nothing left to scale that belongs to this sample
      // alone.  So with a radiance clamp on, the groups are the light BEFORE
      // clamping and will sum slightly above the beauty.  The clamp is off by
      // default; light_group_test checks the sum with it off, and checks that
      // this is the only thing that breaks it.
    }
  }
  // ---- volumes -------------------------------------------------------------------
  // Applied to the CAMERA ray, after the path is done: what the path found is
  // attenuated by whatever it had to shine through, and the volume adds the
  // light it scatters towards the camera on the way.
  //
  // Single scattering only - one shadow ray per step, no bounce between steps -
  // and isotropic, so a step is lit by every light and dimmed by the volume in
  // front of it and by any geometry in the way.  That is what "lit and shaded"
  // means here; multiple scattering is not in this.
  if (sv.numVolumes > 0 && sv.voxels && st.volumeSteps > 0) {
    const float far = (primaryT < 1e29f) ? primaryT : sv.camera.farClip;
    for (int vi = 0; vi < sv.numVolumes; ++vi) {
      const VolumeGrid& g = sv.volumes[vi];
      float t0, t1;
      if (!volumeSlab(g, primaryO, primaryD, 0.0f, far, t0, t1)) continue;
      const int nsteps = st.volumeSteps;
      const float dt = (t1 - t0) / float(nsteps);

      // DEEP: a volume is not one surface, it is a depth of them, so it is cut
      // into segments and each one becomes its own sample.  One sample for the
      // whole volume would put all of it at a single distance and defeat the
      // point of rendering it deep - geometry inside the smoke could not sit
      // between the front and the back of it.
      //
      // The coverage written is the fraction of light the segment removed, which
      // is what the deep composite wants, and the colour is the light it added.
      const int nseg = (wantDeep && st.volumeDeepSegments > 0)
                     ? (st.volumeDeepSegments < nsteps ? st.volumeDeepSegments : nsteps) : 0;
      const int stepsPerSeg = nseg > 0 ? ((nsteps + nseg - 1) / nseg) : 0;
      const Vec3 camZv = normalize(sv.camera.camToWorld.vector(Vec3(0.0f, 0.0f, -1.0f)));
      const float zScale = dot(primaryD, camZv);      // t along the ray -> depth
      float segTrStart = 1.0f;
      Vec3  segCol(0.0f);
      float segFrontT = t0;
      // jittered, or a march lands on the same planes every pixel and the volume
      // gets banded across the frame
      const float jitter = rng.uniform();
      float Tr = 1.0f;
      Vec3 scat(0.0f);
      for (int k = 0; k < nsteps; ++k) {
        const float t = t0 + (float(k) + jitter) * dt;
        if (t >= t1) break;
        const Vec3 P = primaryO + primaryD * t;
        // Emission first, and NOT gated on density: a temperature grid can be hot
        // where the smoke is thin, and gating it would put the flames out at
        // exactly the wrong edge.  It is attenuated by whatever is already in
        // front of it, which is what Tr carries.
        const Vec3 emit = volumeEmission(sv, g, P, shutterTime);
        if (emit.x > 0.0f || emit.y > 0.0f || emit.z > 0.0f) {
          const Vec3 e2 = Tr * emit * dt;
          scat += e2;
          segCol += e2;
        }

        const float dens = volumeDensity(sv, g, P, shutterTime);
        if (dens > 0.0f) {
          const float aTr = expf(-dens * dt);
          // A matte for the volume: what this step actually covered, which is
          // what it took out of everything behind it.
          if (wantCrypto && g.cryptoId != 0.0f)
            cryptoAdd(cryptoOut, st.aov.cryptoSlots, g.cryptoId, Tr * (1.0f - aTr));
          // light arriving here, from every light, through the volume and the scene
          Vec3 Lin(0.0f);
          for (int li = 0; li < sv.numLights; ++li) {
            const Light& light = sv.lights[li];
            Vec3 wi, Li; float dist;
            // The dome is a light like any other here, and a common way to light
            // smoke - it just needs sampling rather than pointing at.  Its pdf is
            // divided out the way the surface path does it; there is no MIS
            // because the phase function is not sampled as well, and no selection
            // probability because every light is summed rather than one picked.
            if (light.type == kLightDome) {
              float pdfL = 0.0f;
              const float r1 = rng.uniform(), r2 = rng.uniform();
              if (!domeSample(sv, light, r1, r2, wi, Li, pdfL) || pdfL <= 0.0f) continue;
              Li = Li * (1.0f / pdfL);
              dist = 1e30f;                       // it is infinitely far away
            }
            else if (!sampleLight(sv, light, P, rng, wi, dist, Li)) continue;
            Vec3 shadow(1.0f);
            if (light.shadowEnable) {
              const float shMax = (dist > 1e29f) ? 1e30f : (dist - 2.0f * st.rayEpsilon);
              Ray sh(P, wi, st.rayEpsilon, shMax, shutterTime);
              if (tr.occluded(sh)) shadow = light.shadowColor;
            }
            if (maxComp(shadow) <= 0.0f) continue;
            // and the volume shadows itself: the light has to get here through it,
            // plus whatever found its way in after bouncing
            const float tau = volumeOpticalDepth(sv, P, wi, dist, st.volumeShadowSteps, shutterTime);
            const float vt = volumeMultiScatter(tau, st.volumeOctaves);
            Lin += Li * shadow * vt;
          }
          // isotropic phase: 1/4pi in every direction
          const Vec3 add = Tr * (1.0f - aTr) * st.volumeAlbedo * Lin * 0.079577472f;
          scat += add;
          segCol += add;
          Tr *= aTr;
        }

        // close a segment off and give it its own deep sample
        if (nseg > 0 && ((k + 1) % stepsPerSeg == 0 || k == nsteps - 1)) {
          const float segCov = segTrStart - Tr;      // what this slice took out
          if (segCov > 1e-6f) {
            const float id = -float(vi * 64 + (k / stepsPerSeg) + 1);
            const int slot = deepSlotFor(deepOut, st.aov.deepSlots, id);
            if (slot >= 0) {
              const float zf = (t0 + float(k + 1 - stepsPerSeg) * dt) * zScale;
              const float zb = (t0 + float(k + 1) * dt) * zScale;
              deepAddDepth(deepOut, slot, zf > 0.0f ? zf : 0.0f);
              deepAddDepth(deepOut, slot, zb > 0.0f ? zb : 0.0f);
              float* e = deepOut + slot * kDeepSlotFloats;
              e[1] += segCov;
              e[4] += segCol.x; e[5] += segCol.y; e[6] += segCol.z;
            }
          }
          segTrStart = Tr;
          segCol = Vec3(0.0f);
          segFrontT = t0 + float(k + 1) * dt;
        }
      }
      (void)segFrontT;
      L = L * Tr + scat;
      out.alpha = out.alpha * Tr + (1.0f - Tr);
    }
  }

  // ---- one bad sample must not take the pixel with it ------------------------------
  // A sample can come back non-finite from arithmetic that is perfectly correct
  // until the numbers get silly: importance sampling divides the radiance at a
  // texel by a pdf built from a COARSE average of its neighbourhood, so a single
  // enormous texel in an otherwise dark cell is amplified by the ratio between
  // them.  Measured on an HDRI dome that arrived a thousand-fold too bright under
  // an OCIO config: 5283 of 19200 samples came back NaN.
  //
  // A NaN averages to NaN however many good samples surround it, so the pixel is
  // lost - and a whole frame can be lost to a handful of texels.  Dropping the
  // sample loses a little energy in a place that was already meaningless, and
  // keeps the frame.
  if (!(L.x == L.x) || !(L.y == L.y) || !(L.z == L.z)
      || L.x > 3.0e38f || L.y > 3.0e38f || L.z > 3.0e38f
      || L.x < -3.0e38f || L.y < -3.0e38f || L.z < -3.0e38f) {
    L = Vec3(0.0f);
    aovDD = aovDS = aovID = aovIS = aovEm = aovShadow = Vec3(0.0f);
  }
  out.color = L;
  // the sample's radiance goes to the surface it started on
  if (wantDeep && deepSlot >= 0) {
    float* e = deepOut + deepSlot * kDeepSlotFloats;
    e[4] += L.x; e[5] += L.y; e[6] += L.z;
  }
  out.shadow = aovShadow;
  out.directDiffuse = aovDD; out.directSpecular = aovDS;
  out.indirectDiffuse = aovID; out.indirectSpecular = aovIS;
  out.emission = aovEm;
}

// The aovs that are AVERAGED over the samples, as the beauty is.  Both back-ends
// call these two, so a new one cannot reach one device and not the other - which
// is easy to do, because the two accumulation loops are otherwise identical and
// sit in different files.
IR_HD void accumulateAovs(PixelResult& sum, const PixelResult& pr)
{
  sum.directDiffuse += pr.directDiffuse;
  sum.directSpecular += pr.directSpecular;
  sum.indirectDiffuse += pr.indirectDiffuse;
  sum.indirectSpecular += pr.indirectSpecular;
  sum.emission += pr.emission;
  sum.shadow += pr.shadow;
  sum.occlusion += pr.occlusion;
}

// Fold the averages back into the first sample's record, which is what carries
// the aovs that come from ONE sample - depth, the ids, the surface properties.
IR_HD void finishAovs(PixelResult& first, const PixelResult& sum, float inv)
{
  first.directDiffuse = sum.directDiffuse * inv;
  first.directSpecular = sum.directSpecular * inv;
  first.indirectDiffuse = sum.indirectDiffuse * inv;
  first.indirectSpecular = sum.indirectSpecular * inv;
  first.emission = sum.emission * inv;
  first.shadow = sum.shadow * inv;
  first.occlusion = sum.occlusion * inv;
}

// One pixel's extra aovs into the packed record.  Both back-ends call this, so
// the layout is written down in exactly one place.
IR_HD void writeExtraAovs(const AovLayout& a, const PixelResult& pr, float* extra)
{
  if (!extra || a.stride <= 0) return;
  if (a.position >= 0) { extra[a.position] = pr.position.x; extra[a.position + 1] = pr.position.y; extra[a.position + 2] = pr.position.z; }
  if (a.motion >= 0) { extra[a.motion] = pr.motionX; extra[a.motion + 1] = pr.motionY; }
  if (a.uv >= 0) { extra[a.uv] = pr.u; extra[a.uv + 1] = pr.v; }
  if (a.materialId >= 0) extra[a.materialId] = pr.materialId;
  if (a.objectId >= 0) extra[a.objectId] = pr.objectId;
  if (a.directDiffuse >= 0) { extra[a.directDiffuse] = pr.directDiffuse.x; extra[a.directDiffuse + 1] = pr.directDiffuse.y; extra[a.directDiffuse + 2] = pr.directDiffuse.z; }
  if (a.directSpecular >= 0) { extra[a.directSpecular] = pr.directSpecular.x; extra[a.directSpecular + 1] = pr.directSpecular.y; extra[a.directSpecular + 2] = pr.directSpecular.z; }
  if (a.indirectDiffuse >= 0) { extra[a.indirectDiffuse] = pr.indirectDiffuse.x; extra[a.indirectDiffuse + 1] = pr.indirectDiffuse.y; extra[a.indirectDiffuse + 2] = pr.indirectDiffuse.z; }
  if (a.indirectSpecular >= 0) { extra[a.indirectSpecular] = pr.indirectSpecular.x; extra[a.indirectSpecular + 1] = pr.indirectSpecular.y; extra[a.indirectSpecular + 2] = pr.indirectSpecular.z; }
  if (a.emission >= 0) { extra[a.emission] = pr.emission.x; extra[a.emission + 1] = pr.emission.y; extra[a.emission + 2] = pr.emission.z; }
  if (a.surface >= 0) {
    extra[a.surface] = pr.roughness;
    extra[a.surface + 1] = pr.metallic;
    extra[a.surface + 2] = pr.surfaceOpacity;
    extra[a.surface + 3] = pr.facing;
  }
  if (a.specularColor >= 0) { extra[a.specularColor] = pr.specularColor.x; extra[a.specularColor + 1] = pr.specularColor.y; extra[a.specularColor + 2] = pr.specularColor.z; }
  if (a.geoNormal >= 0) { extra[a.geoNormal] = pr.geoNormal.x; extra[a.geoNormal + 1] = pr.geoNormal.y; extra[a.geoNormal + 2] = pr.geoNormal.z; }
  if (a.occlusion >= 0) extra[a.occlusion] = pr.occlusion;
  if (a.shadow >= 0) { extra[a.shadow] = pr.shadow.x; extra[a.shadow + 1] = pr.shadow.y; extra[a.shadow + 2] = pr.shadow.z; }
}

} // namespace ir
