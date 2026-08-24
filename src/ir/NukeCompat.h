// InstanceRender - NukeCompat.h
//
// The NDK moves between releases, and this node is built once per Nuke minor
// version (14.1 through 17.0).  Everything that changed between them lives here
// so the rest of the plugin can be written once:
//
//   Nuke 14.1  no pxr headers at all -> no USD front end, classic 3D only
//   Nuke 15.2  pxr 23.11, stages come from the static GeomOp::buildStage()
//   Nuke 16.0  pxr 24.05, GeometryProviderI::getGeometryStage(), no BuildStage
//   Nuke 16.1  pxr 24.05, and GeometryProviderI::BuildStage() as well
//   Nuke 17.0  pxr 25.08, the same as 16.1
//
// IR_NUKE_VER is major * 100 + minor (1401, 1502, 1600, 1601, 1700), set by CMake.
//
// Strict ASCII.
#pragma once

#ifndef IR_NUKE_VER
#error "IR_NUKE_VER must be defined by the build (major * 100 + minor)"
#endif

#include "DDImage/Op.h"

#include <string>
#include <cstdlib>
#include "DDImage/CameraOp.h"
#include "DDImage/GeomOp.h"
#include "DDImage/GeoOp.h"

#if IR_NUKE_VER >= 1600
#include "ndk/geo/camera/LensProjection.h"
#include "DDImage/GeometryProviderI.h"
#endif

#if IR_HAVE_USD
#include <usg/api.h>
#include <usg/geom/Stage.h>
#include <usg/geom/Layer.h>
#if IR_NUKE_VER < 1600
#include <usg/base/ArgSet.h>
#endif
#endif

namespace ir {

// ---- the input op as it stands at another time ---------------------------------
// Op::inputAt() only arrived in Nuke 16.1; node_input() with an explicit context
// is the same thing and is in every version this plugin builds against.
inline DD::Image::Op* inputAtContext(const DD::Image::Op* self, int input, const DD::Image::OutputContext& ctx)
{
  return const_cast<DD::Image::Op*>(self)->node_input(input, DD::Image::Op::EXECUTABLE_INPUT, &ctx);
}

// ---- what kind of scene is this? ------------------------------------------------
// Only GeomOp (the USD 3D system) advertises a geometry provider; the classic
// GeoOps never do, so the two front ends can be told apart the same way in every
// version.
inline bool isStageSource(DD::Image::Op* op)
{
  if (!op) return false;
#if IR_NUKE_VER >= 1600
  return op->geometryProvider() != nullptr;
#else
  return dynamic_cast<DD::Image::GeomOp*>(op) != nullptr;
#endif
}

// anything this node can render from: a USD stage or classic 3D geometry
inline bool isGeometrySource(DD::Image::Op* op)
{
  if (!op) return false;
  if (isStageSource(op)) return true;
  return dynamic_cast<DD::Image::GeoOp*>(op) != nullptr;
}

// ---- orthographic cameras -------------------------------------------------------
// LensProjection lived in DD::Image before Nuke 16 and in ndk from 16 on.
inline bool isOrthographic(DD::Image::CameraOp* cam)
{
  if (!cam) return false;
#if IR_NUKE_VER >= 1600
  return cam->projectionMode() == ndk::LensProjection::Orthographic;
#else
  return cam->projectionMode() == DD::Image::LensProjection::ORTHOGRAPHIC;
#endif
}

#if IR_HAVE_USD
// ---- the stage ------------------------------------------------------------------
// Ask the scene input for its composed stage, carrying every sample time the
// render needs in ONE build (see the long note at the call site: building the
// same stage repeatedly is what trips Nuke's own material ops).
//
// 16.0 and later have GeometryProviderI, which is the sanctioned route and hands
// back the provider's own composed stage.  15.2 has no provider interface at all,
// so the stage is built from the GeomOp directly - the same call the viewer and
// ScanlineRender2 make there, and it takes the sample times too.
// set from InstanceRender.cpp so the stage hand-off can be traced with IR_LOG
typedef void (*CompatLogFn)(const std::string&);
inline CompatLogFn& compatLog() { static CompatLogFn fn = 0; return fn; }
inline void clog(const std::string& m) { if (compatLog()) compatLog()(m); }

inline usg::StageRef acquireStage(DD::Image::Op* sceneOp, const fdk::TimeValueSet& sampleTimes, bool& usedFallback)
{
  usedFallback = false;
  usg::StageRef stage;
  if (!sceneOp) return stage;
  clog("acquireStage: enter");
#if IR_NUKE_VER >= 1600
  if (DD::Image::GeometryProviderI* provider = sceneOp->geometryProvider())
    stage = provider->getGeometryStage(sampleTimes);
  if (!stage || !stage->isValid()) {
#if IR_NUKE_VER >= 1601
    // 16.1 and 17 can author into a stage of our own; 16.0 has no such entry point
    usedFallback = true;
    stage = usg::Stage::CreateInMemory();
    if (stage) {
      DD::Image::OpGraphLocation location(sceneOp);
      DD::Image::GeometryProviderI::BuildStage(stage, location, sampleTimes);
    }
#endif
  }
#else
  // Nothing here: before Nuke 16.0 a plugin has no supported way to be handed a
  // composed stage (see the note in CMakeLists.txt), so those builds carry the
  // classic front end only and never reach this function.
  (void)sampleTimes;
#endif
  return stage;
}
#endif // IR_HAVE_USD

} // namespace ir
