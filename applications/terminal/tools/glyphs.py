#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rasterise the indicator glyphs of the SHADOW Terminal style sheet.

    glyphs.py <theme.json> <output-dir>
    glyphs.py --list                      names of the files one run writes

A Qt style sheet can only show images it loads from files ("image: url(...)"),
and Qt reads SVG there only when the optional qt6-svg-plugins are installed.
So the handful of glyphs the style sheet needs (check marks, chevrons) are
drawn here, at build time, as PNG files in the colours of one theme variant.

<theme.json> is the colour table scripts/gen-tokens.py rendered from the
design tokens for that variant (style/theme.json.in), so every colour still
comes from the tokens. The shapes are the stroke paths of the matching SHADOW
symbolic icons (desktop/ui/icons/check.svg, chevron-*.svg), on their 24-unit
grid. Each glyph is written at 16 px and as an "@2x" file for high-DPI screens.

Standard library only.
"""
import json
import struct
import sys
import zlib
from pathlib import Path

GRID = 24.0          # the icons' viewBox
SIZE = 16            # logical size in the style sheet (size.iconSmall)
SUPERSAMPLE = 4      # 4×4 samples per pixel for anti-aliasing

# name: (colour token, stroke width on the 24-unit grid, polyline)
GLYPHS = {
    "check":                 ("textOnAccent", 2.6, [(5, 12.6), (9.4, 17), (19, 7.4)]),
    "menu-check":            ("accent", 2.0, [(5, 12.6), (9.4, 17), (19, 7.4)]),
    "chevron-down":          ("textSecondary", 1.9, [(6, 9.5), (12, 15.5), (18, 9.5)]),
    "chevron-down-disabled": ("textDisabled", 1.9, [(6, 9.5), (12, 15.5), (18, 9.5)]),
    "chevron-up":            ("textSecondary", 1.9, [(6, 14.5), (12, 8.5), (18, 14.5)]),
    "chevron-up-disabled":   ("textDisabled", 1.9, [(6, 14.5), (12, 8.5), (18, 14.5)]),
    "chevron-right":         ("textMuted", 1.9, [(9.5, 6), (15.5, 12), (9.5, 18)]),
    "chevron-left":          ("textMuted", 1.9, [(14.5, 6), (8.5, 12), (14.5, 18)]),
}


def segment_distance_sq(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    t = 0.0 if length_sq == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_sq))
    cx, cy = ax + t * dx, ay + t * dy
    return (px - cx) ** 2 + (py - cy) ** 2


def render(points, stroke, pixels):
    """Coverage (0..255) of a round-capped, round-joined polyline, row by row."""
    scale = GRID / pixels
    radius_sq = (stroke / 2.0) ** 2
    step = 1.0 / SUPERSAMPLE
    segments = list(zip(points, points[1:]))
    rows = []
    for y in range(pixels):
        row = []
        for x in range(pixels):
            hits = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    px = (x + (sx + 0.5) * step) * scale
                    py = (y + (sy + 0.5) * step) * scale
                    if any(segment_distance_sq(px, py, a[0], a[1], b[0], b[1]) <= radius_sq for a, b in segments):
                        hits += 1
            row.append(round(255 * hits / (SUPERSAMPLE * SUPERSAMPLE)))
        rows.append(row)
    return rows


def png(rows, rgb):
    """8-bit RGBA PNG of a single colour with the given per-pixel alpha."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    height, width = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)                               # filter type: none
        for alpha in row:
            raw.extend((rgb[0], rgb[1], rgb[2], alpha))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def file_names():
    return [f"{name}{suffix}.png" for name in GLYPHS for suffix in ("", "@2x")]


def main() -> int:
    if sys.argv[1:] == ["--list"]:
        print("\n".join(file_names()))
        return 0
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    colours = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name, (token, stroke, points) in GLYPHS.items():
        value = colours[token]
        rgb = tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
        for factor, suffix in ((1, ""), (2, "@2x")):
            (out / f"{name}{suffix}.png").write_bytes(png(render(points, stroke, SIZE * factor), rgb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
