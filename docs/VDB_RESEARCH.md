# VDB / volume support - research notes

Findings only; nothing here is implemented.  Everything below was measured on
this machine against Nuke 17.0v3, with `test/vdb_probe.py` and the plugin's
`IR_STAGE_PROBE=1` stage dump.

## 1. Where a volume could come from

**Nuke's own Fields are out of reach.**  Nuke ships a large field system -
`FieldNoise`, `FieldGrid`, `FieldVolume` (reads a `.vdb`), `FieldMerge`,
`FieldVolumeWrite`, `FieldRender` (its own ray marcher), ~40 nodes in all,
backed by `Fields.dll` / `FnOpFields.dll`.  It is a **separate graph** from the
USD 3D system:

* `GeoScene.setInput(0, <field>)` is refused, and so is InstanceRender's `scn`.
* `usg/geom` has no volume or field prim type at all (no `VolumePrim.h`), so a
  field cannot be expressed in the stage a `GeometryProviderI` builds.
* There are no field headers in the NDK, so there is no supported way to read
  one from a plugin either.

So a field authored in Nuke can only reach this renderer by being written out
(`FieldVolumeWrite`) and brought back through USD.

**USD volumes do survive.**  A `Volume` prim with an `OpenVDBAsset` child comes
through Nuke's engine untouched - verified with the stage dump:

    /World/Smoke [Volume] attrs: extent
    /World/Smoke/density [OpenVDBAsset] attrs: fieldName, filePath
    /World/Marker [Mesh] attrs: faceVertexCounts, faceVertexIndices, points, ...

That is the whole interface: `UsdVolVolume` with `field:<name>` relationships to
`UsdVolOpenVDBAsset` prims carrying `filePath` and `fieldName`.  `usd_usdVol`
and `usd_usdVolImaging` ship with headers, so reading that structure needs
nothing new.

## 2. Reading the grids

Nuke ships:

| what | ships? |
| --- | --- |
| `openvdb.dll` (1205 exported symbols) | yes |
| `openvdb.lib` (import library) | **no** |
| OpenVDB headers | **no** |
| `usd_hioOpenVDB.dll` + `.lib` + headers | yes |
| `nanovdb_print.exe` / `nanovdb_validate.exe` | yes - "built against NanoVDB version 32.7.0", i.e. OpenVDB 11.x |

`pxr/imaging/hioOpenVDB/utils.h` is exactly the entry point we would want:

    openvdb::GridBase::Ptr HioOpenVDBGridFromAsset(const std::string& name,
                                                   const std::string& assetPath);

It resolves the asset path through `Ar` (so USD asset resolution, packages and
relative paths all work) and hands back a grid.  Its header includes
`openvdb/openvdb.h`, which is what we do not have.

**The way in is the one this project already used for Python.**
`third_party/python311.lib` was generated from `python311.dll` because Nuke
ships no import library; the same applies here: dump `openvdb.dll`'s exports,
build an `openvdb.lib`, and vendor OpenVDB 11.x headers to match.  OpenVDB core
needs TBB, which Nuke ships and this project already links.

Risks: the vendored headers must match the shipped build closely (ABI, and
OpenVDB's `OPENVDB_ABI_VERSION_NUMBER`), and a Nuke point release could move
it.  A cheap guard is to keep the VDB code in its own translation unit behind a
delay-loaded DLL, exactly like `embree4.dll` and `usd_hio.dll` are handled now,
so a mismatch degrades to "volumes not available" instead of a plugin that will
not load.

## 3. Representation inside the renderer

**NanoVDB**, converted at load with `nanovdb::createNanoGrid(...)`.  It is a
single flat, pointer-free buffer that both a CPU pointer and a device pointer
can walk with *identical* code - which is the constraint this whole renderer is
built around (one kernel, two devices, `src/ir/Kernel.h`).  The alternative,
sampling OpenVDB's tree on the CPU and something else on the GPU, would fork the
sampling code and break parity by construction.

NanoVDB ships inside the OpenVDB distribution, so vendoring the headers gets
both.  `SceneView` would gain `const void* nvdbGrids` + a per-volume descriptor,
in the same shape as the texture pool.

## 4. What rendering volumes actually needs

Beyond loading, this is a second integrator path, not a small addition:

* **Bounds in the BVH.**  Each volume needs its own intersectable - the
  cheapest is to put the grid's world bounding box into the same acceleration
  structure as a box primitive (Embree user geometry / OptiX custom primitive
  with an AABB), so the existing traversal finds it and instancing keeps
  working: a `Volume` under a PointInstancer prototype would then be instanced
  exactly like a mesh, which is the interesting case for this renderer.
* **Marching or tracking.**  Ratio tracking / delta tracking through the grid
  for unbiased results, or fixed-step ray marching for speed.  Both need a
  density scale, an extinction colour and a phase function (Henyey-Greenstein),
  and both must run in `Kernel.h` so the two devices agree.
* **Lighting.**  Next event estimation from inside the medium, with transmittance
  along the shadow ray (ratio tracking again).  The existing light sampling can
  be reused as is; only the visibility term changes.
* **Motion blur.**  NanoVDB grids per motion key would multiply memory; the
  honest first version is to interpolate the volume's transform only, exactly
  like the mesh instances do today.

## 5. Rough shape of the work

1. Generate `third_party/openvdb.lib` from the shipped DLL, vendor OpenVDB 11.x
   headers, and prove `HioOpenVDBGridFromAsset` returns a grid from inside Nuke
   (a gate test in the style of `test/hio_probe.py`).
2. Loader: find `UsdVolVolume` prims, read their `OpenVDBAsset` fields, convert
   to NanoVDB, store the blobs in the scene.
3. Backends: a box primitive per volume in both acceleration structures.
4. Kernel: transmittance and in-scattering, shared by both devices.
5. Tests: a density ramp with a known analytic transmittance; a volume under a
   PointInstancer to prove instancing survives; CPU/GPU parity.

Step 1 is the gate - if the import library or the headers do not line up with
the shipped `openvdb.dll`, everything after it changes shape (the fallback being
to require pre-converted `.nvdb` files, which needs no OpenVDB at all but does
need users to convert their assets).
