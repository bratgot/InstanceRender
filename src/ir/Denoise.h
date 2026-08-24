// InstanceRender - Denoise.h
// The OptiX AI denoiser, as a post-process on a finished frame.
//
// It runs on the GPU, but it is NOT tied to the GPU renderer: denoising is a
// post-process on host buffers, so a CPU render is denoised the same way as
// long as the machine has a CUDA device.  That keeps the two back-ends looking
// alike, which is the point of this project.
//
// The albedo and normal passes are handed over as guides when they are there -
// the denoiser keeps far more texture detail with them than without - and the
// alpha channel is carried through untouched, since coverage is not noise.
//
// Strict ASCII.
#pragma once

#include <string>
#include <vector>

namespace ir {

class Denoiser {
public:
  Denoiser();
  ~Denoiser();

  // A CUDA device and the OptiX denoiser are present in this build.
  static bool available();

  // Denoises 'rgba' (4 floats per pixel) in place.  'albedo' and 'normal' are
  // 3 floats per pixel and may be empty.  Returns false with a reason in 'err',
  // leaving the image untouched.
  bool run(int width, int height, std::vector<float>& rgba,
           const std::vector<float>& albedo, const std::vector<float>& normal,
           std::string& err);

  // What the last run did, for the node's info panel.
  const std::string& stats() const { return _stats; }

private:
  struct Impl;
  Impl* _impl;
  std::string _stats;

  Denoiser(const Denoiser&);
  Denoiser& operator=(const Denoiser&);
};

} // namespace ir
