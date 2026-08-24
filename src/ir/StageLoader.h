// InstanceRender - StageLoader.h
// pxr UsdStage -> ir::Scene: meshes, native instances, PointInstancers,
// UsdPreviewSurface materials, UsdLux lights, UsdGeomCamera.  Strict ASCII.
#pragma once

#include "Scene.h"
#include "Image.h"
#include <functional>

#include <pxr/usd/usd/stage.h>

#include <string>

namespace ir {

// A texture whose path names a Nuke op rather than a file.  The loader cannot
// read one itself, so the host hands it a way to: see the note in loadTexture().
typedef std::function<bool(const std::string& path, int maxSize, ImageData& out)> NukeOpImageFn;
// Empty means the op is no longer feeding this render. Otherwise a token that
// changes whenever the op's PICTURE changes.
typedef std::function<std::string(const std::string& path)> NukeOpIdentityFn;

struct LoaderOptions {
  double timeCode = 1.0;          // stage time code (frame) to sample
  // The sample a motion key extrapolates FROM.  USD applies a PointInstancer's
  // velocities only when the requested time differs from this - that is its
  // answer to "varying-topology particle streams", and it also means every key
  // describes the SAME particles, so nothing has to be matched up.  Equal to
  // timeCode (the default) means no extrapolation: positions are interpolated
  // between samples, which is what a stream that gains and loses particles
  // cannot do.
  double baseTimeCode = 0.0;
  bool   hasBaseTime = false;
  std::string cameraPath;         // empty = first UsdGeomCamera found
  bool includeGuides = false;     // purpose guide / proxy prims
  bool skipInvisible = true;
  double maxExpandedTriangles = 0.0;   // 0 = no guard; otherwise refuse scenes above this many rendered triangles
  bool textures = true;                // resolve UsdUVTexture / dome-light images
  bool mipFilter = true;               // build mip pyramids so distant textures can be filtered
  int  maxTextureSize = 4096;          // box-downsample anything larger (0 = full resolution)
  // A dense grid is nx*ny*nz floats, so it grows as the cube of the resolution.
  // Hio downsamples to fit this rather than refusing, which is the behaviour
  // wanted here: a heavy volume renders coarse instead of not at all.
  int  maxVolumeMemoryMB = 512;
  // Blur a volume by reading the shutter-close frame of the sequence too.  Off
  // for the extra pass the motion keys already make, which only wants transforms.
  //
  // The close FRAME is passed rather than worked out from shutterOpen/Close,
  // because the main load carries neither - it is given the frame and a zero
  // shutter, and the motion is handled by separate passes.
  bool volumeBlur = false;
  int  volumeCloseFrame = 0;
  // Textures kept between loads, so a scene rebuilt for a handle drag does not
  // pay to bake and mip an unchanged image again.  Optional; null just means
  // build everything every time.
  class TextureCache* textureCache = nullptr;
  int  pointDetail = 2;                // sphere rings for a UsdGeomPoints point
  int  curveSides = 6;                 // tube sides for a UsdGeomBasisCurves curve
  int  curveSegments = 4;              // samples per curve span
  int  subdivLevels = 0;               // Catmull-Clark refinement levels (0 = render the control cage)
  double shutterOpen = 0.0;            // motion blur: frames relative to timeCode
  double shutterClose = 0.0;           // equal to shutterOpen = no motion blur
  int  motionKeys = 2;                 // transforms sampled across the shutter (2 = one linear segment)
  bool lightsVisible = true;           // area lights are drawn where they are, for camera rays
  bool transformsOnly = false;         // skip the geometry: the second motion pass only wants instance xforms
  NukeOpImageFn nukeOpImage;           // reads a texture that names a Nuke op (empty = cannot)
  // What is the op an "nkop:" path names doing NOW?
  //
  // The URI carries a hash of the op's state, so it looks like it identifies the
  // picture - and it does, at the moment the input is MADE. Nuke does not
  // maintain it afterwards: grading the node feeding a GeoDomeLight leaves the
  // URI byte for byte identical (measured, and Nuke's own GeoRender is equally
  // blind to it), and disconnecting the node does not clear it either.
  //
  // Since that URI is also the texture cache key, a stale one is answered from
  // the cache for ever - which is what made "refresh render" fail to help.
  NukeOpIdentityFn nukeOpIdentity;     // empty function = assume everything is live and current
};

// Fills 'scene' from the composed stage.  Returns false (with scene.warnings) on a hard failure.
bool loadStage(const PXR_NS::UsdStageRefPtr& stage, const LoaderOptions& opt, Scene& scene);

} // namespace ir
