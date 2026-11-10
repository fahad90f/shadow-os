#!/usr/bin/env python3
"""Generate the NEXUS OS logo family.

    assets/src/logos/gen-logos.py

Original artwork, authored as geometry in this script so every variant
(symbol, wordmark, lockups, monochrome) stays consistent:

  * Symbol: a hexagon — the shape of a shield and of a network cell — holding
    the letter N drawn as a path between two nodes: a connection, a nexus.
  * Wordmark: NEXUS in a constructed, single-weight geometric alphabet with
    wide tracking; the E is three open bars, the X a plain crossing.

Colours come from the design tokens (brand gradient cyan → blue → violet).
Output: assets/logos/*.svg
"""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "assets/logos"
TOK = json.loads((ROOT / "desktop/themes/tokens/nexus-tokens.json").read_text())
B = TOK["brand"]
C1, C2, C3, OBS = B["gradientStart"], B["gradientMid"], B["gradientEnd"], B["obsidian"]
TEXT_DARK = TOK["color"]["dark"]["textPrimary"]
TEXT_LIGHT = TOK["color"]["light"]["textPrimary"]


def hexagon(cx, cy, r):
    pts = [(cx + r * math.cos(math.radians(60 * i - 90)), cy + r * math.sin(math.radians(60 * i - 90))) for i in range(6)]
    return "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + "Z"


def gradient(gid, x1, y1, x2, y2, stops=None):
    stops = stops or [(0, C1), (0.55, C2), (1, C3)]
    return (f'<linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
            + "".join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in stops) + "</linearGradient>")


def symbol(size=256, *, mono=None, plate=False, glow=True):
    """The symbol mark on a size×size canvas. mono: single colour instead of the gradient."""
    s = size / 256.0
    cx = cy = 128
    paint = mono or "url(#nx-g)"
    n_paint = mono or "url(#nx-n)"
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 256 256">',
             "<title>NEXUS OS</title>", "<defs>",
             gradient("nx-g", 40, 40, 216, 216),
             gradient("nx-n", 84, 84, 172, 172, [(0, "#FFFFFF"), (1, "#CFE9FF")]) if not mono else "",
             '<filter id="nx-glow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="9"/></filter>',
             "</defs>"]
    if plate:
        parts.append(f'<rect width="256" height="256" rx="56" fill="{OBS}"/>')
    hexd = hexagon(cx, cy, 100)
    if glow and not mono:
        parts.append(f'<path d="{hexd}" fill="none" stroke="url(#nx-g)" stroke-width="14" stroke-linejoin="round" '
                     'opacity="0.55" filter="url(#nx-glow)"/>')
    parts.append(f'<path d="{hexd}" fill="none" stroke="{paint}" stroke-width="13" stroke-linejoin="round"/>')
    # The N: up the left stem, across the diagonal, up the right stem.
    parts.append(f'<path d="M92 170V88L164 168V86" fill="none" stroke="{n_paint}" stroke-width="15" '
                 'stroke-linecap="round" stroke-linejoin="round"/>')
    # Nodes where the connection starts and ends.
    for (x, y, col) in ((92, 88, C1), (164, 168, C3)):
        parts.append(f'<circle cx="{x}" cy="{y}" r="13.5" fill="{mono or col}"/>')
        parts.append(f'<circle cx="{x}" cy="{y}" r="5.5" fill="{OBS if not mono else "none"}"/>' if not mono else "")
    parts.append("</svg>")
    del s
    return "\n".join(p for p in parts if p) + "\n"


# Constructed alphabet on a 100-unit cap height; each glyph: (advance width, path data).
GLYPHS = {
    "N": (78, "M0 100V0L78 100V0"),
    "E": (62, "M0 0H62M0 50H50M0 100H62"),
    "X": (80, "M0 0L80 100M80 0L0 100"),
    "U": (76, "M0 0V62A38 38 0 0 0 76 62V0"),
    "S": (70, "M68 12C56 2 40 0 30 0 12 0 2 12 2 25 2 40 14 46 35 50 56 54 68 60 68 75 68 88 58 100 40 100 26 100 12 96 0 86"),
    "O": (86, "M43 0A43 50 0 1 0 43 100 43 50 0 1 0 43 0Z"),
}


def word(text, x, y, height, tracking, colour, weight):
    """Lay out constructed glyphs; returns (svg, width)."""
    scale = height / 100.0
    out, pen = [], 0.0
    for ch in text:
        if ch == " ":
            pen += 46 * scale
            continue
        adv, d = GLYPHS[ch]
        out.append(f'<path transform="translate({x + pen:.2f} {y}) scale({scale})" d="{d}" fill="none" '
                   f'stroke="{colour}" stroke-width="{weight / scale:.2f}" stroke-linecap="round" stroke-linejoin="round"/>')
        pen += (adv + tracking) * scale
    return "".join(out), pen - tracking * scale


def wordmark(colour, *, with_os=True, height=56, weight=9):
    body, w = word("NEXUS", 6, 6, height, 34, colour, weight)
    os_svg, os_w = ("", 0)
    if with_os:
        os_svg, os_w = word("OS", 6 + w + 0.62 * height, 6, height, 30, C1 if colour != "#000000" else colour, weight * 0.62)
        w += 0.62 * height + os_w
    width, total_h = w + 12, height + 12
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{total_h:.0f}" '
            f'viewBox="0 0 {width:.2f} {total_h:.2f}"><title>NEXUS OS</title>{body}{os_svg}</svg>\n'), width, total_h


def lockup(colour, plate=None):
    """Symbol + wordmark, horizontal."""
    mark = symbol(256, glow=False).split("\n", 1)[1].rsplit("</svg>", 1)[0]
    wm, ww, wh = wordmark(colour, height=64, weight=10)
    inner = wm.split(">", 1)[1].rsplit("</svg>", 1)[0].replace("<title>NEXUS OS</title>", "")
    h = 160
    width = 160 + 36 + ww
    bg = f'<rect width="{width:.0f}" height="{h}" fill="{plate}"/>' if plate else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{h}" viewBox="0 0 {width:.2f} {h}">'
            f'<title>NEXUS OS</title>{bg}<g transform="scale(0.625)">{mark}</g>'
            f'<g transform="translate(196 {(h - wh) / 2:.2f})">{inner}</g></svg>\n')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    files = {
        "nexus-symbol.svg": symbol(256),
        "nexus-symbol-flat.svg": symbol(256, glow=False),
        "nexus-symbol-plate.svg": symbol(256, plate=True),
        "nexus-symbol-mono-light.svg": symbol(256, mono=TEXT_DARK, glow=False),
        "nexus-symbol-mono-dark.svg": symbol(256, mono=TEXT_LIGHT, glow=False),
        "nexus-wordmark-light.svg": wordmark(TEXT_DARK)[0],
        "nexus-wordmark-dark.svg": wordmark(TEXT_LIGHT)[0],
        "nexus-logo-light.svg": lockup(TEXT_DARK),
        "nexus-logo-dark.svg": lockup(TEXT_LIGHT),
    }
    for name, content in files.items():
        (OUT / name).write_text(content)
    print(f"{len(files)} logo files written to {OUT}")


if __name__ == "__main__":
    main()
