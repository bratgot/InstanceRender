// InstanceRender - HdIrMesh.cpp
// Hydra hands meshes over one Sync() at a time; this turns each one into the
// triangles and transforms the render pass needs.  Instancing is taken straight
// from HdInstancer, so a PointInstancer stays one prototype and a transform per
// copy - which is the whole point of this renderer.
// Strict ASCII.
#include "HdIrMesh.h"
#include "HdIrSubdiv.h"

#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The value of a primvar by name, whatever interpolation it was authored at.
VtValue _FindPrimvar(HdSceneDelegate* sd, const SdfPath& id, const TfToken& name,
                     HdInterpolation* interp)
{
  for (int i = 0; i < HdInterpolationCount; ++i) {
    const HdPrimvarDescriptorVector pvs = sd->GetPrimvarDescriptors(id, HdInterpolation(i));
    for (size_t p = 0; p < pvs.size(); ++p) {
      if (pvs[p].name != name) continue;
      const VtValue v = sd->Get(id, name);
      if (v.IsEmpty()) continue;
      *interp = HdInterpolation(i);
      return v;
    }
  }
  return VtValue();
}

} // namespace

HdIrMesh::HdIrMesh(SdfPath const& id, HdIrScene* scene)
  : HdMesh(id), _scene(scene)
{
}

HdDirtyBits HdIrMesh::GetInitialDirtyBitsMask() const
{
  return HdChangeTracker::Clean
       | HdChangeTracker::DirtyPoints
       | HdChangeTracker::DirtyTopology
       | HdChangeTracker::DirtyTransform
       | HdChangeTracker::DirtyVisibility
       | HdChangeTracker::DirtyPrimvar
       | HdChangeTracker::DirtyNormals
       | HdChangeTracker::DirtyInstancer
       | HdChangeTracker::DirtyInstanceIndex;
}

HdDirtyBits HdIrMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
  return bits;
}

void HdIrMesh::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
  // one representation is enough: this renderer draws surfaces, nothing else
  if (std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken)) == _reprs.end())
    _reprs.emplace_back(reprToken, HdReprSharedPtr());
}

void HdIrMesh::Finalize(HdRenderParam* renderParam)
{
  if (_scene) _scene->removeMesh(GetId());
}

void HdIrMesh::Sync(HdSceneDelegate* sceneDelegate,
                    HdRenderParam*   renderParam,
                    HdDirtyBits*     dirtyBits,
                    TfToken const&   reprToken)
{
  if (!sceneDelegate || !_scene) return;
  const SdfPath& id = GetId();

  // THIS is what tells the rprim which instancer it belongs to.  Without it
  // GetInstancerId() stays empty and every copy of a PointInstancer prototype
  // collapses to one - the instancer is created, it just never gets asked.
  _UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());

  HdIrMeshData mesh;
  mesh.visible = sceneDelegate->GetVisible(id);

  // ---- topology, triangulated -----------------------------------------------
  const HdMeshTopology topology = GetMeshTopology(sceneDelegate);
  VtVec3iArray triangles;
  VtIntArray primitiveParams;
  HdMeshUtil meshUtil(&topology, id);
  meshUtil.ComputeTriangleIndices(&triangles, &primitiveParams);

  // ---- points ----------------------------------------------------------------
  const VtValue pointsValue = sceneDelegate->Get(id, HdTokens->points);
  if (pointsValue.IsHolding<VtVec3fArray>()) {
    const VtVec3fArray& pts = pointsValue.UncheckedGet<VtVec3fArray>();
    mesh.points.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
      mesh.points.push_back(ir::Vec3(pts[i][0], pts[i][1], pts[i][2]));
  }
  if (mesh.points.empty() || triangles.empty()) {
    _scene->removeMesh(id);
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    return;
  }

  mesh.indices.reserve(triangles.size() * 3);
  for (size_t i = 0; i < triangles.size(); ++i) {
    for (int c = 0; c < 3; ++c) {
      const int v = triangles[i][c];
      mesh.indices.push_back(uint32_t(v >= 0 && size_t(v) < mesh.points.size() ? v : 0));
    }
  }

  if (const char* dbg = std::getenv("IR_HYDRA_LOG")) {
    std::ofstream f(dbg, std::ios::app);
    const char* kNames[] = { "constant", "uniform", "varying", "vertex", "faceVarying", "instance" };
    f << "primvars on " << id.GetString() << " (" << topology.GetNumPoints() << " points, scheme "
      << topology.GetScheme().GetString() << "):";
    for (int i = 0; i < HdInterpolationCount; ++i) {
      const HdPrimvarDescriptorVector pvs = sceneDelegate->GetPrimvarDescriptors(id, HdInterpolation(i));
      for (size_t p = 0; p < pvs.size(); ++p) {
        const VtValue v = sceneDelegate->Get(id, pvs[p].name);
        size_t n = 0;
        if (v.IsHolding<VtVec3fArray>()) n = v.UncheckedGet<VtVec3fArray>().size();
        else if (v.IsHolding<VtVec2fArray>()) n = v.UncheckedGet<VtVec2fArray>().size();
        f << " " << pvs[p].name.GetString() << "[" << kNames[i] << ":" << n << "]";
      }
    }
    f << std::endl;
  }

  // ---- colour, from displayColor when it is a single value ------------------
  const VtValue colorValue = sceneDelegate->Get(id, HdTokens->displayColor);
  if (colorValue.IsHolding<VtVec3fArray>()) {
    const VtVec3fArray& c = colorValue.UncheckedGet<VtVec3fArray>();
    if (!c.empty()) mesh.color = ir::Vec3(c[0][0], c[0][1], c[0][2]);
  }

  // ---- normals and st -------------------------------------------------------
  // Both can be authored at any interpolation, and both have to be handled the
  // same way, because they share the vertices.  Vertex and varying values index
  // by point.  A faceVarying value belongs to one CORNER and cannot be shared
  // between the triangles meeting at a point, so a mesh with either of them
  // faceVarying gets one vertex per corner - the same split the USD front end
  // does.  Nuke's own geometry arrives with faceVarying normals, and dropping
  // them (which a per-point size check does) is what makes a sphere faceted.
  {
    HdInterpolation nInterp = HdInterpolationVertex;
    VtValue nValue = _FindPrimvar(sceneDelegate, id, HdTokens->normals, &nInterp);
    if (nValue.IsEmpty()) {
      nValue = sceneDelegate->Get(id, HdTokens->normals);
      nInterp = HdInterpolationVertex;
      if (nValue.IsHolding<VtVec3fArray>()
          && nValue.UncheckedGet<VtVec3fArray>().size() != mesh.points.size())
        nValue = VtValue();       // an unlabelled array of the wrong length is no use
    }
    VtVec3fArray normals;
    if (nValue.IsHolding<VtVec3fArray>()) normals = nValue.UncheckedGet<VtVec3fArray>();

    static const TfToken kSt("st"), kUv("uv");
    HdInterpolation sInterp = HdInterpolationVertex;
    VtValue sValue = _FindPrimvar(sceneDelegate, id, kSt, &sInterp);
    if (sValue.IsEmpty()) sValue = _FindPrimvar(sceneDelegate, id, kUv, &sInterp);
    VtVec2fArray st;
    if (sValue.IsHolding<VtVec2fArray>()) st = sValue.UncheckedGet<VtVec2fArray>();
    else if (sValue.IsHolding<VtVec3fArray>()) {
      const VtVec3fArray& v3 = sValue.UncheckedGet<VtVec3fArray>();
      st.resize(v3.size());
      for (size_t i = 0; i < v3.size(); ++i) st[i] = GfVec2f(v3[i][0], v3[i][1]);
    }

    // per corner, once triangulated - empty unless that attribute is faceVarying
    VtVec3fArray cornerN;
    VtVec2fArray cornerSt;
    if (!normals.empty() && nInterp == HdInterpolationFaceVarying) {
      VtValue tri;
      if (meshUtil.ComputeTriangulatedFaceVaryingPrimvar(normals.cdata(), int(normals.size()),
                                                         HdTypeFloatVec3, &tri)
          && tri.IsHolding<VtVec3fArray>())
        cornerN = tri.UncheckedGet<VtVec3fArray>();
    }
    if (!st.empty() && sInterp == HdInterpolationFaceVarying) {
      VtValue tri;
      if (meshUtil.ComputeTriangulatedFaceVaryingPrimvar(st.cdata(), int(st.size()),
                                                         HdTypeFloatVec2, &tri)
          && tri.IsHolding<VtVec2fArray>())
        cornerSt = tri.UncheckedGet<VtVec2fArray>();
    }

    // whatever indexes by point can be filled in place
    const bool nByPoint = !normals.empty()
        && (nInterp == HdInterpolationVertex || nInterp == HdInterpolationVarying)
        && normals.size() >= mesh.points.size();
    const bool sByPoint = !st.empty()
        && (sInterp == HdInterpolationVertex || sInterp == HdInterpolationVarying)
        && st.size() >= mesh.points.size();
    const bool nConstant = !normals.empty() && nInterp == HdInterpolationConstant;
    if (nByPoint || nConstant) {
      mesh.normals.reserve(mesh.points.size());
      for (size_t i = 0; i < mesh.points.size(); ++i) {
        const GfVec3f& v = nConstant ? normals[0] : normals[i];
        mesh.normals.push_back(ir::Vec3(v[0], v[1], v[2]));
      }
    }
    if (sByPoint) {
      mesh.uvs.assign(mesh.points.size() * 2, 0.0f);
      for (size_t i = 0; i < mesh.points.size(); ++i) {
        mesh.uvs[i * 2] = st[i][0];
        mesh.uvs[i * 2 + 1] = st[i][1];
      }
    }

    // and anything per corner forces one vertex per corner
    if (!cornerN.empty() || !cornerSt.empty()) {
      std::vector<ir::Vec3> points, splitN;
      std::vector<float> uvs;
      std::vector<uint32_t> indices;
      const size_t corners = mesh.indices.size();
      points.reserve(corners);
      splitN.reserve(corners);
      uvs.reserve(corners * 2);
      indices.reserve(corners);
      for (size_t i = 0; i < corners; ++i) {
        const uint32_t v = mesh.indices[i];
        points.push_back(mesh.points[v]);
        if (!cornerN.empty()) {
          const GfVec3f& n = (i < cornerN.size()) ? cornerN[i] : GfVec3f(0.0f);
          splitN.push_back(ir::Vec3(n[0], n[1], n[2]));
        }
        else if (!mesh.normals.empty()) {
          splitN.push_back(mesh.normals[v]);
        }
        if (!cornerSt.empty()) {
          const GfVec2f& uv = (i < cornerSt.size()) ? cornerSt[i] : GfVec2f(0.0f);
          uvs.push_back(uv[0]); uvs.push_back(uv[1]);
        }
        else if (!mesh.uvs.empty()) {
          uvs.push_back(mesh.uvs[v * 2]); uvs.push_back(mesh.uvs[v * 2 + 1]);
        }
        indices.push_back(uint32_t(i));
      }
      mesh.points.swap(points);
      mesh.normals.swap(splitN);
      mesh.uvs.swap(uvs);
      mesh.indices.swap(indices);
    }
  }

  // ---- subdivision ----------------------------------------------------------
  // Refine the POLYGON mesh, not the triangulated one, and let the refined
  // corners become the vertices: that is what keeps the limit surface right and
  // a uv seam a seam.  Same ir::subdivide() the Nuke node uses.
  {
    bool bilinear = false;
    const int levels = irSubdivLevels(topology, _scene->tessellation().subdivLevels, &bilinear);
    if (levels > 0) {
      // The st the refiner needs, one per CORNER of the polygon mesh.
      //
      // A faceVarying st already is that.  A VERTEX one is not, and this used to
      // take only the faceVarying case - so a subdivided mesh whose st was
      // authored per point arrived at the refiner with no st at all, and the
      // swap below then replaced perfectly good uvs with nothing.  What that
      // looked like was a GeoCard rendering FLAT WHITE through GeoRender, its
      // texture being sampled at one texel, while a sphere beside it - not a
      // subdivision mesh, so never refined - was textured correctly.
      VtVec2fArray cornerSt;
      {
        static const TfToken kSt("st"), kUv("uv");
        HdInterpolation si = HdInterpolationVertex;
        VtValue sv = _FindPrimvar(sceneDelegate, id, kSt, &si);
        if (sv.IsEmpty()) sv = _FindPrimvar(sceneDelegate, id, kUv, &si);
        VtVec2fArray st2;
        if (sv.IsHolding<VtVec2fArray>()) st2 = sv.UncheckedGet<VtVec2fArray>();
        else if (sv.IsHolding<VtVec3fArray>()) {
          const VtVec3fArray& v3 = sv.UncheckedGet<VtVec3fArray>();
          st2.resize(v3.size());
          for (size_t i = 0; i < v3.size(); ++i) st2[i] = GfVec2f(v3[i][0], v3[i][1]);
        }
        if (si == HdInterpolationFaceVarying) {
          cornerSt = st2;
        }
        else if (!st2.empty()
                 && (si == HdInterpolationVertex || si == HdInterpolationVarying)) {
          // one per point -> one per corner, by walking the polygons
          const VtIntArray& fvi = topology.GetFaceVertexIndices();
          cornerSt.resize(fvi.size());
          for (size_t i = 0; i < fvi.size(); ++i) {
            const int v = fvi[i];
            cornerSt[i] = (v >= 0 && size_t(v) < st2.size()) ? st2[size_t(v)] : GfVec2f(0.0f);
          }
        }
      }
      VtVec3fArray pts;
      const VtValue pv = sceneDelegate->Get(id, HdTokens->points);
      if (pv.IsHolding<VtVec3fArray>()) pts = pv.UncheckedGet<VtVec3fArray>();
      HdIrMeshData refined;
      if (irSubdivide(topology, pts, cornerSt, levels, bilinear, refined)) {
        mesh.points.swap(refined.points);
        mesh.normals.swap(refined.normals);
        mesh.uvs.swap(refined.uvs);
        mesh.indices.swap(refined.indices);
        if (const char* dbg = std::getenv("IR_HYDRA_LOG")) {
          std::ofstream f(dbg, std::ios::app);
          f << "subdivided " << id.GetString() << " x" << levels
            << (bilinear ? " (bilinear)" : " (catmullClark)") << " -> "
            << mesh.points.size() << " point(s), " << (mesh.indices.size() / 3)
            << " triangle(s)" << std::endl;
        }
      }
    }
  }

  mesh.materialId = sceneDelegate->GetMaterialId(id);

  // ---- where the copies go ---------------------------------------------------
  // With no instancer there is one copy, at the prim's own transform.  With one,
  // Hydra computes the transforms and each becomes an instance of this single
  // prototype - no geometry is duplicated.
  if (std::getenv("IR_HYDRA_LOG")) {
    // does Hydra offer this prim at more than one time?  Motion blur through
    // GeoRender depends entirely on the answer.
    HdTimeSampleArray<GfMatrix4d, 4> sa;
    sceneDelegate->SampleTransform(id, &sa);
    HdTimeSampleArray<GfMatrix4d, 4> si;
    sceneDelegate->SampleTransform(id, -0.5f, 0.5f, &si);
    std::ofstream f(std::getenv("IR_HYDRA_LOG"), std::ios::app);
    f << "transform samples for " << id.GetString() << ": default " << sa.count;
    for (size_t i = 0; i < sa.count; ++i) f << " t=" << sa.times[i];
    f << ";  interval [-0.5,0.5] " << si.count;
    for (size_t i = 0; i < si.count; ++i) f << " t=" << si.times[i];
    if (si.count > 1) {
      const GfVec3d a0 = si.values[0].ExtractTranslation();
      const GfVec3d a1 = si.values[si.count - 1].ExtractTranslation();
      f << "  moved (" << (a1[0] - a0[0]) << ", " << (a1[1] - a0[1]) << ", " << (a1[2] - a0[2]) << ")";
    }
    f << std::endl;
  }

  const GfMatrix4d primXf = sceneDelegate->GetTransform(id);
  const SdfPath& instancerId = GetInstancerId();
  if (instancerId.IsEmpty()) {
    mesh.instances.push_back(irXformOf(primXf));
  }
  else {
    HdRenderIndex& index = sceneDelegate->GetRenderIndex();
    HdInstancer* instancer = index.GetInstancer(instancerId);
    VtMatrix4dArray transforms;
    VtVec3fArray instColors;
    if (HdIrInstancer* ours = dynamic_cast<HdIrInstancer*>(instancer)) {
      transforms = ours->ComputeInstanceTransforms(id);
      // Per-copy colour.  Without this every copy took the PROTOTYPE's single
      // displayColor, so CopyToPointsUSD's colour variance - and its painted
      // colour - came out one flat colour across the whole scatter.  The
      // instancer had been collecting the primvar all along; nothing read it.
      instColors = ours->ComputeInstanceColors(id);
    }
    if (transforms.empty()) {
      mesh.instances.push_back(irXformOf(primXf));
    }
    else {
      mesh.instances.reserve(transforms.size());
      for (size_t i = 0; i < transforms.size(); ++i)
        mesh.instances.push_back(irXformOf(primXf * transforms[i]));
      if (instColors.size() == transforms.size()) {
        mesh.instanceColors.resize(instColors.size());
        for (size_t i = 0; i < instColors.size(); ++i)
          mesh.instanceColors[i] = ir::Vec3(instColors[i][0], instColors[i][1], instColors[i][2]);
      }
    }
  }

  if (const char* dbg = std::getenv("IR_HYDRA_LOG")) {
    std::ofstream f(dbg, std::ios::app);
    f << "mesh " << id.GetString() << ": " << mesh.points.size() << " point(s), "
      << (mesh.indices.size() / 3) << " triangle(s), instancer="
      << (instancerId.IsEmpty() ? std::string("none") : instancerId.GetString())
      << ", instances=" << mesh.instances.size() << std::endl;
  }
  _scene->setMesh(id, mesh);
  *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE
