#!/bin/bash
# Regenerates the binary parts of the boot menu theme (GRUB gfxmenu):
#   assets/boot/grub/shadow/   images (*.png, icons/*.png) and fonts (*.pf2)
#
#   gen-grub-theme.sh [--images-only | --fonts-only] OUTDIR
#
# OUTDIR receives a complete theme: the generated files plus a copy of the
# hand-written theme.txt, so it can be looked at with a GRUB preview before it
# is copied over assets/boot/grub/shadow/.
#
# Images need python3 (standard library only) and rsvg-convert, plus the Inter
# and JetBrains Mono fonts for the two pictures that hold text (wordmark and
# key hints). Fonts need grub-mkfont and the same two font families.
# The build chroot has rsvg-convert but no grub-mkfont, so there run
#   scripts/dev.sh exec -- /src/assets/src/boot/gen-grub-theme.sh --images-only /tmp/grub-theme
# and build the fonts with --fonts-only on a Debian trixie system that has the
# package grub-common. See docs/architecture/boot-menu.md.
#
# Rules this script keeps, because GRUB is strict about them (each one was seen
# to break the menu in a real GRUB 2.12):
#   - every image is an 8-bit RGB or RGBA PNG, not interlaced. A palette or
#     grey PNG is drawn wrong; one unreadable picture makes GRUB drop the
#     whole theme, wait 10 seconds and show its plain text menu;
#   - pictures are made at their final pixel size (GRUB never shrinks them);
#   - the stretched parts of a box (edges, the centre) hold no texture;
#   - the PF2 font names are exactly the names theme.txt asks for.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"                 # the assets/ directory
THEME_TXT="$SRC/boot/grub/shadow/theme.txt"

usage() { sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "gen-grub-theme.sh: $*" >&2; exit 1; }

IMAGES=1; FONTS=1; OUT=""
for arg in "$@"; do
    case "$arg" in
        --images-only) FONTS=0 ;;
        --fonts-only) IMAGES=0 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option $arg" ;;
        *) [ -z "$OUT" ] || die "only one output directory, please"; OUT="$arg" ;;
    esac
done
[ -n "$OUT" ] || { usage >&2; exit 2; }
[ "$IMAGES" = 1 ] || [ "$FONTS" = 1 ] || die "--images-only and --fonts-only exclude each other"

command -v fc-match >/dev/null || die "fc-match not found (package fontconfig)"
if [ "$IMAGES" = 1 ]; then
    command -v python3 >/dev/null || die "python3 not found"
    command -v rsvg-convert >/dev/null || die "rsvg-convert not found (package librsvg2-bin); --fonts-only skips the images"
fi
if [ "$FONTS" = 1 ]; then
    command -v grub-mkfont >/dev/null || die "grub-mkfont not found (package grub-common); --images-only skips the fonts"
fi

# The font files. fc-match always answers with some file, so check that the
# answer really is the family that was asked for.
font_file() {    # font_file PATTERN WORD-EXPECTED-IN-THE-FILE-NAME
    local f
    f="$(fc-match -f '%{file}' "$1")"
    case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in
        *"$2"*) printf '%s\n' "$f" ;;
        *) die "font '$1' is not installed (fc-match gave $f)" ;;
    esac
}
# Read the list first: "fc-list | grep -q" under pipefail counts as failed when
# grep stops reading early on a system with many fonts.
FAMILIES="$(fc-list : family)"
if grep -qx 'Inter Variable' <<<"$FAMILIES"; then
    SANS_FAMILY="Inter Variable"                  # Debian's fonts-inter-variable
else
    SANS_FAMILY="Inter"
fi
SANS="$(font_file "$SANS_FAMILY" inter)"
MONO="$(font_file 'JetBrains Mono:style=Regular' jetbrainsmono-regular)"
MONO_BOLD="$(font_file 'JetBrains Mono:style=Bold' jetbrainsmono-bold)"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# ---- images -------------------------------------------------------------------
if [ "$IMAGES" = 1 ]; then
    mkdir -p "$OUT/icons"
    SANS_FAMILY="$SANS_FAMILY" python3 - "$SRC" "$OUT" <<'PY'
import math
import os
import re
import struct
import subprocess
import sys
import zlib

SRC, OUT = sys.argv[1], sys.argv[2]
SANS_FAMILY = os.environ["SANS_FAMILY"]

# The shared palette of the boot and session surfaces.
CYAN, PALE, HOT = "#22D3EE", "#A5F3FC", "#CFFAFE"
BLUE, VIOLET = "#60A5FA", "#8B5CF6"
SLATE, TEXT, TEXT2 = "#94A3B8", "#E2E8F0", "#9FB0C3"
WORDMARK = "#CBD5E1"
UNLIT = "#2E4A5C"                               # a trace without power


# ---- a small PNG reader and writer --------------------------------------------
# An image here is (width, height, channels, pixels): 3 or 4 bytes per pixel,
# rows one after the other. Writing the files ourselves guarantees the format
# GRUB needs (8 bits per channel, colour type 2 or 6, not interlaced).

def png_decode(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("gen-grub-theme: rsvg-convert did not write a PNG")
    pos, idat, head = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
    w, h, depth, ctype, _, _, interlace = head
    if depth != 8 or ctype not in (2, 6) or interlace != 0:
        raise SystemExit("gen-grub-theme: unexpected PNG kind from rsvg-convert: %r" % (head,))
    n = 3 if ctype == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = w * n
    pix = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        base = y * (stride + 1)
        ftype = raw[base]
        row = bytearray(raw[base + 1:base + 1 + stride])
        if ftype == 1:
            for i in range(n, stride):
                row[i] = (row[i] + row[i - n]) & 255
        elif ftype == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 255
        elif ftype == 3:
            for i in range(stride):
                left = row[i - n] if i >= n else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:
            for i in range(stride):
                a = row[i - n] if i >= n else 0
                b = prev[i]
                c = prev[i - n] if i >= n else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pred) & 255
        pix += row
        prev = row
    return (w, h, n, pix)


def png_encode(img):
    w, h, n, pix = img
    stride = w * n
    rows = [bytes(pix[y * stride:(y + 1) * stride]) for y in range(h)]

    def weight(line):                           # small when the bytes are near zero
        return sum(v if v < 128 else 256 - v for v in line)

    # Three candidates per row (as it is, difference to the left, difference
    # to the row above). Either one filter for the whole picture or, row by
    # row, the one with the smallest weight; the smallest file wins.
    whole = [bytearray(), bytearray(), bytearray()]
    mixed = bytearray()
    prev = bytes(stride)
    for row in rows:
        cand = (row,
                row[:n] + bytes((row[i] - row[i - n]) & 255 for i in range(n, stride)),
                bytes((a - b) & 255 for a, b in zip(row, prev)))
        for f in range(3):
            whole[f].append(f)
            whole[f] += cand[f]
        f = min(range(3), key=lambda k: weight(cand[k]))
        mixed.append(f)
        mixed += cand[f]
        prev = row
    best = min((zlib.compress(bytes(raw), 9) for raw in whole + [mixed]), key=len)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    head = struct.pack(">IIBBBBB", w, h, 8, 2 if n == 3 else 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", best) + chunk(b"IEND", b"")


def crop(img, x, y, w, h):
    iw, ih, n, pix = img
    assert 0 <= x and 0 <= y and x + w <= iw and y + h <= ih
    out = bytearray()
    for r in range(y, y + h):
        out += pix[(r * iw + x) * n:(r * iw + x + w) * n]
    return (w, h, n, out)


def opaque_box(img, level=96):
    """Bounding box (x0, y0, x1, y1) of the pixels whose alpha is at least level."""
    w, h, n, pix = img
    xs, ys = [], []
    for y in range(h):
        row = pix[y * w * 4 + 3:(y + 1) * w * 4:4]
        cols = [x for x in range(w) if row[x] >= level]
        if cols:
            ys.append(y)
            xs += (cols[0], cols[-1])
    if not ys:
        raise SystemExit("gen-grub-theme: a text picture came out empty (font missing?)")
    return (min(xs), ys[0], max(xs), ys[-1])


written = []


def save(name, img):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(png_encode(img))
    written.append(name)


def render(svg_body, w, h, defs=""):
    """Rasterise SVG markup at exactly w x h pixels."""
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
           '<defs>%s</defs>%s</svg>' % (w, h, w, h, defs, svg_body))
    done = subprocess.run(["rsvg-convert", "--format=png"], input=svg.encode(),
                          stdout=subprocess.PIPE, check=True)
    img = png_decode(done.stdout)
    if img[:3] != (w, h, 4):
        raise SystemExit("gen-grub-theme: asked for %dx%d RGBA, got %r" % (w, h, img[:3]))
    return img


def nine(img, edge, prefix):
    """Cut a square box picture into the nine slices of a GRUB styled box."""
    w, h = img[0], img[1]
    xs = [(0, edge), (edge, w - 2 * edge), (w - edge, edge)]
    ys = [(0, edge), (edge, h - 2 * edge), (h - edge, edge)]
    names = [["nw", "n", "ne"], ["w", "c", "e"], ["sw", "s", "se"]]
    for r in range(3):
        for c in range(3):
            save("%s_%s.png" % (prefix, names[r][c]), crop(img, xs[c][0], ys[r][0], xs[c][1], ys[r][1]))


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def spoke(cx, cy, r1, r2, deg, style):
    x1, y1 = polar(cx, cy, r1, deg)
    x2, y2 = polar(cx, cy, r2, deg)
    return '<line x1="%.3f" y1="%.3f" x2="%.3f" y2="%.3f" %s/>' % (x1, y1, x2, y2, style)


# ---- background ---------------------------------------------------------------
# 1280x800, computed here pixel by pixel (no SVG): GRUB stretches it to the
# screen, so it holds only soft, wide shapes. The field gradient of every
# Shadow OS boot surface (#04060B to #020306), a cyan glow where the instrument
# sits, a horizon glow along the bottom and three faint wide arcs.
def background():
    W, H = 1280, 800
    cx, cy = W / 2.0, 122.0                     # the instrument's centre at 1280x800
    top, bottom = (4.0, 6.0, 11.0), (2.0, 3.0, 6.0)
    gx = [math.exp(-((x - cx) / 380.0) ** 2) for x in range(W)]
    hx = [math.exp(-((x - cx) / 640.0) ** 2) for x in range(W)]
    dx2 = [(x - cx) ** 2 for x in range(W)]
    arcs = (300.0, 470.0, 660.0)
    pix = bytearray(W * H * 3)
    i = 0
    for y in range(H):
        t = y / (H - 1.0)
        base = [top[k] + (bottom[k] - top[k]) * t for k in range(3)]
        gy = 0.10 * math.exp(-((y - cy) / 240.0) ** 2)
        hy = 0.17 * math.exp(-((y - H * 1.03) / 170.0) ** 2)
        dy2 = (y - cy) ** 2
        for x in range(W):
            g = gy * gx[x]
            hz = hy * hx[x]
            r = base[0] + 34 * g + 40 * hz
            gr = base[1] + 211 * g + 150 * hz
            b = base[2] + 238 * g + 245 * hz
            d = math.sqrt(dx2[x] + dy2)
            for radius in arcs:
                off = abs(d - radius)
                if off < 5.0:
                    a = 0.075 * math.exp(-(off / 1.4) ** 2)
                    r += 148 * a
                    gr += 163 * a
                    b += 184 * a
            pix[i] = int(r + 0.5)
            pix[i + 1] = int(gr + 0.5)
            pix[i + 2] = int(b + 0.5)
            i += 3
    save("background.png", (W, H, 3, pix))


# ---- the instrument at rest ---------------------------------------------------
# 176x176. The Shadow symbol (60 px) on its plate (144 px across): 72 fine
# ticks, six cyan index marks on the hexagon's own directions, a violet dashed
# ring. Round the plate runs a thin track: GRUB puts the countdown dots on it
# (same box in theme.txt) and takes them off one group per second, so the
# track shows how much of the circle is already gone. The picture carries no
# words and stays on screen after a key press, when the dots disappear.
def reticle():
    S, c = 176, 88.0
    symbol = open(os.path.join(SRC, "logos", "shadow-symbol.svg"), encoding="utf-8").read()
    inner = re.search(r"<svg[^>]*>(.*)</svg>", symbol, re.S).group(1)
    inner = re.sub(r"<title>.*?</title>", "", inner, flags=re.S)
    defs = ('<radialGradient id="halo" cx="%g" cy="%g" r="46" gradientUnits="userSpaceOnUse">'
            '<stop offset="0" stop-color="%s" stop-opacity="0.30"/>'
            '<stop offset="0.55" stop-color="%s" stop-opacity="0.10"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>' % (c, c, CYAN, CYAN, CYAN))

    def ring(r, style, cx=c, cy=c):
        return '<circle cx="%g" cy="%g" r="%g" fill="none" %s/>' % (cx, cy, r, style)

    body = ['<circle cx="%g" cy="%g" r="46" fill="url(#halo)"/>' % (c, c)]
    # The dot track. GRUB's integer circle is centred half a pixel off the
    # middle of its box and has a radius of 84 (measured in screenshots).
    body.append(ring(84.0, 'stroke="%s" stroke-opacity="0.26" stroke-width="1"' % SLATE, c + 0.5, c - 0.5))
    # Plate rings.
    body.append(ring(70.5, 'stroke="%s" stroke-opacity="0.34" stroke-width="1"' % SLATE))
    body.append(ring(45.5, 'stroke="%s" stroke-opacity="0.30" stroke-width="1"' % SLATE))
    # 72 fine ticks, one every 5 degrees; every third one a little longer.
    for k in range(72):
        long = (k % 3 == 0)
        body.append(spoke(c, c, 62.0 if long else 64.0, 68.0, k * 5 - 90,
                          'stroke="%s" stroke-opacity="%s" stroke-width="1"' % (SLATE, "0.70" if long else "0.50")))
    # Six cyan index marks at the hexagon's vertices (multiples of 60 degrees
    # from 12 o'clock), each with a wider soft copy below as its glow.
    for k in range(6):
        deg = k * 60 - 90
        body.append(spoke(c, c, 59.0, 71.5, deg, 'stroke="%s" stroke-opacity="0.28" stroke-width="5" stroke-linecap="round"' % CYAN))
        body.append(spoke(c, c, 59.5, 71.5, deg, 'stroke="%s" stroke-width="2"' % CYAN))
        body.append(spoke(c, c, 62.0, 69.0, deg, 'stroke="%s" stroke-width="0.8"' % PALE))
    # Violet dashed ring: 36 dashes of 5 degrees.
    r = 53.0
    dash = 2 * math.pi * r / 72
    body.append(ring(r, 'stroke="%s" stroke-opacity="0.85" stroke-width="1.6" stroke-dasharray="%.4f %.4f" '
                        'transform="rotate(-92.5 %g %g)"' % (VIOLET, dash, dash, c, c)))
    # Short blue marks on the inner ring, on the hexagon's flat sides.
    for k in range(6):
        body.append(spoke(c, c, 41.0, 49.0, k * 60 - 60, 'stroke="%s" stroke-opacity="0.85" stroke-width="1.4"' % BLUE))
    # The symbol itself.
    body.append('<svg x="%g" y="%g" width="60" height="60" viewBox="0 0 256 256">%s</svg>' % (c - 30, c - 30, inner))
    save("reticle.png", render("".join(body), S, S, defs))


# ---- the conduit --------------------------------------------------------------
# 624x92, drawn behind the header: from a node on each side of the instrument
# a thin trace runs outwards under the wordmark (left) and under the place of
# the countdown number (right), branches at a second node and drops at 60
# degrees onto the top edge of the entry frame. Lines only at multiples of 30
# degrees; colour runs cyan, blue, violet with the distance from the core.
# Under each level run hangs a row of short unlit ticks, like the scale of an
# instrument without figures, so that the run on the right does not look
# empty once the countdown number above it is gone.
# It is plain structure: it says nothing and looks the same with or without
# the countdown.
def conduit():
    W, H = 624, 92
    mid, y0 = W / 2.0, 8.5                      # the trace's height: 4%+108 on screen
    x_in, x_branch, y_end = 100.0, 244.0, H - 4.5
    x_end = x_branch + (y_end - y0) / math.tan(math.radians(60))
    defs, body = [], []
    for side, sign in (("l", -1), ("r", 1)):
        defs.append('<linearGradient id="t%s" gradientUnits="userSpaceOnUse" x1="%.1f" y1="0" x2="%.1f" y2="0">'
                    '<stop offset="0" stop-color="%s"/><stop offset="0.62" stop-color="%s"/>'
                    '<stop offset="1" stop-color="%s"/></linearGradient>'
                    % (side, mid + sign * x_in, mid + sign * x_end, CYAN, BLUE, VIOLET))
        # Leave the node rings open: the line stops at their rims.
        path = "M%.2f %.2f L%.2f %.2f M%.2f %.2f L%.2f %.2f" % (
            mid + sign * (x_in + 2.5), y0, mid + sign * (x_branch - 3.0), y0,
            mid + sign * (x_branch + 1.5), y0 + 2.6, mid + sign * x_end, y_end)
        body.append('<path d="%s" fill="none" stroke="url(#t%s)" stroke-opacity="0.16" stroke-width="5" stroke-linecap="round"/>' % (path, side))
        body.append('<path d="%s" fill="none" stroke="url(#t%s)" stroke-opacity="0.90" stroke-width="1.5"/>' % (path, side))
        for k in range(11):                     # the unlit scale: every fifth tick longer
            x = round(mid + sign * (x_in + 12 + k * 12))
            body.append('<rect x="%d" y="%d" width="1" height="%d" fill="%s"/>'
                        % (x, int(y0) + 4, 6 if k % 5 == 0 else 3, UNLIT))
        body.append('<circle cx="%.2f" cy="%.2f" r="2.5" fill="none" stroke="%s" stroke-width="1.3"/>' % (mid + sign * x_in, y0, CYAN))
        body.append('<circle cx="%.2f" cy="%.2f" r="3" fill="none" stroke="%s" stroke-width="1.3"/>' % (mid + sign * x_branch, y0, BLUE))
    save("conduit.png", render("".join(body), W, H, "".join(defs)))


def small_parts():
    # One countdown dot (GRUB places 40 of them on a circle and removes them
    # one group per second). 7x7: a pale core, a cyan body and a soft rim.
    defs = ('<radialGradient id="d" cx="3.5" cy="3.5" r="3.5" gradientUnits="userSpaceOnUse">'
            '<stop offset="0" stop-color="%s"/><stop offset="0.45" stop-color="%s"/>'
            '<stop offset="0.70" stop-color="%s" stop-opacity="0.55"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>' % (HOT, CYAN, CYAN, CYAN))
    save("tick.png", render('<circle cx="3.5" cy="3.5" r="3.5" fill="url(#d)"/>', 7, 7, defs))
    # The circle's centre picture must exist; ours is empty (the instrument is
    # its own image so that it stays after a key press).
    save("blank.png", (1, 1, 4, bytearray(4)))

    # Screen corner brackets: square-ended L shapes, 2 px thick, 40 px long.
    st = 'fill="%s" fill-opacity="0.55"' % SLATE
    shapes = {
        "nw": '<rect x="0" y="0" width="40" height="2" %s/><rect x="0" y="0" width="2" height="40" %s/>',
        "ne": '<rect x="0" y="0" width="40" height="2" %s/><rect x="38" y="0" width="2" height="40" %s/>',
        "sw": '<rect x="0" y="38" width="40" height="2" %s/><rect x="0" y="0" width="2" height="40" %s/>',
        "se": '<rect x="0" y="38" width="40" height="2" %s/><rect x="38" y="0" width="2" height="40" %s/>',
    }
    for name, shape in shapes.items():
        save("corner_%s.png" % name, render(shape % (st, st), 40, 40))


# ---- boxes --------------------------------------------------------------------
def boxes():
    # The frame round the entry list: 12 px slices, slate hairline, a dark
    # see-through fill, corners cut at the top left and the bottom right. The
    # cut corners carry a short cyan stroke. Edges and centre are flat colour
    # because GRUB stretches them.
    def frame(size, cut, fill, line, accent):
        e = size - 0.5
        pts = [(cut + 0.5, 0.5), (e, 0.5), (e, e - cut), (e - cut, e), (0.5, e), (0.5, cut + 0.5)]
        path = "M" + " L".join("%.1f %.1f" % p for p in pts) + "Z"
        out = '<path d="%s" %s/>' % (path, fill)
        out += '<path d="%s" fill="none" %s stroke-width="1"/>' % (path, line)
        if accent:
            out += '<path d="M0.5 %.1f L%.1f 0.5" fill="none" %s/>' % (cut + 0.5, cut + 0.5, accent)
            out += '<path d="M%.1f %.1f L%.1f %.1f" fill="none" %s/>' % (e, e - cut, e - cut, e, accent)
        return out

    menu = frame(36, 9, 'fill="rgb(7,12,22)" fill-opacity="0.80"',
                 'stroke="%s" stroke-opacity="0.42"' % SLATE,
                 'stroke="%s" stroke-width="2" stroke-linecap="square"' % CYAN)
    nine(render(menu, 36, 36), 12, "menu")

    # The frame round the command line and the entry editor: 14 px slices,
    # solid, so that nothing of the menu below shows through. GRUB paints the
    # inside black itself.
    term = frame(42, 10, 'fill="rgb(3,5,10)"',
                 'stroke="%s" stroke-opacity="0.70"' % CYAN,
                 'stroke="%s" stroke-width="2" stroke-linecap="square"' % PALE)
    nine(render(term, 42, 42), 14, "term")

    # Entry rows: one centre slice each, drawn at exactly the width GRUB gives
    # a row (616 - 2 * 12 = 592) and the row height, so nothing is stretched
    # and the selected row sits exactly on its slot. The left 44 px stay free
    # for the bar and the entry's icon.
    CW, RH = 592, 40
    item = ('<rect x="0" y="39" width="%d" height="1" fill="%s" fill-opacity="0.16"/>' % (CW, SLATE)
            + '<rect x="0" y="13" width="2" height="14" fill="%s" fill-opacity="0.50"/>' % SLATE)
    save("item_c.png", render(item, CW, RH))

    cut = 12
    shape = "M0.5 0.5 L%.1f 0.5 L%.1f %.1f L%.1f %.1f L0.5 %.1f Z" % (
        CW - 0.5, CW - 0.5, RH - 0.5 - cut, CW - 0.5 - cut, RH - 0.5, RH - 0.5)
    defs = ('<linearGradient id="f" x1="0" y1="0" x2="%d" y2="0" gradientUnits="userSpaceOnUse">'
            '<stop offset="0" stop-color="%s" stop-opacity="0.20"/>'
            '<stop offset="0.5" stop-color="%s" stop-opacity="0.13"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0.09"/></linearGradient>' % (CW, CYAN, CYAN, CYAN))
    sel = '<path d="%s" fill="url(#f)"/>' % shape
    sel += '<path d="%s" fill="none" stroke="%s" stroke-opacity="0.85" stroke-width="1"/>' % (shape, CYAN)
    sel += '<rect x="0" y="0" width="4" height="%d" fill="%s"/>' % (RH, CYAN)
    sel += '<rect x="1" y="3" width="1" height="%d" fill="%s"/>' % (RH - 6, PALE)
    # Three short tabs on the top edge, fading to the right. They sit above
    # the text line on purpose: GRUB cannot keep a title away from the row's
    # right end (a long one is simply cut at the edge), so nothing may be
    # drawn at text height right of the icon.
    for k, alpha in enumerate((1.0, 0.62, 0.30)):
        sel += ('<rect x="%d" y="0" width="12" height="3" fill="%s" fill-opacity="%.2f"/>'
                % (CW - 64 + k * 16, CYAN, alpha))
    save("sel_c.png", render(sel, CW, RH, defs))

    # Scrollbar (shown only when the entries do not fit): a faint track and a
    # cyan thumb in the middle of a 10 px strip, which GRUB puts into the
    # frame's right edge. Flat colour, they are stretched to their length.
    save("sb_frame_c.png", render('<rect x="4" y="0" width="2" height="8" fill="%s" fill-opacity="0.28"/>' % SLATE, 10, 8))
    save("sb_thumb_c.png", render('<rect x="3" y="0" width="4" height="8" fill="%s"/>' % CYAN, 10, 8))

    # Countdown bar: only a 2 px line under GRUB's own "Ns remaining." text.
    # 28 px high like the bar in theme.txt (GRUB makes a bar at least as high
    # as its text needs, 27 px with the 16 px sans font, and would stretch a
    # lower picture).
    save("pb_c.png", render('<rect x="0" y="26" width="8" height="2" fill="%s" fill-opacity="0.30"/>' % SLATE, 8, 28))
    save("pbh_c.png", render('<rect x="0" y="25" width="8" height="1" fill="%s" fill-opacity="0.35"/>' % CYAN
                             + '<rect x="0" y="26" width="8" height="2" fill="%s"/>' % CYAN, 8, 28))


# ---- pictures that hold text --------------------------------------------------
# Real GRUB text is one flat colour without spacing control, and under UEFI
# Secure Boot only the built-in 16 px Unifont may load. So the two designed
# pieces of text are pictures.
def wordmark():
    # "SHADOW OS", Inter SemiBold capitals, tracking 0.32 em, with a soft cyan
    # glow; the same recipe as the wordmark of the boot splash. 150x24, the
    # letters ending 6 px before the right edge (the side of the instrument).
    W, H, size = 150, 24, 15.0
    text = ('<text x="%%.2f" y="%%.2f" font-family="\'%s\'" font-weight="600" font-size="%.1f" '
            'letter-spacing="%.2f" %%s>SHADOW OS</text>' % (SANS_FAMILY, size, size * 0.32))
    probe = render(text % (40, 60, 'fill="#FFFFFF"'), 420, 100)
    x0, y0, x1, y1 = opaque_box(probe)
    if x1 - x0 + 1 > W - 12:
        raise SystemExit("gen-grub-theme: the wordmark is %d px wide, too wide for %d" % (x1 - x0 + 1, W))
    px = 40 + (W - 6 - 1 - x1)
    py = 60 + ((H - (y1 - y0 + 1)) // 2 - y0)
    defs = '<filter id="g" x="-10%" y="-80%" width="120%" height="260%"><feGaussianBlur stdDeviation="2.4"/></filter>'
    body = text % (px, py, 'fill="%s" fill-opacity="0.55" filter="url(#g)"' % CYAN)
    body += text % (px, py, 'fill="%s"' % WORDMARK)
    save("wordmark.png", render(body, W, H, defs))


def hints():
    # The keys that work in this menu: a framed key name, then what it does.
    # JetBrains Mono capitals; every letter is placed on its own so that the
    # layout does not depend on how a renderer spaces text. 440x16.
    W, H, size = 440, 16, 10.5
    step = 7.5                                   # letter pitch
    pad, gap, group_gap = 6.0, 8.0, 30.0
    groups = (("ENTER", "BOOT"), ("E", "EDIT"), ("C", "CONSOLE"), ("ESC", "BACK"))
    total = sum(len(k) * step + 2 * pad + gap + len(a) * step for k, a in groups) + group_gap * (len(groups) - 1)
    if total > W:
        raise SystemExit("gen-grub-theme: the key hints need %d px, more than %d" % (total, W))
    x = math.floor((W - total) / 2.0)
    body = []

    def letters(word, left, colour):
        for n, ch in enumerate(word):
            body.append('<text x="%.2f" y="12" text-anchor="middle" font-family="\'JetBrains Mono\'" font-weight="500" '
                        'font-size="%.1f" fill="%s">%s</text>' % (left + (n + 0.5) * step, size, colour, ch))

    for key, action in groups:
        kw = len(key) * step + 2 * pad
        kx = round(x)
        body.append('<rect x="%.1f" y="0.5" width="%d" height="15" fill="%s" fill-opacity="0.10" '
                    'stroke="%s" stroke-opacity="0.75" stroke-width="1"/>' % (kx + 0.5, round(kw) - 1, CYAN, CYAN))
        letters(key, kx + pad, TEXT)
        letters(action, kx + kw + gap, TEXT2)
        x += kw + gap + len(action) * step + group_gap
    save("hints.png", render("".join(body), W, H))


# ---- entry icons --------------------------------------------------------------
# GRUB picks icons/<class>.png by the entry's --class. Original line glyphs,
# 20x20, in a 32x20 picture: the left 12 px are empty so that the glyph keeps
# clear of the selection bar (GRUB draws an icon at the row's left edge).
def icons():
    line = 'fill="none" stroke="%s" stroke-width="1.6" stroke-linejoin="miter"' % TEXT2
    cy_line = 'fill="none" stroke="%s" stroke-width="1.6" stroke-linejoin="miter"' % CYAN
    hexagon = " ".join("%.2f,%.2f" % polar(22, 10, 8.6, k * 60 - 90) for k in range(6))
    glyphs = {
        # Shadow OS: the hexagon with one node.
        "shadow": '<polygon points="%s" %s/>' % (hexagon, cy_line)
                  + '<circle cx="22" cy="10" r="2.6" fill="%s"/>' % PALE,
        # Another GNU/Linux system: a shell prompt.
        "gnu-linux": '<rect x="13.8" y="2.8" width="16.4" height="14.4" %s/>' % line
                     + '<path d="M17 7 L20 10 L17 13" %s/>' % cy_line
                     + '<path d="M22 13.2 L26.5 13.2" %s/>' % line,
        # Any other system: a plain square.
        "os": '<rect x="14.8" y="2.8" width="14.4" height="14.4" %s/>' % line
              + '<rect x="19.5" y="7.5" width="5" height="5" fill="%s"/>' % TEXT2,
        # A submenu: two chevrons.
        "submenu": '<path d="M15 4.5 L20.5 10 L15 15.5" fill="none" stroke="%s" stroke-width="1.8"/>' % VIOLET
                   + '<path d="M22 4.5 L27.5 10 L22 15.5" fill="none" stroke="%s" stroke-width="1.8"/>' % VIOLET,
        # Settings (firmware set-up): two sliders.
        "settings": '<path d="M13 6 L31 6 M13 14 L31 14" %s/>' % line
                    + '<circle cx="26" cy="6" r="2.4" fill="#04060B" stroke="%s" stroke-width="1.6"/>' % CYAN
                    + '<circle cx="18" cy="14" r="2.4" fill="#04060B" stroke="%s" stroke-width="1.6"/>' % CYAN,
        # A local disk.
        "disk": '<rect x="13.8" y="4.8" width="16.4" height="10.4" %s/>' % line
                + '<circle cx="26" cy="10" r="1.6" fill="%s"/>' % CYAN
                + '<path d="M16.5 10 L21.5 10" %s/>' % line,
        # A system found by os-prober with the class "windows": a neutral
        # window outline with a title bar, no vendor mark.
        "windows": '<rect x="13.8" y="3.8" width="16.4" height="12.4" %s/>' % line
                   + '<path d="M13.8 7.8 L30.2 7.8" %s/>' % line
                   + '<rect x="15.8" y="5.3" width="1.6" height="1.2" fill="%s"/>' % CYAN,
    }
    for name, glyph in glyphs.items():
        save("icons/%s.png" % name, render(glyph, 32, 20))


background()
reticle()
conduit()
small_parts()
boxes()
wordmark()
hints()
icons()

# Read every file back and check what GRUB needs.
for name in written:
    data = open(os.path.join(OUT, name), "rb").read()
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", data[16:29])
    if data[:8] != b"\x89PNG\r\n\x1a\n" or depth != 8 or ctype not in (2, 6) or interlace != 0:
        raise SystemExit("gen-grub-theme: %s is not an 8-bit RGB/RGBA non-interlaced PNG" % name)
print("images: %d files, %d bytes" % (len(written), sum(os.path.getsize(os.path.join(OUT, n)) for n in written)))
PY
fi

# ---- fonts --------------------------------------------------------------------
# grub-mkfont builds the PF2 name as "<-n value> <Regular or Bold> <size>",
# taking "Bold" from the font file itself, and theme.txt must ask for exactly
# that name: an unknown name silently becomes the font that was loaded last.
# update-grub loads the *.pf2 of a theme in alphabetical order, so the file
# names decide which font is last:
#   a-sans-16.pf2   "Shadow Sans Regular 16"  entries, countdown text
#   b-monob-28.pf2  "Shadow Mono Bold 28"     the T-NN countdown number
#   z-mono-14.pf2   "Shadow Mono Regular 14"  asked for by nobody: it is the
#                   LAST font, so GRUB's fallback (and its plain text menu) is
#                   a small monospace face. "z-" also sorts after the font
#                   files of the previous theme, which stay in /boot/grub.
# The countdown number is the real bold face of JetBrains Mono. Do not add
# grub-mkfont's -b: it smears every glyph one pixel to the right on top of
# that (the dot inside the zero closes up).
# Only the glyph ranges the menu needs are kept; anything else is drawn with
# GRUB's own Unifont.
if [ "$FONTS" = 1 ]; then
    LATIN="0x20-0x7e,0xa0-0x17f,0x2010-0x2026"
    # grub-mkfont prints one warning per OpenType feature table it skips.
    mkfont() { grub-mkfont "$@" 2> >(grep -v 'unsupported font feature' >&2 || true); }
    mkfont -s 16 -n "Shadow Sans" -r "$LATIN" -o "$OUT/a-sans-16.pf2" "$SANS"
    mkfont -s 28 -n "Shadow Mono" -r "0x20-0x7e" -o "$OUT/b-monob-28.pf2" "$MONO_BOLD"
    mkfont -s 14 -n "Shadow Mono" -r "$LATIN" -o "$OUT/z-mono-14.pf2" "$MONO"
    echo "fonts: $(cd "$OUT" && ls ./*.pf2 | tr '\n' ' ')"
fi

# The hand-written part, so that OUTDIR is a whole theme.
if [ "$OUT" != "$(cd "$(dirname "$THEME_TXT")" && pwd)" ]; then
    cp "$THEME_TXT" "$OUT/theme.txt"
fi
echo "generated: $OUT"
