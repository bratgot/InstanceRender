// InstanceRender - OptixBackend.h
// GPU back-end: one GAS per prototype + an IAS of instances, running the same
// kernel as the CPU path (src/ir/Kernel.h through OptixKernel.cu).
// Strict ASCII.
#pragma once

#include "Scene.h"

#include <atomic>
#include <functional>
#include <string>

namespace ir {

class GpuRenderer {
public:
  GpuRenderer();
  ~GpuRenderer();
  static bool available();          // a CUDA device + OptiX initialise?
  static std::string deviceName();
  bool build(const Scene& scene, std::string& err);
  void render(const Scene& scene, const RenderSettings& settings, FrameBuffers& fb, std::atomic<bool>* cancel,
              const std::function<void(float)>& progress = nullptr);
  void release();
  // Forget the textures held on the device.  The device copy is keyed by what
  // the loader says a texture is, so a file that changed on disk WITHOUT
  // changing its path still looks current - this is the way out of that, and it
  // belongs on the same button as the loader's own cache.
  void forgetTextures();
  std::string stats() const { return _stats; }
private:
  struct Impl;
  Impl* _impl;
  std::string _stats, _buildStats;
};

} // namespace ir
