# Third-party notices

InstanceRender itself is MIT (see `LICENSE`). It is a **plugin**: it is compiled
against, and loaded by, software it does not contain and does not redistribute.
This file records what it uses and under what terms, and - just as importantly -
what this repository does **not** ship.

## Source repository and binary release ship different things

This file covers two things, and the difference matters:

* **This repository** vendors no third-party source or binary at all. Everything
  below is supplied by your own installs at build time.
* **The binary release zip** (`InstanceRender-<version>-Nuke...-win64.zip`) does
  ship three runtime libraries, because a compiled plugin is no use without
  them: **Embree** and **oneTBB** (both Apache-2.0, which permits
  redistribution) and the **CUDA runtime** `cudart64_12.dll`, which NVIDIA lists
  as a redistributable component of the CUDA Toolkit. Their licence texts travel
  in that zip's `licenses/` folder. Nothing else third-party is in it - no Nuke,
  no USD, no OptiX, no OpenVDB.

### What this repository does NOT contain

No third-party source or binary is vendored here. In particular:

* **No Nuke headers, libraries or DLLs.** The build links against the Nuke
  installation on the machine doing the building (`NUKE_ROOT`). Nothing from
  Foundry is copied into this repository or into a release of it.
* **No import libraries.** `third_party/*.lib` and `*.exp` are generated
  locally by `third_party/mkimportlibs.ps1` from the DLLs your own Nuke ships,
  and are gitignored. They are a Windows linking artefact - a table of symbol
  names - and are not redistributed.
* **No Embree, TBB, CUDA, OptiX or USD binaries.** All are supplied by your own
  installs when you build. (The binary release zip does carry Embree, oneTBB and
  the CUDA runtime - see above. OptiX and USD are never shipped in either form:
  OptiX is loaded from the NVIDIA driver, USD comes from your Nuke.)

The only exception is `third_party/python311.def`, a plain-text list of the
symbol names CPython exports, used to synthesise the import library above.

## Build and runtime dependencies

| Component | Licence | How it is used |
|---|---|---|
| **Nuke NDK** (DDImage, Ndk, FdkBase, FnUsdAbstraction, FnUsdEngine, `usg`) | Foundry proprietary, per your Nuke licence | Linked at build time; supplied by your Nuke install. A plugin needs a licensed Nuke to build and to run. |
| **Embree 4** | Apache-2.0 | The CPU ray tracer. Supplied by your `EMBREE_ROOT`. |
| **oneTBB / TBB** | Apache-2.0 | Required by Embree and by USD. Supplied by Embree or by Nuke. |
| **OpenUSD (pxr)** | Modified Apache-2.0 (Pixar) | The USD front end and the Hydra delegate. Nuke ships its own build; this links against that. |
| **NVIDIA CUDA Toolkit** | NVIDIA CUDA EULA | Optional GPU back-end. Headers and runtime from your own toolkit. |
| **NVIDIA OptiX SDK** | NVIDIA OptiX SDK licence | Optional GPU back-end and the denoiser. Headers only - OptiX is loaded through the driver at runtime. |
| **OpenVDB**, reached through USD's `Hio` | Mozilla Public License 2.0 | Volume grids are read by `HioFieldTextureData`, whose OpenVDB backend is a plugin found through USD's registry. Supplied by your Nuke; no OpenVDB source or binary is here. |
| **Boost** (headers only) | Boost Software License 1.0 | Only for Nuke 15 and 16, whose pxr headers were built against boost.python. Nuke 17's USD vendors its own copy and needs none. |

Being MIT does not grant you any right to Nuke, CUDA, OptiX or any other
component above; each remains under its own terms.

## Algorithms implemented here from published descriptions

These are original implementations in this repository's own source, written from
public descriptions. They are listed because the ideas are not ours.

* **MurmurHash3 (x86, 32-bit)** - Austin Appleby, released into the **public
  domain**. Re-implemented in `src/ir/Crypto.h` because the Cryptomatte
  specification names this exact hash.
* **Cryptomatte** - the manifest and ranked-coverage layout follow the
  Cryptomatte specification (Psyop, BSD-3-Clause). No Psyop code is used.
* **CIE colour matching functions** - the multi-lobe Gaussian fits published by
  Wyman, Sloan and Shirley, "Simple Analytic Approximations to the CIE XYZ Color
  Matching Functions" (JCGT, 2013), used in `src/ir/Blackbody.h`.
* **IESNA LM-63** - the photometric file format, parsed in `src/ir/Ies.h` from
  the published format description. IES holds no rights in files you supply.
* **UsdPreviewSurface** - the shading model follows Pixar's published
  specification.

## Icons

`nuke/icons/*.png` are drawn by `tools/make_icons.py` in this repository. They
are stylistically consistent with Nuke's own icon set - a grey body, dark
interior lines, one colour accent - but no Foundry artwork is copied,
traced or included.
