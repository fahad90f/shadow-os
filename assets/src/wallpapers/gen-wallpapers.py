#!/usr/bin/env python3
"""Generate the NEXUS OS wallpapers (original, procedural artwork).

    assets/src/wallpapers/gen-wallpapers.py [--size 3840x2160] [--only NAME]

Each wallpaper is described as SVG geometry here and rasterised with
rsvg-convert, then given a trace of film grain with ImageMagick so the large
dark gradients do not band on 8-bit displays. A fixed random seed keeps the
output identical from build to build.

  horizon   (default) a planet's limb at night: the thin lit edge of a world
            against obsidian — NEXUS HORIZON
  lattice   a hexagonal lattice with a few lit connections — the nexus motif
  aurora    slow ribbons of the brand colours
  daylight  the horizon at dawn, for the light appearance

Output: assets/wallpapers/nexus-<name>.jpg (+ a 16:10 crop-safe composition:
all detail sits inside the central 4:3 area).
"""
import argparse
import json
import math
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "assets/wallpapers"
TOK = json.loads((ROOT / "desktop/themes/tokens/nexus-tokens.json").read_text())
B = TOK["brand"]
C1, C2, C3, OBS = B["gradientStart"], B["gradientMid"], B["gradientEnd"], B["obsidian"]


def head(w, h):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'


def stars(w, h, rng, count, y_max):
    out = []
    for _ in range(count):
        x, y = rng.uniform(0, w), rng.uniform(0, y_max)
        r = rng.choice([0.6, 0.8, 1.0, 1.0, 1.3, 1.8]) * w / 3840
        o = rng.uniform(0.15, 0.7) * (1 - y / y_max) ** 0.6
        out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.2f}" fill="#DCEBFF" opacity="{o:.2f}"/>')
    return "".join(out)


def horizon(w, h, rng, light=False):
    # The planet: a huge circle whose top edge crosses the lower third of the frame.
    R = w * 1.55
    cx, cy = w * 0.5, h * 0.80 + R
    sky_top, sky_bottom = ("#DCE8F7", "#F6F1EC") if light else ("#020306", "#0A1120")
    planet_top, planet_bottom = ("#3A4B66", "#1B2535") if light else ("#070B13", "#020305")
    rim = [(0, C1), (0.5, C2), (1, C3)]
    s = [head(w, h), "<defs>",
         f'<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{sky_top}"/><stop offset="1" stop-color="{sky_bottom}"/></linearGradient>',
         f'<linearGradient id="planet" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{planet_top}"/><stop offset="0.25" stop-color="{planet_bottom}"/></linearGradient>',
         '<linearGradient id="rim" x1="0" y1="0" x2="1" y2="0">' + "".join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in rim) + "</linearGradient>",
         f'<radialGradient id="sun" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="{C1}" stop-opacity="{0.55 if not light else 0.75}"/><stop offset="0.45" stop-color="{C2}" stop-opacity="0.18"/><stop offset="1" stop-color="{C3}" stop-opacity="0"/></radialGradient>',
         f'<filter id="blur1" x="-10%" y="-200%" width="120%" height="500%"><feGaussianBlur stdDeviation="{w * 0.012:.1f}"/></filter>',
         f'<filter id="blur2" x="-10%" y="-200%" width="120%" height="500%"><feGaussianBlur stdDeviation="{w * 0.0035:.1f}"/></filter>',
         f'<clipPath id="above"><rect x="0" y="0" width="{w}" height="{h}"/></clipPath>',
         "</defs>",
         f'<rect width="{w}" height="{h}" fill="url(#sky)"/>']
    if not light:
        s.append(stars(w, h, rng, 420, h * 0.78))
    # Glow of the star about to rise behind the limb, slightly left of centre.
    gx, gy, gr = w * 0.44, h * 0.80, w * 0.42
    s.append(f'<ellipse cx="{gx}" cy="{gy}" rx="{gr}" ry="{gr * 0.42}" fill="url(#sun)"/>')
    # Atmosphere: wide soft band, then a tight bright line, hugging the limb.
    s.append(f'<circle cx="{cx}" cy="{cy}" r="{R}" fill="none" stroke="url(#rim)" stroke-width="{w * 0.02:.1f}" opacity="0.55" filter="url(#blur1)"/>')
    s.append(f'<circle cx="{cx}" cy="{cy}" r="{R}" fill="url(#planet)"/>')
    s.append(f'<circle cx="{cx}" cy="{cy}" r="{R}" fill="none" stroke="url(#rim)" stroke-width="{w * 0.0042:.1f}" opacity="0.9" filter="url(#blur2)"/>')
    s.append(f'<circle cx="{cx}" cy="{cy}" r="{R}" fill="none" stroke="#FFFFFF" stroke-width="{w * 0.0007:.1f}" opacity="{0.55 if not light else 0.8}"/>')
    s.append("</svg>")
    return "".join(s)


def lattice(w, h, rng):
    size = w / 34.0                                   # hexagon circumradius
    dx, dy = size * math.sqrt(3), size * 1.5
    cols, rows = int(w / dx) + 3, int(h / dy) + 3
    centres = {}
    for r in range(-1, rows):
        for c in range(-1, cols):
            centres[(c, r)] = (c * dx + (dx / 2 if r % 2 else 0), r * dy)
    hexes, lit, nodes = [], [], []

    def hexpath(x, y, rad):
        pts = [(x + rad * math.cos(math.radians(60 * i - 90)), y + rad * math.sin(math.radians(60 * i - 90))) for i in range(6)]
        return "M" + " L".join(f"{px:.1f} {py:.1f}" for px, py in pts) + "Z"

    for (c, r), (x, y) in centres.items():
        hexes.append(hexpath(x, y, size * 0.97))
    # A handful of lit walks across neighbouring cells: connections forming.
    for _ in range(9):
        c, r = rng.randrange(4, cols - 6), rng.randrange(3, rows - 4)
        path = [centres[(c, r)]]
        for _ in range(rng.randrange(3, 8)):
            c += rng.choice([1, 1, 0, -1])
            r += rng.choice([1, 0, -1])
            if (c, r) not in centres:
                break
            path.append(centres[(c, r)])
        if len(path) > 2:
            lit.append("M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in path))
            nodes += [path[0], path[-1]]
    s = [head(w, h), "<defs>",
         f'<radialGradient id="bg" cx="0.5" cy="0.45" r="0.75"><stop offset="0" stop-color="#0E1524"/><stop offset="1" stop-color="{OBS}"/></radialGradient>',
         f'<linearGradient id="lit" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{C1}"/><stop offset="0.5" stop-color="{C2}"/><stop offset="1" stop-color="{C3}"/></linearGradient>',
         '<radialGradient id="fade" cx="0.5" cy="0.45" r="0.6"><stop offset="0" stop-color="#FFFFFF" stop-opacity="1"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/></radialGradient>',
         '<mask id="m"><rect width="100%" height="100%" fill="url(#fade)"/></mask>',
         f'<filter id="g" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="{w * 0.003:.1f}"/></filter>',
         "</defs>",
         f'<rect width="{w}" height="{h}" fill="url(#bg)"/>',
         f'<g mask="url(#m)"><path d="{" ".join(hexes)}" fill="none" stroke="#2A3A58" stroke-width="{w / 3840 * 1.6:.2f}" opacity="0.55"/>',
         f'<path d="{" ".join(lit)}" fill="none" stroke="url(#lit)" stroke-width="{w / 3840 * 9:.1f}" stroke-linecap="round" stroke-linejoin="round" opacity="0.6" filter="url(#g)"/>',
         f'<path d="{" ".join(lit)}" fill="none" stroke="url(#lit)" stroke-width="{w / 3840 * 3.2:.1f}" stroke-linecap="round" stroke-linejoin="round"/>']
    for x, y in nodes:
        s.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{w / 3840 * 9:.1f}" fill="{OBS}" stroke="url(#lit)" stroke-width="{w / 3840 * 3.2:.1f}"/>')
    s.append("</g></svg>")
    return "".join(s)


def aurora(w, h, rng):
    s = [head(w, h), "<defs>",
         f'<filter id="b" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="{w * 0.045:.1f}"/></filter>',
         f'<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#04060A"/><stop offset="1" stop-color="#090E19"/></linearGradient>',
         "</defs>", f'<rect width="{w}" height="{h}" fill="url(#bg)"/>', '<g filter="url(#b)">']
    for colour, y0, amp, phase, width, op in ((C3, 0.66, 0.10, 0.3, 0.16, 0.50), (C2, 0.56, 0.12, 1.4, 0.13, 0.55),
                                              (C1, 0.46, 0.09, 2.6, 0.10, 0.50)):
        pts = []
        for i in range(0, 33):
            t = i / 32
            x = t * w * 1.2 - w * 0.1
            y = h * (y0 + amp * math.sin(t * math.pi * 2.2 + phase) + 0.04 * math.sin(t * math.pi * 6 + phase * 2))
            pts.append((x, y))
        d = f"M{pts[0][0]:.0f} {pts[0][1]:.0f} " + " ".join(f"L{x:.0f} {y:.0f}" for x, y in pts[1:])
        s.append(f'<path d="{d}" fill="none" stroke="{colour}" stroke-width="{h * width:.0f}" stroke-linecap="round" opacity="{op}"/>')
    s.append("</g>")
    s.append(f'<rect width="{w}" height="{h}" fill="#000000" opacity="0.28"/>')
    s.append("</svg>")
    return "".join(s)


DESIGNS = {
    "horizon": lambda w, h, rng: horizon(w, h, rng),
    "lattice": lattice,
    "aurora": aurora,
    "daylight": lambda w, h, rng: horizon(w, h, rng, light=True),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", default="3840x2160")
    ap.add_argument("--only")
    args = ap.parse_args()
    w, h = (int(v) for v in args.size.split("x"))
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in DESIGNS.items():
        if args.only and name != args.only:
            continue
        rng = random.Random(f"nexus-{name}")
        with tempfile.TemporaryDirectory() as tmp:
            svg, png = Path(tmp) / "w.svg", Path(tmp) / "w.png"
            svg.write_text(fn(w, h, rng))
            subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h), str(svg), "-o", str(png)], check=True)
            target = OUT / f"nexus-{name}.jpg"
            # Fine monochrome grain hides banding; strip metadata for reproducible files.
            subprocess.run(["convert", str(png), "-seed", "7", "-attenuate", "0.18", "+noise", "Gaussian",
                            "-strip", "-sampling-factor", "4:4:4", "-quality", "92", str(target)], check=True)
        print(f"{target.relative_to(ROOT)}  {target.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
