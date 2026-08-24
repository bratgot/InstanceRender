// InstanceRender - HdIrPoints.cpp
// Turning HdPoints and HdBasisCurves into the triangles and transforms the
// render pass already knows how to draw.  The tessellation itself is shared
// with the Nuke node - ir::buildUnitSphere and ir::buildTube - so a curve looks
// the same whichever front end it came in through.
// Strict ASCII.
#include "HdIrPoints.h"
#include "HdIrMesh.h"

#include "ir/Tessellate.h"

#include <pxr/imaging/hd/basisCurvesTopology.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <cstdlib>
#include <fstream>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

HdDirtyBits _initialBits()
{
  return HdChangeTracker::Clean
       | HdChangeTracker::DirtyPoints
       | HdChangeTracker::DirtyTopology
       | HdChangeTracker::DirtyWidths
       | HdChangeTracker::DirtyTransform
       | HdChangeTracker::DirtyVisibility
       | HdChangeTracker::DirtyPrimvar
       | HdChangeTracker::DirtyMaterialId
       | HdChangeTracker::DirtyInstancer
       | HdChangeTracker::DirtyInstanceIndex;
}

// Reading a Vec3f array off whichever interpolation the prim happens to use.
VtVec3fArray _vec3(HdSceneDelegate* sd, const SdfPath& id, const TfToken& name)
{
  const VtValue v = sd->Get(id, name);
  if (v.IsHolding<VtVec3fArray>()) return v.UncheckedGet<VtVec3fArray>();
  return VtVec3fArray();
}

VtFloatArray _floats(HdSceneDelegate* sd, const SdfPath& id, const TfToken& name)
{
  const VtValue v = sd->Get(id, name);
  if (v.IsHolding<VtFloatArray>()) return v.UncheckedGet<VtFloatArray>();
  if (v.IsHolding<float>()) { VtFloatArray a; a.push_back(v.UncheckedGet<float>()); return a; }
  return VtFloatArray();
}

// Where the copies of this rprim go: its own transform, or one per instancer
// copy.  Identical to the mesh path, and deliberately so.
std::vector<ir::Xform> _outerTransforms(HdSceneDelegate* sd, const SdfPath& id,
                                        const SdfPath& instancerId)
{
  std::vector<ir::Xform> out;
  const GfMatrix4d primXf = sd->GetTransform(id);
  if (instancerId.IsEmpty()) {
    out.push_back(irXformOf(primXf));
    return out;
  }
  HdInstancer* instancer = sd->GetRenderIndex().GetInstancer(instancerId);
  VtMatrix4dArray transforms;
  if (HdIrInstancer* ours = dynamic_cast<HdIrInstancer*>(instancer))
    transforms = ours->ComputeInstanceTransforms(id);
  if (transforms.empty()) out.push_back(irXformOf(primXf));
  else {
    out.reserve(transforms.size());
    for (size_t i = 0; i < transforms.size(); ++i)
      out.push_back(irXformOf(primXf * transforms[i]));
  }
  return out;
}

void _copySoup(const ir::TriangleSoup& soup, HdIrMeshData& mesh)
{
  mesh.points = soup.points;
  mesh.normals = soup.normals;
  mesh.uvs = soup.uvs;
  mesh.indices = soup.indices;
}

void _log(const SdfPath& id, const char* what, size_t a, size_t b)
{
  if (const char* dbg = std::getenv("IR_HYDRA_LOG")) {
    std::ofstream f(dbg, std::ios::app);
    f << what << " " << id.GetString() << ": " << a << " -> "
      << b << " instance(s)" << std::endl;
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Points

HdDirtyBits HdIrPoints::GetInitialDirtyBitsMask() const { return _initialBits(); }

void HdIrPoints::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
  if (std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken)) == _reprs.end())
    _reprs.emplace_back(reprToken, HdReprSharedPtr());
}

void HdIrPoints::Finalize(HdRenderParam* renderParam)
{
  if (_scene) _scene->removeMesh(GetId());
}

void HdIrPoints::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
                      HdDirtyBits* dirtyBits, TfToken const& reprToken)
{
  if (!sceneDelegate || !_scene) return;
  const SdfPath& id = GetId();

  _UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());

  const VtVec3fArray positions = _vec3(sceneDelegate, id, HdTokens->points);
  if (positions.empty()) {
    _scene->removeMesh(id);
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    return;
  }
  const VtFloatArray widths = _floats(sceneDelegate, id, HdTokens->widths);
  const VtVec3fArray colors = _vec3(sceneDelegate, id, HdTokens->displayColor);

  HdIrMeshData mesh;
  mesh.visible = sceneDelegate->GetVisible(id);
  mesh.materialId = sceneDelegate->GetMaterialId(id);

  // one sphere, shared by every point
  ir::TriangleSoup sphere;
  ir::buildUnitSphere(_scene->tessellation().pointDetail, sphere);
  _copySoup(sphere, mesh);

  if (!colors.empty()) mesh.color = ir::Vec3(colors[0][0], colors[0][1], colors[0][2]);

  const std::vector<ir::Xform> outer = _outerTransforms(sceneDelegate, id, GetInstancerId());
  mesh.instances.reserve(outer.size() * positions.size());
  for (size_t o = 0; o < outer.size(); ++o) {
    for (size_t i = 0; i < positions.size(); ++i) {
      float w = 1.0f;
      if (widths.size() == positions.size()) w = widths[i];
      else if (!widths.empty()) w = widths[0];
      const float r = 0.5f * w;
      if (!(r > 0.0f)) continue;
      // the point puts the sphere where it is and sizes it; the outer transform
      // is wherever the prim itself sits
      ir::Xform local = ir::Xform::identity();
      local.m[0] = r; local.m[5] = r; local.m[10] = r;
      local.m[3] = positions[i][0];
      local.m[7] = positions[i][1];
      local.m[11] = positions[i][2];
      mesh.instances.push_back(ir::mul(outer[o], local));
      if (colors.size() == positions.size())
        mesh.instanceColors.push_back(ir::Vec3(colors[i][0], colors[i][1], colors[i][2]));
    }
  }
  if (mesh.instanceColors.size() != mesh.instances.size()) mesh.instanceColors.clear();

  _log(id, "points", positions.size(), mesh.instances.size());
  _scene->setMesh(id, mesh);
  *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

// ---------------------------------------------------------------------------
// Basis curves

HdDirtyBits HdIrCurves::GetInitialDirtyBitsMask() const { return _initialBits(); }

void HdIrCurves::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
  if (std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken)) == _reprs.end())
    _reprs.emplace_back(reprToken, HdReprSharedPtr());
}

void HdIrCurves::Finalize(HdRenderParam* renderParam)
{
  if (_scene) _scene->removeMesh(GetId());
}

void HdIrCurves::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
                      HdDirtyBits* dirtyBits, TfToken const& reprToken)
{
  if (!sceneDelegate || !_scene) return;
  const SdfPath& id = GetId();

  _UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());

  const VtVec3fArray cvs = _vec3(sceneDelegate, id, HdTokens->points);
  const HdBasisCurvesTopology topology = GetBasisCurvesTopology(sceneDelegate);
  const VtIntArray counts = topology.GetCurveVertexCounts();
  if (cvs.empty() || counts.empty()) {
    _scene->removeMesh(id);
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    return;
  }
  const VtFloatArray widths = _floats(sceneDelegate, id, HdTokens->widths);
  const VtVec3fArray colors = _vec3(sceneDelegate, id, HdTokens->displayColor);

  const bool cubic = (topology.GetCurveType() == HdTokens->cubic);
  int basisId = 0;                                          // bspline
  const TfToken basisTok = topology.GetCurveBasis();
  if (basisTok == HdTokens->catmullRom) basisId = 1;
  else if (basisTok == HdTokens->bezier) basisId = 2;

  const HdIrTessellation tess = _scene->tessellation();
  const int samples = (tess.curveSegments < 1) ? 1
                    : ((tess.curveSegments > 16) ? 16 : tess.curveSegments);

  ir::TriangleSoup soup;
  size_t at = 0;
  int built = 0;
  for (size_t c = 0; c < counts.size(); ++c) {
    const int nCv = counts[c];
    if (nCv < 2 || at + size_t(nCv) > cvs.size()) { at += size_t(nCv > 0 ? nCv : 0); continue; }
    const size_t first = at;
    at += size_t(nCv);

    // the width of a control vertex, however the prim chose to author it
    struct W {
      const VtFloatArray& widths; size_t total, curveFirst;
      float at(size_t cv) const
      {
        if (widths.size() == total) return widths[curveFirst + cv];
        if (!widths.empty()) return widths[0];
        return 0.1f;
      }
    };
    const W w = { widths, cvs.size(), first };

    std::vector<ir::Vec3> centres;
    std::vector<float> radii;
    if (!cubic) {
      for (int i = 0; i < nCv; ++i) {
        centres.push_back(ir::Vec3(cvs[first + size_t(i)][0], cvs[first + size_t(i)][1],
                                   cvs[first + size_t(i)][2]));
        radii.push_back(0.5f * w.at(size_t(i)));
      }
    }
    else {
      const int step = (basisId == 2) ? 3 : 1;              // bezier spans step by three
      for (int i = 0; i + 3 < nCv; i += step) {
        const ir::Vec3 p0(cvs[first + size_t(i)][0], cvs[first + size_t(i)][1], cvs[first + size_t(i)][2]);
        const ir::Vec3 p1(cvs[first + size_t(i + 1)][0], cvs[first + size_t(i + 1)][1], cvs[first + size_t(i + 1)][2]);
        const ir::Vec3 p2(cvs[first + size_t(i + 2)][0], cvs[first + size_t(i + 2)][1], cvs[first + size_t(i + 2)][2]);
        const ir::Vec3 p3(cvs[first + size_t(i + 3)][0], cvs[first + size_t(i + 3)][1], cvs[first + size_t(i + 3)][2]);
        for (int sIdx = 0; sIdx < samples; ++sIdx) {
          const float t = float(sIdx) / float(samples);
          centres.push_back(ir::evalCubic(p0, p1, p2, p3, t, basisId));
          const float wa = w.at(size_t(i + 1)), wb = w.at(size_t(i + 2));
          radii.push_back(0.5f * (wa + (wb - wa) * t));
        }
      }
      if (!centres.empty()) {                               // close the last span
        const int last = nCv - 1;
        const ir::Vec3 p0(cvs[first + size_t(last - 3)][0], cvs[first + size_t(last - 3)][1], cvs[first + size_t(last - 3)][2]);
        const ir::Vec3 p1(cvs[first + size_t(last - 2)][0], cvs[first + size_t(last - 2)][1], cvs[first + size_t(last - 2)][2]);
        const ir::Vec3 p2(cvs[first + size_t(last - 1)][0], cvs[first + size_t(last - 1)][1], cvs[first + size_t(last - 1)][2]);
        const ir::Vec3 p3(cvs[first + size_t(last)][0], cvs[first + size_t(last)][1], cvs[first + size_t(last)][2]);
        centres.push_back(ir::evalCubic(p0, p1, p2, p3, 1.0f, basisId));
        radii.push_back(0.5f * w.at(size_t(last)));
      }
    }
    if (centres.size() >= 2) { ir::buildTube(centres, radii, tess.curveSides, soup); ++built; }
  }
  if (built == 0 || soup.indices.empty()) {
    _scene->removeMesh(id);
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    return;
  }

  HdIrMeshData mesh;
  mesh.visible = sceneDelegate->GetVisible(id);
  mesh.materialId = sceneDelegate->GetMaterialId(id);
  _copySoup(soup, mesh);
  if (!colors.empty()) mesh.color = ir::Vec3(colors[0][0], colors[0][1], colors[0][2]);
  mesh.instances = _outerTransforms(sceneDelegate, id, GetInstancerId());

  _log(id, "curves", size_t(built), mesh.instances.size());
  _scene->setMesh(id, mesh);
  *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE
