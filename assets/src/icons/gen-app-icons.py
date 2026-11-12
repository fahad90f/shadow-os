#!/usr/bin/env python3
"""Generate the SHADOW application and category icons.

    assets/src/icons/gen-app-icons.py [--sheet FILE.svg]

Every SHADOW application icon is the same object: an obsidian tile with a thin
luminous edge, carrying one glyph from the SHADOW symbolic set drawn in a
two-colour gradient with a soft glow. The gradient encodes the family the
application belongs to (system, security, network, files, development,
maintenance), so related tools are recognisable at a glance in the launcher.

Glyph geometry is imported from gen-symbolic.py — an application icon can
only use a glyph that exists in the system icon set.

Output:
  assets/icons/apps/<app-id>.svg          (installed to hicolor/scalable/apps)
  assets/icons/categories/shadow-<name>.svg (arsenal categories)
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("gen_symbolic", ROOT / "assets/src/icons/gen-symbolic.py")
sym = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sym)

TOK = json.loads((ROOT / "desktop/themes/tokens/shadow-tokens.json").read_text())
ACC = {k: v["dark"] for k, v in TOK["accent"].items()}
OBS = TOK["brand"]["obsidian"]

FAMILIES = {
    "system":      (ACC["cyan"], ACC["blue"]),
    "security":    (ACC["blue"], ACC["violet"]),
    "network":     (ACC["cyan"], ACC["emerald"]),
    "files":       (ACC["blue"], ACC["cyan"]),
    "development": (ACC["violet"], ACC["rose"]),
    "maintenance": (ACC["amber"], ACC["rose"]),
    "assistant":   (ACC["cyan"], ACC["violet"]),
}

APPS = {
    # app id                       glyph             family
    "org.shadow.files":            ("folder",         "files"),
    "org.shadow.archivemanager":   ("archive",        "files"),
    "org.shadow.diskmanager":      ("harddrive",      "files"),
    "org.shadow.terminal":         ("terminal",       "development"),
    "org.shadow.editor":           ("edit",           "development"),
    "org.shadow.monitor":          ("activity",       "system"),
    "org.shadow.taskmanager":      ("list",           "system"),
    "org.shadow.sysinfo":          ("cpu",            "system"),
    "org.shadow.controlcenter":    ("settings",       "system"),
    "org.shadow.screenshot":       ("camera",         "system"),
    "org.shadow.store":            ("store",          "system"),
    "org.shadow.updater":          ("arrow-up-circle", "maintenance"),
    "org.shadow.installer":        ("download",       "maintenance"),
    "org.shadow.recovery":         ("life-buoy",      "maintenance"),
    "org.shadow.securitycenter":   ("shield-check",   "security"),
    "org.shadow.arsenal":          ("target",         "security"),
    "org.shadow.networkcenter":    ("globe",          "network"),
    "org.shadow.gallery":          ("layers",         "development"),
    "org.shadow.assistant":        ("zap",            "assistant"),
    "org.shadow.hideme":           ("eye-off",        "security"),
}

CATEGORIES = {
    # arsenal category            glyph           family
    "reconnaissance":            ("radar",        "network"),
    "web-security":              ("globe",        "security"),
    "network-security":          ("ethernet",     "network"),
    "wireless":                  ("wifi",         "network"),
    "password-auditing":         ("key",          "security"),
    "reverse-engineering":       ("cpu",          "development"),
    "forensics":                 ("file-search",  "files"),
    "exploitation":              ("bug",          "security"),
    "development":               ("code",         "development"),
    "blue-team":                 ("shield-check", "system"),
    "red-team":                  ("target",       "maintenance"),
}


def squircle(size, n=5.0, inset=10.0):
    """Superellipse outline: softer than a rounded rectangle at icon sizes."""
    c, a = size / 2, size / 2 - inset
    pts = []
    for i in range(96):
        t = 2 * math.pi * i / 96
        ct, st = math.cos(t), math.sin(t)
        x = c + a * math.copysign(abs(ct) ** (2 / n), ct)
        y = c + a * math.copysign(abs(st) ** (2 / n), st)
        pts.append(f"{x:.2f} {y:.2f}")
    return "M" + " L".join(pts) + "Z"


def icon(glyph, family, size=256):
    c1, c2 = FAMILIES[family]
    body = sym.ICONS[sym.ALIASES.get(glyph, glyph)]
    plate = squircle(size)
    scale = 5.4                                   # 24-unit glyph → ~130 px
    off = (size - 24 * scale) / 2
    stroke = 1.75
    g = (f'<g transform="translate({off:.2f} {off:.2f}) scale({scale})" fill="none" stroke="url(#g)" '
         f'stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round" color="url(#g)">'
         + body.replace("currentColor", "url(#g)") + "</g>")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {size} {size}">
<defs>
<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="4" y1="3" x2="20" y2="21"><stop offset="0" stop-color="{c1}"/><stop offset="1" stop-color="{c2}"/></linearGradient>
<linearGradient id="e" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{c1}" stop-opacity="0.85"/><stop offset="0.5" stop-color="{c2}" stop-opacity="0.25"/><stop offset="1" stop-color="{c2}" stop-opacity="0.7"/></linearGradient>
<linearGradient id="p" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1A2233"/><stop offset="1" stop-color="{OBS}"/></linearGradient>
<radialGradient id="h" cx="0.5" cy="0.42" r="0.55"><stop offset="0" stop-color="{c1}" stop-opacity="0.22"/><stop offset="1" stop-color="{c1}" stop-opacity="0"/></radialGradient>
<filter id="b" x="-40%" y="-40%" width="180%" height="180%"><feGaussianBlur stdDeviation="1.3"/></filter>
</defs>
<path d="{plate}" fill="url(#p)"/>
<path d="{plate}" fill="url(#h)"/>
<path d="{plate}" fill="none" stroke="url(#e)" stroke-width="3"/>
<g opacity="0.55" filter="url(#b)">{g}</g>
{g}
</svg>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sheet")
    args = ap.parse_args()
    apps_dir, cat_dir = ROOT / "assets/icons/apps", ROOT / "assets/icons/categories"
    apps_dir.mkdir(parents=True, exist_ok=True)
    cat_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for app_id, (glyph, family) in APPS.items():
        (apps_dir / f"{app_id}.svg").write_text(icon(glyph, family))
        written.append((app_id, apps_dir / f"{app_id}.svg"))
    for name, (glyph, family) in CATEGORIES.items():
        (cat_dir / f"shadow-{name}.svg").write_text(icon(glyph, family))
        written.append((name, cat_dir / f"shadow-{name}.svg"))
    # The distributor logo doubles as the icon for "Shadow OS" itself.
    (apps_dir / "shadow-os.svg").write_text((ROOT / "assets/logos/shadow-symbol-plate.svg").read_text())
    written.append(("shadow-os", apps_dir / "shadow-os.svg"))

    if args.sheet:
        cols, cell = 8, 150
        rows = math.ceil(len(written) / cols)
        w, h = cols * cell + 40, rows * (cell + 26) + 40
        parts = [f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{w}" height="{h}">',
                 f'<rect width="{w}" height="{h}" fill="#0B0F16"/>']
        for i, (name, path) in enumerate(written):
            x, y = 20 + (i % cols) * cell, 20 + (i // cols) * (cell + 26)
            parts.append(f'<image x="{x + 15}" y="{y}" width="120" height="120" xlink:href="{path}"/>')
            parts.append(f'<text x="{x + cell / 2}" y="{y + 138}" font-family="Inter, sans-serif" font-size="11" '
                         f'fill="#A3B0C6" text-anchor="middle">{name.replace("org.shadow.", "")}</text>')
        parts.append("</svg>")
        Path(args.sheet).write_text("\n".join(parts))
    print(f"{len(written)} icons written")


if __name__ == "__main__":
    main()
