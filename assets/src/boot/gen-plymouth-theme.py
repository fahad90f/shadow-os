#!/usr/bin/env python3
"""Generates every image of the Plymouth splash (assets/boot/plymouth/shadow).

    gen-plymouth-theme.py OUTDIR

Each image is written as SVG text and turned into a PNG by rsvg-convert.
Needs python3 (standard library only), rsvg-convert (librsvg2-bin) and the
Inter font for the wordmark. The symbol comes from
assets/logos/shadow-symbol.svg. Normally called through gen-boot-theme.sh.

The splash script (shadow.script) picks one of three image sets by the size
of the screen and loads only that one:

    s_   small screens (640x480, 800x600)
    m_   1024x768 up to 1440x900
    l_   1600x900 and larger (and, drawn at twice the size, 4K)

Images without a prefix are shared. The numbers below (radii, unit length,
tile sizes) are the same ones the script uses to place the images, so a change
here needs the matching change in the tables at the top of shadow.script.

Everything in the theme directory is copied into every initramfs: this
script prints the total size and fails if a PNG is not plain 8-bit RGBA.
"""
import math
import os
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
SYMBOL = os.path.join(HERE, "..", "..", "logos", "shadow-symbol.svg")
SVG_NS = "http://www.w3.org/2000/svg"

# The palette shared with the boot menu, the login screen and the shell.
CYAN = "#22D3EE"
PALE = "#A5F3FC"      # core of a powered trace
HOT = "#CFFAFE"       # core of the scan beam only
BLUE = "#60A5FA"
VIOLET = "#8B5CF6"
SLATE = "#94A3B8"
UNPOWERED = "#2E4A5C"
BG = "#04060B"
PLATE = "#0B1220"
WORDMARK = "#CBD5E1"


class Set:
    """Sizes of one image set. Lengths are pixels from the logo centre."""

    def __init__(self, **kw):
        self.__dict__.update(kw)


SETS = [
    Set(prefix="s_", u=20, logo=82, plate_r=38, ring_r=47, nseg=30, ring_t=4, tile=12,
        dash_r=None, tick=(60, 63), nticks=60, bezel=None, mark=(58.2, 67), arm=17, thick=2,
        seg_g=4, glow_w=5.5, body_w=2.2, core_w=1.0, dim_w=1.3,
        node_r=3.5, node_w=1.4, node_box=18, ping_box=30, pulse=20, spark=12,
        wordmark=(132, 22, 13.0), entry=(300, 40), cut=8, line=1.0),
    Set(prefix="m_", u=36, logo=150, plate_r=68, ring_r=84, nseg=60, ring_t=7, tile=14,
        dash_r=101, tick=(108, 114), nticks=120, bezel=240, mark=(104.8, 119), arm=30, thick=2,
        seg_g=5, glow_w=7.5, body_w=2.8, core_w=1.3, dim_w=1.5,
        node_r=5, node_w=1.6, node_box=22, ping_box=42, pulse=34, spark=18,
        wordmark=(216, 32, 20.5), entry=(400, 48), cut=10, line=1.0),
    Set(prefix="l_", u=54, logo=225, plate_r=102, ring_r=126, nseg=60, ring_t=10, tile=20,
        dash_r=151.5, tick=(162, 171), nticks=120, bezel=360, mark=(157, 178), arm=45, thick=3,
        seg_g=7, glow_w=11, body_w=4.0, core_w=1.9, dim_w=2.0,
        node_r=7.5, node_w=2.2, node_box=30, ping_box=62, pulse=50, spark=26,
        wordmark=(300, 44, 28.5), entry=(480, 56), cut=12, line=1.5),
]

written = []


def f(x):
    """A number as SVG wants it: at most three decimals, no exponent."""
    s = "%.3f" % x
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def mix(top, under, share):
    """The colour of `top` at opacity `share` over `under` (both "#RRGGBB")."""
    return "#" + "".join("%02X" % int(round(int(top[i:i + 2], 16) * share + int(under[i:i + 2], 16) * (1 - share)))
                         for i in (1, 3, 5))


def svg(w, h, body, defs=""):
    return ('<svg xmlns="%s" width="%d" height="%d" viewBox="0 0 %d %d">'
            '%s%s</svg>' % (SVG_NS, w, h, w, h, "<defs>%s</defs>" % defs if defs else "", body))


def render(outdir, name, text, width=None, height=None):
    path = os.path.join(outdir, name)
    cmd = ["rsvg-convert", "-o", path]
    if width:
        cmd += ["-w", str(width), "-h", str(height)]
    subprocess.run(cmd, input=text.encode("utf-8"), check=True)
    written.append(path)


def polar(r, deg, ox=0.0, oy=0.0):
    """Screen coordinates: degrees clockwise from east, y grows downwards."""
    a = math.radians(deg)
    return ox + r * math.cos(a), oy + r * math.sin(a)


# ---- the symbol, split into its parts ------------------------------------------
def symbol_parts():
    """Returns (defs, hexagon paths, S path, the four node circles) of the logo."""
    root = ET.parse(SYMBOL).getroot()
    tag = lambda name: "{%s}%s" % (SVG_NS, name)
    defs = root.findall(tag("defs"))
    paths = root.findall(tag("path"))
    circles = root.findall(tag("circle"))
    others = [e for e in root if e.tag not in (tag("title"), tag("defs"), tag("path"), tag("circle"))]
    if (len(defs) != 1 or len(paths) != 3 or len(circles) != 4 or others
            or paths[0].get("d") != paths[1].get("d") or paths[2].get("d") == paths[0].get("d")
            or root.get("viewBox") != "0 0 256 256"):
        sys.exit("gen-plymouth-theme: %s no longer has the expected structure "
                 "(2 hexagon paths, 1 S path, 4 circles in a 256 box): "
                 "adapt symbol_parts()" % SYMBOL)
    ET.register_namespace("", SVG_NS)
    text = lambda e: ET.tostring(e, encoding="unicode")
    return text(defs[0]), [text(p) for p in paths[:2]], text(paths[2]), [text(c) for c in circles], paths, circles


def symbol_svg(defs, elements):
    return '<svg xmlns="%s" width="256" height="256" viewBox="0 0 256 256">%s%s</svg>' % (
        SVG_NS, defs, "".join(elements))


def logo_images(out, s, parts):
    defs, hexes, spath, circles, paths, raw_circles = parts
    n = s.logo
    render(out, s.prefix + "logo_hex.png", symbol_svg(defs, hexes), n, n)
    render(out, s.prefix + "logo_s.png", symbol_svg(defs, [spath] + circles), n, n)
    # The cyan node alone: the first thing that lights at boot.
    render(out, s.prefix + "logo_dot.png", symbol_svg(defs, circles[:2]), n, n)
    # Flat one-colour copies for the three-tick "lock" beat.
    for name, colour in (("c", CYAN), ("v", VIOLET)):
        flat = ['<path d="%s" fill="none" stroke="%s" stroke-width="13" stroke-linejoin="round"/>'
                % (paths[1].get("d"), colour),
                '<path d="%s" fill="none" stroke="%s" stroke-width="16" stroke-linecap="round" '
                'stroke-linejoin="round"/>' % (paths[2].get("d"), colour)]
        for c in (raw_circles[0], raw_circles[2]):
            flat.append('<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (c.get("cx"), c.get("cy"), c.get("r"), colour))
        render(out, s.prefix + "logo_ghost_%s.png" % name,
               symbol_svg("", ['<g opacity="0.6">%s</g>' % "".join(flat)]), n, n)


# ---- instrument: plate, bezel frames, progress ring ----------------------------
def ticks(c, r0, r1, count, turn, colour, opacity, width):
    out = []
    for i in range(count):
        a = i * 360.0 / count + turn
        x0, y0 = polar(r0, a, c, c)
        x1, y1 = polar(r1, a, c, c)
        out.append("M%s %sL%s %s" % (f(x0), f(y0), f(x1), f(y1)))
    return '<path d="%s" stroke="%s" stroke-opacity="%s" stroke-width="%s" fill="none"/>' % (
        "".join(out), colour, f(opacity), f(width))


def plate(out, s):
    size = 2 * (s.tick[1] + 8)
    c = size / 2.0
    body = ['<circle cx="%s" cy="%s" r="%s" fill="%s" fill-opacity="0.82"/>' % (f(c), f(c), f(s.plate_r), PLATE),
            '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-opacity="0.34" stroke-width="%s"/>'
            % (f(c), f(c), f(s.plate_r), SLATE, f(s.line))]
    if s.bezel is None:
        # The small set has no moving bezel: its ticks are part of the plate.
        body.append(ticks(c, s.tick[0], s.tick[1], s.nticks, 0, SLATE, 0.5, 1))
    marks = []
    for i in range(6):
        x0, y0 = polar(s.mark[0], i * 60, c, c)
        x1, y1 = polar(s.mark[1], i * 60, c, c)
        marks.append("M%s %sL%s %s" % (f(x0), f(y0), f(x1), f(y1)))
    body.append('<path d="%s" stroke="%s" stroke-width="%s" fill="none"/>' % ("".join(marks), CYAN, f(1.6 * s.line)))
    render(out, s.prefix + "plate.png", svg(size, size, "".join(body)))


def bezel(out, s, frame):
    """Frame i: ticks turned +0.5*i degrees, dashes -0.8333*i degrees. The tick
    ring repeats every 3 degrees and the dash ring every 5, so six frames loop."""
    size = s.bezel
    c = size / 2.0
    body = [ticks(c, s.tick[0], s.tick[1], s.nticks, 0.5 * frame, SLATE, 0.5, s.line)]
    dashes = []
    for i in range(72):
        a0 = i * 5.0 - frame * 5.0 / 6.0
        x0, y0 = polar(s.dash_r, a0, c, c)
        x1, y1 = polar(s.dash_r, a0 + 2.4, c, c)
        dashes.append("M%s %sA%s %s 0 0 1 %s %s" % (f(x0), f(y0), f(s.dash_r), f(s.dash_r), f(x1), f(y1)))
    body.append('<path d="%s" stroke="%s" stroke-opacity="0.8" stroke-width="%s" fill="none"/>'
                % ("".join(dashes), VIOLET, f(1.5 * s.line)))
    render(out, s.prefix + "bezel%d.png" % frame, svg(size, size, "".join(body)))


def ring_strip(out, s, lit):
    """One strip of nseg tiles. Tile i holds segment i exactly as it lies on the
    ring, already turned; the script cuts the strip once and puts tile i at
        centre + floor(r * cos(a) - tile / 2 + 0.501)   (the same for y, sin)
    so the part of the position that is lost by rounding is drawn into the tile."""
    n, tile, r, th = s.nseg, s.tile, s.ring_r, s.ring_t
    pitch = 360.0 / n
    tiles = []
    for i in range(n):
        ac = -90 + (i + 0.5) * pitch
        ix = math.floor(r * math.cos(math.radians(ac)) - tile / 2.0 + 0.501)
        iy = math.floor(r * math.sin(math.radians(ac)) - tile / 2.0 + 0.501)
        ox, oy = -ix, -iy                       # ring centre in tile coordinates
        a0, a1 = ac - 0.35 * pitch, ac + 0.35 * pitch

        def sector(grow):
            ro, ri = r + th / 2.0 + grow, r - th / 2.0 - grow
            da = math.degrees(grow / r)
            p = [polar(ro, a0 - da, ox, oy), polar(ro, a1 + da, ox, oy),
                 polar(ri, a1 + da, ox, oy), polar(ri, a0 - da, ox, oy)]
            return "M%s %sA%s %s 0 0 1 %s %sL%s %sA%s %s 0 0 0 %s %sZ" % (
                f(p[0][0]), f(p[0][1]), f(ro), f(ro), f(p[1][0]), f(p[1][1]),
                f(p[2][0]), f(p[2][1]), f(ri), f(ri), f(p[3][0]), f(p[3][1]))
        if lit:
            shapes = ('<path d="%s" fill="%s" fill-opacity="0.22"/><path d="%s" fill="%s"/>'
                      '<path d="%s" fill="%s" fill-opacity="0.75"/>'
                      % (sector(1.6), CYAN, sector(0), CYAN, sector(-th * 0.28), PALE))
        else:
            shapes = '<path d="%s" fill="%s" fill-opacity="0.34"/>' % (sector(0), SLATE)
        tiles.append('<svg x="%d" y="0" width="%d" height="%d">%s</svg>' % (i * tile, tile, tile, shapes))
    render(out, s.prefix + ("ring_lit.png" if lit else "ring_dim.png"), svg(n * tile, tile, "".join(tiles)))


# ---- network: unit segments, nodes, pings, pulses ------------------------------
def segment(out, s, o, state):
    """One trace segment of length u at o * 30 degrees (counter-clockwise from
    east). The line starts G pixels inside the image: at the left for 0..90
    degrees, at the right for 120 and 150, at the bottom except for 0 degrees.
    The script relies on exactly this placement."""
    a = math.radians(o * 30)
    dx, dy = s.u * math.cos(a), -s.u * math.sin(a)
    g = s.seg_g
    w = int(math.ceil(abs(dx) - 0.000001)) + 2 * g
    h = int(math.ceil(abs(dy) - 0.000001)) + 2 * g
    x0 = g if o <= 3 else w - g
    y0 = g if o == 0 else h - g
    d = "M%s %sL%s %s" % (f(x0), f(y0), f(x0 + dx), f(y0 + dy))
    if state == "dim":
        body = '<path d="%s" stroke="%s" stroke-width="%s" stroke-linecap="round"/>' % (d, UNPOWERED, f(s.dim_w))
    else:
        colour = {"c": CYAN, "b": BLUE, "v": VIOLET}[state]
        # The glow has square ends so that two segments in a row do not add
        # up to a brighter blob at the joint; the solid strokes are rounded.
        # The next segment is drawn over the end of this one, so its rounded
        # body would hide the last pixel of this core and leave a dark notch
        # at every joint. The core is therefore opaque (the 90 % mix is in
        # its colour) and runs on to the tip of the body's round end.
        e = (s.body_w - s.core_w) / 2.0
        ex, ey = e * math.cos(a), -e * math.sin(a)
        dc = "M%s %sL%s %s" % (f(x0 - ex), f(y0 - ey), f(x0 + dx + ex), f(y0 + dy + ey))
        body = ('<path d="%s" stroke="%s" stroke-opacity="0.25" stroke-width="%s"/>'
                '<path d="%s" stroke="%s" stroke-width="%s" stroke-linecap="round"/>'
                '<path d="%s" stroke="%s" stroke-width="%s" stroke-linecap="round"/>'
                % (d, colour, f(s.glow_w), d, colour, f(s.body_w), dc, mix(PALE, colour, 0.9), f(s.core_w)))
    render(out, s.prefix + "seg%d_%s.png" % (o, state), svg(w, h, body))


def nodes(out, s):
    box, c = s.node_box, s.node_box / 2.0
    for state, colour in (("dim", UNPOWERED), ("c", CYAN), ("b", BLUE), ("v", VIOLET)):
        body = ""
        if state != "dim":
            body += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-opacity="0.25" stroke-width="%s"/>' % (
                f(c), f(c), f(s.node_r), colour, f(s.node_w * 3.2))
        body += '<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="%s"/>' % (
            f(c), f(c), f(s.node_r), BG, colour, f(s.node_w))
        if state != "dim":
            body += '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (f(c), f(c), f(s.node_r * 0.32), PALE)
        render(out, s.prefix + "node_%s.png" % state, svg(box, box, body))
    # Four frames of a ring that opens and fades: shown when a node lights.
    box, c = s.ping_box, s.ping_box / 2.0
    for i, (grow, alpha) in enumerate(((0.18, 0.9), (0.42, 0.7), (0.68, 0.45), (0.92, 0.22))):
        r = s.node_r + (c - 2 - s.node_r) * grow
        body = '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-opacity="%s" stroke-width="%s"/>' % (
            f(c), f(c), f(r), PALE, f(alpha), f(s.node_w))
        render(out, s.prefix + "ping%d.png" % i, svg(box, box, body))


def blob(out, name, size, colour, core, strength=1.0):
    """A soft dot: bright core, coloured halo."""
    c = size / 2.0
    defs = ('<radialGradient id="g"><stop offset="0" stop-color="%s" stop-opacity="%s"/>'
            '<stop offset="0.16" stop-color="%s" stop-opacity="%s"/>'
            '<stop offset="0.38" stop-color="%s" stop-opacity="%s"/>'
            '<stop offset="0.7" stop-color="%s" stop-opacity="%s"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
            % (core, f(strength), core, f(0.95 * strength), colour, f(0.55 * strength),
               colour, f(0.12 * strength), colour))
    render(out, name, svg(size, size, '<circle cx="%s" cy="%s" r="%s" fill="url(#g)"/>' % (f(c), f(c), f(c)), defs))


def brackets(out, s):
    a, t = s.arm, s.thick
    for name, sx, sy in (("tl", 0, 0), ("tr", 1, 0), ("bl", 0, 1), ("br", 1, 1)):
        hx = 0 if sx == 0 else a - t          # the vertical arm
        hy = 0 if sy == 0 else a - t          # the horizontal arm
        body = ('<rect x="0" y="%d" width="%d" height="%d" fill="%s"/>'
                '<rect x="%d" y="0" width="%d" height="%d" fill="%s"/>' % (hy, a, t, CYAN, hx, t, a, CYAN))
        render(out, s.prefix + "br_%s.png" % name, svg(a, a, body))


def wordmark(out, s):
    w, h, size = s.wordmark
    # rsvg-convert centres the letters themselves (the tracking after the
    # last letter is not counted), so the anchor is the middle of the image.
    x = w / 2.0
    y = h / 2.0 + 0.36 * size
    text = ('<text x="%s" y="%s" text-anchor="middle" font-family="Inter Variable, Inter" font-weight="600" '
            'font-size="%s" letter-spacing="%s"' % (f(x), f(y), f(size), f(0.32 * size)))
    defs = '<filter id="b" x="-20%%" y="-60%%" width="140%%" height="220%%"><feGaussianBlur stdDeviation="%s"/></filter>' % f(size * 0.16)
    body = ('%s fill="%s" fill-opacity="0.55" filter="url(#b)">SHADOW OS</text>%s fill="%s">SHADOW OS</text>'
            % (text, CYAN, text, WORDMARK))
    render(out, s.prefix + "wordmark.png", svg(w, h, body, defs))


def entry(out, s):
    w, h = s.entry
    c = s.cut
    # Corners cut at the top left and the bottom right.
    d = "M%s 0.5H%sV%sL%s %sH0.5V%sZ" % (f(c + 0.5), f(w - 0.5), f(h - c - 0.5), f(w - c - 0.5), f(h - 0.5), f(c + 0.5))
    render(out, s.prefix + "entry.png", svg(w, h,
           '<path d="%s" fill="%s" fill-opacity="0.94" stroke="%s" stroke-opacity="0.55" stroke-width="1"/>' % (d, PLATE, SLATE)))
    render(out, s.prefix + "entry_lit.png", svg(w, h,
           '<path d="%s" fill="none" stroke="%s" stroke-opacity="0.3" stroke-width="3"/>'
           '<path d="%s" fill="none" stroke="%s" stroke-width="1"/>' % (d, CYAN, d, CYAN)))


# ---- shared images --------------------------------------------------------------
def shared(out):
    # Core glow behind the logo (scaled once at load to 5.5 units).
    defs = ('<radialGradient id="g"><stop offset="0" stop-color="%s" stop-opacity="0.5"/>'
            '<stop offset="0.45" stop-color="%s" stop-opacity="0.2"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>' % (CYAN, CYAN, CYAN))
    render(out, "glow.png", svg(256, 256, '<circle cx="128" cy="128" r="128" fill="url(#g)"/>', defs))

    # Field: the corner points of a honeycomb of pointy-top hexagons, fading
    # towards the edges. Drawn once, behind the instrument.
    w, h, a = 960, 540, 21.0
    levels = {}
    for i in range(-40, 41):
        for j in range(-60, 61):
            if (i - j) % 3 == 0:
                continue                       # the centre of a hexagon: no dot
            x = w / 2.0 + i * a * math.sqrt(3) / 2.0
            y = h / 2.0 + j * a + i * a / 2.0
            fall = 1 - ((x - w / 2.0) / (w / 2.0 - 6)) ** 2 - ((y - h / 2.0) / (h / 2.0 - 6)) ** 2
            if fall <= 0.02:
                continue
            level = int(round(min(1.0, fall ** 1.3) * 8))
            if level:
                levels.setdefault(level, []).append((x, y))
    body = []
    for level in sorted(levels):
        d = "".join("M%s %sh0.01" % (f(x), f(y)) for x, y in levels[level])
        body.append('<path d="%s" stroke="%s" stroke-opacity="%s" stroke-width="2.2" stroke-linecap="round"/>'
                    % (d, SLATE, f(0.07 + 0.045 * level)))
    render(out, "field.png", svg(w, h, "".join(body)))

    # Scan beam: a two-pixel hot line with a tail above it (it travels down),
    # fading towards both ends. Scaled once to the screen width.
    defs = ('<linearGradient id="v" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="%s" stop-opacity="0"/><stop offset="0.72" stop-color="%s" stop-opacity="0.34"/>'
            '<stop offset="0.78" stop-color="%s" stop-opacity="1"/><stop offset="1" stop-color="%s" stop-opacity="1"/>'
            '</linearGradient>'
            '<linearGradient id="hh" x1="0" y1="0" x2="1" y2="0">'
            '<stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset="0.3" stop-color="#fff" stop-opacity="1"/>'
            '<stop offset="0.7" stop-color="#fff" stop-opacity="1"/><stop offset="1" stop-color="#fff" stop-opacity="0"/>'
            '</linearGradient><mask id="m"><rect width="512" height="9" fill="url(#hh)"/></mask>' % (CYAN, CYAN, HOT, HOT))
    render(out, "scan.png", svg(512, 9, '<rect width="512" height="9" fill="url(#v)" mask="url(#m)"/>', defs))

    # Screen-corner brackets (the small set uses the inner 24 pixels).
    a = 44
    for name, sx, sy in (("tl", 0, 0), ("tr", 1, 0), ("bl", 0, 1), ("br", 1, 1)):
        x = 0.75 if sx == 0 else a - 0.75
        y = 0.75 if sy == 0 else a - 0.75
        d = "M%s %sV%sH%s" % (f(x), f(a - y), f(y), f(a - x))
        render(out, "corner_%s.png" % name, svg(a, a,
               '<path d="%s" fill="none" stroke="%s" stroke-opacity="0.45" stroke-width="1.5"/>' % (d, SLATE)))

    # Passphrase cells: one small pointy-top hexagon per typed character.
    hexagon = "M7 1L12.6 4.5V11.5L7 15L1.4 11.5V4.5Z"
    render(out, "cell.png", svg(14, 16,
           '<path d="%s" fill="%s" fill-opacity="0.42" stroke="%s" stroke-width="1.4" stroke-linejoin="round"/>' % (hexagon, CYAN, CYAN)))
    render(out, "cell_hot.png", svg(14, 16,
           '<path d="%s" fill="%s" stroke="%s" stroke-width="1.4" stroke-linejoin="round"/>' % (hexagon, PALE, PALE)))
    # (Slightly translucent on purpose: rsvg-convert writes a fully opaque image
    # as RGB, and every image of the theme is kept RGBA.)
    render(out, "caret.png", svg(2, 20, '<rect width="2" height="20" fill="%s" fill-opacity="0.96"/>' % PALE))
    # "There are more characters than cells": two chevrons.
    render(out, "more.png", svg(14, 16,
           '<path d="M2 3.5L6 8L2 12.5M8 3.5L12 8L8 12.5" fill="none" stroke="%s" stroke-width="1.6"/>' % SLATE))
    # Mark in front of a message line (symbol glyphs are boxes in the initramfs).
    render(out, "msg_mark.png", svg(10, 10,
           '<path d="M2.5 1.5L7 5L2.5 8.5" fill="none" stroke="%s" stroke-width="1.6"/>' % CYAN))
    # Connector from the port node to the prompt (scaled once to its length).
    render(out, "conn.png", svg(2, 2, '<rect width="2" height="2" fill="%s" fill-opacity="0.9"/>' % VIOLET))


def check_png(path):
    with open(path, "rb") as fh:
        head = fh.read(33)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        sys.exit("gen-plymouth-theme: %s is not a PNG" % path)
    width, height, depth, colour, _comp, _filt, interlace = struct.unpack(">IIBBBBB", head[16:29])
    if depth != 8 or colour != 6 or interlace != 0:
        sys.exit("gen-plymouth-theme: %s is not 8-bit RGBA without interlacing "
                 "(depth %d, colour type %d, interlace %d)" % (path, depth, colour, interlace))
    return width, height


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    out = argv[1]
    family = subprocess.run(["fc-match", "-f", "%{family}", "Inter Variable"],
                            stdout=subprocess.PIPE, check=True).stdout.decode()
    if "Inter" not in family:
        sys.exit("gen-plymouth-theme: the Inter font is not installed (fc-match gives %r); "
                 "the wordmark would be drawn in another face" % family)
    os.makedirs(out, exist_ok=True)
    parts = symbol_parts()
    for s in SETS:
        logo_images(out, s, parts)
        plate(out, s)
        if s.bezel:
            for frame in range(6):
                bezel(out, s, frame)
        ring_strip(out, s, False)
        ring_strip(out, s, True)
        for o in range(6):
            for state in ("dim", "c", "b", "v"):
                segment(out, s, o, state)
        nodes(out, s)
        blob(out, s.prefix + "pulse_c.png", s.pulse, CYAN, PALE)
        blob(out, s.prefix + "pulse_v.png", s.pulse, VIOLET, "#DDD6FE")
        blob(out, s.prefix + "spark.png", s.spark, CYAN, PALE, 0.9)
        brackets(out, s)
        wordmark(out, s)
        entry(out, s)
    shared(out)
    total = 0
    for path in written:
        check_png(path)
        total += os.path.getsize(path)
    print("gen-plymouth-theme: %d images, %d bytes, in %s" % (len(written), total, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
