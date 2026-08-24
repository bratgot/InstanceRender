# InstanceRender

A renderer node for Nuke, **14.1 through 17.1**, that **keeps instancing**:
`UsdGeomPointInstancer` copies and `instanceable` prims share one prototype in
the acceleration structure, so 5000 copies of an 80k-triangle mesh cost one BVH +
5000 transforms instead of 400M triangles.  Classic 3D geometry is read the same
way - objects that carry the same mesh become one prototype and a transform each.

* **CPU**: Embree 4, two-level BVH (one scene per prototype + instance geometries).
* **GPU**: OptiX (one GAS per prototype + an IAS of instances) - optional build.
* Both devices run the **same shading kernel** (`src/ir/Kernel.h`): path tracing
  with next-event estimation, UsdPreviewSurface-style BSDF (lambert + GGX).
* No extra nodes: materials are the stage's `UsdPreviewSurface`s, lights the
  stage's `UsdLux` lights, camera a Camera input or a `UsdGeomCamera` prim.
* **Lights**: distant, sphere / point, rect (with its texture), disk, cylinder
  and dome, plus `ShapingAPI` cones (spot lights) and focus, `ShadowAPI`
  enable / colour, and the `diffuse` / `specular` multipliers.  Area lights are
  drawn where they are for camera rays.
* **Textures**: the `UsdUVTexture` maps feeding `diffuseColor`, `emissiveColor`,
  `roughness`, `metallic`, `opacity` and `normal` (any format Nuke can read -
  exr, png, tif, jpg, hdr), with `wrapS/T`, `scale`, `bias`, `sourceColorSpace`
  and per-channel outputs (one ORM map can drive three inputs), `UsdTransform2d`
  on the st input, `<UDIM>` tile sets, and mip filtering from the ray footprint.
* **Dome lights**: the lat-long HDRI is read and **importance sampled** (a 2D
  cdf over the image, MIS against the BSDF), so a small bright sun converges
  instead of speckling.
* **Progressive refinement** in the viewer: a fast preview pass that keeps
  refining to the full sample count while you look at it.
* **Motion blur** on instance transforms, through the acceleration structures
  themselves (Embree instance time steps / OptiX matrix motion), so blurred
  copies stay instanced.
* **Subdivision surfaces**: Catmull-Clark (and bilinear) refinement of meshes
  whose `subdivisionScheme` says so, with semi-sharp creases and pinned
  corners; off by default.
* **Particles**: Nuke's own particles are classic 3D and cannot reach a USD
  renderer at all - they will not connect to a `GeoScene`.  They render here,
  as one sphere prototype with a transform per particle, blurred by matching
  particles to themselves across the shutter by id.
* **Two front ends**: Nuke's USD 3D system (16.0+), and its classic 3D system
  (`Card`, `Sphere`, `Cube`, `ReadGeo`, the classic `CopyToPoints`) in every
  version - which is what Nuke 14.1 and 15.x render through.  See
  [Which Nuke](#which-nuke).
* **Points and curves**: `UsdGeomPoints` renders as spheres that all share one
  prototype (a transform per particle, not a sphere per particle) and
  `UsdGeomBasisCurves` as tubes swept along the evaluated basis.
* **Per-instance overrides**: `displayColor` and `displayOpacity` per instance,
  `invisibleIds`, and a material bound on an `instanceable` prim (which cannot
  reach inside the shared prototype, so the renderer applies it per instance).
* **AOVs**: `rgba`, `depth.Z`, `N`, `instance.id`, `albedo`, `P`, `st`,
  `forward` (motion vectors), `material.id`, `object.id`, ambient occlusion,
  shadow, light groups, and a light split into `direct_diffuse`,
  `indirect_diffuse`, `direct_specular`, `indirect_specular` and `emission` that
  adds back up to the beauty exactly.
* **Cryptomatte** with a manifest in the EXR header, per object, per material
  and **per copy** - so a single instance out of thousands can be picked.
* **Deep** output, including volumes as depth segments.
* **Volumes**: OpenVDB grids through `VolumeToUSD` - absorption, single and
  multiple scattering, self-shadowing, shadows cast onto geometry, dome lighting,
  two emissive grids, sequences, and motion blur by cross-fading the shutter-close
  frame.  Emission can be read as a **blackbody temperature in Kelvin** rather
  than as brightness, which is what a simulation's `temperature` grid actually
  holds.  See [Volumes](#volumes).
* **Analytic gprims**: `Sphere`, `Cube`, `Cylinder`, `Cone` and `Capsule` are
  tessellated at load, standalone or as instancer prototypes.
* **IES profiles**: `shaping:ies:file` is read as IESNA LM-63 and sampled per
  direction, normalised so the profile picks the SHAPE and the light's intensity
  picks the brightness.
* **`UsdPreviewSurface`** in full bar a displacement map: clearcoat,
  occlusion, opacityThreshold and a constant displacement that moves vertices at
  load, so it changes the silhouette and the shadow rather than only the shading.

The `format` knob decides what size to render.  Left alone the node renders at
the `bg` input's format, or the project format when nothing is connected there.

Inputs: `scn` (any GeomOp / stage), `cam` (optional Camera), `bg` (optional
background image, sets the format).

## Which Nuke

One build per Nuke **minor** version - the NDK is not compatible across 16.0 and
16.1, let alone 14 and 17 - and what each one can read differs:

| Nuke | USD scenes (GeoImport, GeoScene ...) | classic 3D (Card, Sphere, ReadGeo ...) |
| --- | --- | --- |
| 14.1 | no | yes |
| 15.x | no | yes |
| 16.0 | yes | yes |
| 16.1 | yes | yes |
| 17.0 | yes | yes |
| 17.1 | yes | yes |

Nuke 14.1 ships the USD libraries but no headers for them, so no plugin can
speak USD there at all.  Nuke 15 has the headers, but the only route to a
composed stage is the static `GeomOp::buildStage()`, and calling that from a
plugin **segfaults** unless Nuke's own ScanlineRender2 has already rendered the
same graph once - so 15 gets the classic front end rather than a crash.  The
supported entry point, `GeometryProviderI::getGeometryStage()`, arrives in 16.0.
Connect a USD scene to a 14.1 or 15.x build and the node says so plainly instead
of failing.

Everything else - the kernel, both devices, every AOV, motion blur, textures,
instancing - is the same code in every version.

## Install / load

    .\build.ps1 -Gpu -Install        # builds and installs into ~/.nuke

That copies `~/.nuke/InstanceRender/{init.py, menu.py, nuke<major>/InstanceRender.dll + embree4.dll}`
and appends an idempotent block to `~/.nuke/init.py`:

    # --- InstanceRender (auto-added by cmake --install) ---
    import nuke
    nuke.pluginAddPath('./InstanceRender')
    # --- end InstanceRender ---

Restart Nuke: the node is under **3D > InstanceRender** (or Tab > InstanceRender).
Without installing, add the build directory by hand:
`nuke.pluginAddPath(r"C:/dev/NukeInstanceRender/build17/Release")`.

## Getting real instancing

The loader instances what the *stage* says is instanced: a `UsdGeomPointInstancer`,
or prims marked `instanceable`. With CopyToPointsUSD that means **mode = instances
(PointInstancer)** or **copies + "copies share geometry (instanceable)"**. Plain
`copies` mode authors N independent prims, so the renderer sees N meshes (correct,
but no sharing).

## Lights

Every `UsdLux` shape is sampled as the shape it is - a disk as a disk, a
cylinder as a cylinder, a sphere over the hemisphere that faces the shaded
point - and `normalize` divides the emission by that area, so resizing a light
keeps its power.  `ShapingAPI` gives cones (a spot light, with `softness` as a
smooth edge) and the `focus` exponent with its tint.  `ShadowAPI`'s
`shadow:enable` and `shadow:color` are honoured, as are the per-light `diffuse`
and `specular` multipliers.

`area lights visible to camera` (on by default) draws rect / disk / sphere
lights where they actually are when a camera ray hits one.  Bounced rays always
find lights through next event estimation, so nothing is counted twice.

IES photometric profiles ARE supported: `shaping:ies:file` is read as IESNA
LM-63, resampled onto a fixed angular table and looked up per direction. The
table is normalised to its own peak, so the profile decides the SHAPE and the
light's intensity decides the brightness - a file carrying absolute candela in
the thousands would otherwise blow out every light that had one. A file with a
TILT table is reported rather than guessed at. Note that Nuke's own light nodes
expose no ies knob, so a profile arrives through an imported USD stage.

Not supported: light filters and portals. Light filters are not a thing this
could read even in principle - core UsdLux's `LightFilter` is an abstract base
with no filtering semantics of its own, every real filter (barn doors, gobos,
blockers) is a renderer-specific schema, and no Nuke node authors one.

## Volumes

A `.vdb` reaches this node as a USD `Volume` prim.  Nuke's own field graph does
not author one - `GeoFieldMesh` gives an isosurface and `GeoFieldSet` leaves the
stage unchanged - so `VolumeToUSD` (in the CopyToPoints repository) sits between
a `FieldVolume` and the `GeoScene` and writes the prim.

Knobs on this node: `volume steps` and `volume shadow steps` (the march), `volume
albedo` (how much a volume scatters rather than swallows - 1 is smoke, low is
soot), `volume octaves` (multiple scattering, each octave seeing the volume as
half as thick for no extra marching), `volume motion blur` and `volume deep
segments`.

### Emission is a temperature, not a brightness

A simulation's `temperature` grid holds **Kelvin**.  Measured on an aerial
explosion: density peaks at 0.89 and flames at 7.3, but temperature peaks at
**8336** with a mean of 1059.  Multiplying that in as radiance is why an
emission scale of 1 once put 46361 in the viewer.

So each emissive slot on `VolumeToUSD` has a **read as** mode:

* **blackbody** - the value is a temperature.  Planck's law is evaluated at it
  and normalised to the Wien peak, so the temperature picks the COLOUR and the
  scale knob alone picks the brightness.  This is what Karma, Arnold and Cycles
  do, and without the normalisation Planck is unusable: its absolute radiance at
  3000 K is around 1e12.
* **intensity** - the value IS the brightness, which is right for a `flames` or
  `fuel` grid.

`temperature` defaults to blackbody and `emission` to intensity, because that is
what those grids usually are.  A grid normalised to 0..1 needs a **K min / K
max** range to be stretched into; left at zero it is taken as Kelvin already, and
a 0..1 grid then falls below the table's floor and comes out as one flat colour -
which the node says out loud in the render report rather than leaving you to
wonder.

`spectral blackbody` on this node chooses how the colour is worked out:
integrating Planck against the CIE colour matching functions (on, the default,
and the deeper red) or sampling one wavelength per channel.  It is blackbody
COLOUR, not spectral light transport.

Volumes render through the **Hydra delegate** as well as the node.

## Progressive refinement

`progressive (viewer)` renders `preview samples` first (default 2) and then keeps
adding passes - each one doubling - until it reaches `samples`, redrawing after
every pass.  Accumulation is exact: the passes cover sample indices
`[0, samples)` between them, so the finished image is the one the node would
have produced in a single pass.  `depth.Z`, `N`, `instance.id`, `albedo`, `P`,
`st`, the ids and the motion vectors come from the first pass and are never
blended - they describe one surface, and an averaged id means nothing.  The five
light layers are averages like the beauty and keep refining with it.

It only runs in the GUI, and never while a Write is executing - `nuke/menu.py`
registers before/after-render callbacks that set `IR_EXECUTING`, and the node
falls back to one full-quality pass while that is set.  Terminal renders
(`nuke -x`, `-t`) ignore progressive entirely.

## Motion blur

`shutter` (frames, 0 = off), `shutter offset` (centred / start / end) and
`motion samples`.  Each instance carries one transform per motion sample across
the shutter; the BVH interpolates between them (Embree instance time steps,
OptiX matrix motion transforms) and the shading kernel interpolates the same
way, so the two devices agree and 5000 blurred copies still cost one prototype.

`motion samples` is 2 by default - one straight segment, which is all a linear
move needs.  Anything spinning or arcing needs more: the stage's own time
samples sit on whole frames, so a sub-frame read of them just interpolates the
neighbouring frames, and the node therefore **builds the stage again at each
key time** to ask the graph for that sub-frame.  That is one stage build per
motion sample, which is why it is opt-in.

To get the second transform the node builds the stage a second time at the
shutter-close frame - Nuke only authors time samples once the graph has been
asked for another frame, so this also makes the *first* render match every one
after it.  `deformation blur` also interpolates the vertices themselves, for geometry that
changes shape rather than just moving.  It reads the whole scene a second time
at shutter close and needs the vertex count to match at both ends, so it is off
by default and always two keys.  A
scene whose instance count changes across the shutter renders without blur and
says so.  It composes with the rest: progressive refinement accumulates blurred
passes to the same image a single pass would give, and subdivided meshes blur
too.

### Particles

Nuke's particle geometry reaches a renderer carrying `id`, `Cf` and `size` and
no velocity - the velocity never leaves the particle SYSTEM - so the loader asks
the system directly and matches it to the points by `id`.  Nothing to switch on.

A reported velocity is not trusted blindly: a bare `ParticleEmitter` hands out
unit-length velocities to particles that never move, and blurring by that
invents motion.  The velocity is compared against pairing where both have an
opinion and preferred only when the two roughly agree; where they disagree it is
still used for whatever pairing could not pair, because there the alternative is
no answer rather than a worse one.

`CopyToPoints` skips the problem entirely by writing each particle's velocity
onto its copy as an object attribute, always - so every copy blurs along its own
velocity, pairs nothing, and the particles born and dying inside the shutter
blur correctly too.

## Subdivision

`subdivision levels` (default 0) refines meshes whose `subdivisionScheme` is
`catmullClark` - USD's fallback value, so most authored meshes - or `bilinear`.
It is off by default deliberately: turning it on changes the look of every
existing scene and multiplies its triangle count by four per level.

The refinement runs on the polygon cage before the renderer splits vertices, so
uv seams stay seams (each face refines its own corners, linearly).  Authored
normals are dropped, as the USD spec says they only apply to scheme `none`, and
smooth normals are computed from the refined mesh.  `creaseIndices` /
`creaseSharpnesses` and `cornerIndices` / `cornerSharpnesses` are honoured,
including fractional sharpness (which loses one level per refinement); `loop`
meshes render as their control cage.

Nuke ships `pxOsd` but not the OpenSubdiv headers it includes, so the
subdivider is our own (`src/ir/Subdivide.h`).

## Points and curves

`UsdGeomPoints` becomes one sphere prototype plus an instance per point, so a
million particles cost a million transforms rather than a million spheres -
which is what the rest of this renderer is built for.  Per-point `widths`,
`displayColor` and `ids` are honoured (the ids come out in `instance.id`), and
`point detail` decides how round each one is drawn.

`UsdGeomBasisCurves` becomes a tube per curve, swept along the evaluated basis -
linear, bspline, catmullRom and bezier - with `curve segments` samples per span
and `curve sides` around.  Widths interpolate along the curve, and the tube
carries uvs (u around, v along) so hair can be textured.

Particles authored in Nuke still reach the renderer the other way too, through
**CopyToPointsUSD**, which turns them into a PointInstancer.

## AOVs

Everything except `rgba` is optional and costs nothing when it is off: the passes
that were asked for are packed into one record per pixel, so a render that only
wants `P` carries three floats a pixel, not twenty-four.

| knob | channels | what it is |
| --- | --- | --- |
| `depth.Z` | `depth.Z` | camera-space distance |
| `N (normal)` | `N.x/y/z` | world-space shading normal |
| `instance.id` | `instance.id` | which copy: PointInstancer ids where the stage has them |
| `albedo` | `albedo.red/green/blue` | the surface colour before any lighting |
| `P (world position)` | `P.x/y/z` | where the surface is in world space |
| `st (texture coordinates)` | `st.u/st.v` | the surface's uvs (Nuke reserves the layer name `uv` for geometry attributes, so this goes out under USD's own name) |
| `forward (motion vectors)` | `forward.u/forward.v` | screen-space motion, see below |
| `material.id` | `material.id` | which material |
| `object.id` | `object.id` | which prototype: every copy of one mesh shares it |
| `lighting` | `direct_diffuse`, `indirect_diffuse`, `direct_specular`, `indirect_specular`, `emission` | the beauty, split |

The light split is exact: the five layers add back up to `rgb` to the last bit of
a 32-bit float, which the test suite checks pixel by pixel.  Light reflected by
the first surface the camera sees is *direct*, split by the lobe that reflected
it; everything the path picks up afterwards is *indirect*, filed under the lobe
the path left that first surface on.  Emission covers surfaces that glow, lights
drawn where they are, and the visible background.

### Motion vectors

`forward.u` and `forward.v` are the channels ScanlineRender writes and VectorBlur
reads, and they carry the same numbers: with the same move, the same camera and
the same shutter, this node and ScanlineRender agree exactly (the test suite
renders both and compares).  Two conventions, as there:

* **per frame** - pixels the surface travels in one frame (ScanlineRender's
  `velocity`).
* **shutter** - half the travel across the open shutter (its `distance`, which is
  what VectorBlur expects by default).

With the shutter shut the render stays **sharp** and the vectors are still
measured, over the frame that follows - which is the workflow most comps want:
render sharp, blur in the comp.  A moving camera counts too: both ends of the
interval are projected through the camera of their own moment.

## Per-instance overrides

A `UsdGeomPointInstancer` can vary its instances with the instance-rate primvars
`displayColor` and `displayOpacity`, and hide them with `invisibleIds`.  Native
(`instanceable`) copies read the same two primvars from the instance prim, plus
its material binding: USD keeps a binding authored on an instanceable prim
outside the shared prototype, so the renderer applies it to that instance's
triangles.  It is applied only when the prototype's own meshes bind nothing of
their own - an instance-level binding covers the whole instance, so it must not
silently replace per-mesh materials inside the prototype.

Note that a native instance shares the subtree *below* the instanceable prim: the
mesh has to be a child of the referenced prim, not the referenced prim itself.
The node warns when a prototype ends up with no geometry.

## Textures

`max texture size` (default 4096) box-downsamples anything larger at load: images
are kept as float RGBA and filtered by the shading kernel itself, because a
hardware `cudaTextureObject_t` filters at reduced precision and would break the
CPU/GPU parity this renderer is built around.  An 8K HDRI is ~500 MB as float
RGBA, 4K is ~134 MB.

faceVarying UVs (what every DCC writes, so that seams can split) are honoured:
the loader splits vertices per distinct corner value instead of averaging them,
which would smear the texture across every seam.

`<UDIM>` paths load every tile that exists on disk (1001..1100) and the tile is
picked from `floor(u), floor(v)`.  `UsdTransform2d` between the primvar reader
and the texture is applied as scale, then rotation, then translation.

`mip filtering` (on by default) builds a pyramid per texture and picks the level
from the ray's cone footprint - the pixel's angular size carried down the ray,
widened by rough bounces - so textures running into the distance stop
shimmering.  It costs about a third more texture memory.

## Classic 3D

The classic system has no notion of instancing: every copy of a mesh is its own
`GeoInfo` carrying its own points.  So the loader looks at what the objects
actually **contain** - objects that share a geometry cache (which is how the
classic `CopyToPoints` makes its copies) or that hold the same mesh in a
different place collapse to one prototype with a transform each, and a hash hit
is verified point by point so a collision costs a comparison rather than the
wrong geometry.

* `uv`, `N` and `Cf` are read from points, vertices or the object, with
  face-varying attributes splitting vertices exactly as the USD path does.
* Each object's material is an Iop, so it is **baked into a texture** and
  sampled with the geometry's uvs.  Shaders that light the surface themselves
  (Phong and friends) pass their input image through, so the texture still comes
  out right - their lighting model does not, and this renderer's own takes over.
* **Particles** (`ParticleEmitter` and anything downstream of it) arrive as a
  primitive with vertices but no faces, carrying `size`, `Cf` and `id` per
  point.  They become one sphere prototype and a transform each - so a million
  particles cost a million transforms, not a million spheres.  A sprite is
  really a camera-facing card; drawing it as a sphere is the approximation a
  path tracer wants, and it keeps them instanced.  `point detail` decides how
  round they are.
* Motion blur pairs each object with itself at the other end of the shutter,
  which classic particles make hard: emitted copies carry no id, and where ids
  exist Nuke recycles them.  Two pairings are built - by key, and by where the
  objects actually are - and whichever explains the motion more coherently wins,
  so there is no threshold to tune.  Where a **velocity** exists there is
  nothing to pair at all and it is used instead; a `UsdGeomPointInstancer` with
  authored `velocities` blurs by extrapolating every key from the frame being
  rendered, which is the only thing that works when the particle count changes
  across the shutter.  USD velocities are units per **second**: 48 at 24 fps is
  2 units a frame.  Nuke's classic particles do not offer one, so those scenes
  get the pairing.  `motion outlier rejection` is the one knob over it.
* Point, spot and directional lights are read from the scene (or found by
  walking the input tree).  Note that this renderer always falls off with the
  square of the distance, whether or not Nuke's `falloff` knob is on, so
  intensities usually want raising compared with ScanlineRender.
* Motion blur works the same way as on the USD side: the object list is read
  again at shutter close, and matching objects get their two transforms.
* **Nuke's classic shaders** - Phong, BasicMaterial, Diffuse, Specular,
  Emission - are read for what they mean, not run.  Their 2D output is only the
  image they carry; the shading itself happens inside ScanlineRender, which a
  path tracer cannot host.  So the loader takes their knobs across: `color` and
  `diffuse` become the albedo (and tint any texture the material carries),
  `emission` becomes emissive, `specular` becomes the reflectance and
  `min/max_shininess` becomes roughness.

  It carries the intent, not the falloff: **this will not match ScanlineRender
  pixel for pixel**, and a high shininess concentrates the highlight far more
  than Phong does - shininess 10 renders a 0.12 highlight where 100 renders
  4.66, because a tight GGX lobe puts the same energy into a smaller spot.  A
  full-strength `specular` is taken as a dielectric 0.04 reflectance rather than
  a mirror; handing it over as-is turned every default material into one.

  A material with no image input has no pixels - its box is 1x1 - and that is
  no longer baked into a one-texel texture; its colour comes from the knobs.
* Set `IR_GEO_PROBE=1` to have the loader report what each object carries.

## Rendering through Hydra (Nuke 17.1)

The same renderer is also a **Hydra render delegate**, so it can be picked in
the Viewer's `renderer` menu and drive a `GeoRender` node.  Instancing survives
the trip: a PointInstancer of 82 copies renders 1868 unique triangles, not 82
times that.

Nuke builds its renderer list once at startup, so the delegate has to be on
`PXR_PLUGINPATH_NAME` **before** Nuke runs - `init.py` is too late in a GUI
session.  Either set it permanently, or launch through the batch file installed
next to the plugin:

    ~/.nuke/InstanceRender/hydra_launch.bat "C:\Program Files\Nuke17.1v1\Nuke17.1.exe"

**Volumes come through it too.**  A volume rprim carries no grids - it carries
field descriptors naming separate `openvdbAsset` bprims - so the delegate has to
support both, and it does: a `.vdb` through `GeoRender` renders the same as
through the node, blackbody and all.

Points and curves come through it too: a `UsdGeomPoints` prim becomes one
sphere prototype and a transform per point, and `UsdGeomBasisCurves` becomes a
tube per curve, tessellated exactly as the Nuke node tessellates them.  **Point
detail**, **Curve sides** and **Curve segments** are renderer settings on
`GeoRender`.

One trap worth knowing, and nothing to do with this renderer: `GeoImport`
contributes nothing to the 3D scene until prims are selected in its scenegraph,
and it only imports at all if the file is set as the node is created.  A render
index holding no geometry usually means one of those, not a renderer problem.

### If a GeoRender frame will not render

Nuke 17.1v1 renamed `GeoRender`'s `render_settings_legacy` knob to
`render_settings_schema`, and a node carrying the **stale** knob - any script
saved from 17.1v1 Beta 4 - **hangs the render**, with Storm as much as with this
renderer.  Delete that line from the script (or re-save the node) and it renders
again.  `docs/HYDRA2_RESEARCH.md` has the rest of the beta-to-release changes.

### AOVs and alpha

`GeoRender` maps an AOV onto an output plane by laying its components into that
plane's channels **in order**, so in 17.1v1 the colour AOV carries its own
coverage:

    GeoRender  render_variables_table { {color 1 rgba} {depth 1 depth} }

(On 17.1v1 Beta 4 that same row wrote the colour's *red* into alpha, and
coverage had to come from the delegate's own one-component `alpha` AOV,
registered by a `GeoRenderVariable` whose `sourceName` matches.  That still
works.)

Paste `ToolSets > InstanceRender > InstanceRender_AOVs` to get that wired up.
`depth.Z` carries camera-space distance, the classic Nuke convention (Storm puts
NDC depth there instead).

It matches the InstanceRender node and ScanlineRender2 pixel for pixel on the
same scene.  Two things had to be handled for that, both of them Nuke's
conventions rather than USD's: Nuke authors normals **faceVarying** (taking only
per-point normals shades every triangle flat), and it hands Hydra a camera with
a **square filmback**, which frames 1.32x wider than every other renderer in
Nuke unless the horizontal aperture is taken as horizontal.

Dome lights work through it, including one fed by a Read: the delegate reads
the HDRI and importance
samples it with the same distribution the node uses (`src/ir/Dome.h`), so a
small bright sun is not left to chance.  A `GeoDomeLight` in Nuke does not point at a
file, it points at the node feeding it - see *Textures fed by Nuke nodes*
in `RELEASE_NOTES.md` - and both renderers fetch that node rather than asking for an image Nuke
may not be holding.

Beyond the beauty it can hand back depth, the normal, the ids, position,
motion, st and the five lighting layers - which add back up to the beauty.  Each
needs a `GeoRenderVariable` to register its name, exactly as `alpha` does.

The delegate renders on the GPU as well: **Use GPU**, **Samples**, **Max
bounces**, **Point detail**, **Curve sides** and **Curve segments** all appear
in `GeoRender`'s renderer settings tab, and both back-ends run the same kernel,
so the two devices agree to the last digit.

With **Use Legacy Hydra API** off, `GeoRender` needs pointing at *both*
`render_settings_prim_path` and `render_product_prim_path`; given only one it
reports "No AOV is mapped to the output channel for this plane" - for Storm just
as much as for this renderer.

`docs/HYDRA2_RESEARCH.md` has the whole write-up, including the two traps that
cost the most time.

## The 3D viewport while this node is viewed

Viewing this node keeps the 3D view populated - card, camera, lights and the
rest - exactly as viewing ScanlineRender does.

Two things make that work.  `Op::geometryProvider()`, not the GL handles, is
what Nuke's 3D view uses to find the scene of the op being looked at, so this
node has a provider of its own that says the geometry is ours and asks the input
for the substance of it - nothing is copied, the layer and stage come straight
from upstream.  And `build_handles` is overridden to pass the question on to the
scene and camera inputs whenever the viewer is in a 3D mode, so their
manipulators still work, behaving exactly as before in 2D.

**Nuke 17.1 only for now.**  The interface has a different shape in every
version that has it - 16.0 const, 17.0 non-const with a renamed method, 17.1
taking a `NodeEvalContext` - and 17.1 is the one this could be tested against.
Earlier versions keep a plain forwarding, which is enough to be recognised as a
geometry source but not to make the viewport draw.

Dragging a handle re-renders, and finished textures are kept between loads and
re-used whole rather than re-baked and re-mipped - which is most of the
difference between a viewport that follows the mouse and one that does not.  The
cache key is everything that decides the content, and Nuke's own paths make good
keys: an `nkop:` path carries a hash of the op's state.  **refresh render**
empties the cache, which is what to press if a texture file changes on disk
without its path changing.

## A card textured by a Nuke node

A GeoCard with an image wired into it does **not** author a UsdPreviewSurface.
It authors a material whose surface is a shader with `info:id` of
`NukeDefaultSurface`, carrying two inputs - `tex_color` and `tex_opacity` - both
connected to an ordinary `UsdUVTexture` whose file is one of Nuke's fake
`nkop:` paths.  Looking only for UsdPreviewSurface found a material with nothing
in it, and the card rendered flat grey while ScanlineRender, which knows Nuke's
own shaders, showed the picture.  Both inputs are now read; everything under
them was already handled, including baking the Nuke node the texture points at.

Two things learned in the same place:

An `nkop:` path must NOT be resolved.  It is deliberately fake - it carries an
Op pointer through USD to be handed to `MaterialOpI::retrieveOpFromAssetPath()`
- and USD's resolver does not leave it alone: `GetResolvedPath()` turned
`nkop:/NkRoot/Read1:1:main:...nkiop` into `"1"`, which is not empty, so the raw
path was never reached and the loader went looking for a file called `1`.  The
raw path wins whenever it is one of Nuke's.

And **whichever renderer runs first in a session gets the texture** - the
second one, whichever it is, gets grey.  ScanlineRender does it to itself as
readily as it does it to this node: rendered first it is textured, rendered
after this one it comes back flat.  Nuke appears to hold the op's texture only
until something consumes it.  Worth knowing before concluding that one renderer
is broken and the other is not.

## If a render is noisy

Measure before turning knobs, because the obvious suspect is usually wrong.  On
a particle scene reported as noisy, the noise was NOT the motion blur and NOT
the sample count - each setting compared against its own high-sample reference,
at 250 spp:

| setting              | rms   |
|----------------------|-------|
| as saved (2 bounces) | 0.334 |
| 1 bounce             | 0.207 |
| 0 bounces            | 0.082 |
| no motion blur       | 0.222 |

Indirect bounces, four times over.  The cause showed up in one number: the
**brightest pixel was 266** against a mean lit signal of 4.  Fireflies - a few
paths finding a bright specular highlight - and a handful of enormous samples is
what noise in a path tracer usually is.

So **clamp radiance** is the knob, not `samples`.  It is aimed exactly at that
tail, and buying the same improvement with samples is hopeless: noise falls with
the square root, so eight times cleaner costs sixty-four times the samples.

| clamp | rms   | brightest | light kept |
|-------|-------|-----------|------------|
| off   | 0.334 | 267       | 100%       |
| 150   | 0.111 | 69        | 89%        |
| 80    | 0.092 | 47        | 84%        |
| 40    | 0.067 | 29        | 75%        |
| 20    | 0.042 | 18        | 64%        |

Clamping is a bias, and the table is there so the trade is visible rather than
guessed: on a scene lit at intensity 156 plenty of pixels are legitimately over
20, so a low clamp dims the picture as well as cleaning it.  80 to 150 buys most
of the improvement for a tenth of the light.

The shutter is also sampled in strata rather than drawn blind - sample i takes
the i'th slice, jittered inside it - which is free and exact where a drawn time
leaves the number of samples landing on a streak varying from pixel to pixel.
It is worth about 3% here, because bounces dominate; it is worth far more in a
scene whose noise really is the blur.

## Denoising

**denoise** on the node, **Denoise** in `GeoRender`'s renderer settings: the
OptiX AI denoiser over the finished frame, guided by the albedo and normal
passes.  It is a post-process, so it needs a GPU but *not* the GPU renderer - a
CPU render is denoised identically:

    2 samples, no denoise    neighbour noise 0.57278, mean 0.7415
    2 samples, denoised      neighbour noise 0.12060, mean 0.8048
    256 samples, reference   neighbour noise 0.14987, mean 0.8093

Two samples denoised come out cleaner than 256 samples raw, and closer to the
converged answer than the noisy frame was.  The alpha is left alone - coverage
is not noise - and the CPU and GPU paths agree (0.12060 against 0.12065).

## Building

**Windows** - one build per Nuke MINOR version, because the NDK is not
compatible across them:

    cmake -G "Visual Studio 17 2022" -A x64 -DNUKE_ROOT="C:/Program Files/Nuke17.1v1" -B build17.1
    cmake --build build17.1 --config Release
    cmake --install build17.1 --prefix "%USERPROFILE%/.nuke"

**Linux**:

    ./build.sh /usr/local/Nuke17.1v1 --install

Use the compiler Foundry documents for that Nuke: gcc 9 for Nuke 14, gcc 11 for
15 to 17.

How far the Linux build is proven, precisely: it configures, compiles and links
into a correctly named `InstanceRender.so` under g++ 11.5 on AlmaLinux 9.  It
has never been RUN - there is no Linux Nuke on the machine it was prepared on,
so the link was against stub libraries, and a shared module on Linux links
happily with undefined symbols.  That demonstrates the build system, not the
plugin.  Treat the first real Linux build as the test.
A small script in the (unshipped) test suite puts the portable headers through
g++ on their own, which is what caught the MSVC-isms.

Optional: `-DIR_WITH_OPTIX=ON` for the GPU back-end (CUDA 12 + OptiX 9),
`-DIR_WITH_HYDRA=OFF` to skip the delegate.

## What it does not render yet

* Light filters and portals - see *Lights* above for why filters are not
  representable.
* A displacement MAP. The constant `displacement` is in - it moves vertices
  at load, so the silhouette and the shadow move with it - but a texture
  connected to that input needs sampling per vertex against the vertex uv,
  and is reported rather than silently ignored.

Nuke's own particle nodes and its Fields will not connect to a `GeoScene` at
all, so neither reaches a USD renderer directly.  Both have a way across:
particles render through the classic front end above, or through `ParticlesToUSD`;
a `FieldVolume` reaches the stage through `VolumeToUSD`.  Both of those nodes are
in the CopyToPoints repository.

## If Nuke seems to freeze

Time it before believing it, because two very different things look identical
from the outside: a render that is still going, and one that will never finish.
Set `IR_LOG` to a file and the node writes one flushed line per step, stamped
with milliseconds, on both devices:

    set IR_LOG=C:/temp/ir.log

A trace that stops mid-render says where it stopped.  A trace that runs to
`render done` in a second says the node was never the problem, and the next
suspect is Nuke itself - opening a script that carries a saved window layout
took **98 seconds** here while the node's own render took 1.2, and the same
graph re-saved without that layout opened in 0.2.

Worth knowing when a viewer render is genuinely heavy: `progressive (viewer)`
is **off** by default, so the viewer shows nothing at all until the whole frame
is done.  At the sample counts a finished shot wants, that is the difference
between "slow" and "frozen".  Turn it on and the first pass arrives almost
immediately and refines.

If it really is stuck, set `IR_WATCHDOG` to a number of seconds BEFORE Nuke
starts and a stack for every thread is written automatically when a render - or
Nuke's whole interface - stops for longer than that.  The `freeze report` button
on the node writes the same thing on demand.  How that works, and why a frozen
interface cannot report its own freeze, is in `RELEASE_NOTES.md`.

## License

MIT - see `LICENSE`.

This is a **plugin**: it is built against, and loaded by, Nuke, which is
Foundry's and is not included here. Nothing third-party is vendored in this
repository - no Nuke headers or libraries, no Embree, USD, CUDA or OptiX
binaries; all come from your own installs. `THIRD_PARTY_NOTICES.md` lists every
dependency, its licence, and the algorithms implemented here from published
descriptions. Being MIT does not grant you rights to any of them.
