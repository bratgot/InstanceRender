// InstanceRender - GeoLoader.h
// Nuke's CLASSIC 3D system (GeoOp / GeometryList) -> ir::Scene.
//
// This is the front end Nuke 14.1 has to use: that release ships the USD
// libraries but no headers for them, so a plugin cannot speak USD there at all.
// It is not a fallback though - it works in every version, and it is the only
// way to render classic geometry (Card, Sphere, ReadGeo, the old CopyToPoints)
// with this renderer.
//
// The classic system has no notion of instancing: every copy of a mesh is its
// own GeoInfo carrying its own points.  So the loader looks at what the objects
// actually CONTAIN - identical geometry becomes one prototype and a transform
// per copy, which is the whole point of this renderer.
//
// Strict ASCII.
#pragma once

#include "Scene.h"

#include <functional>
#include <string>

namespace DD { namespace Image { class GeoOp; class Op; class Iop; } }

namespace ir {

struct ImageData;

// Renders a Nuke Iop into an image: validate, request, read the rows.  The
// classic front end bakes an object's material this way, and the USD front end
// needs the same thing for a texture whose path names a Nuke op rather than a
// file - see the note on "nkop:" paths in StageLoader.cpp.
bool bakeIop(DD::Image::Iop* op, int maxSize, ImageData& out);

struct GeoLoaderOptions {
  double maxTriangles = 5e8;    // in TRIANGLES (the node knob is in millions)
  int  maxTextureSize = 4096;
  bool textures = true;         // bake each object's material Iop into a texture
  bool mipFilter = true;
  bool lights = true;
  int  pointDetail = 2;         // how round a particle is drawn
  // Whether the USER has cancelled the render this load is feeding.  Note
  // cancelled() and not aborted(): a tree is also aborted when an upstream op
  // raises an error, and bailing out of those quietly throws the error away
  // instead of reporting it.  It matters
  // here and not only in the caller because loading VALIDATES upstream ops -
  // the geometry input, and every light - and validating an op after Nuke has
  // cancelled deadlocks: the main thread holds the graph waiting for this
  // render to stop, and validate() waits for the graph.  Scrubbing the playbar
  // is a stream of renders each cancelled by the next, so it hits this
  // constantly.  Empty means "never cancelled", which is what a batch render
  // wants.
  std::function<bool()> aborted;
};

// Reads the geometry the op produces at ITS current output context, so the
// caller controls the time by handing in the op it wants (see the motion pass
// in InstanceRender.cpp).  'lightSearchRoot' is walked for LightOps when the
// scene itself carries none.
bool loadClassicGeometry(DD::Image::GeoOp* geo, DD::Image::Op* lightSearchRoot,
                         const GeoLoaderOptions& opt, Scene& out, std::string& err);

} // namespace ir
