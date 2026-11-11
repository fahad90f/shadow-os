#!/usr/bin/env python3
"""Generate the SHADOW symbolic icon set.

    assets/src/icons/gen-symbolic.py [--out DIR] [--sheet FILE.svg]

Original line icons on a 24x24 grid: 1.75px stroke, round caps and joins, no
fills except small dots. Every icon paints with `currentColor`, so the UI can
tint one file for any theme, accent or state (see NxIcon / SymbolicIconProvider).

The geometry is authored here, in code, so the set stays consistent and
reviewable: adding an icon means adding one entry to ICONS and re-running the
script. Output goes to desktop/ui/icons/ (compiled into libshadowui).
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "desktop/ui/icons"

HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
        'fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">')


def p(d: str) -> str:
    return f'<path d="{d}"/>'


def c(cx, cy, r) -> str:
    return f'<circle cx="{cx}" cy="{cy}" r="{r}"/>'


def dot(cx, cy, r=1.4) -> str:
    return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="currentColor" stroke="none"/>'


def dim(body: str) -> str:
    """Parts of a level glyph that are "not lit": same colour, drawn faint."""
    return f'<g opacity="0.3">{body}</g>'


def r(x, y, w, h, rx=2) -> str:
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}"/>'


def gear(teeth=8, ro=9.6, ri=7.4, hole=3.1) -> str:
    """Cog outline computed from polar coordinates."""
    pts = []
    step = 2 * math.pi / teeth
    for i in range(teeth):
        a = i * step - math.pi / 2
        for da, rad in ((-0.30, ri), (-0.17, ro), (0.17, ro), (0.30, ri)):
            ang = a + da * step * 1.15
            pts.append((12 + rad * math.cos(ang), 12 + rad * math.sin(ang)))
    d = "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + "Z"
    return p(d) + c(12, 12, hole)


def hexagon(cx=12, cy=12, rad=9.2, rot=0.0) -> str:
    pts = [(cx + rad * math.cos(math.radians(60 * i + 30 + rot)), cy + rad * math.sin(math.radians(60 * i + 30 + rot)))
           for i in range(6)]
    return p("M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + "Z")


SHIELD = "M12 3.2l7.3 2.7v5.6c0 4.5-3 8-7.3 9.6-4.3-1.6-7.3-5.1-7.3-9.6V5.9z"
SPEAKER = "M4.5 9.5h3l4.5-3.8v12.6l-4.5-3.8h-3z"

# Wi-Fi fan: the dot and the three arcs are the four "bars" of the signal level.
WIFI_DOT = dot(12, 19, 1.3)
WIFI_ARCS = ["M9 15.9a4.5 4.5 0 0 1 6 0", "M5.9 12.6a9 9 0 0 1 12.2 0", "M2.8 9.3a13.5 13.5 0 0 1 18.4 0"]


def wifi_level(bars: int) -> str:
    """Signal strength 0–4: lit parts at full strength, the rest faint."""
    lit = (WIFI_DOT if bars >= 1 else "") + "".join(p(a) for a in WIFI_ARCS[:max(0, bars - 1)])
    unlit = (WIFI_DOT if bars < 1 else "") + "".join(p(a) for a in WIFI_ARCS[max(0, bars - 1):])
    return lit + (dim(unlit) if unlit else "")


BATTERY_BODY = r(3, 8, 15.5, 8, 2) + p("M21 10.7v2.6")


def battery_level(cells: int) -> str:
    """Charge level as 0–4 cells inside the battery outline."""
    return BATTERY_BODY + (p("".join(f"M{6 + 3 * i} 11v2" for i in range(cells))) if cells else "")


ICONS: dict[str, str] = {
    # ---- basic actions -----------------------------------------------------
    "check": p("M5 12.6l4.4 4.4L19 7.4"),
    "minus": p("M5.5 12h13"),
    "plus": p("M12 5.5v13M5.5 12h13"),
    "close": p("M6.5 6.5l11 11M17.5 6.5l-11 11"),
    "chevron-right": p("M9.5 6l6 6-6 6"),
    "chevron-left": p("M14.5 6l-6 6 6 6"),
    "chevron-up": p("M6 14.5l6-6 6 6"),
    "chevron-down": p("M6 9.5l6 6 6-6"),
    "arrow-right": p("M4.5 12h15M13.5 6l6 6-6 6"),
    "arrow-left": p("M19.5 12h-15M10.5 6l-6 6 6 6"),
    "arrow-up": p("M12 19.5v-15M6 10.5l6-6 6 6"),
    "arrow-down": p("M12 4.5v15M6 13.5l6 6 6-6"),
    "search": c(10.75, 10.75, 6.25) + p("M15.4 15.4l4.6 4.6"),
    "zoom-in": c(10.75, 10.75, 6.25) + p("M15.4 15.4l4.6 4.6M8 10.75h5.5M10.75 8v5.5"),
    "zoom-out": c(10.75, 10.75, 6.25) + p("M15.4 15.4l4.6 4.6M8 10.75h5.5"),
    "menu": p("M4.5 7h15M4.5 12h15M4.5 17h15"),
    "more-horizontal": dot(6, 12, 1.5) + dot(12, 12, 1.5) + dot(18, 12, 1.5),
    "more-vertical": dot(12, 6, 1.5) + dot(12, 12, 1.5) + dot(12, 18, 1.5),
    "grip": "".join(dot(x, y, 1.2) for x in (9, 15) for y in (6.5, 12, 17.5)),
    "refresh": p("M19.6 12a7.6 7.6 0 1 1-1.1-4") + p("M18.5 3.5V8H14"),
    "undo": p("M8 5L4 9l4 4") + p("M4 9h10a5.5 5.5 0 0 1 0 11h-3"),
    "redo": p("M16 5l4 4-4 4") + p("M20 9H10a5.5 5.5 0 0 0 0 11h3"),
    "external-link": p("M13.5 5H19v5.5M19 5l-8 8") + p("M10 6.5H6.5a1.5 1.5 0 0 0-1.5 1.5v9.5A1.5 1.5 0 0 0 6.5 19H16a1.5 1.5 0 0 0 1.5-1.5V14"),
    "filter": p("M4 5.5h16l-6.2 7.3v5.4l-3.6 1.8v-7.2z"),
    "sort": p("M8 5v14M4.5 15.5L8 19l3.5-3.5M16 19V5M12.5 8.5L16 5l3.5 3.5"),
    "copy": r(8.5, 8.5, 11, 11, 2) + p("M5 15.5V6.5A1.5 1.5 0 0 1 6.5 5h9"),
    "cut": c(6.5, 17, 2.4) + c(17.5, 17, 2.4) + p("M8 15.2L17 4M16 15.2L7 4"),
    "paste": r(5.5, 5, 13, 15.5, 2) + p("M9 5V4.2A1.2 1.2 0 0 1 10.2 3h3.6A1.2 1.2 0 0 1 15 4.2V5M9 5h6v2H9zM9 12h6M9 15.5h4"),
    "trash": p("M4.5 6.5h15M9.5 6.5V4.5h5v2M6.5 6.5l.8 12a1.6 1.6 0 0 0 1.6 1.5h6.2a1.6 1.6 0 0 0 1.6-1.5l.8-12M10 10.5v6M14 10.5v6"),
    "edit": p("M4.5 19.5l1-4.2L16.2 4.6a1.6 1.6 0 0 1 2.3 0l.9.9a1.6 1.6 0 0 1 0 2.3L8.7 18.5zM14.5 6.3l3.2 3.2"),
    "save": p("M5.5 4.5h10.6l2.4 2.4v11.1a1.5 1.5 0 0 1-1.5 1.5H6.5a1.5 1.5 0 0 1-1.5-1.5V5a.5.5 0 0 1 .5-.5z") + p("M8.5 4.5v4.5h6V4.5M8 19.5v-5.5h8v5.5"),
    "download": p("M12 4v11M7.5 10.5L12 15l4.5-4.5M5 19.5h14"),
    "upload": p("M12 15V4M7.5 8.5L12 4l4.5 4.5M5 19.5h14"),
    "link": p("M10.5 13.5a3.6 3.6 0 0 0 5.1 0l3-3a3.6 3.6 0 0 0-5.1-5.1l-1.2 1.2") + p("M13.5 10.5a3.6 3.6 0 0 0-5.1 0l-3 3a3.6 3.6 0 0 0 5.1 5.1l1.2-1.2"),
    "pin": p("M9 4.5h6l-.8 5.2 2.8 3.3H7l2.8-3.3zM12 13v7"),
    "star": p("M12 4l2.5 5.1 5.6.8-4 3.9.9 5.6-5-2.7-5 2.7.9-5.6-4-3.9 5.6-.8z"),
    "eject": p("M6 14.5h12L12 6zM6 18.5h12"),
    "play": p("M8 5.5v13l10.5-6.5z"),
    "pause": p("M8.5 5.5v13M15.5 5.5v13"),
    "stop": r(6.5, 6.5, 11, 11, 1.5),
    "skip-forward": p("M6 6l8.5 6L6 18zM18 6v12"),
    "skip-back": p("M18 6l-8.5 6 8.5 6zM6 6v12"),

    # ---- status ------------------------------------------------------------
    "info": c(12, 12, 8.5) + p("M12 11v5.5") + dot(12, 7.9, 1.1),
    "help": c(12, 12, 8.5) + p("M9.7 9.6a2.4 2.4 0 1 1 3.6 2.1c-.8.5-1.3 1-1.3 2") + dot(12, 16.6, 1.1),
    "check-circle": c(12, 12, 8.5) + p("M8.3 12.3l2.6 2.6 4.9-5.3"),
    "x-circle": c(12, 12, 8.5) + p("M9.2 9.2l5.6 5.6M14.8 9.2l-5.6 5.6"),
    "alert-triangle": p("M12 4.2l8.6 14.9H3.4zM12 10v4.4") + dot(12, 16.7, 1.05),
    "dot": dot(12, 12, 4),
    "circle": c(12, 12, 8),
    "activity": p("M3.5 12.5h3.7l2.4-6 4.2 11.5 2.6-5.5h4.1"),
    "gauge": p("M4.6 16.5a8.5 8.5 0 1 1 14.8 0") + p("M12 13.5l3.8-4.6") + dot(12, 13.6, 1.3),
    "zap": p("M13 3.5L5.5 13.5h5.5L10 20.5l8.5-10.5H13z"),
    "leaf": p("M5 19c0-8 5-13.5 14.5-14 .3 9.5-4.5 14.5-12 14.2M5 19l6.5-6.5"),
    "clock": c(12, 12, 8.5) + p("M12 7.5V12l3 2"),
    "calendar": r(4.5, 5.5, 15, 14, 2) + p("M4.5 10h15M8.5 3.5v4M15.5 3.5v4"),
    "timer": c(12, 13, 7) + p("M12 9.5V13l2.2 1.6M9.5 3.5h5"),
    "bell": p("M6.5 16.5V11a5.5 5.5 0 0 1 11 0v5.5l1.5 1.5H5zM10 20.5h4"),
    "bell-off": p("M17.5 14.5V11a5.5 5.5 0 0 0-8.6-4.5M6.6 9.8A5.5 5.5 0 0 0 6.5 11v5.5L5 18h11M10 20.5h4M4.5 4.5l15 15"),

    # ---- files -------------------------------------------------------------
    "home": p("M4.5 11L12 4.5l7.5 6.5M6.5 9.8v9.7h11V9.8M10 19.5v-5h4v5"),
    "folder": p("M3.5 7A1.5 1.5 0 0 1 5 5.5h4.2l2 2.2H19A1.5 1.5 0 0 1 20.5 9.2V17A1.5 1.5 0 0 1 19 18.5H5A1.5 1.5 0 0 1 3.5 17z"),
    "folder-open": p("M3.5 17V7A1.5 1.5 0 0 1 5 5.5h4.2l2 2.2H17.5A1.5 1.5 0 0 1 19 9.2v1.3") + p("M3.7 17.6l2.4-6.2a1.4 1.4 0 0 1 1.3-.9h12.4a1 1 0 0 1 .9 1.4l-2.2 5.7a1.4 1.4 0 0 1-1.3.9H4.6a1 1 0 0 1-.9-.9z"),
    "file": p("M7 3.5h6.5l4.5 4.5v11a1.5 1.5 0 0 1-1.5 1.5h-9.5A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5z") + p("M13.5 3.5V8H18"),
    "file-text": p("M7 3.5h6.5l4.5 4.5v11a1.5 1.5 0 0 1-1.5 1.5h-9.5A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5z") + p("M13.5 3.5V8H18M9 12.5h6M9 16h6"),
    "file-search": p("M12 20.5H7A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5h6.5L18 8v3") + p("M13.5 3.5V8H18") + c(15.5, 16, 2.8) + p("M17.6 18.1l2.4 2.4"),
    "image": r(4, 5, 16, 14, 2) + c(9, 10, 1.6) + p("M4.5 17l4.7-4.7 3.3 3.3 2.6-2.6 4.4 4.4"),
    "music": c(7.5, 17, 2.5) + c(16.5, 15.5, 2.5) + p("M10 17V6.5l9-2v11"),
    "video": r(3.5, 6, 12.5, 12, 2) + p("M16 10.2l4.5-2.7v9l-4.5-2.7"),
    "archive": r(4, 4.5, 16, 4.5, 1) + p("M5.5 9v9A1.5 1.5 0 0 0 7 19.5h10a1.5 1.5 0 0 0 1.5-1.5V9M10 13h4"),
    "package": p("M12 3.5l7.5 4.2v8.6L12 20.5l-7.5-4.2V7.7zM4.7 7.8L12 12l7.3-4.2M12 12v8.5M8.2 5.6l7.5 4.3"),
    "code": p("M8.5 7.5L4 12l4.5 4.5M15.5 7.5L20 12l-4.5 4.5M13.5 5l-3 14"),
    "terminal": r(3.5, 5, 17, 14, 2) + p("M7 10l3 2.5L7 15M12.5 15h4.5"),
    "hash": p("M9.5 4.5l-2 15M16.5 4.5l-2 15M5 9h15M4 15h15"),
    "type": p("M5 7V5.5h14V7M12 5.5v13M9 18.5h6"),
    "database": p("M5 6.5c0-1.4 3.1-2.5 7-2.5s7 1.1 7 2.5v11c0 1.4-3.1 2.5-7 2.5s-7-1.1-7-2.5zM5 6.5C5 7.9 8.1 9 12 9s7-1.1 7-2.5M5 12c0 1.4 3.1 2.5 7 2.5s7-1.1 7-2.5"),
    "server": r(4, 4.5, 16, 6.5, 1.5) + r(4, 13, 16, 6.5, 1.5) + dot(7.5, 7.75, 1) + dot(7.5, 16.25, 1) + p("M11 7.75h5.5M11 16.25h5.5"),
    "cloud": p("M7.5 18.5a4 4 0 0 1-.6-7.96A5.5 5.5 0 0 1 17.5 9.6a4.5 4.5 0 0 1-.5 8.9z"),

    # ---- system / hardware --------------------------------------------------
    "settings": gear(),
    "sliders": p("M4.5 7h8M16.5 7h3M4.5 12h3M11.5 12h8M4.5 17h9M17.5 17h2") + c(14.5, 7, 2) + c(9.5, 12, 2) + c(15.5, 17, 2),
    "wrench": p("M14.6 6.2a4.2 4.2 0 0 0-5.3 5.3l-5 5a1.8 1.8 0 0 0 2.6 2.6l5-5a4.2 4.2 0 0 0 5.3-5.3l-2.6 2.6-2.2-.4-.4-2.2z"),
    "cpu": r(6.5, 6.5, 11, 11, 2) + r(9.5, 9.5, 5, 5, 1) + p("M9.5 3.5v3M14.5 3.5v3M9.5 17.5v3M14.5 17.5v3M3.5 9.5h3M3.5 14.5h3M17.5 9.5h3M17.5 14.5h3"),
    "memory": r(3.5, 7, 17, 8.5, 1.5) + p("M7 10v2.5M10.5 10v2.5M14 10v2.5M17 10v2.5M6 15.5v2.5M9.5 15.5v2.5M14.5 15.5v2.5M18 15.5v2.5"),
    "harddrive": p("M5.2 5.5h13.6l2.2 8v4.5A1.5 1.5 0 0 1 19.5 19.5h-15A1.5 1.5 0 0 1 3 18v-4.5zM3 13.5h18") + dot(17.3, 16.5, 1) + p("M6.5 16.5h5"),
    "disk-lock": p("M10 19.5H4.5A1.5 1.5 0 0 1 3 18v-4.5l2.2-8h13.6l1.2 4.4M3 13.5h8") + r(13.5, 15, 7, 5.5, 1.2) + p("M15.2 15v-1.4a1.8 1.8 0 0 1 3.6 0V15"),
    "usb": r(7, 9.5, 10, 11, 2) + p("M9 9.5V4h6v5.5") + dot(10.8, 6.6, .8) + dot(13.2, 6.6, .8) + p("M10 14.5h4"),
    "monitor": r(3.5, 4.5, 17, 11.5, 2) + p("M9 19.5h6M12 16v3.5"),
    "laptop": p("M5.5 6A1.5 1.5 0 0 1 7 4.5h10A1.5 1.5 0 0 1 18.5 6v9.5h-13zM3 18.5h18l-1.5-3h-15z"),
    "keyboard": r(3, 6.5, 18, 11, 2) + p("M6.5 10h.01M10 10h.01M13.5 10h.01M17 10h.01M6.5 13.5h.01M17 13.5h.01M9.5 14h5"),
    "mouse": r(7, 3.5, 10, 17, 5) + p("M12 7v3"),
    "touchpad": r(4, 5, 16, 14, 2) + p("M4 14.5h16M12 14.5V19"),
    "printer": p("M7 9V4.5h10V9M7 16.5H5.5A1.5 1.5 0 0 1 4 15v-4.5A1.5 1.5 0 0 1 5.5 9h13a1.5 1.5 0 0 1 1.5 1.5V15a1.5 1.5 0 0 1-1.5 1.5H17") + r(7, 13.5, 10, 6, 1),
    "thermometer": p("M10 14.2V5.5a2 2 0 0 1 4 0v8.7a4 4 0 1 1-4 0z") + dot(12, 17.3, 1.5) + p("M12 15.5V9"),
    "battery": r(3, 8, 15.5, 8, 2) + p("M21 10.7v2.6") + p("M6 11v2M9 11v2M12 11v2"),
    "battery-charging": r(3, 8, 15.5, 8, 2) + p("M21 10.7v2.6M11.5 9.6l-2.2 2.9h3.2l-2.2 2.9"),
    # Charge levels: empty (0–5 %), low, half, high, full. "battery" above is
    # the generic glyph (same drawing as battery-high).
    "battery-empty": battery_level(0),
    "battery-low": battery_level(1),
    "battery-half": battery_level(2),
    "battery-high": battery_level(3),
    "battery-full": battery_level(4),
    "power": p("M12 3.5v8.5M7.3 6.6a7.5 7.5 0 1 0 9.4 0"),
    "plug": p("M9 3.5v4M15 3.5v4M6.5 7.5h11V11a5.5 5.5 0 0 1-11 0zM12 16.5v4"),
    "logout": p("M10 4.5H6.5A1.5 1.5 0 0 0 5 6v12a1.5 1.5 0 0 0 1.5 1.5H10M15 8l4 4-4 4M19 12H9.5"),
    "sun": c(12, 12, 3.8) + p("M12 3.2v2M12 18.8v2M3.2 12h2M18.8 12h2M5.8 5.8l1.4 1.4M16.8 16.8l1.4 1.4M5.8 18.2l1.4-1.4M16.8 7.2l1.4-1.4"),
    "moon": p("M19.5 14.2A8 8 0 0 1 9.8 4.5a8 8 0 1 0 9.7 9.7z"),
    "palette": p("M12 3.5a8.5 8.5 0 0 0 0 17c1.3 0 2-.9 2-1.9 0-1.3-1-1.6-1-2.7 0-.9.7-1.6 1.7-1.6h2A3.8 3.8 0 0 0 20.5 10.5c0-3.9-3.8-7-8.5-7z") + dot(7.7, 12, 1.1) + dot(9.5, 8, 1.1) + dot(14, 7.3, 1.1),
    "accessibility": c(12, 12, 8.5) + dot(12, 7.6, 1.2) + p("M7.8 10.3l4.2.9 4.2-.9M12 11.2v3l-2.2 3.6M12 14.2l2.2 3.6"),
    "language": c(12, 12, 8.5) + p("M3.5 12h17M12 3.5c2.4 2.3 3.6 5.2 3.6 8.5s-1.2 6.2-3.6 8.5c-2.4-2.3-3.6-5.2-3.6-8.5s1.2-6.2 3.6-8.5z"),
    "flag": p("M6 20.5V4.5M6 5.2c4-1.9 6.5 1.9 11.5 0v8c-5 1.9-7.5-1.9-11.5 0"),
    "user": c(12, 8.2, 3.7) + p("M4.8 19.8a7.4 7.4 0 0 1 14.4 0"),
    "users": c(9.2, 8.5, 3.2) + p("M3.2 19.5a6.1 6.1 0 0 1 12 0M15.5 5.6a3.2 3.2 0 0 1 0 5.9M17.5 14.4a6 6 0 0 1 3.3 5.1"),

    # ---- windows / layout ---------------------------------------------------
    "grid": r(4.5, 4.5, 6, 6, 1.5) + r(13.5, 4.5, 6, 6, 1.5) + r(4.5, 13.5, 6, 6, 1.5) + r(13.5, 13.5, 6, 6, 1.5),
    "list": p("M9 7h10.5M9 12h10.5M9 17h10.5") + dot(5, 7, 1.2) + dot(5, 12, 1.2) + dot(5, 17, 1.2),
    "columns": r(3.5, 4.5, 17, 15, 2) + p("M12 4.5v15"),
    "rows": r(3.5, 4.5, 17, 15, 2) + p("M3.5 12h17"),
    "sidebar": r(3.5, 4.5, 17, 15, 2) + p("M9.5 4.5v15"),
    "layers": p("M12 4l8.5 4.5L12 13 3.5 8.5zM3.5 12.5L12 17l8.5-4.5M3.5 16L12 20.5l8.5-4.5"),
    "window": r(3.5, 4.5, 17, 15, 2) + p("M3.5 9h17") + dot(6.5, 6.75, .8) + dot(9.2, 6.75, .8),
    "maximize": p("M4.5 9.5v-5h5M19.5 14.5v5h-5M4.5 4.5l6 6M19.5 19.5l-6-6"),
    "minimize": p("M10.5 5.5v5h-5M13.5 18.5v-5h5M10.5 10.5l-6-6M13.5 13.5l6 6"),
    "dock": r(3.5, 4.5, 17, 15, 2) + r(7, 14.5, 10, 3, 1),
    "camera": p("M4 8.5A1.5 1.5 0 0 1 5.5 7h2.2l1.4-2.2h5.8L16.3 7h2.2A1.5 1.5 0 0 1 20 8.5v9A1.5 1.5 0 0 1 18.5 19h-13A1.5 1.5 0 0 1 4 17.5z") + c(12, 13, 3.3),
    "crop": p("M4 8.5V5.5A1.5 1.5 0 0 1 5.5 4h3M15.5 4h3A1.5 1.5 0 0 1 20 5.5v3M20 15.5v3a1.5 1.5 0 0 1-1.5 1.5h-3M8.5 20h-3A1.5 1.5 0 0 1 4 18.5v-3"),

    # ---- connectivity ------------------------------------------------------
    "wifi": p("M2.8 9.3a13.5 13.5 0 0 1 18.4 0M5.9 12.6a9 9 0 0 1 12.2 0M9 15.9a4.5 4.5 0 0 1 6 0") + dot(12, 19, 1.3),
    "wifi-off": p("M5.9 12.6a9 9 0 0 1 3.6-2.1M14.3 10.4a9 9 0 0 1 3.8 2.2M2.8 9.3a13.4 13.4 0 0 1 4-2.6M11 5.6a13.5 13.5 0 0 1 10.2 3.7M9 15.9a4.5 4.5 0 0 1 6 0M4 4l16 16") + dot(12, 19, 1.3),
    "ethernet": r(9, 3.5, 6, 5, 1) + r(3.5, 15.5, 6, 5, 1) + r(14.5, 15.5, 6, 5, 1) + p("M12 8.5V12M6.5 15.5V12h11v3.5"),
    "globe": c(12, 12, 8.5) + p("M3.5 12h17M12 3.5c2.4 2.3 3.6 5.2 3.6 8.5s-1.2 6.2-3.6 8.5c-2.4-2.3-3.6-5.2-3.6-8.5s1.2-6.2 3.6-8.5z"),
    "bluetooth": p("M7 7.5l10 9-5 4.5V3l5 4.5-10 9"),
    "airplane": p("M10.5 3.8a1.5 1.5 0 0 1 3 0V9l7 4.2v2l-7-2.2v4.4l2.2 1.6v1.7L12 19.6l-3.7 1.1V19l2.2-1.6V13l-7 2.2v-2l7-4.2z"),
    # Signal strength of the connected / listed network, 0 (no signal) to 4 bars.
    "wifi-0": wifi_level(0),
    "wifi-1": wifi_level(1),
    "wifi-2": wifi_level(2),
    "wifi-3": wifi_level(3),
    "wifi-4": wifi_level(4),
    "signal": p("M5 19v-3M9.7 19v-6.5M14.3 19V9M19 19V5"),
    "share": c(6.5, 12, 2.5) + c(17.5, 6, 2.5) + c(17.5, 18, 2.5) + p("M8.7 10.8l6.6-3.6M8.7 13.2l6.6 3.6"),
    "radar": c(12, 12, 8.5) + c(12, 12, 4.4) + p("M12 12l6-6") + dot(12, 12, 1.1) + dot(16.4, 14.6, 1),
    "volume-high": p(SPEAKER) + p("M15.5 9a4.3 4.3 0 0 1 0 6M18 6.5a7.8 7.8 0 0 1 0 11"),
    "volume-low": p(SPEAKER) + p("M15.5 9a4.3 4.3 0 0 1 0 6"),
    "volume-mute": p(SPEAKER) + p("M16 9.5l5 5M21 9.5l-5 5"),
    # Volume at zero (not muted): the speaker with its waves unlit.
    "volume-zero": p(SPEAKER) + dim(p("M15.5 9a4.3 4.3 0 0 1 0 6M18 6.5a7.8 7.8 0 0 1 0 11")),
    # Output switched off / muted, struck through like mic-off and wifi-off.
    "volume-off": p("M11.2 6.4l.8-.7v3.6M12 14.6v3.7l-4.5-3.8h-3v-5h2.6M15.9 9.5a4.3 4.3 0 0 1 .8 2.5M4 4l16 16"),
    "mic": r(9, 3.5, 6, 10.5, 3) + p("M5.8 11.5a6.2 6.2 0 0 0 12.4 0M12 17.7v2.8"),
    "mic-off": p("M15 10.5V6.5a3 3 0 0 0-5.8-1.1M9 9v2a3 3 0 0 0 4.7 2.5M5.8 11.5a6.2 6.2 0 0 0 10.4 4.6M18.2 11.5a6 6 0 0 1-.3 1.9M12 17.7v2.8M4 4l16 16"),

    # ---- security ----------------------------------------------------------
    "lock": r(5.5, 10.5, 13, 9.5, 2) + p("M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5") + dot(12, 15.2, 1.2),
    "lock-open": r(5.5, 10.5, 13, 9.5, 2) + p("M8.5 10.5V7.8a3.5 3.5 0 0 1 6.6-1.6") + dot(12, 15.2, 1.2),
    "key": c(8, 15.5, 3.7) + p("M10.7 12.9L19 4.5M15.5 8l2.7 2.7M13 10.5l1.8 1.8"),
    "shield": p(SHIELD),
    "shield-check": p(SHIELD) + p("M8.7 11.8l2.4 2.4 4.3-4.6"),
    "shield-alert": p(SHIELD) + p("M12 8v4.5") + dot(12, 15.3, 1.05),
    "shield-off": p("M8.3 4.6L12 3.2l7.3 2.7v5.6c0 1.6-.4 3.1-1.1 4.5M16.2 17.9A11.7 11.7 0 0 1 12 21.1c-4.3-1.6-7.3-5.1-7.3-9.6V6.9M4 4l16 16"),
    "firewall": r(3.5, 5, 17, 14, 1.5) + p("M3.5 9.7h17M3.5 14.3h17M9 5v4.7M15 5v4.7M6.5 9.7v4.6M12 9.7v4.6M17.5 9.7v4.6M9 14.3V19M15 14.3V19"),
    "eye": p("M2.8 12S6.2 5.8 12 5.8 21.2 12 21.2 12 17.8 18.2 12 18.2 2.8 12 2.8 12z") + c(12, 12, 2.8),
    "eye-off": p("M9.9 6.1A8.6 8.6 0 0 1 12 5.8c5.8 0 9.2 6.2 9.2 6.2a15 15 0 0 1-2.4 3.1M15.5 17.4A8.3 8.3 0 0 1 12 18.2C6.2 18.2 2.8 12 2.8 12a15.2 15.2 0 0 1 3.8-4.4M10 10.1a2.8 2.8 0 0 0 3.9 3.9M4 4l16 16"),
    "bug": p("M8.5 9.5a3.5 3.5 0 0 1 7 0v5a3.5 3.5 0 0 1-7 0zM9.2 7.2L7.5 5M14.8 7.2L16.5 5M8.5 11.5H4.5M8.5 15.5l-3.2 1.8M15.5 11.5h4M15.5 15.5l3.2 1.8M12 12v5.5"),
    "target": c(12, 12, 8.5) + c(12, 12, 4.6) + dot(12, 12, 1.3) + p("M12 1.8v2.8M12 19.4v2.8M1.8 12h2.8M19.4 12h2.8"),
    "fingerprint": p("M7 6.3A7.6 7.6 0 0 1 19.6 12c0 2.6-.3 4.7-.9 6.5M4.6 10.4A7.6 7.6 0 0 0 4.4 12c0 2-.4 3.7-1 5M8 19.8c.9-2.3 1.2-4.8 1.2-7.8a2.8 2.8 0 0 1 5.6 0c0 3.6-.5 6.5-1.6 8.8M12 12c0 2.6-.3 5-1 7.2"),
    "flask": p("M9.5 3.5h5M10.5 3.5v6L5 18.2a1.5 1.5 0 0 0 1.3 2.3h11.4a1.5 1.5 0 0 0 1.3-2.3L13.5 9.5v-6M7.6 14.5h8.8"),
    "life-buoy": c(12, 12, 8.5) + c(12, 12, 3.4) + p("M6 6l3.6 3.6M14.4 14.4L18 18M18 6l-3.6 3.6M9.6 14.4L6 18"),
    "store": p("M5.5 8h13l-1 11a1.5 1.5 0 0 1-1.5 1.5H8a1.5 1.5 0 0 1-1.5-1.5zM9 8V7a3 3 0 0 1 6 0v1"),
    "arrow-up-circle": c(12, 12, 8.5) + p("M12 16.5v-9M8.3 11.2L12 7.5l3.7 3.7"),
    "pie": p("M12 3.5a8.5 8.5 0 1 0 8.5 8.5H12z") + p("M15 3.9A8.5 8.5 0 0 1 20.1 9H15z"),
    "shadow": hexagon(rad=9.3) + p("M8.6 15.8V8.2l6.8 7.6V8.2") + dot(8.6, 8.2, 1.25) + dot(15.4, 15.8, 1.25),
    "app-generic": r(4, 4, 16, 16, 4.5) + dot(9, 9, 1.3) + dot(15, 9, 1.3) + dot(9, 15, 1.3) + dot(15, 15, 1.3),
}

# Aliases keep call sites readable without duplicating geometry.
ALIASES = {
    "brightness": "sun",
    "display": "monitor",
    "network": "ethernet",
    "storage": "harddrive",
    "appearance": "palette",
    "sound": "volume-high",
    "notifications": "bell",
    "privacy": "eye-off",
    "updates": "arrow-up-circle",
    "about": "info",
    "apps": "grid",
    "night-light": "moon",
    "recovery": "life-buoy",
    "lab": "flask",
    "screenshot": "camera",
}


def svg(body: str) -> str:
    return HEAD + body + "</svg>\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--sheet", help="also write a contact sheet SVG for visual review")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    names = sorted(set(ICONS) | set(ALIASES))
    for stale in out.glob("*.svg"):
        if stale.stem not in names:
            stale.unlink()
    for name in names:
        body = ICONS[ALIASES.get(name, name)]
        target = out / f"{name}.svg"
        content = svg(body)
        if not target.exists() or target.read_text() != content:
            target.write_text(content)

    # Resource list consumed by desktop/ui/CMakeLists.txt.
    (out / "icons.list").write_text("".join(f"{n}.svg\n" for n in names))

    if args.sheet:
        cols, cell, pad = 12, 64, 20
        rows = math.ceil(len(names) / cols)
        w, h = cols * cell + pad * 2, rows * (cell + 16) + pad * 2
        parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
                 f'<rect width="{w}" height="{h}" fill="#0B0F16"/>']
        for i, name in enumerate(names):
            x = pad + (i % cols) * cell
            y = pad + (i // cols) * (cell + 16)
            body = ICONS[ALIASES.get(name, name)].replace("currentColor", "#E9EEF8")
            parts.append(f'<g transform="translate({x + 14},{y + 6}) scale(1.5)" fill="none" stroke="#E9EEF8" '
                         f'stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">{body}</g>')
            parts.append(f'<text x="{x + cell / 2}" y="{y + 58}" font-family="Inter, sans-serif" font-size="8" '
                         f'fill="#6B7892" text-anchor="middle">{name}</text>')
        parts.append("</svg>")
        Path(args.sheet).write_text("\n".join(parts))
    print(f"{len(names)} icons written to {out}")


if __name__ == "__main__":
    main()
