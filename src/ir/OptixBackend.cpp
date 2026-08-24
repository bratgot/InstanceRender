// InstanceRender - OptixBackend.cpp  (strict ASCII)
#include "OptixBackend.h"
#include "OptixShared.h"

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>
#include <optix_stack_size.h>
#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

extern const char* g_InstanceRenderPtx;   // generated from OptixKernel.cu

namespace ir {

namespace {

#define IR_CU(call) do { const cudaError_t e = (call); if (e != cudaSuccess) { err = std::string("CUDA: ") + cudaGetErrorString(e); return false; } } while (0)
#define IR_OX(call) do { const OptixResult r = (call); if (r != OPTIX_SUCCESS) { err = std::string("OptiX: ") + optixGetErrorName(r) + " (" + #call + ")"; return false; } } while (0)

template <class T> struct SbtRecord {
  __declspec(align(OPTIX_SBT_RECORD_ALIGNMENT)) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
  T data;
};
struct EmptyData { int unused; };
typedef SbtRecord<EmptyData> EmptyRecord;

void optixLog(unsigned int level, const char* tag, const char* message, void*)
{
  std::fprintf(stderr, "InstanceRender/OptiX [%u][%s]: %s\n", level, tag ? tag : "", message ? message : "");
}

// device buffer helper
struct Buf {
  CUdeviceptr ptr = 0;
  size_t bytes = 0;
  void free() { if (ptr) { cudaFree(reinterpret_cast<void*>(ptr)); ptr = 0; bytes = 0; } }
  template <class T> bool upload(const std::vector<T>& v, std::string& err)
  {
    free();
    if (v.empty()) return true;
    bytes = v.size() * sizeof(T);
    IR_CU(cudaMalloc(reinterpret_cast<void**>(&ptr), bytes));
    IR_CU(cudaMemcpy(reinterpret_cast<void*>(ptr), v.data(), bytes, cudaMemcpyHostToDevice));
    return true;
  }
  bool alloc(size_t n, std::string& err)
  {
    free();
    if (!n) return true;
    bytes = n;
    IR_CU(cudaMalloc(reinterpret_cast<void**>(&ptr), bytes));
    IR_CU(cudaMemset(reinterpret_cast<void*>(ptr), 0, bytes));
    return true;
  }
  template <class T> T* as() const { return reinterpret_cast<T*>(ptr); }
};

// A short string that changes exactly when the texel block does.
//
// The keys come from the loader, which builds each one from everything that
// decides that texture's content; Nuke's own "nkop:" paths carry a hash of the
// op's state, so they change precisely when the picture does.  Hashing the
// texels themselves is not an option - the block runs to hundreds of megabytes,
// and reading it would cost more than the upload it is trying to avoid.
//
// A texture built by the older non-USD path has no key.  That shows up here as
// a size mismatch and yields the empty string, which never compares equal, so
// those scenes simply re-upload every time.  Correct, if not clever.
std::string textureFingerprint(const Scene& scene)
{
  if (scene.textureKeys.size() != scene.textures.size()) return std::string();
  std::ostringstream os;
  os << scene.texels.size();
  for (size_t i = 0; i < scene.textureKeys.size(); ++i) os << '|' << scene.textureKeys[i];
  return os.str();
}

} // namespace

struct GpuRenderer::Impl {
  OptixDeviceContext ctx = nullptr;
  OptixModule module = nullptr;
  OptixPipeline pipeline = nullptr;
  OptixProgramGroup pgRay = nullptr, pgMiss = nullptr, pgMissShadow = nullptr, pgHit = nullptr, pgHitShadow = nullptr;
  OptixShaderBindingTable sbt = {};
  Buf raygenSbt, missSbt, hitSbt;
  std::vector<Buf> gasBuffers;
  std::vector<OptixTraversableHandle> gasHandles;
  Buf iasBuffer, instBuffer;
  OptixTraversableHandle iasHandle = 0;
  // scene data on the device
  Buf vertices, vertices1, normals, uvs, colors, indices, triMaterial, protos, instances, materials, lights;
  Buf volumes, voxels, blackbody, ies;
  Buf texels, textures, udimTiles, domeFunc, domeMarginal, domeConditional;
  Buf motionXforms;         // one OptixMatrixMotionTransform per instance when the scene moves
  bool deform = false;      // the meshes themselves move
  Buf motionKeys;           // the key array itself, for the shading kernel
  bool motion = false;
  Buf outRgba, outDepth, outNormal, outInstance, outAlbedo, outExtra, paramsBuf;
  int width = 0, height = 0;
  bool ready = false;

  // What the pipeline was compiled for.  Both usesMotionBlur and the traversable
  // graph flags come from the single question "does anything move", so one
  // tri-state is the whole cache key: -1 nothing built yet, 0 static, 1 moving.
  int programMotion = -1;
  // What is currently sitting in the texels buffer - see textureFingerprint().
  // Only ever touched on the thread that builds.
  std::string texelKey;
  bool texelKeyValid = false;
  // "refresh render" is pressed on the main thread, which is not the thread that
  // builds and holds no lock against it, so the request is left here rather than
  // acted on: the builder picks it up on its way in.
  std::atomic<bool> forgetTexturesRequested{false};

  // Everything that describes the CURRENT scene, and nothing else.
  //
  // The context, module, pipeline and SBT are deliberately not here.  Nothing
  // about them depends on the scene, and remaking them costs about 60 ms - which
  // was being paid on every redraw, so every drag of a handle in the viewer was
  // recompiling the PTX and relinking the pipeline to render the same shaders.
  // They now outlive a rebuild; releaseProgram() below is what ends them.
  //
  // Textures are the awkward middle case: they are scene data, but they are also
  // large and usually unchanged, so they survive when the caller says they may.
  void releaseScene(bool keepTextures)
  {
    for (size_t i = 0; i < gasBuffers.size(); ++i) gasBuffers[i].free();
    gasBuffers.clear(); gasHandles.clear();
    iasBuffer.free(); instBuffer.free();
    iasHandle = 0;
    vertices.free(); vertices1.free(); normals.free(); uvs.free(); colors.free(); indices.free(); triMaterial.free();
    protos.free(); instances.free(); materials.free(); lights.free();
    textures.free(); udimTiles.free(); domeFunc.free(); domeMarginal.free(); domeConditional.free();
    if (!keepTextures) { texels.free(); texelKey.clear(); texelKeyValid = false; }
    motionKeys.free();
    motionXforms.free();
    outRgba.free(); outDepth.free(); outNormal.free(); outInstance.free(); outAlbedo.free(); outExtra.free(); paramsBuf.free();
    ready = false;
  }

  void releaseProgram()
  {
    if (pipeline) { optixPipelineDestroy(pipeline); pipeline = nullptr; }
    if (pgRay) { optixProgramGroupDestroy(pgRay); pgRay = nullptr; }
    if (pgMiss) { optixProgramGroupDestroy(pgMiss); pgMiss = nullptr; }
    if (pgMissShadow) { optixProgramGroupDestroy(pgMissShadow); pgMissShadow = nullptr; }
    if (pgHit) { optixProgramGroupDestroy(pgHit); pgHit = nullptr; }
    if (pgHitShadow) { optixProgramGroupDestroy(pgHitShadow); pgHitShadow = nullptr; }
    if (module) { optixModuleDestroy(module); module = nullptr; }
    raygenSbt.free(); missSbt.free(); hitSbt.free();
    std::memset(&sbt, 0, sizeof(sbt));
    programMotion = -1;
  }

  void release()
  {
    releaseProgram();
    releaseScene(false);
    if (ctx) { optixDeviceContextDestroy(ctx); ctx = nullptr; }
  }
};

GpuRenderer::GpuRenderer() : _impl(new Impl) {}
GpuRenderer::~GpuRenderer() { release(); delete _impl; }
void GpuRenderer::release() { if (_impl) _impl->release(); }

void GpuRenderer::forgetTextures()
{
  if (_impl) _impl->forgetTexturesRequested.store(true);
}

bool GpuRenderer::available()
{
  int n = 0;
  if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) return false;
  static bool inited = false, ok = false;
  if (!inited) { inited = true; ok = (optixInit() == OPTIX_SUCCESS); }
  return ok;
}

std::string GpuRenderer::deviceName()
{
  int n = 0;
  if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) return "no CUDA device";
  cudaDeviceProp prop;
  if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return "unknown CUDA device";
  std::ostringstream os;
  os << prop.name << " (sm_" << prop.major << prop.minor << ", " << (prop.totalGlobalMem >> 20) << " MB)";
  return os.str();
}

bool GpuRenderer::build(const Scene& scene, std::string& err)
{
  Impl& d = *_impl;

  // A build that fails part way through must not leave the next one looking at
  // state from a configuration that did not work.  Now that things survive a
  // rebuild, saying so has to be explicit.
  struct FailGuard {
    Impl* d; bool ok = false;
    ~FailGuard() { if (!ok) d->release(); }
  } guard{&d};

  if (d.forgetTexturesRequested.exchange(false)) {
    d.texelKey.clear();
    d.texelKeyValid = false;
  }

  // What can be kept?  The texels, if the scene still wants the same pictures.
  const std::string texelKey = textureFingerprint(scene);
  const bool keepTextures = d.texelKeyValid && !texelKey.empty()
                         && texelKey == d.texelKey && d.texels.ptr != 0;
  d.releaseScene(keepTextures);

  const auto t0 = std::chrono::steady_clock::now();
  if (!available()) { err = "no CUDA/OptiX device"; return false; }
  IR_CU(cudaFree(nullptr));   // create the context

  if (!d.ctx) {
    OptixDeviceContextOptions opts = {};
    opts.logCallbackFunction = &optixLog;
    opts.logCallbackLevel = 2;
    IR_OX(optixDeviceContextCreate(nullptr, &opts, &d.ctx));
  }

  // ---- geometry: one GAS per prototype -------------------------------------------
  // vertices are shared; a GAS uses the prototype's slice with local indices
  std::vector<float3> verts(scene.vertices.size());
  for (size_t i = 0; i < scene.vertices.size(); ++i) verts[i] = make_float3(scene.vertices[i].x, scene.vertices[i].y, scene.vertices[i].z);
  Buf devVerts;
  if (!devVerts.upload(verts, err)) return false;
  const bool deform = (scene.vertices1.size() == scene.vertices.size() && !scene.vertices1.empty());
  Buf devVerts1;
  if (deform) {
    std::vector<float3> verts1(scene.vertices1.size());
    for (size_t i = 0; i < scene.vertices1.size(); ++i)
      verts1[i] = make_float3(scene.vertices1[i].x, scene.vertices1[i].y, scene.vertices1[i].z);
    if (!devVerts1.upload(verts1, err)) return false;
  }
  d.gasBuffers.resize(scene.protos.size());
  d.gasHandles.assign(scene.protos.size(), 0);
  std::vector<Buf> idxBufs(scene.protos.size());
  for (size_t p = 0; p < scene.protos.size(); ++p) {
    const ProtoRange& pr = scene.protos[p];
    std::vector<uint3> idx(size_t(pr.numTris > 0 ? pr.numTris : 0));
    for (int t = 0; t < pr.numTris; ++t) {
      idx[size_t(t)] = make_uint3(scene.indices[size_t(pr.firstTri + t) * 3] - unsigned(pr.firstVertex),
                                  scene.indices[size_t(pr.firstTri + t) * 3 + 1] - unsigned(pr.firstVertex),
                                  scene.indices[size_t(pr.firstTri + t) * 3 + 2] - unsigned(pr.firstVertex));
    }
    if (!idxBufs[p].upload(idx, err)) return false;
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    CUdeviceptr vptr = devVerts.ptr + CUdeviceptr(size_t(pr.firstVertex) * sizeof(float3));
    bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    bi.triangleArray.vertexStrideInBytes = sizeof(float3);
    bi.triangleArray.numVertices = unsigned(pr.numVertices);
    CUdeviceptr vptrs[2] = { vptr, 0 };
    if (deform) vptrs[1] = devVerts1.ptr + CUdeviceptr(size_t(pr.firstVertex) * sizeof(float3));
    bi.triangleArray.vertexBuffers = deform ? vptrs : &vptr;
    bi.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    bi.triangleArray.indexStrideInBytes = sizeof(uint3);
    bi.triangleArray.numIndexTriplets = unsigned(pr.numTris);
    bi.triangleArray.indexBuffer = idxBufs[p].ptr;
    unsigned flags[1] = { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT };
    bi.triangleArray.flags = flags;
    bi.triangleArray.numSbtRecords = 1;

    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    if (deform) {
      // vertex motion lives on the GAS itself, one key per shutter end
      ao.motionOptions.numKeys = 2;
      ao.motionOptions.flags = OPTIX_MOTION_FLAG_NONE;
      ao.motionOptions.timeBegin = 0.0f;
      ao.motionOptions.timeEnd = 1.0f;
    }
    OptixAccelBufferSizes sizes = {};
    IR_OX(optixAccelComputeMemoryUsage(d.ctx, &ao, &bi, 1, &sizes));
    Buf temp;
    if (!temp.alloc(sizes.tempSizeInBytes ? sizes.tempSizeInBytes : 4, err)) return false;
    if (!d.gasBuffers[p].alloc(sizes.outputSizeInBytes ? sizes.outputSizeInBytes : 4, err)) return false;
    IR_OX(optixAccelBuild(d.ctx, 0, &ao, &bi, 1, temp.ptr, sizes.tempSizeInBytes,
                          d.gasBuffers[p].ptr, sizes.outputSizeInBytes, &d.gasHandles[p], nullptr, 0));
    IR_CU(cudaDeviceSynchronize());
    temp.free();
  }

  // ---- instances: IAS ---------------------------------------------------------------
  d.motion = scene.hasMotion;
  d.deform = deform;
  // Motion blur: OptiX needs a matrix motion transform NODE between the IAS and
  // each GAS.  Its two keys are linearly interpolated, exactly like Embree's
  // instance time steps and like Instance::xfAt() in the kernel - all three have
  // to agree or the two devices disagree structurally.
  // OptixMatrixMotionTransform is variable length: its declared transform[2][12]
  // grows by 12 floats per extra key, and each record has to stay 64-byte
  // aligned for optixConvertPointerToTraversableHandle.
  const int motionKeys = (scene.motionKeyCount >= 2) ? scene.motionKeyCount : 2;
  size_t motionStride = 0;
  std::vector<char> motions;
  if (d.motion) {
    const size_t base = sizeof(OptixMatrixMotionTransform) + size_t(motionKeys - 2) * 12 * sizeof(float);
    motionStride = ((base + 63) / 64) * 64;
    motions.assign(motionStride * scene.instances.size(), 0);
    for (size_t i = 0; i < scene.instances.size(); ++i) {
      const Instance& src = scene.instances[i];
      OptixMatrixMotionTransform* mt = reinterpret_cast<OptixMatrixMotionTransform*>(&motions[i * motionStride]);
      mt->child = (src.protoId >= 0 && size_t(src.protoId) < d.gasHandles.size()) ? d.gasHandles[size_t(src.protoId)] : 0;
      mt->motionOptions.numKeys = static_cast<unsigned short>(motionKeys);
      mt->motionOptions.flags = OPTIX_MOTION_FLAG_NONE;
      mt->motionOptions.timeBegin = 0.0f;
      mt->motionOptions.timeEnd = 1.0f;
      float* dst = &mt->transform[0][0];
      for (int k = 0; k < motionKeys; ++k) {
        const Xform& x = (scene.motionKeyCount >= 2)
                       ? scene.motionKeys[size_t(src.firstKey + k)]
                       : (k == 0 ? src.xf : src.xf1);
        std::memcpy(dst + size_t(k) * 12, x.m, sizeof(float) * 12);
      }
    }
    if (!d.motionXforms.upload(motions, err)) return false;
  }

  std::vector<OptixInstance> insts(scene.instances.size());
  for (size_t i = 0; i < scene.instances.size(); ++i) {
    const Instance& src = scene.instances[i];
    OptixInstance& oi = insts[i];
    std::memset(&oi, 0, sizeof(oi));
    oi.instanceId = unsigned(i);              // index into scene.instances (Kernel.h expects this)
    oi.sbtOffset = 0;
    oi.visibilityMask = 255;
    oi.flags = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
    if (d.motion) {
      // the transform lives in the motion node, so the instance itself is identity
      oi.transform[0] = 1.0f; oi.transform[5] = 1.0f; oi.transform[10] = 1.0f;
      OptixTraversableHandle mh = 0;
      const CUdeviceptr ptr = d.motionXforms.ptr + CUdeviceptr(i * motionStride);
      IR_OX(optixConvertPointerToTraversableHandle(d.ctx, ptr, OPTIX_TRAVERSABLE_TYPE_MATRIX_MOTION_TRANSFORM, &mh));
      oi.traversableHandle = mh;
    }
    else {
      std::memcpy(oi.transform, src.xf.m, sizeof(float) * 12);
      oi.traversableHandle = (src.protoId >= 0 && size_t(src.protoId) < d.gasHandles.size()) ? d.gasHandles[size_t(src.protoId)] : 0;
    }
  }
  if (!d.instBuffer.upload(insts, err)) return false;
  {
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    bi.instanceArray.instances = d.instBuffer.ptr;
    bi.instanceArray.numInstances = unsigned(insts.size());
    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    if (d.motion) {
      ao.motionOptions.numKeys = static_cast<unsigned short>(motionKeys);
      ao.motionOptions.flags = OPTIX_MOTION_FLAG_NONE;
      ao.motionOptions.timeBegin = 0.0f;
      ao.motionOptions.timeEnd = 1.0f;
    }
    OptixAccelBufferSizes sizes = {};
    IR_OX(optixAccelComputeMemoryUsage(d.ctx, &ao, &bi, 1, &sizes));
    Buf temp;
    if (!temp.alloc(sizes.tempSizeInBytes ? sizes.tempSizeInBytes : 4, err)) return false;
    if (!d.iasBuffer.alloc(sizes.outputSizeInBytes ? sizes.outputSizeInBytes : 4, err)) return false;
    IR_OX(optixAccelBuild(d.ctx, 0, &ao, &bi, 1, temp.ptr, sizes.tempSizeInBytes,
                          d.iasBuffer.ptr, sizes.outputSizeInBytes, &d.iasHandle, nullptr, 0));
    IR_CU(cudaDeviceSynchronize());
    temp.free();
  }

  // ---- module + pipeline ------------------------------------------------------------
  // Neither depends on the scene.  The only thing that varies is whether
  // anything moves: a motion transform adds a traversal level, so a static scene
  // can use single-level instancing, which traverses faster.  That one boolean
  // is therefore the entire cache key, and until it flips the module, the four
  // program groups, the pipeline and the SBT are all reusable as they stand.
  //
  // This is worth caring about because optixModuleCreate compiles the PTX.  Doing
  // that on every build put a floor of about 60 ms under every redraw, whatever
  // the scene: a 512-triangle card with no textures cost the same as a full one.
  const int wantMotion = (d.motion || d.deform) ? 1 : 0;
  const bool rebuildProgram = (d.programMotion != wantMotion);
  if (rebuildProgram) {
    d.releaseProgram();
    OptixPipelineCompileOptions pco = {};
    // a motion transform adds a traversal level, so single-level instancing (which
    // is the faster traversal) is only usable for static scenes
    pco.usesMotionBlur = (d.motion || d.deform) ? 1 : 0;
    pco.traversableGraphFlags = (d.motion || d.deform) ? OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY
                                         : OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pco.numPayloadValues = 5;
    pco.numAttributeValues = 2;
    pco.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pco.pipelineLaunchParamsVariableName = "paramsRaw";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    OptixModuleCompileOptions mco = {};
    mco.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mco.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mco.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    {
      char log[4096]; size_t logSize = sizeof(log);
      const std::string ptx(g_InstanceRenderPtx);
      const OptixResult r = optixModuleCreate(d.ctx, &mco, &pco, ptx.c_str(), ptx.size(), log, &logSize, &d.module);
      if (r != OPTIX_SUCCESS) { err = std::string("OptiX module: ") + optixGetErrorName(r) + " " + log; return false; }
    }
    auto makeGroup = [&](OptixProgramGroupDesc desc, OptixProgramGroup* out) -> bool {
      OptixProgramGroupOptions gopt = {};
      char log[2048]; size_t logSize = sizeof(log);
      const OptixResult r = optixProgramGroupCreate(d.ctx, &desc, 1, &gopt, log, &logSize, out);
      if (r != OPTIX_SUCCESS) { err = std::string("OptiX program group: ") + optixGetErrorName(r) + " " + log; return false; }
      return true;
    };
    {
      OptixProgramGroupDesc desc = {};
      desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
      desc.raygen.module = d.module; desc.raygen.entryFunctionName = "__raygen__ir";
      if (!makeGroup(desc, &d.pgRay)) return false;
    }
    {
      OptixProgramGroupDesc desc = {};
      desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
      desc.miss.module = d.module; desc.miss.entryFunctionName = "__miss__ir";
      if (!makeGroup(desc, &d.pgMiss)) return false;
      desc.miss.entryFunctionName = "__miss__shadow";
      if (!makeGroup(desc, &d.pgMissShadow)) return false;
    }
    {
      OptixProgramGroupDesc desc = {};
      desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
      desc.hitgroup.moduleCH = d.module; desc.hitgroup.entryFunctionNameCH = "__closesthit__ir";
      if (!makeGroup(desc, &d.pgHit)) return false;
    }
    {
      OptixProgramGroup groups[] = { d.pgRay, d.pgMiss, d.pgMissShadow, d.pgHit };
      OptixPipelineLinkOptions plo = {};
      plo.maxTraceDepth = 2;
      char log[2048]; size_t logSize = sizeof(log);
      const OptixResult r = optixPipelineCreate(d.ctx, &pco, &plo, groups, 4, log, &logSize, &d.pipeline);
      if (r != OPTIX_SUCCESS) { err = std::string("OptiX pipeline: ") + optixGetErrorName(r) + " " + log; return false; }
      OptixStackSizes ss = {};
      for (int i = 0; i < 4; ++i) IR_OX(optixUtilAccumulateStackSizes(groups[i], &ss, d.pipeline));
      unsigned dcTrav = 0, dcState = 0, cont = 0;
      IR_OX(optixUtilComputeStackSizes(&ss, 2, 0, 0, &dcTrav, &dcState, &cont));
      IR_OX(optixPipelineSetStackSize(d.pipeline, dcTrav, dcState, cont, 2 /* IAS + GAS */));
    }
    // ---- SBT ---------------------------------------------------------------------------
    {
      std::vector<EmptyRecord> ray(1), miss(2), hit(1);
      IR_OX(optixSbtRecordPackHeader(d.pgRay, &ray[0]));
      IR_OX(optixSbtRecordPackHeader(d.pgMiss, &miss[0]));
      IR_OX(optixSbtRecordPackHeader(d.pgMissShadow, &miss[1]));
      IR_OX(optixSbtRecordPackHeader(d.pgHit, &hit[0]));
      if (!d.raygenSbt.upload(ray, err)) return false;
      if (!d.missSbt.upload(miss, err)) return false;
      if (!d.hitSbt.upload(hit, err)) return false;
      std::memset(&d.sbt, 0, sizeof(d.sbt));
      d.sbt.raygenRecord = d.raygenSbt.ptr;
      d.sbt.missRecordBase = d.missSbt.ptr;
      d.sbt.missRecordStrideInBytes = sizeof(EmptyRecord);
      d.sbt.missRecordCount = 2;
      d.sbt.hitgroupRecordBase = d.hitSbt.ptr;
      d.sbt.hitgroupRecordStrideInBytes = sizeof(EmptyRecord);
      d.sbt.hitgroupRecordCount = 1;
    }

    d.programMotion = wantMotion;
  }

  // ---- scene arrays on the device -----------------------------------------------------
  d.vertices = devVerts;                    // takes ownership
  devVerts.ptr = 0; devVerts.bytes = 0;
  d.vertices1 = devVerts1;
  devVerts1.ptr = 0; devVerts1.bytes = 0;
  if (!d.normals.upload(scene.normals, err)) return false;
  if (!d.uvs.upload(scene.uvs, err)) return false;
  if (!d.colors.upload(scene.colors, err)) return false;
  if (!d.indices.upload(scene.indices, err)) return false;
  if (!d.triMaterial.upload(scene.triMaterial, err)) return false;
  if (!d.protos.upload(scene.protos, err)) return false;
  if (!d.instances.upload(scene.instances, err)) return false;
  // The grid has to go to the device like everything else.  Left behind, the
  // kernel dereferences HOST pointers on the GPU and the volume came out
  // completely opaque - measured, a card behind it rendered 0.00000 on the GPU
  // while the CPU, reading the same struct, showed it correctly.
  if (!d.volumes.upload(scene.volumes, err)) return false;
  if (!d.voxels.upload(scene.voxels, err)) return false;
  if (!d.blackbody.upload(scene.blackbody, err)) return false;
  if (!d.ies.upload(scene.ies, err)) return false;
  if (!d.materials.upload(scene.materials, err)) return false;
  if (!d.lights.upload(scene.lights, err)) return false;
  if (!d.motionKeys.upload(scene.motionKeys, err)) return false;
  // The big one.  A 4096x2048 texture and its mip chain is 170 MB across the
  // bus, about 26 ms, and on a handle drag it is the same 170 MB as last time.
  if (!keepTextures) {
    if (!d.texels.upload(scene.texels, err)) return false;
    d.texelKey = texelKey;
    d.texelKeyValid = !texelKey.empty();
  }
  if (!d.textures.upload(scene.textures, err)) return false;
  if (!d.udimTiles.upload(scene.udimTiles, err)) return false;
  if (!d.domeFunc.upload(scene.domeFunc, err)) return false;
  if (!d.domeMarginal.upload(scene.domeMarginal, err)) return false;
  if (!d.domeConditional.upload(scene.domeConditional, err)) return false;
  for (size_t i = 0; i < idxBufs.size(); ++i) idxBufs[i].free();   // GAS keeps its own copy

  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::ostringstream os;
  os << "OptiX: " << (d.motion ? (scene.motionKeyCount > 2 ? "motion blur (multi-segment), " : "motion blur, ") : "") << scene.protos.size() << " GAS, " << scene.numTriangles() << " unique triangle(s), "
     << scene.instances.size() << " instance(s) in 1 IAS = " << scene.expandedTriangles() / 1e6
     << "M triangles rendered, build " << int(ms) << " ms";
  // worth saying: the number means something different depending on these
  if (!rebuildProgram) os << ", pipeline kept";
  if (keepTextures) os << ", textures kept";
  _stats = _buildStats = os.str();
  d.ready = true;
  guard.ok = true;
  return true;
}

void GpuRenderer::render(const Scene& scene, const RenderSettings& settings, FrameBuffers& fb, std::atomic<bool>* cancel,
                         const std::function<void(float)>& progress)
{
  Impl& d = *_impl;
  const int W = settings.width, H = settings.height;
  fb.allocate(W, H, settings.aov.stride);
  if (!d.ready || W <= 0 || H <= 0) return;
  std::string err;
  const size_t n = size_t(W) * size_t(H);
  if (!d.outRgba.alloc(n * 4 * sizeof(float), err)) return;
  if (!d.outDepth.alloc(n * sizeof(float), err)) return;
  if (!d.outNormal.alloc(n * 3 * sizeof(float), err)) return;
  if (!d.outInstance.alloc(n * sizeof(float), err)) return;
  if (!d.outAlbedo.alloc(n * 3 * sizeof(float), err)) return;
  const size_t stride = size_t(settings.aov.stride > 0 ? settings.aov.stride : 0);
  d.outExtra.free();
  if (stride > 0 && !d.outExtra.alloc(n * stride * sizeof(float), err)) return;

  LaunchParams lp = {};
  SceneView sv = scene.view();
  sv.settings = settings;
  sv.camera.width = W; sv.camera.height = H;
  sv.cameraClose.width = W; sv.cameraClose.height = H;
  // swap the host pointers for the device ones
  sv.vertices = d.vertices.as<const Vec3>();
  sv.vertices1 = d.vertices1.ptr ? d.vertices1.as<const Vec3>() : nullptr;
  sv.normals = d.normals.as<const Vec3>();
  sv.uvs = d.uvs.as<const float>();
  sv.colors = d.colors.as<const Vec3>();
  sv.indices = d.indices.as<const uint32_t>();
  sv.triMaterial = d.triMaterial.as<const int>();
  sv.protos = d.protos.as<const ProtoRange>();
  sv.instances = d.instances.as<const Instance>();
  sv.volumes = d.volumes.ptr ? d.volumes.as<const VolumeGrid>() : nullptr;
  sv.voxels = d.voxels.ptr ? d.voxels.as<const float>() : nullptr;
  sv.blackbody = d.blackbody.ptr ? d.blackbody.as<const Vec3>() : nullptr;
  sv.ies = d.ies.ptr ? d.ies.as<const float>() : nullptr;
  if (!sv.volumes || !sv.voxels) sv.numVolumes = 0;
  sv.motionKeys = d.motionKeys.as<const Xform>();
  sv.texels = d.texels.as<const float>();
  sv.textures = d.textures.as<const TextureDesc>();
  sv.udimTiles = d.udimTiles.as<const int>();
  sv.domeFunc = d.domeFunc.as<const float>();
  sv.domeMarginal = d.domeMarginal.as<const float>();
  sv.domeConditional = d.domeConditional.as<const float>();
  sv.materials = d.materials.as<const Material>();
  sv.lights = d.lights.as<const Light>();
  lp.scene = sv;
  lp.handle = d.iasHandle;
  lp.rgba = d.outRgba.as<float>();
  lp.depth = d.outDepth.as<float>();
  lp.normal = d.outNormal.as<float>();
  lp.instanceId = d.outInstance.as<float>();
  lp.albedo = d.outAlbedo.as<float>();
  lp.extra = stride > 0 ? d.outExtra.as<float>() : nullptr;
  lp.width = W; lp.height = H; lp.samples = settings.samples > 0 ? settings.samples : 1;
  lp.sampleOffset = settings.sampleOffset > 0 ? settings.sampleOffset : 0;

  if (!d.paramsBuf.alloc(sizeof(LaunchParams), err)) return;
  if (cudaMemcpy(reinterpret_cast<void*>(d.paramsBuf.ptr), &lp, sizeof(lp), cudaMemcpyHostToDevice) != cudaSuccess) return;
  const auto t0 = std::chrono::steady_clock::now();
  const OptixResult r = optixLaunch(d.pipeline, 0, d.paramsBuf.ptr, sizeof(LaunchParams), &d.sbt, unsigned(W), unsigned(H), 1);
  if (r != OPTIX_SUCCESS) { _stats = _buildStats + std::string("; launch failed: ") + optixGetErrorName(r); return; }
  if (cudaDeviceSynchronize() != cudaSuccess) { _stats = _buildStats + "; cudaDeviceSynchronize failed"; return; }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  cudaMemcpy(fb.rgba.data(), reinterpret_cast<void*>(d.outRgba.ptr), n * 4 * sizeof(float), cudaMemcpyDeviceToHost);
  cudaMemcpy(fb.depth.data(), reinterpret_cast<void*>(d.outDepth.ptr), n * sizeof(float), cudaMemcpyDeviceToHost);
  cudaMemcpy(fb.normal.data(), reinterpret_cast<void*>(d.outNormal.ptr), n * 3 * sizeof(float), cudaMemcpyDeviceToHost);
  cudaMemcpy(fb.instanceId.data(), reinterpret_cast<void*>(d.outInstance.ptr), n * sizeof(float), cudaMemcpyDeviceToHost);
  cudaMemcpy(fb.albedo.data(), reinterpret_cast<void*>(d.outAlbedo.ptr), n * 3 * sizeof(float), cudaMemcpyDeviceToHost);
  if (stride > 0 && fb.extra.size() == n * stride)
    cudaMemcpy(fb.extra.data(), reinterpret_cast<void*>(d.outExtra.ptr), n * stride * sizeof(float), cudaMemcpyDeviceToHost);

  size_t hits = 0;
  for (size_t i = 0; i < n; ++i) if (fb.rgba[i * 4 + 3] > 0.0f) ++hits;
  std::ostringstream os;
  os << _buildStats << "; launch " << int(ms) << " ms; hits " << hits << "/" << n;
  _stats = os.str();
  if (progress) progress(1.0f);
  (void)cancel;
}

} // namespace ir
