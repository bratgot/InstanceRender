// InstanceRender - Scene.h
// Renderer-side scene description, filled by the stage loader, consumed by
// the CPU (Embree) and GPU (OptiX) back-ends through the same flat
// SceneView.  Geometry is "prototypes" (triangle soups) and "instances"
// (prototype + transform + per-instance colour); non-instanced meshes are
// prototypes with one identity instance, so the two back-ends only ever deal
// with one structure.  Strict ASCII.
#pragma once

#include "Math.h"

#include <string>
#include <vector>

namespace ir {

// ---- flat records (POD, shared with the GPU) ----------------------------------
enum TexWrap { kWrapRepeat = 0, kWrapClamp = 1, kWrapMirror = 2, kWrapBlack = 3 };

// One image in the shared texel pool.  Texels are plain float RGBA and both
// back-ends filter them with the same code in Kernel.h - a hardware
// cudaTextureObject_t would filter at reduced precision and break parity.
enum { kMaxMipLevels = 14 };

struct TextureDesc {
  int firstTexel;         // index into SceneView::texels, in TEXELS (x4 for floats)
  int width, height;
  int wrapS, wrapT;
  int mipCount;           // 1 = no pyramid
  int mipOffset[kMaxMipLevels];
  int mipW[kMaxMipLevels], mipH[kMaxMipLevels];
  int pad0, pad1;
  IR_HD TextureDesc() : firstTexel(0), width(0), height(0), wrapS(kWrapRepeat), wrapT(kWrapRepeat), mipCount(1), pad0(0), pad1(0)
  {
    for (int i = 0; i < kMaxMipLevels; ++i) { mipOffset[i] = 0; mipW[i] = 0; mipH[i] = 0; }
  }
};

// A UsdUVTexture feeding one UsdPreviewSurface input: read RGBA, apply
// scale/bias, then take 'channel' for the scalar inputs.
struct TexRef {
  int  index;             // texture id, -1 = not textured
  int  channel;           // 0=r 1=g 2=b 3=a (scalar inputs; ORM maps share one image)
  int  udimSet;           // first of 100 entries in SceneView::udimTiles, -1 = a single image
  int  pad0;
  Vec4 scale, bias;
  float uv[6];            // UsdTransform2d as a 2x3: u' = uv0*u + uv1*v + uv2, v' = uv3*u + uv4*v + uv5
  IR_HD TexRef() : index(-1), channel(0), udimSet(-1), pad0(0), scale(1.0f, 1.0f, 1.0f, 1.0f), bias(0.0f, 0.0f, 0.0f, 0.0f)
  {
    uv[0] = 1.0f; uv[1] = 0.0f; uv[2] = 0.0f;
    uv[3] = 0.0f; uv[4] = 1.0f; uv[5] = 0.0f;
  }
  IR_HD bool valid() const { return index >= 0 || udimSet >= 0; }
};

struct Material {
  Vec3  diffuse;          // UsdPreviewSurface diffuseColor
  Vec3  emissive;         // emissiveColor
  float metallic;
  float roughness;
  float opacity;
  float ior;
  int   useSpecularWorkflow;
  Vec3  specularColor;
  int   useDisplayColor;  // 1: diffuse comes from the geometry's displayColor (no material bound - Hydra's fallback look)
  float normalScale;      // normal map strength
  // UsdPreviewSurface's remaining inputs.
  //   clearcoat        a second, always-smooth specular lobe on top - car paint,
  //                    varnish, a wet look.  0 is off and costs nothing.
  //   clearcoatRoughness  how blurred that second lobe is.
  //   occlusion        baked ambient occlusion, which darkens INDIRECT light
  //                    only: applying it to direct light as well is the classic
  //                    way to make an AO map look like dirt.
  //   opacityThreshold below this, opacity is taken as zero and the surface is a
  //                    hole rather than a tint - what cutouts on leaves need,
  //                    and what makes them shadow correctly.
  float clearcoat;
  float clearcoatRoughness;
  float occlusion;
  float opacityThreshold;
  // displacement: how far along the normal the surface moves, in scene units.
  // Done at LOAD by moving vertices, not in shading, so it changes the
  // silhouette and the shadow rather than only the shading normal.
  float displacement;
  int   pad1;
  TexRef diffuseTex, emissiveTex, roughnessTex, metallicTex, opacityTex, normalTex;
  TexRef clearcoatTex, occlusionTex;
  IR_HD Material() : diffuse(0.18f, 0.18f, 0.18f), emissive(0.0f), metallic(0.0f), roughness(0.5f), opacity(1.0f), ior(1.5f),
                     useSpecularWorkflow(0), specularColor(0.0f), useDisplayColor(0), normalScale(1.0f),
                     clearcoat(0.0f), clearcoatRoughness(0.01f), occlusion(1.0f), opacityThreshold(0.0f),
                     displacement(0.0f), pad1(0) {}
};

enum LightType { kLightDistant = 0, kLightPoint = 1, kLightSphere = 2, kLightRect = 3, kLightDome = 4,
                 kLightDisk = 5, kLightCylinder = 6 };

struct Light {
  int   type;
  int   texture;          // dome / rect: index into textures, -1 = constant colour
  float intensity;        // already includes exposure
  float radius;           // sphere / disk / cylinder
  Vec3  color;            // colour * intensity (* 1/area when the light normalises its power)
  Vec3  position;         // point / sphere / rect / disk / cylinder centre (world)
  Vec3  direction;        // distant: direction the light travels; area lights: their normal (local -Z)
  Vec3  u, v;             // rect: half extents along the rect axes (world); disk/cylinder: its two other axes
  Vec3  lx, ly, lz;       // dome: world -> light basis (rows), for the lat-long lookup
  float angle;            // distant: angular diameter in degrees
  float length;           // cylinder: half length along its axis (u)
  float area;             // emitting area of the shape, in world units
  // UsdLuxShapingAPI
  float coneCos;          // cos(cone angle); -1 = no cone
  float coneCosInner;     // where the cone starts to fall off
  float focus;            // shaping:focus exponent (0 = off)
  Vec3  focusTint;
  // UsdLuxShadowAPI + LightAPI multipliers
  Vec3  shadowColor;
  int   shadowEnable;
  float diffuseMul, specularMul;
  int   normalizePower;
  int   visibleToCamera;  // draw the shape itself for camera rays
  // An IES photometric profile: which table in SceneView::ies this light uses,
  // -1 for none.  It decides the SHAPE of the light's output by direction; the
  // intensity above still decides how bright it is, because a profile carries
  // absolute candela in the thousands and multiplying that in would blow out
  // every light that had one.
  int   iesProfile;
  int   pad0;
  IR_HD Light() : type(0), texture(-1), intensity(1.0f), radius(0.5f), color(1.0f), position(0.0f),
                  direction(0.0f, -1.0f, 0.0f), u(1.0f, 0.0f, 0.0f), v(0.0f, 1.0f, 0.0f),
                  lx(1.0f, 0.0f, 0.0f), ly(0.0f, 1.0f, 0.0f), lz(0.0f, 0.0f, 1.0f),
                  angle(0.53f), length(0.5f), area(1.0f), coneCos(-1.0f), coneCosInner(-1.0f), focus(0.0f),
                  focusTint(1.0f), shadowColor(0.0f), shadowEnable(1), diffuseMul(1.0f), specularMul(1.0f),
                  normalizePower(0), visibleToCamera(0), iesProfile(-1), pad0(0) {}
};

struct Camera {
  Xform camToWorld;       // camera space: looks down -Z, +Y up
  float tanHalfFovX, tanHalfFovY;   // from the projection
  float nearClip, farClip;
  int   width, height;
  int   orthographic;
  float orthoHalfW, orthoHalfH;
  IR_HD Camera() : camToWorld(Xform::identity()), tanHalfFovX(0.5f), tanHalfFovY(0.5f), nearClip(0.01f), farClip(1e6f),
                   width(1), height(1), orthographic(0), orthoHalfW(1.0f), orthoHalfH(1.0f) {}
};

struct Instance {
  Xform xf;               // object (prototype) -> world (at shutter open)
  Xform xf1;              // the same at shutter close; equal to xf when the instance does not move
  int   firstKey;         // first of SceneView::motionKeyCount transforms in SceneView::motionKeys
  int   protoId;
  int   hasColor;         // per-instance displayColor override
  int   instanceId;       // stable id (PointInstancer ids / running index)
  int   materialOverride; // -1 = use the prototype's per-triangle materials
  Vec3  color;
  float opacity;          // per-instance displayOpacity, multiplied into the material's (1 = none)
  // The cryptomatte id this copy writes.  It lives on the INSTANCE rather than
  // the prototype so that the granularity is a decision the host makes when it
  // fills this in: per object, every copy of a mesh gets the same id; per copy,
  // each gets its own.  The kernel just writes what it is given and has no mode
  // to branch on.
  float cryptoId;
  IR_HD Instance() : xf(Xform::identity()), xf1(Xform::identity()), firstKey(0), protoId(0), hasColor(0),
                     instanceId(0), materialOverride(-1), color(1.0f), opacity(1.0f), cryptoId(0.0f) {}
  // the transform a ray at shutter time t sees, with only the two end keys
  IR_HD Xform xfAt(float t) const
  {
    Xform x;
    for (int i = 0; i < 12; ++i) x.m[i] = xf.m[i] + (xf1.m[i] - xf.m[i]) * t;
    return x;
  }
};

// One OpenVDB density grid, read into a dense float array.
//
// Dense rather than sparse on purpose: a sparse tree is a pointer chase per
// sample, which is the wrong shape for a GPU and for the shared kernel both
// backends compile.  What that costs is said out loud in the info line, and the
// grid Hio hands back is already dense - the file's sparseness is spent on disk.
//
// The ray is put into the grid's own space to march it, so the box test is exact
// whatever transform the prim carries, and the volume can be rotated.
// how many emissive grids one volume can carry
enum { kVolumeEmissive = 2 };
// How an emissive grid's values are read.
//   kEmitIntensity  the value IS the brightness - a flames or fuel grid, 0..1ish
//   kEmitBlackbody  the value is a TEMPERATURE in Kelvin, and decides the colour
enum { kEmitIntensity = 0, kEmitBlackbody = 1 };
enum { kBlackbodyLutSize = 256 };

// An IES profile resampled onto a fixed grid: vertical angle 0..180 down the
// rows, horizontal 0..360 across.  Fixed so the kernel does one bilinear lookup
// rather than carrying each file's own irregular angle lists.
enum { kIesVRes = 64, kIesHRes = 128 };
// The span the table covers. Anything colder clamps to the first entry, so a
// grid that is not in Kelvin comes out as one flat colour - which is why the
// loader checks a grid's peak against this floor and says so.
const float kBlackbodyMinK = 500.0f;
const float kBlackbodyMaxK = 12000.0f;

// One sampled grid: where its voxels are, how many, and the box they fill.
struct GridRef {
  int  firstVoxel;        // into Scene::voxels, -1 = not present
  int  nx, ny, nz;
  Vec3 bmin, bmax;
  IR_HD GridRef() : firstVoxel(-1), nx(0), ny(0), nz(0), bmin(0.0f), bmax(0.0f) {}
  IR_HD bool valid() const { return firstVoxel >= 0 && nx > 0 && ny > 0 && nz > 0; }
};

// One OpenVDB volume: a density grid, up to two emissive grids, and a second
// copy of each at shutter close.
//
// Dense rather than sparse on purpose: a sparse tree is a pointer chase per
// sample, which is the wrong shape for a GPU and for the shared kernel both
// backends compile.  What that costs is said out loud in the info line, and the
// grid Hio hands back is already dense - the file's sparseness is spent on disk.
//
// The ray is put into the grid's own space to march it, so the box test is exact
// whatever transform the prim carries, and the volume can be rotated.
//
// SLOT 1 IS THE SHUTTER-CLOSE FRAME.  A simulation has no velocity grid to advect
// by - the explosion this was built against carries density, temperature and
// flames and nothing else - so the only way to blur it is to read the NEXT frame
// of the sequence too and cross-fade.  The two frames are different resolutions
// and different boxes, which is why each grid carries its own.
struct VolumeGrid {
  Xform worldToLocal;     // world -> the grid's own space
  GridRef density[2];     // [0] shutter open, [1] shutter close (invalid = no blur)
  GridRef emissive[kVolumeEmissive][2];
  Vec3  emissionColor[kVolumeEmissive];
  float emissionScale[kVolumeEmissive];
  int   emissionMode[kVolumeEmissive];    // kEmitIntensity / kEmitBlackbody
  // A grid that holds 0..1 rather than Kelvin is remapped into this range, which
  // is how a normalised flames grid can still drive a blackbody.
  float emitKmin[kVolumeEmissive], emitKmax[kVolumeEmissive];
  float densityScale;
  // A volume is an object like any other to a matte: the prim's path is hashed
  // on the host, because the device has no names.
  float cryptoId;
  IR_HD VolumeGrid() : worldToLocal(Xform::identity()), densityScale(1.0f), cryptoId(0.0f)
  {
    for (int i = 0; i < kVolumeEmissive; ++i) {
      emissionColor[i] = Vec3(1.0f);
      emissionScale[i] = 0.0f;
      emissionMode[i] = kEmitIntensity;
      emitKmin[i] = 0.0f; emitKmax[i] = 0.0f;
    }
  }
};

// one prototype = one triangle soup (several gprims flattened, materials per triangle)
struct ProtoRange {
  int firstVertex, numVertices;   // into the shared vertex arrays
  int firstTri, numTris;          // into the shared index / per-triangle arrays
  int hasNormals, hasUVs, hasColors;
  // The cryptomatte id for this prototype's name, worked out on the host because
  // the device has no names.  Copies of one mesh share a prototype and therefore
  // share an id, which is what "CryptoObject" means here: a name selects the
  // object, and every instance of it comes with.
  float cryptoId;
};

// Where each extra AOV sits inside the per-pixel float record.  -1 means the
// user did not ask for it, and it costs nothing: the offsets are packed, so a
// render that only wants P carries three floats per pixel, not twenty-four.
// Per-light AOVs.  The LAST slot is "everything else": lights beyond the first
// kMaxLightGroups-1, surface emission, and the background.  That is what makes
// the slots add up to the beauty exactly, however many lights a scene has.
//
// This is a cap on the count, NOT a buffer anywhere.  The obvious shape - an
// array of eight colours in the per-sample record - cost 65% of GPU render time
// on scenes with the pass switched OFF, because a per-thread array that big goes
// to local memory whether or not it is used.  The contributions go straight into
// the output buffer instead, so when the pass is off there is nothing to pay for.
// Measured: 0.278 s baseline, 0.463 s with the array, 0.295 s with an array of
// two, back to baseline without one.
static const int kMaxLightGroups = 8;

struct AovLayout {
  int position;
  int motion;
  int uv;
  int materialId;
  int objectId;
  int directDiffuse;
  int directSpecular;
  int indirectDiffuse;
  int indirectSpecular;
  int emission;
  // What the first surface the camera ray met IS, rather than how it was lit.
  // All of it is already worked out to shade that hit, so these cost no extra
  // tracing - only the room in the buffer.
  int surface;            // 4: roughness, metallic, opacity, facing ratio
  int specularColor;      // 3: reflectance at normal incidence (F0)
  int geoNormal;          // 3: the geometry's own normal, before any normal map
  // These two DO cost rays, or would if they were free: occlusion traces its own
  // and shadow rides along with the lighting that was being done anyway.
  int occlusion;          // 1: how open the sky is above the first hit, 1 = clear
  int shadow;             // 3: the direct light that geometry stopped from arriving
  int lightGroups;        // 3 * lightGroupCount: one colour per light, last = everything else
  int lightGroupCount;
  int crypto;             // 2 * cryptoSlots: (id, coverage) pairs, heaviest first
  int cryptoSlots;
  int deep;               // 6 * deepSlots: (id, coverage, depth, r, g, b), front first
  int deepSlots;
  int stride;             // floats per pixel; 0 = no extra AOVs at all
  IR_HD AovLayout() : position(-1), motion(-1), uv(-1), materialId(-1), objectId(-1), directDiffuse(-1),
                      directSpecular(-1), indirectDiffuse(-1), indirectSpecular(-1), emission(-1),
                      surface(-1), specularColor(-1), geoNormal(-1), occlusion(-1), shadow(-1),
                      lightGroups(-1), lightGroupCount(0), crypto(-1), cryptoSlots(0), deep(-1), deepSlots(0), stride(0) {}
};

// Which slots of the packed record are AVERAGES over the samples, and so keep
// refining as a progressive render adds passes.
//
// The rest describe ONE surface - the ids, P, the motion vectors, the surface
// properties - and are held from the first pass, because averaging them is
// meaningless: half of one instance id and half of another is a third object
// that is not there.  Getting this wrong is quiet either way round, so it is
// written down here, next to the layout, rather than as a list of offsets at
// the far end of the node.
inline bool aovSlotIsAveraged(const AovLayout& a, int slot)
{
  if (a.directDiffuse >= 0 && slot >= a.directDiffuse && slot < a.emission + 3) return true;
  if (a.shadow >= 0 && slot >= a.shadow && slot < a.shadow + 3) return true;
  if (a.occlusion >= 0 && slot == a.occlusion) return true;
  if (a.lightGroups >= 0 && slot >= a.lightGroups
      && slot < a.lightGroups + 3 * a.lightGroupCount) return true;
  // NOT the deep table either, for the same reason as cryptomatte: it holds ids
  // and depths, and half of one surface and half of another is a third that is
  // not there.
  // NOT the cryptomatte table.  Half of one id and half of another is a third
  // object that is not in the scene, and the coverages belong to whichever ids
  // happen to sit beside them.  A progressive pass merges that table BY ID
  // instead - see mergeCryptoTables() in the node.
  return false;
}

struct RenderSettings {
  int   width, height;
  int   samples;          // paths per pixel in THIS pass
  int   sampleOffset;     // index of the first path (progressive refinement continues a render)
  int   maxBounces;       // 0 = direct lighting only
  int   seed;
  int   aovFlags;
  float clampRadiance;    // 0 = off
  float rayEpsilon;
  Vec3  background;       // colour seen by camera rays that miss (alpha 0)
  int   backgroundVisible; // 1: write background colour into rgb (alpha stays 0)
  int   motionBlur;        // 1: instances carry a second transform and rays get a shutter time
  int   mipFilter;         // 1: pick a mip level from the ray's footprint
  int   deformationBlur;   // 1: vertices have a second set of positions to interpolate
  float shutterFrames;     // shutter length in frames, so motion vectors can be per frame
  AovLayout aov;
  float fixedShutterTime;  // debug: >= 0 pins every ray to that shutter time
  // 0 = continuous: every ray gets its own time across the shutter, which is a
  // smooth streak.  N >= 1 = ScanlineRender's kind of blur: the shutter is
  // sampled at N fixed instants and the result is N discrete copies.  Nobody
  // needs stepping for its own sake - it is here because a shot already graded
  // against Scanline has to keep looking like itself.
  int   motionSteps;
  // ambient occlusion: rays per camera sample, and how far they look
  // (0 = as far as the scene goes)
  // Volumes: how finely the camera ray is marched, how finely a shadow ray
  // through the volume is, and how much of what a step absorbs comes back out
  // as scattered light rather than being lost to heat.
  int   volumeSteps;
  int   volumeShadowSteps;
  float volumeAlbedo;
  int   volumeOctaves;   // 1 = single scattering only
  int   volumeDeepSegments;  // deep samples through the depth of a volume
  int   occlusionSamples;
  float occlusionDistance;
  IR_HD RenderSettings() : width(0), height(0), samples(16), sampleOffset(0), maxBounces(2), seed(0), aovFlags(0), clampRadiance(0.0f),
                           rayEpsilon(1e-4f), background(0.0f), backgroundVisible(0), motionBlur(0), mipFilter(1), deformationBlur(0), shutterFrames(0.0f),
                           fixedShutterTime(-1.0f), motionSteps(0), volumeSteps(64), volumeShadowSteps(16), volumeAlbedo(0.9f), volumeOctaves(3), volumeDeepSegments(8),
                           occlusionSamples(8), occlusionDistance(0.0f) {}
};

// Flat, pointer-based view of the scene - the same struct is filled with host
// pointers for the CPU path and with device pointers for the GPU path.
struct SceneView {
  Camera cameraClose;             // the camera at shutter close
  int    cameraMoves;             // 0 = it stands still, and every ray uses 'camera'
  int    volumeMoves;             // any volume carrying a shutter-close frame
  Camera cameraMv0, cameraMv1;    // the camera at shutter open / close, for motion vectors
  const Vec3*       vertices;     // all prototypes, object space (at shutter open)
  const Vec3*       vertices1;    // the same at shutter close, or null when nothing deforms
  const Vec3*       normals;      // per vertex (zero vector = none)
  const float*      uvs;          // 2 per vertex
  const Vec3*       colors;       // per vertex displayColor (or white)
  const uint32_t*   indices;      // 3 per triangle (vertex ids are absolute)
  const int*        triMaterial;  // per triangle
  const ProtoRange* protos;
  const VolumeGrid* volumes;
  const Vec3*       blackbody;   // temperature -> colour, built on the host
  const float*      ies;         // kIesVRes * kIesHRes per profile, back to back
  float             bbMinK, bbMaxK;
  const float*      voxels;
  const Instance*   instances;
  const Material*   materials;
  const Light*      lights;
  const Xform*      motionKeys;   // motionKeyCount transforms per instance, from Instance::firstKey
  const float*      texels;       // 4 per texel, all textures
  const TextureDesc* textures;
  const int*        udimTiles;    // 100 texture ids per UDIM set, -1 where no tile exists
  int numProtos, numInstances, numMaterials, numLights, numTextures;
  int numVolumes;
  int motionKeyCount;             // 0 or 1 = no motion; >= 2 = that many keys across the shutter
  int domeLight;                  // index of the dome light or -1
  // dome importance sampling (pbrt-style 2D distribution over a coarse grid)
  const float* domeFunc;          // domeW * domeH, luminance * sin(theta)
  const float* domeMarginal;      // domeH + 1
  const float* domeConditional;   // domeH * (domeW + 1)
  int   domeW, domeH;
  float domeFuncInt;              // average of domeFunc over the unit square
  Camera camera;
  RenderSettings settings;
};

// ---- host-side containers -------------------------------------------------------
struct Scene {
  std::vector<Vec3>       vertices;
  std::vector<Vec3>       vertices1;       // shutter-close positions, empty when nothing deforms
  std::vector<Vec3>       normals;
  std::vector<float>      uvs;
  std::vector<Vec3>       colors;
  std::vector<uint32_t>   indices;
  std::vector<int>        triMaterial;
  std::vector<ProtoRange> protos;
  std::vector<VolumeGrid> volumes;
  std::vector<Vec3>       blackbody;   // kBlackbodyLutSize entries over [bbMinK, bbMaxK]
  std::vector<float>      ies;         // resampled IES tables, back to back
  float bbMinK = 0.0f, bbMaxK = 0.0f;
  std::vector<std::string> volumeNames;   // one per volume, for the matte
  std::vector<float>      voxels;   // every grid's samples, back to back
  std::vector<std::string> protoNames;
  std::vector<Instance>   instances;
  // Host only, never uploaded: what makes an instance "the same one" at the other
  // end of the shutter.  Particles are born and die, so matching them by position
  // in the list blurs the wrong ones - or gives up when the count moves.
  std::vector<uint64_t>   instanceMatchKey;
  // Per-instance velocity in units per FRAME, when the geometry carries one.
  // Nuke's classic particles do not, but CopyToPoints will copy the particle
  // system's "vel" onto every copy as an object attribute (its "copy
  // attributes" knob), and that is worth far more than any amount of guessing
  // by proximity: a velocity is the object's OWN account of where it is going,
  // so it needs no partner at the other end of the shutter and is right for
  // particles that are being born or dying inside it.
  std::vector<Vec3>       instanceVel;
  std::vector<char>       instanceVelValid;   // told, as opposed to standing still
  bool                    hasVelocities = false;
  // false: the keys are only positions in the object list, which shift whenever
  // a particle is born or dies - see src/ir/MatchInstances.h
  bool                    matchKeysAreIds = false;
  std::vector<Material>   materials;
  std::vector<std::string> materialNames;
  // One per light, parallel to lights.  Nothing in the render uses these - they
  // are what a per-light AOV is named after, and what a cryptomatte manifest is
  // made of, so both loaders fill them even though only the front end reads them.
  std::vector<std::string> lightNames;
  // How many textures wired from a NUKE NODE could not be read this time.
  //
  // Not the same as a missing file, which will still be missing next time.
  // Reading one of these BAKES the node, and a bake can fail while the timeline
  // is moving - so this is a "come back and try again", and the renderer treats
  // a scene carrying it as one that was cut short.
  int nukeTextureFailures = 0;
  std::vector<Light>      lights;
  std::vector<Xform>      motionKeys;      // motionKeyCount per instance, in instance order
  int    motionKeyCount = 0;
  std::vector<float>      texels;          // 4 floats per texel, all textures back to back
  std::vector<TextureDesc> textures;
  std::vector<std::string> textureNames;
  // One per texture, describing what decides its content rather than what it is
  // called.  The GPU backend uses these to tell whether the texels it already
  // holds are still the right ones; left empty by the older non-USD path, which
  // simply means no such shortcut there.
  std::vector<std::string> textureKeys;
  std::vector<int>        udimTiles;       // 100 per set: udim 1001..1100 -> texture id
  std::vector<float>      domeFunc, domeMarginal, domeConditional;
  int    domeW = 0, domeH = 0;
  float  domeFuncInt = 0.0f;
  Camera  camera;
  // The camera at shutter CLOSE, and whether it went anywhere.  A scene that
  // does not move is still blurred by a camera that does, and the primary ray
  // is the only place that can happen - no instance transform describes it.
  Camera  cameraClose;
  bool    cameraMoves = false;
  Camera  cameraMv0, cameraMv1;   // the same camera at the ends of the motion interval
  bool    hasCamera = false;
  std::string info;         // human readable summary for the node
  std::string warnings;

  bool hasMotion = false;         // any instance whose two transforms differ
  bool hasNukeMaterialOps = false; // the stage carries materials authored by Nuke's material nodes

  size_t numTriangles() const { return indices.size() / 3; }
  // total triangles the renderer would see if everything were expanded
  double expandedTriangles() const
  {
    double t = 0.0;
    for (size_t i = 0; i < instances.size(); ++i) {
      const int p = instances[i].protoId;
      if (p >= 0 && size_t(p) < protos.size()) t += double(protos[size_t(p)].numTris);
    }
    return t;
  }
  SceneView view() const
  {
    SceneView v;
    v.vertices = vertices.empty() ? nullptr : vertices.data();
    v.vertices1 = (vertices1.size() == vertices.size() && !vertices1.empty()) ? vertices1.data() : nullptr;
    v.normals = normals.empty() ? nullptr : normals.data();
    v.uvs = uvs.empty() ? nullptr : uvs.data();
    v.colors = colors.empty() ? nullptr : colors.data();
    v.indices = indices.empty() ? nullptr : indices.data();
    v.triMaterial = triMaterial.empty() ? nullptr : triMaterial.data();
    v.protos = protos.empty() ? nullptr : protos.data();
    v.instances = instances.empty() ? nullptr : instances.data();
    v.materials = materials.empty() ? nullptr : materials.data();
    v.lights = lights.empty() ? nullptr : lights.data();
    v.motionKeys = motionKeys.empty() ? nullptr : motionKeys.data();
    v.motionKeyCount = motionKeyCount;
    v.texels = texels.empty() ? nullptr : texels.data();
    v.textures = textures.empty() ? nullptr : textures.data();
    v.udimTiles = udimTiles.empty() ? nullptr : udimTiles.data();
    v.numProtos = int(protos.size()); v.numInstances = int(instances.size());
    v.numMaterials = int(materials.size()); v.numLights = int(lights.size());
    v.numTextures = int(textures.size());
    v.domeFunc = domeFunc.empty() ? nullptr : domeFunc.data();
    v.domeMarginal = domeMarginal.empty() ? nullptr : domeMarginal.data();
    v.domeConditional = domeConditional.empty() ? nullptr : domeConditional.data();
    v.domeW = domeW; v.domeH = domeH; v.domeFuncInt = domeFuncInt;
    v.domeLight = -1;
    for (size_t i = 0; i < lights.size(); ++i) if (lights[i].type == kLightDome) { v.domeLight = int(i); break; }
    v.volumes = volumes.empty() ? nullptr : volumes.data();
    v.voxels = voxels.empty() ? nullptr : voxels.data();
    v.numVolumes = int(volumes.size());
    v.blackbody = blackbody.empty() ? nullptr : blackbody.data();
    v.ies = ies.empty() ? nullptr : ies.data();
    v.bbMinK = bbMinK; v.bbMaxK = bbMaxK;
    v.camera = camera;
    v.cameraClose = cameraClose;
    v.cameraMoves = cameraMoves ? 1 : 0;
    v.volumeMoves = 0;
    for (size_t i = 0; i < volumes.size(); ++i)
      if (volumes[i].density[1].valid()) { v.volumeMoves = 1; break; }
    v.cameraMv0 = cameraMv0;
    v.cameraMv1 = cameraMv1;
    return v;
  }
};

// ---- output buffers -----------------------------------------------------------------
enum AovFlags { kAovDepth = 1, kAovNormal = 2, kAovInstanceId = 4, kAovAlbedo = 8 };
struct FrameBuffers {
  int width = 0, height = 0;
  std::vector<float> rgba;        // 4 per pixel, premultiplied colour + alpha (coverage)
  std::vector<float> depth;       // camera-space Z distance (positive), 0 = miss
  std::vector<float> normal;      // 3 per pixel, world space
  std::vector<float> instanceId;  // 1 per pixel (-1 = miss)
  std::vector<float> albedo;      // 3 per pixel
  std::vector<float> extra;       // aovStride per pixel, laid out by AovLayout
  int aovStride = 0;
  void allocate(int w, int h, int stride = 0)
  {
    width = w; height = h;
    aovStride = stride;
    rgba.assign(size_t(w) * h * 4, 0.0f);
    depth.assign(size_t(w) * h, 0.0f);
    normal.assign(size_t(w) * h * 3, 0.0f);
    instanceId.assign(size_t(w) * h, -1.0f);
    albedo.assign(size_t(w) * h * 3, 0.0f);
    extra.assign(size_t(w) * h * size_t(stride > 0 ? stride : 0), 0.0f);
  }
};

} // namespace ir
