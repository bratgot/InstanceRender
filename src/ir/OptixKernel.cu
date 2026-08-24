// InstanceRender - OptixKernel.cu
// The GPU side: ray generation + hit/miss programs that fill the same
// HitRecord the CPU tracer fills, then call the shared renderSample() from
// Kernel.h.  Compiled to PTX by nvcc (see CMakeLists.txt).  Strict ASCII.

#include <optix.h>

#include "ir/Math.h"
#include "ir/Scene.h"
#include "ir/Kernel.h"
#include "ir/OptixShared.h"

// A __constant__ variable must be constant-initialised; LaunchParams has
// constructors (they are shared with the CPU path), so the parameters live in a
// raw aligned blob and are read through a reference.
extern "C" {
__constant__ __align__(16) char paramsRaw[sizeof(ir::LaunchParams)];
}
#define params (*reinterpret_cast<const ir::LaunchParams*>(paramsRaw))

namespace {

// payload: pack a HitRecord through 5 registers
__device__ __forceinline__ void setHitPayload(int instId, int primId, float t, float u, float v)
{
  optixSetPayload_0(unsigned(instId));
  optixSetPayload_1(unsigned(primId));
  optixSetPayload_2(__float_as_uint(t));
  optixSetPayload_3(__float_as_uint(u));
  optixSetPayload_4(__float_as_uint(v));
}

// Tracer with the same interface as the Embree one, so Kernel.h is identical
struct OptixTracer {
  __device__ __forceinline__ bool closest(const ir::Ray& r, ir::HitRecord& h) const
  {
    unsigned p0 = 0xFFFFFFFFu, p1 = 0u, p2 = 0u, p3 = 0u, p4 = 0u;
    optixTrace(params.handle,
               make_float3(r.o.x, r.o.y, r.o.z), make_float3(r.d.x, r.d.y, r.d.z),
               r.tmin, r.tmax, r.time,
               OptixVisibilityMask(255), OPTIX_RAY_FLAG_NONE,
               0 /*SBT offset*/, 1 /*SBT stride*/, 0 /*miss index*/,
               p0, p1, p2, p3, p4);
    if (p0 == 0xFFFFFFFFu) return false;
    h.instId = int(p0);
    h.primId = int(p1);
    h.t = __uint_as_float(p2);
    h.u = __uint_as_float(p3);
    h.v = __uint_as_float(p4);
    return true;
  }
  __device__ __forceinline__ bool occluded(const ir::Ray& r) const
  {
    // Shadow rays run no hit program (any-hit is disabled on the geometry and
    // closest-hit is skipped), so the payload must ASSUME occlusion and the miss
    // program clears it - the other way round the shadow ray always reports
    // "visible" and the GPU renders without shadows.
    unsigned hit = 1u;
    optixTrace(params.handle,
               make_float3(r.o.x, r.o.y, r.o.z), make_float3(r.d.x, r.d.y, r.d.z),
               r.tmin, r.tmax, r.time,
               OptixVisibilityMask(255),
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT | OPTIX_RAY_FLAG_DISABLE_ANYHIT,
               0, 1, 1 /*miss index 1 = shadow*/,
               hit);
    return hit != 0u;
  }
};

} // namespace

extern "C" __global__ void __raygen__ir()
{
  const uint3 idx = optixGetLaunchIndex();
  const int px = int(idx.x), py = int(idx.y);
  if (px >= params.width || py >= params.height) return;
  OptixTracer tracer;
  ir::Vec3 sumC(0.0f);
  float sumA = 0.0f;
  ir::PixelResult first, sum;
  const int spp = params.samples > 0 ? params.samples : 1;
  const int s0 = params.sampleOffset > 0 ? params.sampleOffset : 0;
  const unsigned pixel = unsigned(py) * unsigned(params.width) + unsigned(px);
  const ir::AovLayout& aovL = params.scene.settings.aov;
  // this pixel's per-light slice, which the samples add straight into - nothing
  // per-thread holds them, see the note on kMaxLightGroups in Scene.h
  float* groups = nullptr;
  const int groupFloats = aovL.lightGroupCount * 3;
  if (params.extra && aovL.lightGroups >= 0 && aovL.stride > 0) {
    groups = params.extra + size_t(pixel) * size_t(aovL.stride) + size_t(aovL.lightGroups);
    for (int g = 0; g < groupFloats; ++g) groups[g] = 0.0f;
  }
  float* crypto = nullptr;
  const int cryptoFloats = aovL.cryptoSlots * 2;
  if (params.extra && aovL.crypto >= 0 && aovL.stride > 0) {
    crypto = params.extra + size_t(pixel) * size_t(aovL.stride) + size_t(aovL.crypto);
    for (int g = 0; g < cryptoFloats; ++g) crypto[g] = 0.0f;
  }
  float* deep = nullptr;
  const int deepFloats = aovL.deepSlots * ir::kDeepSlotFloats;
  if (params.extra && aovL.deep >= 0 && aovL.stride > 0) {
    deep = params.extra + size_t(pixel) * size_t(aovL.stride) + size_t(aovL.deep);
    for (int g = 0; g < deepFloats; ++g) deep[g] = 0.0f;
  }
  for (int s = 0; s < spp; ++s) {
    ir::PixelResult pr;
    ir::renderSample(params.scene, tracer, px, py, s0 + s, pr, groups, crypto, deep);
    sumC += pr.color; sumA += pr.alpha;
    ir::accumulateAovs(sum, pr);
    if (s == 0) first = pr;
  }
  const float inv = 1.0f / float(spp);
  if (groups) for (int g = 0; g < groupFloats; ++g) groups[g] *= inv;
  if (deep) {
    for (int g = 0; g < aovL.deepSlots; ++g) {
      float* e = deep + g * ir::kDeepSlotFloats;
      const float hits = e[1];
      if (hits <= 0.0f) { for (int k = 0; k < ir::kDeepSlotFloats; ++k) e[k] = 0.0f; continue; }
      // e[2] and e[3] are the nearest and farthest this surface came:
      // extremes, not sums, so nothing is divided into them
      e[1] = hits * inv;
      e[4] *= inv; e[5] *= inv; e[6] *= inv;
    }
    ir::deepSortByDepth(deep, aovL.deepSlots);
  }
  if (crypto) {
    for (int g = 0; g < aovL.cryptoSlots; ++g) crypto[g * 2 + 1] *= inv;
    ir::cryptoSort(crypto, aovL.cryptoSlots);
  }
  const unsigned pi = pixel;
  params.rgba[pi * 4 + 0] = sumC.x * inv;
  params.rgba[pi * 4 + 1] = sumC.y * inv;
  params.rgba[pi * 4 + 2] = sumC.z * inv;
  params.rgba[pi * 4 + 3] = sumA * inv;
  if (params.depth) params.depth[pi] = first.depth;
  if (params.normal) { params.normal[pi * 3] = first.normal.x; params.normal[pi * 3 + 1] = first.normal.y; params.normal[pi * 3 + 2] = first.normal.z; }
  if (params.instanceId) params.instanceId[pi] = first.instanceId;
  if (params.albedo) { params.albedo[pi * 3] = first.albedo.x; params.albedo[pi * 3 + 1] = first.albedo.y; params.albedo[pi * 3 + 2] = first.albedo.z; }
  const ir::AovLayout& aov = params.scene.settings.aov;
  if (params.extra && aov.stride > 0) {
    ir::finishAovs(first, sum, inv);
    ir::writeExtraAovs(aov, first, params.extra + size_t(pi) * size_t(aov.stride));
  }
}

extern "C" __global__ void __miss__ir()
{
  optixSetPayload_0(0xFFFFFFFFu);
}

extern "C" __global__ void __miss__shadow()
{
  optixSetPayload_0(0u);
}

extern "C" __global__ void __closesthit__ir()
{
  const float2 bary = optixGetTriangleBarycentrics();
  setHitPayload(int(optixGetInstanceId()), int(optixGetPrimitiveIndex()), optixGetRayTmax(), bary.x, bary.y);
}

