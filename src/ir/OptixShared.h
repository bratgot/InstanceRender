// InstanceRender - OptixShared.h
// Launch parameters shared by the host (OptixBackend.cpp) and the device
// kernel (OptixKernel.cu).  Strict ASCII.
#pragma once

#include "Scene.h"

namespace ir {

struct LaunchParams {
  SceneView scene;              // device pointers
  unsigned long long handle;    // OptixTraversableHandle (IAS)
  float* rgba;                  // W*H*4
  float* depth;                 // W*H
  float* normal;                // W*H*3
  float* instanceId;            // W*H
  float* albedo;                // W*H*3
  float* extra;                 // W*H*scene.settings.aov.stride, packed by AovLayout
  int width, height, samples, sampleOffset;
};

} // namespace ir
