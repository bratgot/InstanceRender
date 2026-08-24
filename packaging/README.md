# InstanceRender

A renderer node for Nuke that **keeps instancing**. A `UsdGeomPointInstancer`
with 5000 copies of an 80,000-triangle mesh costs one acceleration structure
plus 5000 transforms, not 400 million triangles - so scattered geometry that
Nuke's own renderer cannot hold renders here in a few seconds.

It renders Nuke's USD 3D system and its classic 3D system, uses the stage's own
`UsdPreviewSurface` materials and `UsdLux` lights, and adds no shading nodes of
its own.

This zip is a **compiled Windows x64 release**. Nothing needs building.

---

## Install

Double-click **`install.bat`**, or from PowerShell:

```powershell
.\install.ps1                 # every Nuke version in this zip
.\install.ps1 -Versions 17.1  # just the one you use (saves ~38 MB each)
```

Then start Nuke. **InstanceRender** appears on the **3D** toolbar.

`INSTALL.md` covers custom locations, studio installs, uninstalling and what to
do when the node does not appear.

## Which Nuke

**14.1, 15.2, 16.0, 16.1, 17.0 and 17.1** - Windows x64.

The build has to match the Nuke **minor** version exactly: a 16.0 build does not
load in 16.1. That is Nuke's ABI, not a policy - `COMPATIBILITY.md` has the
matrix and explains what the error looks like when it happens.

## What you need

* **Nuke** 14.1 or newer, Windows x64. A commercial or non-commercial licence
  both work; this is an ordinary plugin.
* **Nothing else.** Embree, oneTBB and the CUDA runtime are in this zip and the
  installer puts them where the plugin looks.
* **Optional: an NVIDIA GPU** for the OptiX back-end. Without one - or without a
  recent NVIDIA driver - the renderer runs on the CPU through Embree and every
  feature still works. The `device` knob picks between them.

## What is in the box

| | |
|---|---|
| `InstanceRender/` | the plugin: one folder per Nuke version, plus icons and the scripts that load it |
| `runtime/` | Embree 4.3.3, oneTBB 2021.11, CUDA runtime 12.6 - copied beside each build at install time |
| `ToolSets/` | an AOV recipe, which appears under **ToolSets > InstanceRender** |
| `licenses/` | the licence texts for the three libraries above |
| `INSTALL.md` | installing, uninstalling, and what to do when it does not appear |
| `COMPATIBILITY.md` | which build for which Nuke, and what each version supports |

## The short version of what it does

* **Instancing kept end to end** - `UsdGeomPointInstancer`, `instanceable` prims,
  and classic 3D objects that share a mesh all collapse to one prototype and a
  transform each. Motion blur goes through the acceleration structure itself, so
  blurred copies stay instanced.
* **Path tracing** with next-event estimation on CPU (Embree) or GPU (OptiX),
  running the same shading kernel either way.
* **The stage's own materials and lights** - `UsdPreviewSurface` including
  clearcoat, occlusion, opacity threshold and displacement; distant, sphere,
  rect, disk, cylinder and dome lights, `ShapingAPI` cones, `ShadowAPI`, and IES
  profiles. Dome HDRIs are importance sampled, so a small bright sun converges
  instead of speckling.
* **Volumes** - OpenVDB density and temperature grids, with blackbody emission
  that reads temperature as Kelvin and normalises it the way Karma and Cycles do.
* **Textures** - any format Nuke reads, `<UDIM>` tile sets, `UsdTransform2d`,
  per-channel outputs so one ORM map can drive three inputs, and mip filtering
  from the ray footprint. A texture can also be **a Nuke node**, read live.
* **Progressive refinement** in the viewer, and a **Hydra render delegate** on
  Nuke 17.0 and 17.1 so InstanceRender appears in the Viewer's renderer menu and
  in `GeoRender`.
* **Cryptomatte**, AOVs, and the OptiX denoiser.

Every knob has rollover help; that is the fastest documentation there is.

## Licence

InstanceRender is **MIT** - see `LICENSE`. Use it commercially, modify it, ship
it inside a pipeline; keep the copyright notice.

It is a plugin: it does not contain Nuke and gives you no rights to it. The
three libraries in `runtime/` keep their own licences (Apache-2.0 for Embree and
oneTBB, the NVIDIA CUDA EULA for the CUDA runtime) - texts in `licenses/`, and
`THIRD_PARTY_NOTICES.md` says what is used and why.

## Source

<https://github.com/bratgot/InstanceRender>
