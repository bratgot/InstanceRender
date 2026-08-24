// InstanceRender - EmbreeBackend.cpp  (strict ASCII)
#include "EmbreeBackend.h"
#include "Kernel.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace ir {

namespace {

struct EmbreeTracer {
  RTCScene scene;
  IR_HD bool closest(const Ray& r, HitRecord& h) const
  {
    RTCRayHit rh;
    rh.ray.org_x = r.o.x; rh.ray.org_y = r.o.y; rh.ray.org_z = r.o.z;
    rh.ray.dir_x = r.d.x; rh.ray.dir_y = r.d.y; rh.ray.dir_z = r.d.z;
    rh.ray.tnear = r.tmin; rh.ray.tfar = r.tmax; rh.ray.time = r.time;
    rh.ray.mask = 0xFFFFFFFFu; rh.ray.id = 0; rh.ray.flags = 0;
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.primID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    rtcIntersect1(scene, &rh, &args);
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) return false;
    h.instId = int(rh.hit.instID[0]);      // top-level instance geometry id == our instance index
    h.primId = int(rh.hit.primID);
    h.t = rh.ray.tfar;
    h.u = rh.hit.u; h.v = rh.hit.v;
    return true;
  }
  IR_HD bool occluded(const Ray& r) const
  {
    RTCRay ray;
    ray.org_x = r.o.x; ray.org_y = r.o.y; ray.org_z = r.o.z;
    ray.dir_x = r.d.x; ray.dir_y = r.d.y; ray.dir_z = r.d.z;
    ray.tnear = r.tmin; ray.tfar = r.tmax; ray.time = r.time;
    ray.mask = 0xFFFFFFFFu; ray.id = 0; ray.flags = 0;
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    rtcOccluded1(scene, &ray, &args);
    return ray.tfar < 0.0f;   // embree sets tfar = -inf when occluded
  }
};

void embreeError(void*, RTCError code, const char* str)
{
  std::fprintf(stderr, "InstanceRender/Embree error %d: %s\n", int(code), str ? str : "");
}

} // namespace

CpuRenderer::CpuRenderer() : _device(nullptr), _top(nullptr) {}
CpuRenderer::~CpuRenderer() { release(); }

std::string CpuRenderer::version()
{
  return "Embree " + std::to_string(RTC_VERSION_MAJOR) + "." + std::to_string(RTC_VERSION_MINOR) + "." + std::to_string(RTC_VERSION_PATCH);
}

void CpuRenderer::release()
{
  if (_top) { rtcReleaseScene(_top); _top = nullptr; }
  for (size_t i = 0; i < _protoScenes.size(); ++i) if (_protoScenes[i]) rtcReleaseScene(_protoScenes[i]);
  _protoScenes.clear();
  if (_device) { rtcReleaseDevice(_device); _device = nullptr; }
}

bool CpuRenderer::build(const Scene& scene, std::string& err)
{
  release();
  const auto t0 = std::chrono::steady_clock::now();
  _device = rtcNewDevice(nullptr);
  if (!_device) { err = "rtcNewDevice failed"; return false; }
  rtcSetDeviceErrorFunction(_device, embreeError, nullptr);

  // one scene per prototype (triangle soup)
  _protoScenes.resize(scene.protos.size(), nullptr);
  for (size_t p = 0; p < scene.protos.size(); ++p) {
    const ProtoRange& pr = scene.protos[p];
    RTCScene ps = rtcNewScene(_device);
    rtcSetSceneFlags(ps, RTC_SCENE_FLAG_COMPACT);
    rtcSetSceneBuildQuality(ps, RTC_BUILD_QUALITY_HIGH);
    if (pr.numTris > 0 && pr.numVertices > 0) {
      RTCGeometry g = rtcNewGeometry(_device, RTC_GEOMETRY_TYPE_TRIANGLE);
      const bool deform = (scene.vertices1.size() == scene.vertices.size() && !scene.vertices1.empty());
      // deforming geometry gets a second vertex buffer, one per shutter end
      if (deform) rtcSetGeometryTimeStepCount(g, 2);
      float* vb = static_cast<float*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), size_t(pr.numVertices)));
      float* vb1 = deform ? static_cast<float*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 1, RTC_FORMAT_FLOAT3, 3 * sizeof(float), size_t(pr.numVertices))) : nullptr;
      unsigned* ib = static_cast<unsigned*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), size_t(pr.numTris)));
      if (!vb || !ib || (deform && !vb1)) { err = "Embree buffer allocation failed"; rtcReleaseGeometry(g); rtcReleaseScene(ps); return false; }
      for (int i = 0; i < pr.numVertices; ++i) {
        const Vec3& v = scene.vertices[size_t(pr.firstVertex + i)];
        vb[i * 3] = v.x; vb[i * 3 + 1] = v.y; vb[i * 3 + 2] = v.z;
        if (vb1) {
          const Vec3& v1 = scene.vertices1[size_t(pr.firstVertex + i)];
          vb1[i * 3] = v1.x; vb1[i * 3 + 1] = v1.y; vb1[i * 3 + 2] = v1.z;
        }
      }
      for (int t = 0; t < pr.numTris; ++t) {
        // indices are absolute -> make them local to the prototype
        for (int k = 0; k < 3; ++k) ib[t * 3 + k] = scene.indices[size_t(pr.firstTri + t) * 3 + k] - unsigned(pr.firstVertex);
      }
      rtcCommitGeometry(g);
      rtcAttachGeometry(ps, g);
      rtcReleaseGeometry(g);
    }
    rtcCommitScene(ps);
    _protoScenes[p] = ps;
  }

  // top level: one instance geometry per instance, geometry id == instance index
  _top = rtcNewScene(_device);
  rtcSetSceneFlags(_top, RTC_SCENE_FLAG_DYNAMIC);
  for (size_t i = 0; i < scene.instances.size(); ++i) {
    const Instance& inst = scene.instances[i];
    RTCGeometry ig = rtcNewGeometry(_device, RTC_GEOMETRY_TYPE_INSTANCE);
    RTCScene ps = (inst.protoId >= 0 && size_t(inst.protoId) < _protoScenes.size()) ? _protoScenes[size_t(inst.protoId)] : nullptr;
    if (ps) rtcSetGeometryInstancedScene(ig, ps);
    if (scene.hasMotion) {
      // one key per motion sample, linearly interpolated between them - the same
      // interpolation instanceXformAt() uses to rebuild the surface, and the same
      // OptiX matrix motion does
      const int keys = (scene.motionKeyCount >= 2) ? scene.motionKeyCount : 2;
      rtcSetGeometryTimeStepCount(ig, unsigned(keys));
      for (int k = 0; k < keys; ++k) {
        const Xform& x = (scene.motionKeyCount >= 2)
                       ? scene.motionKeys[size_t(inst.firstKey + k)]
                       : (k == 0 ? inst.xf : inst.xf1);
        rtcSetGeometryTransform(ig, unsigned(k), RTC_FORMAT_FLOAT3X4_ROW_MAJOR, x.m);
      }
    }
    else {
      rtcSetGeometryTimeStepCount(ig, 1);
      rtcSetGeometryTransform(ig, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, inst.xf.m);
    }
    rtcCommitGeometry(ig);
    const unsigned id = rtcAttachGeometry(_top, ig);
    (void)id;   // sequential from 0 -> equals i
    rtcReleaseGeometry(ig);
  }
  rtcCommitScene(_top);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::ostringstream os;
  os << "Embree: " << scene.protos.size() << " prototype(s), " << scene.numTriangles() << " unique triangle(s), "
     << scene.instances.size() << " instance(s) = " << std::fixed << scene.expandedTriangles() / 1e6 << "M triangles rendered, build "
     << int(ms) << " ms";
  _stats = _buildStats = os.str();
  return true;
}

void CpuRenderer::render(const Scene& scene, const RenderSettings& settings, FrameBuffers& fb, std::atomic<bool>* cancel,
                         const std::function<void(float)>& progress)
{
  const int W = settings.width, H = settings.height;
  fb.allocate(W, H, settings.aov.stride);
  if (!_top || W <= 0 || H <= 0) return;
  SceneView sv = scene.view();
  sv.settings = settings;
  sv.camera.width = W; sv.camera.height = H;
  EmbreeTracer tracer; tracer.scene = _top;
  const int spp = std::max(1, settings.samples);
  const int s0 = std::max(0, settings.sampleOffset);   // progressive: continue the sequence

  const int tile = 16;
  const int tilesX = (W + tile - 1) / tile, tilesY = (H + tile - 1) / tile;
  const int numTiles = tilesX * tilesY;
  std::atomic<int> nextTile(0);
  std::atomic<int> doneTiles(0);
  unsigned nThreads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  auto worker = [&]() {
    for (;;) {
      const int t = nextTile.fetch_add(1);
      if (t >= numTiles) break;
      if (cancel && cancel->load()) break;
      const int tx = t % tilesX, ty = t / tilesX;
      const int x0 = tx * tile, y0 = ty * tile;
      const int x1 = std::min(W, x0 + tile), y1 = std::min(H, y0 + tile);
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          Vec3 sumC(0.0f); float sumA = 0.0f;
          PixelResult first, sum;
          const size_t pi = size_t(y) * W + x;
          // this pixel's per-light slice, which the samples add straight into
          float* groups = nullptr;
          const int groupFloats = settings.aov.lightGroupCount * 3;
          if (settings.aov.lightGroups >= 0 && fb.aovStride > 0) {
            groups = &fb.extra[pi * size_t(fb.aovStride) + size_t(settings.aov.lightGroups)];
            for (int g = 0; g < groupFloats; ++g) groups[g] = 0.0f;
          }
          float* crypto = nullptr;
          const int cryptoFloats = settings.aov.cryptoSlots * 2;
          if (settings.aov.crypto >= 0 && fb.aovStride > 0) {
            crypto = &fb.extra[pi * size_t(fb.aovStride) + size_t(settings.aov.crypto)];
            for (int g = 0; g < cryptoFloats; ++g) crypto[g] = 0.0f;
          }
          float* deep = nullptr;
          const int deepFloats = settings.aov.deepSlots * kDeepSlotFloats;
          if (settings.aov.deep >= 0 && fb.aovStride > 0) {
            deep = &fb.extra[pi * size_t(fb.aovStride) + size_t(settings.aov.deep)];
            for (int g = 0; g < deepFloats; ++g) deep[g] = 0.0f;
          }
          for (int s = 0; s < spp; ++s) {
            PixelResult pr;
            renderSample(sv, tracer, x, y, s0 + s, pr, groups, crypto, deep);
            sumC += pr.color; sumA += pr.alpha;
            // the light aovs are averaged like the beauty; the geometric ones
            // come from one sample, as depth and the ids already do
            accumulateAovs(sum, pr);
            if (s == 0) first = pr;
          }
          const float inv = 1.0f / float(spp);
          fb.rgba[pi * 4] = sumC.x * inv; fb.rgba[pi * 4 + 1] = sumC.y * inv; fb.rgba[pi * 4 + 2] = sumC.z * inv; fb.rgba[pi * 4 + 3] = sumA * inv;
          fb.depth[pi] = first.depth;
          fb.normal[pi * 3] = first.normal.x; fb.normal[pi * 3 + 1] = first.normal.y; fb.normal[pi * 3 + 2] = first.normal.z;
          fb.instanceId[pi] = first.instanceId;
          fb.albedo[pi * 3] = first.albedo.x; fb.albedo[pi * 3 + 1] = first.albedo.y; fb.albedo[pi * 3 + 2] = first.albedo.z;
          if (groups) for (int g = 0; g < groupFloats; ++g) groups[g] *= inv;
          if (deep) {
            // coverage as a fraction of the samples, depth as the mean over the
            // samples that HIT, colour as the mean over all of them - which is
            // what makes the entries add back up to the beauty
            for (int g = 0; g < settings.aov.deepSlots; ++g) {
              float* e = deep + g * kDeepSlotFloats;
              const float hits = e[1];
              if (hits <= 0.0f) { for (int k = 0; k < kDeepSlotFloats; ++k) e[k] = 0.0f; continue; }
              // e[2] and e[3] are the nearest and farthest this surface came:
              // extremes, not sums, so nothing is divided into them
              e[1] = hits * inv;
              e[4] *= inv; e[5] *= inv; e[6] *= inv;
            }
            deepSortByDepth(deep, settings.aov.deepSlots);
          }
          if (crypto) {
            // coverage as a fraction of the samples, heaviest first: "rank" in
            // the format means exactly that order
            for (int g = 0; g < settings.aov.cryptoSlots; ++g) crypto[g * 2 + 1] *= inv;
            cryptoSort(crypto, settings.aov.cryptoSlots);
          }
          if (fb.aovStride > 0) {
            finishAovs(first, sum, inv);
            writeExtraAovs(settings.aov, first, &fb.extra[pi * size_t(fb.aovStride)]);
          }
        }
      }
      const int d = doneTiles.fetch_add(1) + 1;
      if (progress && (d % 16) == 0) progress(float(d) / float(numTiles));
    }
  };
  for (unsigned i = 0; i < nThreads; ++i) threads.emplace_back(worker);
  for (auto& th : threads) th.join();
  if (progress) progress(1.0f);

  // diagnostics: coverage + one centre ray, appended to the build stats
  {
    size_t hits = 0;
    for (size_t i = 0; i < size_t(W) * H; ++i) if (fb.rgba[i * 4 + 3] > 0.0f) ++hits;
    Ray centre = cameraRay(sv.camera, float(W) * 0.5f, float(H) * 0.5f);
    HitRecord h;
    const bool hit = tracer.closest(centre, h);
    std::ostringstream os;
    os << _buildStats << "; hits " << hits << "/" << (size_t(W) * H)
       << "; centre ray o(" << centre.o.x << "," << centre.o.y << "," << centre.o.z << ") d("
       << centre.d.x << "," << centre.d.y << "," << centre.d.z << ") -> " << (hit ? "hit" : "miss");
    if (hit) os << " inst " << h.instId << " prim " << h.primId << " t " << h.t;
    _stats = os.str();
  }
}

} // namespace ir
