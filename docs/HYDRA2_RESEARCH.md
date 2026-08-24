# Hydra 2.0 in Nuke 17.1 - what it is, and what it means for InstanceRender

Findings only, measured against **Nuke 17.1v1-Beta.4** (16 July 2026) on 2026-08-20.
Nothing here is implemented.

## What actually shipped

The release notes describe it from the artist's side.  From the SDK's side, three
things arrived:

**1. A renderer plug-in point that is plain OpenUSD.**  `usg/imaging/RenderInterface.h`
is the *consumer* half - Nuke uses it to drive a renderer - and it enumerates
renderers with

    struct RenderPluginDesc { Token id; std::string displayName; int priority; };
    static const std::vector<RenderPluginDesc>& rendererPlugins();

Those three fields are exactly what a pxr `HdRendererPlugin` declares in its
`plugInfo.json`.  Nuke ships two: `hdStorm` (displayName "GL") and `hdStormGS`,
under `FnUSD/plugin/usd/`.  Discovery is the stock USD mechanism -
`PXR_PLUGINPATH_NAME`, confirmed as the variable their `usd_plug.dll` reads.

**Verified**: a `plugInfo.json` declaring a renderer plug-in with no library
behind it at all, on `PXR_PLUGINPATH_NAME`, appears in Nuke's menus:

    GeoRender.renderer -> ['HdStormRendererPlugin\tGL\t\tnuke',
                           'HdInstanceRenderRendererPlugin\tInstanceRender (probe)']
    Viewer.renderer    -> the same two

So a third-party renderer can present itself to Nuke 17.1 without Foundry's
involvement.  The `Viewer` node also gained `display_aov` (currently only
`color`, which is all Storm offers) - the AOV menu is fed by the delegate.

**2. The UsdRender schema, authored by nodes.**  New prim wrappers
(`usg/geom/RenderSettingsPrim.h`, `RenderProductPrim.h`, `RenderVarPrim.h`,
`RenderPassPrim.h`) and the nodes that write them:

| node | authors | knobs that matter |
| --- | --- | --- |
| `GeoRenderSetting` | UsdRenderSettings | camera, products, resolution, includedPurposes, instantaneousShutter, materialBindingPurposes, renderingColorSpace |
| `GeoRenderProduct` | UsdRenderProduct | productName, productType (raster / deepRaster), orderedVars |
| `GeoRenderVariable` | UsdRenderVar | dataType (the full Sdf type list), sourceName, sourceType |
| `GeoRender` | the renderer Iop | renderer, render_settings_prim_path, render_product_prim_path, renderer_settings, render_variables_table, render_settings_legacy |

This is how "auto apply schema" works: the render settings live in the USD scene
as prims, and the delegate reads them.  A delegate's own settings appear in
`GeoRender.renderer_settings`, which is why Renderman's knobs show up there
without Foundry writing any Renderman UI.

**3. Hydra 2.0 scene indices.**  `usg/hd/` wraps the scene-index API
(`SceneIndex.h`, `FilteringSceneIndex.h`, `DataSource*.h`, `SceneIndexObserver.h`),
and `FnUSD/plugin/usd/` gained `fnNukeSceneIndices` and `fnSharedSceneIndices`.
Nuke's own 3D graph is fed to Hydra through scene indices now.

Also measured while testing 17.1: the **TfToken material bug is fixed there**.
The minimal repro in test/nuke_material_bug_repro.py (CheckerBoard2 ->
PreviewSurface -> GeoSphere -> GeoScene -> ScanlineRender2) renders clean on
17.1 and still errors on 17.0.  The stage-build error suppression this node
carries is still needed for 14.1 to 17.0, but on 17.1 the 'report stage-build
errors' knob can be left on.

Unrelated to renderers but in the same release: `ndk/geo/render/SurfaceTraits.h`,
`RayContext.h` and `SamplingRng.h` are the ScanlineRender2 *shader* side (opacity
/ presence / transmission traits) - that is the cutout feature, not a renderer
entry point.

## What Foundry's own documentation and source say

Reading `Documentation/` in the 17.1 install (thank you for the pointer) settles
most of the guesswork:

**The NDK never documents consuming a stage.**  The Developers Guide has a whole
`3d-usd` chapter - basic concepts, API usage, writing plugins - and every plugin
kind it covers *produces* or *modifies* geometry (`SourceGeomOp`, `ModifyGeomOp`)
or is a shader.  `GeometryProviderI`, `GeomOp::buildStage` and the word
"renderer plugin" appear nowhere in it, and the only 3D examples shipped are
`GeoTriangle.cpp` (a SourceGeomOp) and `GeoTwist.cpp` (a modifier) - in 15.2 as
well as 17.1.  So an Iop that pulls a composed stage, which is what this node is,
is undocumented territory in every version.  That it works from 16.0 is because
`GeometryProviderI` is public API; that it crashes in 15.2 is unsurprising in
that light.

**Shaders have a documented plugin path, in two flavours.**  ScanlineRender2 no
longer uses `Iop`/`Material` at all: it looks up an `SlrShader` plugin by
prefixing the `ShaderPrim`'s `info:id` with `slr` (so `UsdPreviewSurface` ->
`slrUsdPreviewSurface`).  For the Hydra viewer, materials come from a
`SurfaceShaderSchema` subclass that emits **glslfx** source through a
`ShaderSource` builder.  Neither is a renderer entry point, but both explain
where the "contexts" below come from.

**Nuke 17.1 can be run against your own USD build**, and the source of the layer
that binds it ships: `source/FnUsdShim/` next to the executable, with
`FdkBaseConfig.cmake` and `FnUsdAbstractionConfig.cmake` in `cmake/`.  The
relevant environment variables are `USG_SHIMLIB_NAME`, `USG_USD_LIB_PATH`,
`USG_USD_PLUGIN_PATH` and `USG_PLUGINS_PATH`.

That source includes `RenderInterfaceImpl.cpp` and `nuke/NukeRenderInterfaceImpl.cpp`
- the host side of Hydra - so the contract a delegate has to meet can simply be
read off:

| what Nuke wants | where it comes from |
| --- | --- |
| the renderer list | `HdRendererPluginRegistry::GetInstance().GetPluginDescs()` - stock Hydra, exactly as the probe showed |
| the trailing context token | `renderDelegate->GetMaterialRenderContexts()`.  Nuke calls `GetOrCreateRendererPlugin` -> `IsSupported()` -> `CreateDelegate()` on every plugin it finds, purely to ask.  Storm answers `nuke`, which is why its entry reads `...\tGL\t\tnuke` |
| `GeoRender.renderer_settings` | `renderDelegate->GetRenderSettingDescriptors()` |
| which AOVs exist | `renderDelegate->GetDefaultAovDescriptor(token)`, matched against the `UsdRenderVar` prims the `GeoRenderVariable` nodes author |
| the Viewer's `display_aov` menu | `NukeRenderInterface::getSupportedAovTokens()`, from the same descriptors |
| the render itself | an `HdxTaskController`, with `SetRenderOutputSettings` per AOV |

So the delegate side is entirely stock Hydra - `GetRenderSettingDescriptors`,
`GetDefaultAovDescriptor`, `GetMaterialRenderContexts` - and the mapping from
this renderer is direct: the knobs become setting descriptors, and the AOVs
(`P`, `st`, `forward`, `material.id`, `object.id`, the light split) become aov
descriptors that a `GeoRenderVariable` can name.

## What it would mean for InstanceRender

Today InstanceRender is an `Iop` that pulls a composed stage and renders it.
The alternative Hydra 2.0 offers is to be an **`HdRendererPlugin` + `HdRenderDelegate`**,
which would mean:

* it appears in the Viewer's renderer menu, so the 3D viewer is rendered by it
  interactively - progressive refinement is already how this renderer works;
* `GeoRender` outputs its AOVs as render vars, driven by `GeoRenderVariable`
  nodes, instead of the node's own AOV checkboxes;
* its settings (samples, bounces, device, clamp ...) appear in
  `GeoRender.renderer_settings` automatically, because that table is built from
  the delegate's own settings descriptors;
* the same delegate would work in Katana (`usg/katana/KatanaRenderInterface.h` is
  the same stack) and in any other Hydra host.

**What carries over unchanged**: everything below the front end.  `Kernel.h`
(the shared CPU/GPU kernel), `Scene.h`, both back-ends, `Texture`/`Image`,
`Subdivide.h`, `Tessellate.h` - that is the bulk of the renderer, and none of it
knows where its scene came from.

**What would be rewritten**: `StageLoader.cpp` becomes Hydra prim handling
(`HdMesh`, `HdPoints`, `HdBasisCurves`, `HdInstancer`, `HdMaterial`, `HdLight`,
`HdCamera`) or scene-index consumption, and `InstanceRender.cpp` (the Iop, its
knobs, the progressive loop) is replaced by `HdRenderDelegate` +
`HdRenderPass` + `HdRenderBuffer`.  OpenUSD's own `hdEmbree` is the reference
implementation for exactly this shape of renderer.

## It works: a delegate built against Nuke's pxr renders in Nuke

`src/hydra/HdInstanceRender.cpp` is the first step - a delegate that paints one
flat colour - and on 2026-08-20 it went the whole way in Nuke 17.1v1-Beta.4:

    library loaded
    static registration running
    registered as: HdIrRendererPlugin
    plugin constructed
    IsSupported(gpu=1) -> true
    delegate constructed
    CreateRenderPass()
    render pass executing, 4 aov binding(s)

    GeoRender -> Write -> Read, centre pixel rgb = 0.150 0.450 0.850

which is exactly the colour the render pass writes.  So Nuke discovers, loads,
instantiates and drives a third-party delegate built against its own pxr, and
its output comes back through `GeoRender` as an ordinary image.  Both remaining
unknowns from the list above are answered.

### The one hard part, worth writing down

`TF_REGISTRY_FUNCTION(TfType)` - the way every OpenUSD example registers a
plugin type - **silently does nothing** in this build.  On Windows the macro
emits its entry as an unreferenced `const` in an anonymous namespace placed in a
`.pxrctor` section, and MSVC drops it before the linker ever sees it: the object
file has no `.pxrctor` section at all, while Nuke's own `hdStorm.dll` has one.
`/OPT:NOREF` does not help, because the loss happens at compile time.

The symptom is nasty, because nothing fails: the library loads, USD registers the
*plugin* from its `plugInfo.json`, and the renderer appears in the menus - it is
only when something tries to create it that Nuke says

    Render interface is not available for the selected renderer

Registering from an ordinary static initialiser instead

    namespace {
      struct RegisterPluginType {
        RegisterPluginType() { HdRendererPluginRegistry::Define<HdIrRendererPlugin>(); }
      };
      static RegisterPluginType g_registerPluginType;
    }

runs reliably, and the delegate is instantiated from that point on.  The
`TF_REGISTRY_FUNCTION` is kept alongside it for hosts that do dispatch it.

A second thing to know: `PXR_PLUGINPATH_NAME` must point at the directory that
*contains* `plugInfo.json` (the `resources` folder), not at its parent.

## Step two: it renders the geometry

`HdIrMesh` syncs Hydra's meshes into an `ir::Scene` and the render pass path
traces it with the same Embree back-end the Nuke node uses.  A `GeoSphere`
through `GeoRender` comes back as a lit sphere - centre pixel 0.275, corners
black - and the delegate's own trace reads

    mesh /GeoSphere1: 932 point(s), 1740 triangle(s), instancer=none, instances=1
    render pass: 1 mesh(es), 1 instance(s), 1740 triangle(s), 640x480

So the whole path works: Hydra prims -> ir::Scene -> Embree -> AOV -> Nuke.

## Step four: lights and materials

`HdIrLight` reads the UsdLux parameters Hydra hands over - colour, intensity,
exposure, radius, width/height, cone angle and softness, shadow enable/colour,
the diffuse and specular multipliers - and maps distant, sphere, disk, rect,
cylinder and dome onto the light types the kernel already has.  `HdIrMaterial`
reads the `UsdPreviewSurface` terminal of the material network (diffuseColor,
emissiveColor, specularColor, roughness, metallic, opacity, ior,
useSpecularWorkflow) and each mesh carries the material it is bound to.

Measured on a GeoSphere with a PreviewSurface of 0.9 / 0.2 / 0.2 lit by a
GeoSphereLight:

    render pass: 1 mesh(es), 1 instance(s), 1740 triangle(s), 1 light(s),
                 1 material(s), 640x480
    centre pixel red/green ratio 4.19   (the material is 0.9 / 0.2 = 4.5)

so both the light and the material are reaching the render.  A headlight is
still used when the scene has no lights at all, as the Nuke node does.

## Step five: textures and the other AOVs

`HdIrMesh` now reads the `st` primvar - vertex and varying st index by point,
faceVarying st splits a vertex per corner through
`ComputeTriangulatedFaceVaryingPrimvar`, the same split the USD front end does -
and `HdIrMaterial` follows the material network's relationships back from each
`UsdPreviewSurface` input to the `UsdUVTexture` driving it and loads the file
with the loader the Nuke node uses.  Measured with a ColorWheel feeding
diffuseColor:

    render pass: 1 mesh(es), 1 instance(s), 1740 triangle(s), 1 light(s),
                 1 material(s), 1 texture(s), 320x240

and the surface's hue changes from the flat material's 0.9/0.2 to the texture's,
so it is being sampled rather than ignored.  usd_hio is delay-loaded and fetched
from next to the running Nuke, as the plugin does.

Every bound AOV is now filled rather than only the colour: depth, the normal and
the two ids come from the same frame buffers the Nuke node writes to channels,
in whichever format Hydra asked for (float, vec3, vec4, half, unorm8, int32).

**What is left**:

1. Beta.  17.1 is Beta 4, and the schema-driven settings path ("auto apply
   schema") and the scene-index wiring may still move.

## Step three: instancing survives

A CopyToPointsUSD PointInstancer reaches the delegate as ONE prototype and a
transform per copy:

    CreateInstancer(/CopyToPointsUSD1/instancer)
    mesh /CopyToPointsUSD1/instancer/Prototypes/proto_0/ForInstancer<hash>/GeoSphere2:
         932 point(s), 1740 triangle(s), instancer=/CopyToPointsUSD1/instancer,
         instances=81
    render pass: 2 mesh(es), 82 instance(s), 1868 triangle(s), 640x480

1868 unique triangles for 82 copies, not 81 x 1740 - the entire point of this
renderer, now working through Hydra as well as through the Nuke node.  The
rendered image shows the scattered spheres (81 of 841 sampled points lit).

### The bit that cost the time

`HdRprim::GetInstancerId()` is filled in by **`_UpdateInstancer()`**, a protected
method the rprim has to call ITSELF during `Sync()`.  Miss it and everything
looks nearly right: Hydra creates the instancer (the delegate's
`CreateInstancer` is called), the prototype arrives with a path that says
`ForInstancer<hash>`, and yet `GetInstancerId()` is empty, so every
PointInstancer collapses to a single copy.  The two lines that fix it:

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());

## How to use it

Nuke builds its renderer list ONCE while it starts up, so the folder holding
`plugInfo.json` has to be on `PXR_PLUGINPATH_NAME` **before** Nuke runs -
registering the plugin from `init.py` or `menu.py` works headlessly but is
always too late in a GUI session.  Either set it permanently:

    setx PXR_PLUGINPATH_NAME "%USERPROFILE%\.nuke\InstanceRender\nuke17.1\hydra\hdInstanceRender\resources"

or launch through `~/.nuke/InstanceRender/hydra_launch.bat "<path to Nuke17.1.exe>"`.

Then pick **InstanceRender** in the Viewer's `renderer` menu or in a `GeoRender`
node.  Nuke 17.1 only: 16.1 and 17.0 have a `renderer` knob on the Viewer but it
is empty, and `GeoRender` does not exist there.

## Step six: AOVs and alpha through GeoRender

The colour arrived; the alpha did not.  It turned out not to be a delegate bug
at all - **Storm behaves identically**, which is what settled it:

    storm_legacy_rgb    channels ['rgba.blue', 'rgba.green', 'rgba.red']
    ours_legacy_rgb     channels ['rgba.blue', 'rgba.green', 'rgba.red']

### How GeoRender maps an AOV to channels

`GeoRender`'s **render variables table** is the whole mechanism.  It has no
Python getter or setter that sticks - `toScript()` returns `''` and
`fromScript()` is ignored - but it round-trips through *script parsing*, as rows
of `{aovName enabled channel}`.  Foundry's own `ToolSets/3D/Rendering/
GR_CustomAOVs.nk` is the worked example, and this is what its rows look like:

    render_variables_table { {color 1 rgb} {depth 1 depth} {Neye 1 {}} {primId 1 {}} }

The channel column names an **output plane**, and the AOV's components are laid
into that plane's channels **in order**.  That single rule explains everything
observed:

| row                    | result                                        |
|------------------------|-----------------------------------------------|
| `{color 1 rgb}`        | `rgba.red/green/blue` - components 0,1,2       |
| `{color 1 rgba}`       | `rgba.alpha` ONLY, holding the **red** value   |
| `{color 1 depth}`      | `depth.Z` holding red                          |
| `{normal 1 depth}`     | `depth.Z` holding the normal's x               |
| `{color 1 rgba.alpha}` | error - only a plane name parses, not a channel|
| `{color 1 beauty}`     | error - the layer has to exist already         |

So the four-channel colour AOV can never deliver coverage: mapped onto `rgba`,
Nuke resolves that plane to a single channel and writes red into it.

### What the delegate does about it

It offers a **one-component `alpha` AOV** (and `albedo` alongside it), so
coverage is component 0 of an AOV of its own and lands where it should:

    HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f))   // alpha
    rgba[0] = fb.rgba[p * 4 + 3];                            // filled from coverage

One catch: a hand-written table row only binds a name **Nuke already knows** -
the Hydra standard set (`color`, `depth`, `cameraDepth`, `normal`, `Neye`,
`primId`, `instanceId`, ...).  A custom name is silently dropped and the node
falls back to `color -> rgb`.  What registers a custom name is a
**GeoRenderVariable** whose `sourceName` matches, piped into `GeoRender`:

    GeoRenderVariable  prim_path /Render/Vars/alpha  sourceName alpha  dataType float
    GeoRender          render_variables_table { {color 1 rgb} {alpha 1 rgba} {depth 1 depth} }

which measures, on a unit sphere at 5 units:

    rgba.alpha = 1.0000 centre / 0.0000 corner
    depth.Z    = 5.0000 centre / 0.0000 corner
    rgba.red   = 0.2795 centre / 0.0000 corner

`test/gui_probe_alpha/menu.py` asserts exactly that, and
`nuke/ToolSets/InstanceRender_AOVs.nk` ships it as a paste-in recipe.

### Schema mode needs BOTH prim paths

With "Use Legacy Hydra API" off, `GeoRender` errors with

    No output AOVs are mapped to channels.  Assign at least one AOV to an
    output layer in the Outputs tab.
    No AOV is mapped to the output channel for this plane.

until it is pointed at **both** ends of the settings chain - which is what
Foundry's `SchemaGeoRender` does, and what was missing here:

    render_settings_prim_path /Render/Settings/main
    render_product_prim_path  /Render/Products/beauty

With only the settings path it still refuses.  Storm fails the same way, so this
is a wiring requirement, not a delegate gap.  Once both are set, schema mode
renders and the standard AOVs map; custom names (our `alpha`) do **not** bind
there yet, so legacy mode plus a GeoRenderVariable remains the route to
coverage.

### One divergence worth knowing

Our `depth` AOV writes **camera-space distance** (5.0 for a unit sphere with the
camera at z=6), which is Nuke's classic `depth.Z` convention.  Storm writes NDC
depth (0.98) into the same channel.  Ours is the more useful of the two, but it
is a deliberate difference, not a bug.

## Step seven: points and curves

`HdPoints` and `HdBasisCurves` now sync into the same `HdIrMeshData` a mesh
does, using the tessellation the Nuke node already had - `ir::buildUnitSphere`
and `ir::buildTube` - so a scene looks the same whichever front end it came in
through, and materials, textures, instancing and the AOVs need no special case.

A UsdGeomPoints prim becomes ONE sphere prototype and a transform per point,
which is the entire reason this renderer exists: a million particles cost a
million transforms, not a million spheres.  Per-point `displayColor` rides along
as a per-instance colour.  UsdGeomBasisCurves becomes a tube per curve, swept
along the evaluated basis, with bezier spans stepping by three.

    CreateRprim(points, /World/Particles)
    CreateRprim(basisCurves, /World/Curve)
    points /World/Particles: 3 -> 3 instance(s)
    curves /World/Curve: 1 -> 1 instance(s)
    render pass: 2 mesh(es), 4 instance(s), 264 triangle(s)

264 unique triangles for the whole scene.  `test/gui_probe_points/menu.py`
renders `test/data/points_curves.usda` and checks the three particles come out
red, green and blue left to right, all the same size, above a yellow bar.

How finely both are tessellated are render settings - **Point detail**, **Curve
sides** and **Curve segments** - so they appear in `GeoRender`'s renderer
settings table, defaulting to what the Nuke node defaults to.

### Two things about GeoImport, which cost the time here

Neither is a Hydra question, and both are easy to mistake for a broken delegate,
because the symptom is a render index holding **zero** rprims:

1. **GeoImport contributes nothing to the Nuke 3D scene until prims are selected
   in its scenegraph.**  With the default `selection 0` even ScanlineRender2
   renders an empty frame.  `nukescripts.create` pops that scenegraph up the
   moment the node is made, so a human never sees this; a script has to say so
   itself.  The knob serialises as

       usd_scenegraph { scg3dmodel.1 selection 3 /World /World/Particles /World/Curve folded 0 ... }

   The InstanceRender **node** is unaffected - it reads the composed stage
   itself rather than the imported scene, which is why its headless tests pass
   against files that render as nothing through `GeoRender`.

2. **The file has to be set as the node is created.**  Script parsing or
   `nuke.createNode("GeoImport", "file {...}")` both work; `nuke.nodes.
   GeoImport()` followed by `knob("file").setValue(...)` imports nothing, no
   matter what the selection is afterwards or whether `reload` is executed.

## Step eight: the GPU, and the rest of the AOVs

**Both back-ends.**  The delegate now builds with `OptixBackend.cpp` and the
same embedded PTX the node uses, and picks between them from the **Use GPU**
render setting, falling back to Embree when the machine has no device or the
OptiX build fails:

    rendered on the GPU: OptiX: 2 GAS, 264 unique triangle(s), 4 instance(s)
    in 1 IAS = 0.000504M triangles rendered, build 169 ms; launch 120 ms

Rendering the same scene with the setting on and off gives a worst per-channel
difference of **0.00000** - they run the same kernel, so the choice changes the
speed and nothing else.  (`test/gui_probe_gpu` checks the delegate log really
does say GPU for one of the two renders; a parity claim measured against two CPU
renders would be worth nothing.)

**The render settings are real knobs.**  Everything
`GetRenderSettingDescriptors()` offers turns up on the `GeoRender` node and can
be set from a script - which is more than the render variables table manages:

    knobs the renderer added: ['samples', 'maxBounces', 'useGpu',
                               'pointDetail', 'curveSides', 'curveSegments']

**The packed AOVs.**  Position, motion, st, the material and object ids and the
five lighting layers live in one interleaved buffer whose layout is decided per
render from the bindings, so a render that asks for none pays for none.  Each is
offered under its own name, and needs a `GeoRenderVariable` to register it, the
same as `alpha`.  Measured on a unit sphere with the camera 6 units back:

| AOV                | centre pixel        | what it should be         |
|--------------------|---------------------|---------------------------|
| `position`         | (0.0008, 0.0011, 1) | the near pole of the unit sphere |
| `color`            | 0.2786              | the beauty                |
| `directDiffuse`    | 0.2428              |                           |
| `directSpecular`   | 0.0357              | the headlight's specular lobe |
| `indirectDiffuse`  | 0                   | nothing to bounce off     |
| `indirectSpecular` | 0                   |                           |
| `emission`         | 0                   |                           |
| **sum of the five**| **0.2785**          | **the beauty, 0.2786**    |
| `uv`               | (0.0001, 0.5005)    | st on the sphere          |
| `objectId`         | 0 on it, -1 off it  | the only object           |
| `motion`           | 0                   | nothing moves             |

The five lighting layers adding back up to the beauty is the check worth having:
it catches a layer written into the wrong slot of the packed buffer, which
nothing else here would notice.

## Step nine: making it look like the node

Reported from use: the shading was faceted, and the look did not match the
InstanceRender node.  Three separate causes, each measured by rendering the same
sphere through the node, through the delegate and through ScanlineRender2:

### Faceted shading: Nuke authors normals faceVarying

    primvars on /GeoSphere1 (932 points, scheme none):
        points[vertex:932] normals[faceVarying:3540] st[faceVarying:3540]

The mesh sync only accepted normals when there was exactly one per point, so
3540 normals on a 932-point mesh were dropped and every triangle shaded flat -
17 distinct values across the sphere where the node had 31.  Normals now get
exactly the treatment st already had: read at whatever interpolation they were
authored, triangulated with `ComputeTriangulatedFaceVaryingPrimvar`, and sharing
one corner split with st so the two stay on the same vertices.

Worth noting what this cost beyond the silhouette: the *specular* was wrong too,
0.0357 against the node's 0.0505, because a facet normal is tilted a few degrees
off the view direction and a highlight falls away fast.  Diffuse barely noticed.

### Too bright: the fallback surface is 0.18, not 0.8

An unbound mesh shades with `displayColor`, and the delegate defaulted it to
0.8.  Hydra's fallback surface - and `UsdPreviewSurface`'s own default
`diffuseColor` - is **0.18** grey, which is what the node reports through its
albedo AOV:

    node albedo 0.1801, direct_diffuse 0.0550 + direct_specular 0.0505 = 0.1055
    delegate (before)  direct_diffuse 0.2428 + direct_specular 0.0357 = 0.2786

4.44x, exactly 0.8/0.18.

### The framing: Nuke gives Hydra a square filmback

    hdCamera: focal 5.0 hAperture 2.4576 vAperture 2.4576 windowPolicy 2
              framingValid 1 pixelAspect 1.0 display 2048x1556

The vertical aperture is a **copy of the horizontal**, so the camera's filmback
is square and the window policy widens it to the image.  The projection that
comes back says `tanHalfFovX 0.3235, tanHalfFovY 0.2458` - the vertical field of
view is the *horizontal* aperture, and the horizontal is that again times the
image aspect.  Nothing else in Nuke frames a shot that way: ScanlineRender2 and
the node both take the horizontal aperture for the horizontal field of view and
derive the vertical from the image, giving `0.2458, 0.1867`.  The same camera
therefore framed **1.32x wider** through GeoRender than through ScanlineRender2.

So when the camera reports a square filmback, the delegate takes its aperture as
the horizontal one and derives the vertical from the image and the pixel aspect.
A camera that reports a genuinely different vertical aperture is left alone and
its projection is used as given.

### The result

    node       44 lit samples, 31 distinct values
    delegate   44 lit samples, 31 distinct values
    worst difference across the slice: 0.0000
    node/delegate/scanline sphere spans x 0.1562..0.8398 (0.6836 wide)

`test/gui_probe_look/menu.py` requires all three renderers to agree, on the
profile, on the number of distinct shading values (faceting collapses it) and on
the framing.

## Step ten: dome lights

Reported: the delegate ignores `GeoDomeLight`.  It did.  The light itself was
mapped - type, orientation, intensity - but its image never was:

    L.texture = -1;      // the HDRI itself is not read yet

so a dome lit the scene with a flat colour, whatever HDRI was hanging off it.

Fixing it needed the image to be loaded where the scene's textures live, which
is the render pass, not the light's `Sync()` - Hydra syncs prims from whichever
thread it likes, and the texel array belongs to the scene being assembled.  So
`HdIrLight` now reads the `textureFile` parameter and records the path, and the
render pass loads it, appends it to the scene's textures, and builds the
importance-sampling distribution.

That distribution moved into `src/ir/Dome.h`, shared with `StageLoader`, so a
dome light is sampled the same way whichever front end built the scene - a
small bright sun in an HDRI is pure noise without it.

Decoded images are cached by path: Hydra rebuilds the scene on every render and
a 4k HDRI is far too slow to read each time.

### The authored path, not the resolved one

    dome asset: authored 'nkop:/NkRoot/Read1:1:main:...nkiop'
                resolved '/NkRoot/Read1:1:main:...nkiop'

A `GeoDomeLight` in Nuke does not point at a file.  Its texture is an **`nkop:`**
path naming the node feeding it, which Nuke's own Hio plugin resolves from
inside the process.  Resolving that asset path strips the scheme, and what is
left reads as a 1x1 image - so the **authored** path is what gets used, with the
resolved one only as a fallback.

### What still does not work, for either renderer

Even with the scheme intact, an `nkop:` path read out here hands back a 1x1
image - to the InstanceRender node exactly as much as to the delegate.  Both
then light from a flat dome, and they agree to four decimals doing it, so this
is not a difference between them.  Pointed at a file on disk both read the image
and light from it, and agree to four decimals again:

    node       left (0.2627, 0.0000, 0.5459)  right (0.6270, 0.0000, 0.2573)
    delegate   left (0.2627, 0.0000, 0.5459)  right (0.6270, 0.0000, 0.2573)

`test/gui_probe_dome/menu.py` checks that the two sides of a sphere under a
split lat-long come out different colours - a flat dome cannot produce that -
and that the delegate matches the node.  `IR_PROBE_DOME_NKOP=1` runs the same
scene the way Nuke wires it, which is how the 1x1 was found.

### A note for the delegate: textures fed by Nuke nodes

Reported separately, and worth recording here because it limits what a delegate
can do at all: a texture wired from a Nuke node reaches a renderer as an
`nkop:` path naming the op, and reading one only works while Nuke is holding a
texture image for it.  Foundry's own plugin says what happens otherwise:

    TextureIopInterface::ctor nodepath='/NkRoot/Read1:1:main:...' hasTextureImage=0, w=0, h=0
    ImageInterface::create(): error, image is either zero-sized or has no channels
    to read. Delete interface so will fallback to producing a default 1x1 grey texture.

(`USG_DEBUG_TEXTURE_ERRORS=1` and `USG_DEBUG_TEXTURE_HANDLING=1` turn those on -
they are the fastest way to find out why a texture came back grey.)

The **node** works around it by rendering the op itself.  The **delegate** can
do exactly the same, which was worth correcting: it runs inside Nuke, and the
NDK has a sanctioned way in.

    MaterialOpI::retrieveOpFromAssetPath(path, &outputContext)

parses the path - node, frame, view, proxy, hash - and returns that Op at that
context.  Foundry's own header even names the failure this causes when the
context is wrong: "anomalous texture behavior in Hydra and ScanlineRender".  So
both front ends now share `src/ir/NukeOpImage.cpp`, and a dome light or a
texture wired from a Nuke node works whichever renders it.  The delegate links
DDImage for it, which costs nothing: it only ever runs inside Nuke.

### And two things the delegate was getting wrong about textures

Measured against the node on a flat 0.5 texture, the delegate was **2.94x too
bright**.  Two causes, both in how a `UsdUVTexture` is meant to be read:

* **`scale` and `bias` were ignored.**  Nuke carries the shader's own colour in
  the texture's scale - a PreviewSurface with `diffuseColor 0.18` and a texture
  authors `inputs:scale = (0.18, 0.18, 0.18, 1)`.  (That attribute is also the
  one in the "Can't set time sample ... expected a value of type TfToken" bug.)
* **A bound diffuse texture must REPLACE the constant**, not multiply with it -
  the constant is already in the scale.  Applied after the parameters are read,
  because Nuke authors the knob value as well as the connection.

    flat 0.50 texture   node 0.0692  delegate 0.0781  ratio 1.13  (was 2.94)
    flat 0.25 texture   node 0.0599  delegate 0.0643  ratio 1.07  (was 2.12)
    a checkerboard      worst difference 0.0167              (was 0.2672)

`test/gui_probe_nuketex` renders a CheckerBoard through a PreviewSurface both
ways and requires the pattern to be there and the two to agree.  The ~10% that
remains on a flat texture is mip filtering, and is inside the tolerance.

## Step eleven: 17.1v1 Beta 4 -> 17.1v1 release

The release moves things the beta had, and two of the traps written up above are
gone.  Worth reading before trusting anything earlier in this document.

### The knob rename that hangs a render

`GeoRender`'s **`render_settings_legacy`** (a Boolean, true = the old path) is
now **`render_settings_schema`** (false = the same path).  That matters more
than a rename should, because a `GeoRender` carrying the STALE knob - any script
saved from the beta - **hangs the render**.  Not an error, not a warning: the
execute never returns, with Storm exactly as much as with this renderer.  The
delegate is created and then never asked for anything, which looks for all the
world like a delegate that cannot render.

    beta     GeoRender { ... render_settings_legacy true ... }   -> hangs in 17.1v1
    release  GeoRender { ... }  then render_settings_schema=0    -> renders in 0.6s

If a frame will not render, that is the first thing to check.

### The colour AOV carries its alpha now

On the beta, `{color 1 rgba}` wrote the colour's RED into `rgba.alpha`, and
coverage had to come from a one-component AOV of our own.  The release maps it
properly:

    {color 1 rgb}    ->  rgb = colour
    {color 1 rgba}   ->  rgb = colour AND alpha = coverage
    {alpha 1 rgba}   ->  the one-component AOV lands in RED (components in order)

So the recipe is now simply `{color 1 rgba}`, and the ToolSet ships that.  The
delegate still offers its `alpha` AOV: it costs nothing and a beta needs it.

### The camera is fixed

The beta handed Hydra a square filmback - `hAperture 2.4576, vAperture 2.4576` -
which framed 1.32x wider than ScanlineRender2.  The release reports
`vAperture 1.8672`, and the projection that comes back is right:

    beta      tanHalfFovX 0.3235  tanHalfFovY 0.2458
    release   tanHalfFovX 0.2458  tanHalfFovY 0.1867

Our workaround only fires when the filmback is square, so it now stays dormant
and the framing still matches the node to the pixel (`test/gui_probe_look`).

### And schema mode is less strict

With only `render_settings_prim_path` set and no product prim, the beta refused
with "No AOV is mapped to the output channel for this plane".  The release
renders.

Not fixed in the release: motion blur through `GeoRender`, and the `nkop:`
texture reads (both front ends here fetch the op themselves - see the note
above).  From the release notes, **ID 618533** is still open: "PreviewSurface
format is incorrect on all but diffuse input".

### Motion blur through GeoRender: not available to any delegate

The node blurs by reading the scene twice, at shutter open and close.  A
delegate cannot do that - it only ever sees the frame Nuke set - so it depends
on Hydra handing over time samples, and Nuke hands over none:

    shutter: open 0.000000 close 0.000000
    transform samples for /GeoSphere1: default 1 t=0;  interval [-0.5,0.5] 1 t=0

That is a prim animated across the frame, asked both ways - the plain
`SampleTransform` and the overload that names an explicit interval.  One sample,
at the current time, either way.  `GeoRender` has no shutter or motion knob of
its own either.  So motion blur through `GeoRender` is blocked on Nuke, for this
renderer and for any other; the InstanceRender **node** blurs, and does
deformation blur as well.  `test/gui_probe_mblur` is the measurement.

## Step twelve: subdivision and denoising in the delegate

**Catmull-Clark** now runs in the delegate, through the same `ir::subdivide()`
the node uses, so a mesh limits to the same surface either way - a cube's
silhouette goes 0.5800 -> 0.4500 wide at two levels, node and delegate agreeing
to 0.0000.  The Hydra side reads the polygon topology and its crease tags off
`HdMeshTopology` and refines BEFORE anything is triangulated or split, then
hands back one vertex per refined corner with normals from the refined mesh.
Meshes authored `scheme none` are left alone, as USD asks - which is every
primitive Nuke makes, so a GeoCube stays a cube.

**Denoising** is the OptiX AI denoiser as a post-process (`src/ir/Denoise.cpp`),
guided by the albedo and normal passes.  It is deliberately not tied to the GPU
renderer: it works on host buffers, so a CPU render is denoised the same way as
long as there is a CUDA device.  Both front ends have it - a **Denoise** render
setting on `GeoRender`, a **denoise** knob on the node:

    2 samples, no denoise    neighbour noise 0.57278
    2 samples, denoised      neighbour noise 0.12060   (256 samples raw: 0.14987)
    the same on the GPU      neighbour noise 0.12065

The alpha is carried through untouched (`OPTIX_DENOISER_ALPHA_MODE_COPY`, and
the original alpha is restored afterwards) because coverage is not noise.
