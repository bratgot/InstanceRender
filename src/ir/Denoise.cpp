// InstanceRender - Denoise.cpp
// See Denoise.h.  Strict ASCII.
//
// The OptiX function table is defined by OptixBackend.cpp - exactly one
// translation unit may define it - so this file takes the stubs only.
#include "Denoise.h"
#include "OptixBackend.h"

#include <optix.h>
#include <optix_stubs.h>
#include <cuda_runtime.h>

#include <chrono>
#include <sstream>

namespace ir {

namespace {

struct DBuf {
  CUdeviceptr ptr = 0;
  size_t bytes = 0;
  bool alloc(size_t n, std::string& err)
  {
    if (n == bytes && ptr) return true;
    free();
    if (n == 0) return true;
    if (cudaMalloc(reinterpret_cast<void**>(&ptr), n) != cudaSuccess) {
      err = "out of GPU memory for the denoiser";
      ptr = 0;
      return false;
    }
    bytes = n;
    return true;
  }
  void free()
  {
    if (ptr) cudaFree(reinterpret_cast<void*>(ptr));
    ptr = 0;
    bytes = 0;
  }
};

OptixImage2D image2D(CUdeviceptr p, int w, int h, OptixPixelFormat fmt, unsigned int comps)
{
  OptixImage2D img = {};
  img.data = p;
  img.width = unsigned(w);
  img.height = unsigned(h);
  img.rowStrideInBytes = unsigned(w) * comps * unsigned(sizeof(float));
  img.pixelStrideInBytes = comps * unsigned(sizeof(float));
  img.format = fmt;
  return img;
}

} // namespace

struct Denoiser::Impl {
  OptixDeviceContext ctx = nullptr;
  OptixDenoiser denoiser = nullptr;
  int width = 0, height = 0;
  bool withAlbedo = false, withNormal = false;
  DBuf state, scratch, intensity;
  DBuf colour, out, albedo, normal;
  size_t stateSize = 0, scratchSize = 0;

  ~Impl()
  {
    if (denoiser) optixDenoiserDestroy(denoiser);
    if (ctx) optixDeviceContextDestroy(ctx);
    state.free(); scratch.free(); intensity.free();
    colour.free(); out.free(); albedo.free(); normal.free();
  }

  // The denoiser is built for a size and a guide combination; both change
  // rarely, so it is kept until one of them does.
  bool setup(int w, int h, bool guideAlbedo, bool guideNormal, std::string& err)
  {
    if (denoiser && w == width && h == height
        && guideAlbedo == withAlbedo && guideNormal == withNormal) return true;
    if (denoiser) { optixDenoiserDestroy(denoiser); denoiser = nullptr; }

    if (!ctx) {
      CUcontext cu = nullptr;
      cudaFree(nullptr);                       // make sure a context exists
      if (optixDeviceContextCreate(cu, nullptr, &ctx) != OPTIX_SUCCESS) {
        err = "could not create an OptiX context for the denoiser";
        return false;
      }
    }

    OptixDenoiserOptions options = {};
    options.guideAlbedo = guideAlbedo ? 1u : 0u;
    options.guideNormal = guideNormal ? 1u : 0u;
    options.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;   // coverage is not noise
    if (optixDenoiserCreate(ctx, OPTIX_DENOISER_MODEL_KIND_HDR, &options, &denoiser) != OPTIX_SUCCESS) {
      err = "could not create the OptiX denoiser";
      denoiser = nullptr;
      return false;
    }

    OptixDenoiserSizes sizes = {};
    if (optixDenoiserComputeMemoryResources(denoiser, unsigned(w), unsigned(h), &sizes) != OPTIX_SUCCESS) {
      err = "the denoiser would not report its memory needs";
      return false;
    }
    stateSize = sizes.stateSizeInBytes;
    scratchSize = sizes.withoutOverlapScratchSizeInBytes;
    if (!state.alloc(stateSize, err) || !scratch.alloc(scratchSize, err)
        || !intensity.alloc(sizeof(float), err)) return false;

    if (optixDenoiserSetup(denoiser, nullptr, unsigned(w), unsigned(h),
                           state.ptr, stateSize, scratch.ptr, scratchSize) != OPTIX_SUCCESS) {
      err = "the denoiser would not set itself up";
      return false;
    }
    width = w; height = h;
    withAlbedo = guideAlbedo; withNormal = guideNormal;
    return true;
  }
};

Denoiser::Denoiser() : _impl(new Impl()) {}
Denoiser::~Denoiser() { delete _impl; }

bool Denoiser::available()
{
  return GpuRenderer::available();          // a CUDA device, and optixInit() done
}

bool Denoiser::run(int width, int height, std::vector<float>& rgba,
                   const std::vector<float>& albedo, const std::vector<float>& normal,
                   std::string& err)
{
  _stats.clear();
  const size_t n = size_t(width) * size_t(height);
  if (width <= 0 || height <= 0 || rgba.size() != n * 4) { err = "nothing to denoise"; return false; }
  if (!available()) { err = "no GPU available for the denoiser"; return false; }

  const bool guideAlbedo = (albedo.size() == n * 3);
  const bool guideNormal = (normal.size() == n * 3);
  Impl& d = *_impl;
  if (!d.setup(width, height, guideAlbedo, guideNormal, err)) return false;

  const auto t0 = std::chrono::steady_clock::now();
  if (!d.colour.alloc(n * 4 * sizeof(float), err) || !d.out.alloc(n * 4 * sizeof(float), err)) return false;
  if (guideAlbedo && !d.albedo.alloc(n * 3 * sizeof(float), err)) return false;
  if (guideNormal && !d.normal.alloc(n * 3 * sizeof(float), err)) return false;

  if (cudaMemcpy(reinterpret_cast<void*>(d.colour.ptr), rgba.data(),
                 n * 4 * sizeof(float), cudaMemcpyHostToDevice) != cudaSuccess) {
    err = "could not upload the image to the denoiser";
    return false;
  }
  if (guideAlbedo)
    cudaMemcpy(reinterpret_cast<void*>(d.albedo.ptr), albedo.data(), n * 3 * sizeof(float), cudaMemcpyHostToDevice);
  if (guideNormal)
    cudaMemcpy(reinterpret_cast<void*>(d.normal.ptr), normal.data(), n * 3 * sizeof(float), cudaMemcpyHostToDevice);

  OptixDenoiserGuideLayer guide = {};
  if (guideAlbedo) guide.albedo = image2D(d.albedo.ptr, width, height, OPTIX_PIXEL_FORMAT_FLOAT3, 3);
  if (guideNormal) guide.normal = image2D(d.normal.ptr, width, height, OPTIX_PIXEL_FORMAT_FLOAT3, 3);

  OptixDenoiserLayer layer = {};
  layer.input = image2D(d.colour.ptr, width, height, OPTIX_PIXEL_FORMAT_FLOAT4, 4);
  layer.output = image2D(d.out.ptr, width, height, OPTIX_PIXEL_FORMAT_FLOAT4, 4);

  // the HDR model wants to know how bright the image is
  if (optixDenoiserComputeIntensity(d.denoiser, nullptr, &layer.input, d.intensity.ptr,
                                    d.scratch.ptr, d.scratchSize) != OPTIX_SUCCESS) {
    err = "the denoiser could not measure the image";
    return false;
  }

  OptixDenoiserParams params = {};
  params.hdrIntensity = d.intensity.ptr;
  params.blendFactor = 0.0f;
  if (optixDenoiserInvoke(d.denoiser, nullptr, &params, d.state.ptr, d.stateSize,
                          &guide, &layer, 1, 0, 0, d.scratch.ptr, d.scratchSize) != OPTIX_SUCCESS) {
    err = "the denoiser failed";
    return false;
  }
  if (cudaDeviceSynchronize() != cudaSuccess) { err = "the denoiser did not finish"; return false; }

  std::vector<float> result(n * 4);
  if (cudaMemcpy(result.data(), reinterpret_cast<void*>(d.out.ptr),
                 n * 4 * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess) {
    err = "could not read the denoised image back";
    return false;
  }
  // coverage is not noise: keep the alpha the renderer computed
  for (size_t i = 0; i < n; ++i) result[i * 4 + 3] = rgba[i * 4 + 3];
  rgba.swap(result);

  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::ostringstream os;
  os << "denoised in " << int(ms) << " ms";
  if (guideAlbedo || guideNormal) {
    os << " (guided by " << (guideAlbedo ? "albedo" : "");
    if (guideAlbedo && guideNormal) os << " and ";
    os << (guideNormal ? "normals" : "") << ")";
  }
  else {
    os << " (no guide passes)";
  }
  _stats = os.str();
  return true;
}

} // namespace ir
