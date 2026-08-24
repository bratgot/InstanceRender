# Compatibility

Windows x64. Nuke 14.1 through 17.1.

## The build must match the Nuke minor version exactly

There is one build per Nuke **minor** version, and they are not interchangeable:

| Nuke | folder in this zip |
|---|---|
| 14.1 | `nuke14.1` |
| 15.2 | `nuke15.2` |
| 16.0 | `nuke16.0` |
| 16.1 | `nuke16.1` |
| 17.0 | `nuke17.0` |
| 17.1 | `nuke17.1` |

Nuke's plugin ABI changes between minor versions - 16.0 and 16.1 are as
incompatible as 14 and 17. A mismatched build does not misbehave subtly, it
fails to load outright with *"the specified procedure could not be found"*.

So the plugin's `init.py` matches the running Nuke **exactly** and loads nothing
at all if there is no match, printing which build is missing:

```
InstanceRender: no build for Nuke 16.1 in ...\.nuke\InstanceRender (installed: nuke17.1)
```

That message is the single most common thing people hit. It means install the
matching build, not that anything is broken.

**Patch versions do not matter.** `Nuke17.1v1`, `v2`, `v3` all use the `nuke17.1`
build - it is the minor version that counts.

## What each version supports

| | 14.1 | 15.2 | 16.0 | 16.1 | 17.0 | 17.1 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| Classic 3D (`ReadGeo`, `Card`, `Sphere`, classic `CopyToPoints`) | yes | yes | yes | yes | yes | yes |
| Nuke's USD 3D system (`GeoScene`, USD stages) | - | - | yes | yes | yes | yes |
| `UsdGeomPointInstancer` and `instanceable` prims | - | - | yes | yes | yes | yes |
| OpenVDB volumes and blackbody emission | - | - | yes | yes | yes | yes |
| Hydra render delegate (Viewer renderer menu, `GeoRender`) | - | - | - | - | yes | yes |
| CPU path tracing (Embree) | yes | yes | yes | yes | yes | yes |
| GPU path tracing and denoiser (OptiX) | yes | yes | yes | yes | yes | yes |
| Motion blur, AOVs, Cryptomatte | yes | yes | yes | yes | yes | yes |

**14.1 and 15.2 have no USD front end.** Nuke gained `GeometryProviderI` - the
entry point that hands a plugin a composed USD stage - in 16.0. Those two
versions ship the USD libraries but not that header, so InstanceRender reads
their **classic** 3D geometry instead and recovers instancing from what the
objects contain: objects sharing a mesh become one prototype and a transform
each. It is the same renderer and the same shading; only the way geometry
arrives differs.

**The Hydra delegate needs 17.0 or newer**, where Nuke discovers renderers
through USD's own plugin registry. It also has to be registered *before* Nuke
starts, because Nuke builds its renderer list once during startup - use
`hydra_launch.bat`. The InstanceRender **node** needs none of this and works
normally in 17.0 and 17.1 either way.

## GPU

The GPU back-end is compiled into every build here. It needs:

* an **NVIDIA** GPU (OptiX is NVIDIA-only), and
* a driver new enough for OptiX 9 - a 2024 or later driver.

There is nothing to install: OptiX is loaded from the driver, and the CUDA
runtime is in this zip.

Without a suitable card or driver the `device` knob's GPU entry fails and the
CPU back-end renders the same image through Embree. Every feature works on the
CPU; the GPU is a speed choice, not a capability one. On a machine with no
NVIDIA card at all, use CPU - that path is what runs on a render farm without
GPUs anyway.

## Operating system

**Windows x64 only, in this zip.** The source builds on Linux (gcc 9 for Nuke
14, gcc 11 for 15 to 17) and the CMake is set up for it, but no Linux binary is
shipped here and none has been run - build from source if you need one:
<https://github.com/bratgot/InstanceRender>

There is no macOS build: Nuke on macOS is ARM, and the GPU back-end is CUDA.

## Libraries this ships

| | version | licence |
|---|---|---|
| Embree | 4.3.3 | Apache-2.0 |
| oneTBB | 2021.11 | Apache-2.0 |
| CUDA runtime | 12.6 | NVIDIA CUDA EULA (redistributable component) |

USD, Hydra and OpenVDB are **not** shipped - those come from your own Nuke, which
is why the plugin has to match its version so exactly.
