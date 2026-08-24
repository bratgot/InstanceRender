# Toolbar / node-graph icons, in Nuke's own visual language.
#
# MEASURED off Nuke 17.1's own icons (plugins/icons) rather than guessed:
#
#   format      PNG, RGBA 8-bit. 24x24, plus a 48x48 twin named "<name>@2x.png"
#               - the Qt high-dpi convention, which Nuke uses throughout.
#   body        a SUBTLE vertical grey ramp, about 175 at the top to 135 at the
#               bottom. Not a dramatic gradient; four tones would read as five
#               different icons on a toolbar.
#   detail      #323232 for interior lines and cut-outs. Nuke does NOT outline
#               the whole silhouette - the dark lines are internal edges only,
#               which is what keeps the shapes reading as solid objects.
#   accent      one small, discrete COLOURED TOKEN, never a tint of the whole
#               icon. GeoImport is a green hexagon (#91B346), GeoExport the same
#               hexagon in red (#A23D40). That is the whole vocabulary: a badge
#               that says what the node is FOR, on an otherwise grey object.
#
# Everything is drawn at 8x and downsampled, so the diagonals and the 1px dark
# edges survive antialiasing at 24px, which is where these actually live.
#
#   python tools/make_icons.py [outdir]
#
# Strict ASCII.
import os
import sys

from PIL import Image, ImageDraw

SS = 8                      # supersample factor
BASE = 48                   # the @2x size; 24 is derived from it
N = BASE * SS

# ---- the measured palette ---------------------------------------------------
BODY_TOP = (183, 183, 183)
BODY_BOT = (129, 129, 129)
DARK = (50, 50, 50)         # #323232, Nuke's interior line
DARKER = (38, 38, 38)

# accents, in Nuke's own key: green = in, red = out, and a warm one for fire
GREEN = (145, 179, 70)      # #91B346, straight off GeoImport
RED = (162, 61, 64)         # #A23D40, straight off GeoExport
BLUE = (86, 140, 190)
ORANGE = (222, 138, 52)
VIOLET = (150, 118, 190)


def body_gradient(size=N):
    """The grey ramp, as a full-tile image to be masked."""
    g = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(g)
    for y in range(size):
        t = y / float(size - 1)
        c = tuple(int(BODY_TOP[i] + (BODY_BOT[i] - BODY_TOP[i]) * t) for i in range(3))
        d.line([(0, y), (size, y)], fill=c)
    return g


def shade(t):
    """One tone off the ramp, t = 0 at the top of the icon."""
    return tuple(int(BODY_TOP[i] + (BODY_BOT[i] - BODY_TOP[i]) * t) for i in range(3))


def new_canvas():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def cube(dr, cx, cy, r, line=None):
    """An isometric cube, shaded the way Nuke's Cube icon is.

    Three faces, each a flat tone off the ramp - top lightest, then left, then
    right - separated by dark interior lines. No outline around the whole shape.
    """
    if line is None:
        line = max(2, int(r * 0.11))
    hw = r                        # half width
    hh = r * 0.56                 # half height of the top rhombus
    top = [(cx, cy - r * 0.86), (cx + hw, cy - r * 0.30),
           (cx, cy + r * 0.26), (cx - hw, cy - r * 0.30)]
    left = [(cx - hw, cy - r * 0.30), (cx, cy + r * 0.26),
            (cx, cy + r * 1.12), (cx - hw, cy + r * 0.56)]
    right = [(cx + hw, cy - r * 0.30), (cx, cy + r * 0.26),
             (cx, cy + r * 1.12), (cx + hw, cy + r * 0.56)]
    dr.polygon(top, fill=shade(0.10))
    dr.polygon(left, fill=shade(0.55))
    dr.polygon(right, fill=shade(0.92))
    for a, b in (((cx, cy - r * 0.86), (cx + hw, cy - r * 0.30)),
                 ((cx + hw, cy - r * 0.30), (cx, cy + r * 0.26)),
                 ((cx, cy + r * 0.26), (cx - hw, cy - r * 0.30)),
                 ((cx - hw, cy - r * 0.30), (cx, cy - r * 0.86)),
                 ((cx, cy + r * 0.26), (cx, cy + r * 1.12)),
                 ((cx - hw, cy - r * 0.30), (cx - hw, cy + r * 0.56)),
                 ((cx + hw, cy - r * 0.30), (cx + hw, cy + r * 0.56)),
                 ((cx - hw, cy + r * 0.56), (cx, cy + r * 1.12)),
                 ((cx + hw, cy + r * 0.56), (cx, cy + r * 1.12))):
        dr.line([a, b], fill=DARK, width=line)


def hexagon(dr, cx, cy, r, colour):
    """The accent token. Nuke uses a flat hexagon for exactly this job."""
    pts = []
    for k in range(6):
        import math
        a = math.radians(60 * k - 30)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    dr.polygon(pts, fill=colour)


def speedlines(dr, x0, y, w, h, gap, colour, n=3):
    """The little stack of dashes Nuke puts beside its import/export token."""
    for k in range(n):
        yy = y + (k - (n - 1) / 2.0) * gap
        dr.rectangle([x0, yy - h / 2.0, x0 + w, yy + h / 2.0], fill=colour)


def save(img, outdir, name):
    """Write the 48px @2x and the 24px, both RGBA, the way Nuke ships them."""
    big = img.resize((BASE, BASE), Image.LANCZOS)
    small = img.resize((BASE // 2, BASE // 2), Image.LANCZOS)
    big.save(os.path.join(outdir, "%s@2x.png" % name))
    small.save(os.path.join(outdir, "%s.png" % name))
    return name


# ---- the icons ---------------------------------------------------------------

def icon_copy_to_points(accent=None):
    """Three cubes on a ground bar: one source, two copies.

    Sized to FILL the tile. Nuke's own icons leave very little margin - its Cube
    runs almost edge to edge - and a first pass drawn politely in the middle
    turned to mush at 24px, which is the size that actually matters.
    """
    img = new_canvas()
    dr = ImageDraw.Draw(img)
    # the surface the points sit on: a bold slab, not a hairline
    slab = [(N * 0.02, N * 0.80), (N * 0.98, N * 0.80),
            (N * 0.86, N * 0.96), (N * 0.14, N * 0.96)]
    dr.polygon(slab, fill=shade(0.88))
    dr.line(slab + [slab[0]], fill=DARK, width=max(3, N // 60), joint="curve")
    cube(dr, N * 0.50, N * 0.30, N * 0.27)      # the source
    cube(dr, N * 0.155, N * 0.545, N * 0.155)   # two copies
    cube(dr, N * 0.845, N * 0.545, N * 0.155)
    if accent:
        for cx in (N * 0.155, N * 0.50, N * 0.845):
            rr = N * 0.062
            dr.ellipse([cx - rr, N * 0.875 - rr, cx + rr, N * 0.875 + rr], fill=accent)
    return img


def icon_instance_render():
    """A film frame, drawn as an OUTLINE, with instanced cubes inside it.

    The first version filled the frame with a pale panel and it read as a page
    with something on it - the odd one out in a set where every Nuke icon is a
    silhouette on transparency. Stroking the frame and leaving the middle empty
    puts it back in the same language, and lets the cubes carry the contrast.
    """
    img = new_canvas()
    dr = ImageDraw.Draw(img)
    m, r = N * 0.045, N * 0.11
    t = max(6, N // 26)
    dr.rounded_rectangle([m, m, N - m, N - m], radius=r, outline=shade(0.30), width=t)
    # the sprocket bar: the one detail that says "film" at any size
    for k in range(4):
        y = N * (0.20 + 0.20 * k)
        dr.rounded_rectangle([N * 0.115, y - N * 0.052, N * 0.225, y + N * 0.052],
                             radius=N * 0.022, fill=shade(0.45))
    dr.line([(N * 0.285, m + t * 0.4), (N * 0.285, N - m - t * 0.4)],
            fill=shade(0.55), width=max(4, N // 44))
    # two cubes: one instanced from the other, which is the whole point
    cube(dr, N * 0.645, N * 0.415, N * 0.215)
    cube(dr, N * 0.815, N * 0.695, N * 0.125)
    return img


def icon_volume_to_usd():
    """A volume box with a hot core: the .vdb, and what is inside it.

    The warm centre is the one place colour earns itself here - a fire grid IS
    why this node exists, and the renderer reads its temperature as a blackbody.
    The wire is deliberately heavy: a hairline box vanished at 24px.
    """
    img = new_canvas()
    dr = ImageDraw.Draw(img)
    w = max(5, N // 34)
    a, b = N * 0.06, N * 0.72
    off = N * 0.20
    front = [(a, a + off), (b, a + off), (b, b + off), (a, b + off)]
    back = [(a + off, a), (b + off, a), (b + off, b), (a + off, b)]
    for i2 in range(4):
        dr.line([front[i2], back[i2]], fill=shade(0.75), width=w)
    dr.line(back + [back[0]], fill=shade(0.30), width=w, joint="curve")
    # the hot core, big enough to read as the subject rather than a detail
    cx, cy = N * 0.42, N * 0.55
    for rad, col in ((N * 0.275, (192, 88, 36)), (N * 0.185, ORANGE),
                     (N * 0.095, (250, 214, 132))):
        dr.ellipse([cx - rad, cy - rad * 0.88, cx + rad, cy + rad * 0.88], fill=col)
    dr.line(front + [front[0]], fill=shade(0.30), width=w, joint="curve")
    return img


def icon_particles_to_usd():
    """A spray of particles gathering into a token: points becoming a prim.

    Four dots, not seven. The first pass scattered small ones and they turned
    into noise at 24px - a particle icon that reads as dirt on the screen.
    """
    img = new_canvas()
    dr = ImageDraw.Draw(img)
    for fx, fy, fr in ((0.10, 0.24, 0.085), (0.09, 0.72, 0.070),
                       (0.28, 0.48, 0.115), (0.30, 0.86, 0.055)):
        cx, cy, rr = N * fx, N * fy, N * fr
        dr.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=shade(fy))
    hexagon(dr, N * 0.68, N * 0.47, N * 0.305, BLUE)
    hexagon(dr, N * 0.68, N * 0.47, N * 0.185, (28, 48, 68))
    return img


def icon_multiply_cf():
    """Nuke's MergeMultiply shape - a grey tile with a bold dark X knocked out -
    with a small RGB strip along the bottom saying WHICH multiply this is.

    A version with the whole tile in colour bands read clearly but was the only
    saturated icon in the set, which is not what Nuke does: its own multiply is
    flat grey. The colour belongs as a dash, not as the object.
    """
    img = new_canvas()
    dr = ImageDraw.Draw(img)
    m, r = N * 0.055, N * 0.13
    dr.rounded_rectangle([m, m, N - m, N - m], radius=r, fill=shade(0.35))
    dr.rounded_rectangle([m, m, N - m, N - m], radius=r, outline=DARK, width=max(4, N // 46))
    # the X, knocked out dark and thick enough to survive 24px
    cx, cy = N * 0.5, N * 0.455
    sz = N * 0.215
    t = max(9, N // 15)
    for a2, b2 in (((cx - sz, cy - sz), (cx + sz, cy + sz)),
                   ((cx - sz, cy + sz), (cx + sz, cy - sz))):
        dr.line([a2, b2], fill=DARK, width=t)
    # the dash of colour: Cf, as three chips on the bottom edge
    bw = N * 0.155
    gap = N * 0.035
    x0 = cx - (bw * 1.5 + gap)
    for k, col in enumerate(((196, 78, 78), (104, 176, 98), (82, 134, 198))):
        x = x0 + k * (bw + gap)
        dr.rounded_rectangle([x, N * 0.755, x + bw, N * 0.855], radius=N * 0.028, fill=col)
    return img


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "nuke", "icons")
    outdir = os.path.abspath(outdir)
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    made = []
    made.append(save(icon_instance_render(), outdir, "InstanceRender"))
    # the classic node and the USD one are the same idea, told apart by the
    # accent: green for the classic 3D system, violet for USD
    made.append(save(icon_copy_to_points(GREEN), outdir, "CopyToPoints"))
    made.append(save(icon_copy_to_points(VIOLET), outdir, "CopyToPointsUSD"))
    made.append(save(icon_volume_to_usd(), outdir, "VolumeToUSD"))
    made.append(save(icon_particles_to_usd(), outdir, "ParticlesToUSD"))
    made.append(save(icon_multiply_cf(), outdir, "MultiplyCf"))
    print("wrote %d icons (24px + @2x) to %s" % (len(made), outdir))
    for m in made:
        print("   %s.png / %s@2x.png" % (m, m))


if __name__ == "__main__":
    main()
