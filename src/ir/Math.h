// InstanceRender - Math.h
// Small host/device math library shared by the CPU (MSVC) and GPU (NVCC)
// paths.  Everything in here must compile in both; no std:: math, no
// exceptions, no virtuals.  Strict ASCII.
#pragma once

#ifdef __CUDACC__
#  define IR_HD __host__ __device__ __forceinline__
#  define IR_DEVICE __device__
#else
#  define IR_HD inline
#  define IR_DEVICE
#endif

#include <math.h>
#include <stdint.h>

namespace ir {

IR_HD float irMin(float a, float b) { return a < b ? a : b; }
IR_HD float irMax(float a, float b) { return a > b ? a : b; }
IR_HD float irClamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
IR_HD float irAbs(float x) { return x < 0.0f ? -x : x; }
IR_HD float irSqrt(float x) { return sqrtf(x > 0.0f ? x : 0.0f); }
IR_HD float irLerp(float a, float b, float t) { return a + (b - a) * t; }

struct Vec3 {
  float x, y, z;
  IR_HD Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
  IR_HD Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
  IR_HD explicit Vec3(float a) : x(a), y(a), z(a) {}
  IR_HD Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
  IR_HD Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
  IR_HD Vec3 operator*(const Vec3& o) const { return Vec3(x * o.x, y * o.y, z * o.z); }
  IR_HD Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
  IR_HD Vec3 operator/(float s) const { const float r = 1.0f / s; return Vec3(x * r, y * r, z * r); }
  IR_HD Vec3 operator-() const { return Vec3(-x, -y, -z); }
  IR_HD Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  IR_HD Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  IR_HD Vec3& operator*=(const Vec3& o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
  IR_HD float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
IR_HD Vec3 operator*(float s, const Vec3& v) { return Vec3(v.x * s, v.y * s, v.z * s); }
IR_HD float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
IR_HD Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
IR_HD float length(const Vec3& v) { return irSqrt(dot(v, v)); }
IR_HD Vec3 normalize(const Vec3& v) { const float l = length(v); return l > 1e-20f ? v * (1.0f / l) : Vec3(0.0f, 0.0f, 1.0f); }
IR_HD float maxComp(const Vec3& v) { return irMax(v.x, irMax(v.y, v.z)); }
IR_HD float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
IR_HD Vec3 vmin(const Vec3& a, const Vec3& b) { return Vec3(irMin(a.x, b.x), irMin(a.y, b.y), irMin(a.z, b.z)); }
IR_HD Vec3 vmax(const Vec3& a, const Vec3& b) { return Vec3(irMax(a.x, b.x), irMax(a.y, b.y), irMax(a.z, b.z)); }
IR_HD Vec3 vlerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
IR_HD Vec3 reflect(const Vec3& i, const Vec3& n) { return i - n * (2.0f * dot(i, n)); }

struct Vec4 {
  float x, y, z, w;
  IR_HD Vec4() : x(0), y(0), z(0), w(0) {}
  IR_HD Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

// 3x4 affine transform: row-major rows r0..r2, applied as p' = M * [p, 1]
struct Xform {
  float m[12];
  IR_HD static Xform identity()
  {
    Xform x;
    for (int i = 0; i < 12; ++i) x.m[i] = 0.0f;
    x.m[0] = 1.0f; x.m[5] = 1.0f; x.m[10] = 1.0f;
    return x;
  }
  IR_HD Vec3 point(const Vec3& p) const
  {
    return Vec3(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
  }
  IR_HD Vec3 vector(const Vec3& v) const
  {
    return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[4] * v.x + m[5] * v.y + m[6] * v.z,
                m[8] * v.x + m[9] * v.y + m[10] * v.z);
  }
  // normals transform with the inverse transpose of the 3x3 part
  IR_HD Vec3 normal(const Vec3& n) const
  {
    // cofactor matrix of the 3x3 (inverse transpose up to scale)
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const Vec3 r0(e * i - f * h, f * g - d * i, d * h - e * g);
    const Vec3 r1(c * h - b * i, a * i - c * g, b * g - a * h);
    const Vec3 r2(b * f - c * e, c * d - a * f, a * e - b * d);
    return normalize(Vec3(dot(r0, n), dot(r1, n), dot(r2, n)));
  }
};

// b applied first, then a - the same order as multiplying two 4x4 matrices with
// the implied 0,0,0,1 bottom row.
IR_HD Xform mul(const Xform& a, const Xform& b)
{
  Xform r;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      r.m[row * 4 + col] = a.m[row * 4 + 0] * b.m[0 * 4 + col]
                         + a.m[row * 4 + 1] * b.m[1 * 4 + col]
                         + a.m[row * 4 + 2] * b.m[2 * 4 + col];
    }
    r.m[row * 4 + 3] = a.m[row * 4 + 0] * b.m[3]
                     + a.m[row * 4 + 1] * b.m[7]
                     + a.m[row * 4 + 2] * b.m[11]
                     + a.m[row * 4 + 3];
  }
  return r;
}

struct Ray {
  Vec3 o, d;
  float tmin, tmax;
  float time;        // normalised shutter time [0,1] for motion blur
  float spread;      // ray cone half-angle: how wide the ray's footprint grows per unit distance
  IR_HD Ray() : tmin(0.0f), tmax(1e30f), time(0.0f), spread(0.0f) {}
  IR_HD Ray(const Vec3& origin, const Vec3& dir, float mn = 0.0f, float mx = 1e30f, float t = 0.0f, float sp = 0.0f)
    : o(origin), d(dir), tmin(mn), tmax(mx), time(t), spread(sp) {}
};

// orthonormal basis around n (Duff et al.)
IR_HD void basis(const Vec3& n, Vec3& t, Vec3& b)
{
  const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (sign + n.z);
  const float bb = n.x * n.y * a;
  t = Vec3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
  b = Vec3(bb, sign + n.y * n.y * a, -n.y);
}

// ---- deterministic random numbers (PCG32) - identical on CPU and GPU ----
struct Rng {
  uint64_t state, inc;
  IR_HD void seed(uint64_t s, uint64_t seq)
  {
    state = 0u; inc = (seq << 1u) | 1u;
    next(); state += s; next();
  }
  IR_HD uint32_t next()
  {
    const uint64_t old = state;
    state = old * 6364136223846793005ULL + inc;
    const uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = uint32_t(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
  }
  IR_HD float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }   // [0,1)
};

IR_HD uint32_t hashU32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

} // namespace ir
