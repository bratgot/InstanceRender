// InstanceRender - EmbreeBackend.h
// CPU back-end: Embree 4 two-level BVH (one scene per prototype, instance
// geometries in the top-level scene) driving the shared Kernel.h integrator
// on a thread pool.  Strict ASCII.
#pragma once

#include "Scene.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct RTCDeviceTy;
struct RTCSceneTy;

namespace ir {

class CpuRenderer {
public:
  CpuRenderer();
  ~CpuRenderer();
  // (re)build the acceleration structures for the scene; returns false + message on failure
  bool build(const Scene& scene, std::string& err);
  // render the whole frame into fb (allocated here); cancel may be polled
  void render(const Scene& scene, const RenderSettings& settings, FrameBuffers& fb, std::atomic<bool>* cancel,
              const std::function<void(float)>& progress = nullptr);
  void release();
  std::string stats() const { return _stats; }
  static std::string version();
private:
  RTCDeviceTy* _device;
  RTCSceneTy*  _top;
  std::vector<RTCSceneTy*> _protoScenes;
  std::string _stats, _buildStats;
};

} // namespace ir
