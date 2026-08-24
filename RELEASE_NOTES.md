# InstanceRender - engineering notes

Moved out of the README, which is a reference for using the node rather than a
record of how it got here. Nothing is edited: these are the original write-ups
of problems that were hard to find and are easy to reintroduce, kept because
the reasoning is the useful part.

The transferable versions - the ones that apply to any Nuke plugin rather than
to this one - live in `NDK_NOTES.md` in the NukeDevRules repository.

These notes name the tests that caught each problem. Those tests are developer
scaffolding and are not shipped in this repository, so the paths below are a
record of how something was pinned down rather than files you will find here.

## Textures fed by Nuke nodes

A texture wired from a Nuke node - a `CheckerBoard` into a `PreviewSurface`, a
`Read` into a `GeoDomeLight` - is not a file.  Nuke authors an asset path that
names the op instead:

    nkop:/NkRoot/Read1:12:main:0xffffffffffffffff:0[1,1,0,0]:0x9740....nkiop
          \_________/ \/
           the op     the frame

Reading one of those the ordinary way only works while Nuke happens to be
holding a texture image for that op.  When it is not, its own plugin says so and
gives up:

    TextureIopInterface::ctor nodepath='/NkRoot/Read1:1:main:...' hasTextureImage=0, w=0, h=0
    ImageInterface::create(): error, image is either zero-sized or has no channels
    to read. Delete interface so will fallback to producing a default 1x1 grey texture.

Whether it holds one depends on what has been evaluated, and the path carries
the frame - so the texture appeared on some frames and vanished on others, and a
dome light lit the scene flat, or black on a first render.

**This node renders the op itself** rather than asking for that cached image:
the op is upstream of it, so it is found by name, validated and read like any
other Nuke image - which works on every frame.  `test/timeline_test.py` renders
a still scene on six frames and requires them to be identical and to carry the
image; `test/gui_probe_timeline` does the same in a GUI session.

The Hydra delegate does the same - `MaterialOpI::retrieveOpFromAssetPath()` is
the NDK's own way in, and the delegate only ever runs inside Nuke - so a texture
or a dome light fed by a Nuke node renders the same through `GeoRender` as
through the node.

## A Nuke bug worked around

Nuke's own material ops fail while authoring into a USD stage that a renderer
asks for:

    ERROR: GeoBindMaterial1: Can't set time sample on
    </materials/NukeMaterialOps/PreviewSurface1_NdkSurfaceShader/CheckerBoard1_UVTexture.inputs:scale>
    to (0.18, 0.18, 0.18, 1): expected a value of type "TfToken"

It turns an upstream node red and aborts the render, even though the geometry
and the render itself are fine.  It is not specific to this node -
ScanlineRender2 fed from the same `GeoScene` fails identically, and
`test/nuke_material_bug_repro.py` reproduces it with stock nodes; Scanline only
escapes it when it is fed geometry directly, where it never asks for a stage.

**This node now builds its stage with error observation off**, so those messages
cannot abort the render.  Genuine failures are not swept away: when the stage
comes back with nothing in it, the node says so in its report, and the
`report stage-build errors` knob turns the raw upstream messages back on for
diagnosis.

## When something upstream goes wrong

The `info` knob's report names any error an upstream node raised *while a stage
was being built*, and which build it was - with motion blur on the node builds
two stages, at the frame and at shutter close, so a message that only appears
for one of them says where to look:

    CPU render 960x540 x 16 spp, 2 bounces, shutter 0.5 (2 stage builds): 240 ms (...)
    upstream error while building the shutter-close stage (frame 1.250000): GeoSphere1: ...

Hit `refresh` under `info` after the error appears.

See the README's Building section.

## Scrubbing, and being interruptible

Nuke asks a render to stop by setting `aborted()` on the node, and until v0.30.0
this renderer never looked.  That is worse than it sounds.  A load VALIDATES the
ops upstream - the geometry input, and every light - and validating an input
after Nuke has cancelled the render **deadlocks**: the main thread is holding the
graph waiting for this render to notice it should stop, and `validate()` waits
for the graph.  Scrubbing the playbar is a stream of renders each cancelled by
the next, so it hits that constantly, and when it hits, Nuke stops responding
and does not recover.  Measured on a 1800-copy particle scene: `aborted()` was
already true at exactly the line that hung.

The node now checks before every one of those calls, through the object loop,
and hands the back-ends a cancel flag; the CPU renderer checks it between
16x16 tiles.

Two distinctions this cost blood to get right.  `aborted()` is true whenever a
tree this op is in was stopped **for any reason, including an upstream op
raising an error**; `cancelled()` means user interaction - and a viewer
abandoning a render to start the next frame does NOT set it, so guarding on
`cancelled()` froze exactly as before.  Everything therefore guards on
`aborted()`, and the price of that is that an ERROR also looks like an abort:
give up early on one and the report comes back empty, which is the one moment
anybody wants to read it.  So nothing gives up early - the refetch is skipped
rather than the load abandoned, and the render is cut short from inside instead.

And whether a render may be CACHED is decided by a flag recorded while it ran,
not by asking `aborted()` afterwards: by then Nuke has usually cleared the abort
to start the next frame, so a render that was cut short looks complete, gets
cached, and the viewer keeps its half-filled buffer for good.
A cancelled render is also no longer *remembered* as a rendered one - the hash
would match next time and the viewer would keep the blank buffer the render was
interrupted in the middle of filling.  `test/gui_probe_freeze` drags the playhead
across a scene heavy enough to matter and checks all three: that Nuke survives,
that renders really are cancelled, and that one still completes once the drag
stops.

One limitation: an OptiX launch is a single call, so a GPU render is interrupted
between frames rather than during one.  A CPU render stops at the next tile.

## The render thread must not touch the node graph

v0.31.0.  Loading a scene asks Nuke's node graph questions - which op feeds this
input, at this frame - and those take a lock inside Nuke.  A render runs on a
pooled render thread (measured: `_open()` arrived on 16688, 43732, 47488 ...
while `_validate()` was always 44784).  When the main thread is inside a viewer
paint waiting for that render to finish, which is most of the time in a GUI, the
two wait on each other for ever.  Caught in the act with `test/attach_freeze`:

    MAIN    QGLWidget::paintEvent -> ... -> DD::Image::Thread::wait
    render  InstanceRender::loadClassicScene -> RtlEnterCriticalSection

and twenty-four more threads queued behind the same lock.

So **everything that touches the graph now happens in `_validate()`**
(`prepareScene()`), and the render only builds a BVH and traces it.

Resolving the input *pointers* early and keeping the loading where it was does
NOT work, and was tried: `node_input()` returns the same `Op*` whatever context
you ask for, so the frame it speaks for is a side effect of the call itself.
Defer the call and you defer the effect - motion blur stopped dead (travel
1.000 -> 0.000).  The work has to move, not the lookup.

**What this costs.** The loading is now on the main thread, so it blocks the
interface instead of a render thread.  Scrubbing 25 frames of an 1800-copy
particle scene: 14 stalls over half a second, 17.1s stalled in total, against
2.6s before - all of it in the scene load, about a second per frame for two
stage builds.  That is a bad trade only if you have never seen the alternative:
before, the same scrub could wedge Nuke permanently.  Shortening the stall means
making the load cheaper, which is a separate job.

## Capturing a freeze

    .	est\capture_freeze.ps1 -Script your.nk -Nuke Nuke14.1v8

Launches NukeX with a watchdog armed, and prints a folder at the end.  Work
normally; when it locks up, **leave it locked up for half a minute** before
killing it - the watchdog needs that long to notice and write.  Killing Nuke
afterwards is fine, the report is already on disk.

What it produces, and why each part exists:

* `ir.log` - every render step, stamped with milliseconds.  Says how far the
  render got.
* `ir_freeze_*.txt` - **a stack for every thread in the process**, written while
  it was still stuck.  Says what it is stuck ON, which the log cannot.  The
  first thread is marked `MAIN` (Nuke's interface: if that one is stuck, the
  interface is frozen) and, if a render was running, the thread inside it is
  marked `RENDER`.
* `ir_freeze_*.dmp` with `-Minidump` - the same for WinDbg or Visual Studio.

The plugin ships a `.pdb` beside its `.dll`, so our own frames come back as
functions and line numbers rather than addresses:

    thread 22196   <== RENDER (this one is in the phase above)
        KERNELBASE      SleepEx+0x91
        InstanceRender  InstanceRender::renderFrame+0x42f  [InstanceRender.cpp:1490]
        InstanceRender  InstanceRender::_open+0x111        [InstanceRender.cpp:1735]
        DDImage         DD::Image::Op::open+0xb1

There are **two** clocks, because the two freezes worth telling apart look
identical from outside.  A *phase* clock fires when a render sits in one step
too long - that is our fault, and the report names the step.  A *pulse* clock
fires when Nuke's main thread stops answering a timer, which catches a freeze
whether or not this node is involved; in that case the report says the renderer
was idle, and the culprit is whatever `MAIN` is sitting in.  The watchdog is a
plain OS thread and keeps running while Qt is dead, which is the whole point -
a frozen interface cannot report that the interface is frozen.

Proving the tool works, rather than trusting it: `IR_STALL=<seconds>` hangs a
render on purpose and `IR_PROBE_MODE=wedge` hangs Nuke's main thread on purpose.
A freeze detector that has never been seen to detect a freeze is not evidence of
anything - "no report" has to mean "it did not freeze", never "the tool was
broken".

## Motion blur: where the particle velocity comes from

Nuke's particle geometry reaches a renderer carrying `id`, `Cf` and `size` and
nothing else - no velocity, `GeoInfo::VEL_ref` empty.  Measured on a scene of
nothing but particles: `425 point(s), vel absent, VEL_ref none, attribs:
id(g2 t5) Cf(g2 t3) size(g2 t0)`.

The velocity does exist; it just never leaves the particle SYSTEM.  So the
loader asks the system directly - the same `ParticleRender::getParticleSystem()`
CopyToPoints uses - and matches it to the points by the `id` they do carry.
Nothing to switch on, and it matters most exactly where pairing is worst: Nuke
RECYCLES particle ids, so pairing goes wrong as particles die.

**The velocity has to earn it.**  A particle system will report velocities its
particles do not follow - a bare `ParticleEmitter` hands out velocities of unit
length to particles that never move - and blurring by that INVENTS motion, which
is worse than the sharp copies it was meant to fix.  It showed up as a blurred
render with less coverage than the sharp one, the streaks sweeping particles off
their real positions.  So the two are compared where both have an opinion, on
the paired instances, and the velocity is preferred everywhere only if its median
travel is within a factor of two of what pairing measures.  Where it disagrees
it is still used for whatever pairing could not pair, because there the
alternative is not a better answer but no answer at all.

### CopyToPoints hands the velocity over by itself

The best fix for the static copies is not to pair objects at all.  CopyToPoints
writes the particle's velocity onto every copy as an object attribute - always,
not only when its "copy attributes" knob asks for it - and this node then blurs
each copy along its own velocity and pairs nothing.  Nothing to switch on.  A velocity needs no partner at the
other end of the shutter, so the particles being born and dying inside it - the
ones pairing can never reach - blur correctly too.

Measured on the scene this was reported from, at the same frame:

|                        | pairing by position | from the velocity |
|------------------------|---------------------|-------------------|
| blurred                | 411                 | 469               |
| held still (sharp)     | 58                  | **0**             |
| max travel             | 1.6680              | 0.6300            |

The travel falls because pairing's worst case includes pairs that were simply
wrong; a velocity cannot be wrong about which object it belongs to.

**use velocities** on the Scene tab turns it off, which is worth having when a
scene's velocities do not describe its motion - though the renderer checks that
they agree with the pairing before preferring them, so this is a last resort
rather than a routine control.  Two things to know: the velocity moves an object
without TURNING it, so a copy that spins across the shutter needs pairing (or
`rotvel`, which is not read yet); and Nuke's `vel` is in units per FRAME, which
`test/velocity_attr_test.py` checks by rendering the same scene both ways and
holding the velocity to what pairing independently measures - per frame and per
second differ by 24, and a blur 24 times too long still looks like a blur.

### Why ScanlineRender has no static copies

Because it never pairs anything.  ScanlineRender re-renders the whole scene at
each of its samples, so a particle that dies halfway through the shutter is
simply present in the early samples and absent from the late ones, and it fades.
This renderer reads the scene twice and pairs objects between the two, which is
far cheaper - one BVH, not N - but pairing can fail, and what fails is held
still and renders sharp.  The two-pass pairing above is what closes most of that
gap; the rest is particles genuinely born or dying inside the shutter, which
have no partner to find because they do not exist at the other end.

### Matching ScanlineRender's blur

This renderer gives every ray its own instant across the shutter, which draws a
smooth streak.  ScanlineRender samples the shutter at a few FIXED instants and
averages them, which draws that many separate copies.  Neither is wrong and the
streak is the better picture, but a shot already graded against Scanline has to
keep looking like itself - so **shutter steps** samples the shutter at N fixed
instants instead.  0 (the default) is the smooth streak.

Where the instants sit was measured against ScanlineRender rather than guessed,
and the guess was wrong: they are at the MIDDLE of each of N slices, not at the
two ends.  Ends-included spans the whole travel; mid-slice spans all but one
slice of it, and that is what Scanline does.  Matched on a cube crossing the
frame - 2 samples 174px against Scanline's 175, 4 samples 247 against 248.

One thing to know before comparing anything: **ScanlineRender's shutter opens AT
the frame** (`shutteroffset` defaults to `start`) while this node centres it.
Compare the two without settling that first and you are measuring the offset
rather than the blur - it looks like agreement at 2 samples and disagreement at
4, which is the most misleading result on offer.

Steps cost samples.  A streak converges with far fewer paths than a row of
copies does, so leave the knob at 0 unless matching something.

## Classic 3D: pairing objects across the shutter

* Motion blur has to pair each object with itself at the other end of the
  shutter, and particles make that hard: emitted copies carry **no id at all**,
  and where ids exist Nuke **recycles** them - a particle that dies hands its id
  to one born somewhere else.  Pairing those two smears a streak across the
  frame.

  Pairing by position in the list is worse than it looks: one particle dying
  shifts every particle after it by one place, so they *all* pair with a
  neighbour and the whole cloud smears by the spacing between particles.  That
  is also why rejecting outliers cannot rescue it - the wrong answers are the
  majority, so they set the median.

  So both pairings are built - by key, and by **where the objects actually are**
  (`src/ir/MatchInstances.h`: mutual nearest neighbour within a few multiples of
  the particle spacing) - and the one that explains the motion more coherently
  wins, which needs no threshold to tune.  On a 45-particle emitter the reported
  travel drops from 6.9-9.9 units of nonsense to a steady 1.000 a frame.

  Where a **velocity** exists there is nothing to match at all, and that is the
  better answer: a `UsdGeomPointInstancer` with authored `velocities` is now
  blurred by extrapolating every motion key from the frame being rendered, so
  each key describes the SAME particles - USD's own answer to "varying-topology
  particle streams".  A stream of two particles that becomes three still blurs
  correctly, which no amount of pairing between frames could manage.  Note USD
  velocities are units per **second**: 48 at 24 fps is 2 units a frame.

  Nuke's classic particles do not offer one.  Measured on an emitter: the
  geometry that reaches a renderer carries three attributes - `uv`, `N`, `Cf` -
  no id, no velocity, and `GeoInfo::VEL_ref` empty.  So the pairing above is
  what those scenes get.
  Pairing happens twice.  First mutual nearest - both name each other - which is
  never wrong and is never revisited.  Then the leftovers are paired
  shortest-first against whatever partner is still free, repeatedly, until no
  more pairs form.  That second part matters more than it sounds: mutual nearest
  alone leaves most of a crowd unpaired, because in a knot of particles only the
  closest pair name each other and everyone else is somebody's second choice.
  Unpaired means held still, and held still renders as a SHARP object sitting in
  the middle of a blurred cloud - which is what gets reported.  Measured on such
  a scene: **211 of 475 objects held still, down to 77**, against a floor of
  about 49 that are genuinely born or dying inside the shutter and have no
  partner to find.  Not one of the 211 was rejected for travelling too far -
  raising the reach from 4x the spacing to 32x changed nothing at all.

  Pairing harder is also how a renderer starts inventing streaks, so
  `test/match_scale_test.cpp` measures the mistakes rather than trusting them:
  on a cloud where one in twenty dies and is replaced somewhere else, 94.5% of
  pairs move by the drift they were given - the other 5.5% being exactly the
  churn that has no true partner - and the worst mistake stays local instead of
  streaking across the scene.

  Anything still unpaired is held still: no blur rather than a streak.  **reject
  motion beyond** on the Scene tab tunes how far a particle may travel and still
  be recognised (0 = trust the keys, as before).

  That search has to stay cheap, because a particle scene is exactly where it
  gets big.  The grid grows a cube outwards a ring at a time and stops as soon
  as its best candidate is nearer than the ring it just finished; the spacing
  estimate it needs first is a median over a few hundred sampled points rather
  than all of them.  Scanning a fixed radius the size of the scene instead -
  which is what the first version did - costs (2 * cbrt(N) + 1)^3 cells per
  query, and at 18000 copies that is billions of lookups and reads as Nuke
  hanging.  Measured on an emitter scaled up, the pairing now costs about a
  second at 18000 copies, against 5.6s to read that many classic objects in at
  all.

  Two tests hold that.  `test/match_scale_test.cpp` builds without Nuke (the
  header is header-only) and asserts the invariant that actually broke - cells
  examined per query, 27 whether the cloud is 5000 points or 320000.  Seconds
  cannot hold it: the per-point cost climbs about fivefold across that span on
  cache alone, which is room enough to hide a real blow-up.
  `test/particle_scale_test.py` renders the scene the freeze was reported on
  with the emitter scaled up, and checks it finishes.

## The 3D viewport, and why dragging a handle was jerky

Viewing this node keeps the 3D view populated - card, camera, lights and the
rest - exactly as viewing ScanlineRender does.

Two things make that work.  `build_handles` is overridden to pass the question
on to the scene and camera inputs whenever the viewer is in a 3D mode, and to
behave exactly as before in 2D; a 2D `Iop` otherwise never asks a geometry input
to draw itself.  And `Op::geometryProvider()` - not the GL handles - is what
Nuke's 3D view actually uses to find the scene of the op being looked at, so
this node hands on the provider it was given: the stage behind it IS the scene
being rendered.

| node             | advertises geometry | draws in the 3D view |
|------------------|---------------------|----------------------|
| GeoCard          | yes                 | yes                  |
| ScanlineRender2  | yes                 | yes                  |
| InstanceRender   | **no** -> now yes   | no  -> expected yes  |

`build_handles` is also overridden, so the scene and camera still offer their
manipulators, but that was not what made the viewport go dark - a fix built on
it changed nothing.

Note the provider is only there once the op has been validated: asked before
that, `input(0)` is not resolved yet and the answer is honestly "none".

Handing back the INPUT's provider is not enough, and the reason is one line of
the interface: `GeometryProviderI::getGeometryProviderOp()` names the Op a
provider belongs to.  Forwarding the GeoCard's provider therefore answered "this
geometry belongs to GeoCard1" while the viewer was asking about
InstanceRender1 - so it drew nothing.  ScanlineRender2 has its OWN provider,
which is why it draws while exposing no more interfaces than this node did.
That is what made the difference so hard to see: three interfaces forwarded,
132 `geometryProvider()` calls per redraw, all answered, and still an empty
viewport.

So this node now has a provider of its own that says the geometry is ours and
asks the input for the substance of it - nothing is copied, the layer and stage
come straight from upstream.  Confirmed: `provider owner: InstanceRender1 (this
node is InstanceRender1)`.

**Nuke 17.1 only for now.**  The interface has a different shape in every
version that has it - 16.0 const, 17.0 non-const with a renamed method, 17.1
taking a `NodeEvalContext` - and 17.1 is the one this could be tested against.
Earlier versions keep the plain forwarding, which is enough to be recognised as
a geometry source but not to make the viewport draw.

### Why dragging a handle was jerky

| step                          | before | after |
|-------------------------------|--------|-------|
| bake the Nuke node            |  85 ms |   -   |
| mip chain and copy            | 200 ms |   -   |
| the rest of the scene load    |  27 ms | 49 ms |
| **scene load total**          | **262 ms** | **49 ms** |

Finished textures are now kept between loads and re-used whole - one memcpy
instead of a bake and a twelve-level mip chain.  The key is everything that
decides the content, and Nuke's own paths make good keys: an `nkop:` path
carries a hash of the op's state, so it changes exactly when the picture does.
**refresh render** empties the cache, which is what to press if a texture file
changes on disk without its path changing.

What is left is the render itself, about 180 ms at 2K and 16 spp, of which 107
is the OptiX acceleration structure - rebuilt every drag because the geometry
really has changed.  For a TRANSFORM drag it has not, and that one could be
cached too; it has not been.
