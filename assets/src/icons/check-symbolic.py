#!/usr/bin/env python3
"""Check that every symbolic icon the code asks for is shipped.

    assets/src/icons/check-symbolic.py [--root DIR] [--log FILE_OR_DIR ...] [--verbose]

Lists the names of SHADOW symbolic icons (desktop/ui/icons, drawn by
gen-symbolic.py, served as image://nxicon/<name>) referenced from QML, JS and
C++ under desktop/ and applications/, compares them with desktop/ui/icons, and
prints every name that is referenced but not shipped, with where it is used.
Such a name is drawn as a red placeholder box at run time.

What counts as a reference (string literals only; computed names cannot be
seen statically, hence --log):
  QML/JS  values of `icon:`, `iconName:`, `glyph:`, `<anything>Icon:` and
          `<anything>Glyph:` bindings and object keys (ternaries and ||
          chains included, comparison operands excluded); `name:` inside
          NxIcon { } and PeIcon { }; "image://nxicon/<name>" URLs; the return
          values of functions whose name contains "icon" or "glyph".
  C++     values of "icon"/"iconName"/"glyph" map keys; arguments of
          icon(...), iconButton(...), bindIcon(...); the icon field of
          brace-initialised struct tables (struct { ... icon; ... } t[] = {...}).
Theme (freedesktop) icon names are a different set (image://nxtheme,
NxAppIcon, notifications, tray, desktop files) and are not checked: the files
and keys that carry them are skipped (THEME_CONTEXTS).

--log adds names from run-time warnings ("nxicon: unknown symbolic icon 'x'",
written by SymbolicIconProvider), e.g. the logs of the lab's smoke runs.

Exit status: 0 nothing missing, 1 something missing, 2 usage error.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NAME = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
RUNTIME = re.compile(r"nxicon: unknown symbolic icon '([^']*)'")
SKIP_DIRS = {"build", ".git", "node_modules", "3rdparty", "thirdparty"}

# Places whose icon names are freedesktop theme names, not symbolic ones, or
# that do not use desktop/ui at all (the SDDM theme has its own icons).
THEME_CONTEXTS = (
    "desktop/themes/",
    "desktop/horizon-shell/modules/notifications/",
    "desktop/horizon-shell/modules/tray/",
    "desktop/horizon-shell/modules/power/powerupdates.cpp",
    "desktop/horizon-shell/modules/power/powerservice.cpp",
    "desktop/horizon-shell/modules/power/powernotifier.cpp",
    "applications/store/",
    "applications/control-center/backends/startupbackend.cpp",
)

QML_PROPERTY = re.compile(r"(?<![\w.])(icon|iconName|glyph|[a-z][A-Za-z0-9]*(?:Icon|Glyph))\s*:(?!:)")
QML_FUNCTION = re.compile(r"\bfunction\s+([A-Za-z_]\w*)\s*\(")
QML_ICON_BLOCK = re.compile(r"\b(NxIcon|PeIcon)\s*\{")
NXICON_URL = re.compile(r"image://nxicon/([a-z0-9-]+)")
CPP_KEY = re.compile(r'(?:QStringLiteral|QLatin1String|QLatin1StringView|QString::fromLatin1)?\(?\s*u?"(icon|iconName|glyph)"(?:_s|_L1)?\s*\)?\s*,')
CPP_CALL = re.compile(r"\b(icon|iconButton|bindIcon)\s*\(")
STRUCT = re.compile(r"\bstruct\s*(\w*)\s*\{")


class Refs:
    def __init__(self):
        self.where: dict[str, list[str]] = defaultdict(list)

    def add(self, name: str, where: str):
        if NAME.match(name) and where not in self.where[name]:
            self.where[name].append(where)


# ---- small lexer helpers -------------------------------------------------------------


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, keeping string literals and line numbers."""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'`":
            j = skip_string(text, i)
            out.append(text[i:j])
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def skip_string(text: str, i: int) -> int:
    quote, j = text[i], i + 1
    while j < len(text) and text[j] != quote:
        if text[j] == "\\":
            j += 1
        elif text[j] == "\n" and quote != "`":
            break
        j += 1
    return j + 1


def match_brace(text: str, i: int) -> int:
    """Index just after the bracket that closes the one at text[i]."""
    pairs = {"{": "}", "(": ")", "[": "]"}
    stack, j = [pairs[text[i]]], i + 1
    while j < len(text) and stack:
        ch = text[j]
        if ch in "\"'`":
            j = skip_string(text, j)
            continue
        if ch in pairs:
            stack.append(pairs[ch])
        elif ch == stack[-1]:
            stack.pop()
        j += 1
    return j


def expression(text: str, i: int) -> str:
    """The expression starting at i: up to a ',', ';' or closing bracket at
    depth 0, or the end of a line that does not continue on the next one."""
    j, n = i, len(text)
    while j < n:
        ch = text[j]
        if ch in "\"'`":
            j = skip_string(text, j)
            continue
        if ch in "{([":
            j = match_brace(text, j)
            continue
        if ch in ",;})]":
            break
        if ch == "\n":
            before = text[i:j].rstrip()
            rest = text[j + 1:].lstrip()
            if before.endswith(("?", ":", "||", "&&", "+", "(", "=")) or rest.startswith(("?", ":", "||", "&&", "+", ".")):
                j += 1
                continue
            break
        j += 1
    return text[i:j]


LITERAL = re.compile(r"""(?:QStringLiteral|QLatin1String|QLatin1StringView|QString::fromLatin1|QString)?\(?\s*u?(["'])((?:\\.|(?!\1).)*)\1(?:_s|_L1)?\s*\)?""")
COMPARE_BEFORE = re.compile(r"(?:===|!==|==|!=|\bcase|\.startsWith\(|\.endsWith\(|\.indexOf\(|\.includes\(|\.contains\(|\bin)\s*$")
COMPARE_AFTER = re.compile(r"^\s*(?:===|!==|==|!=|\bin\b)")


def value_literals(expr: str):
    """String literals of an expression that are values, not comparison
    operands, translations, object keys or parts of a concatenation."""
    for m in LITERAL.finditer(expr):
        before, after = expr[:m.start()], expr[m.end():]
        if COMPARE_BEFORE.search(before) or COMPARE_AFTER.match(after):
            continue
        if re.search(r"[\w.]\s*\(?\s*$", before) and not re.search(r"\b(return|case)\s*$", before):
            continue                         # an argument of a call (qsTr(), map.value(), ...)
        if re.match(r"^\s*:", after) and re.search(r"[{,]\s*$", before):
            continue                         # an object key
        if re.match(r"^\s*\+", after) or re.search(r"\+\s*$", before):
            yield ("dynamic", m.group(2))
            continue
        yield ("value", m.group(2))


def line_of(text: str, i: int) -> int:
    return text.count("\n", 0, i) + 1


# ---- QML / JS ------------------------------------------------------------------------


def scan_qml(path: Path, rel: str, refs: Refs, dynamic: Refs):
    text = strip_comments(path.read_text(errors="replace"))

    def take(expr: str, at: int):
        for kind, value in value_literals(expr):
            (refs if kind == "value" else dynamic).add(value, f"{rel}:{line_of(text, at)}")

    for m in QML_PROPERTY.finditer(text):
        take(expression(text, m.end()), m.start())
    for m in QML_ICON_BLOCK.finditer(text):
        start = text.index("{", m.start())
        block = text[start:match_brace(text, start)]
        depth = 0
        for k, ch in enumerate(block):
            if ch in "{(":
                depth += 1
            elif ch in "})":
                depth -= 1
            elif depth == 1 and block.startswith("name", k) and re.match(r"name\s*:", block[k:]) \
                    and not re.match(r"[\w.]", block[k - 1]):
                take(expression(block, k + block[k:].index(":") + 1), start + k)
    for m in NXICON_URL.finditer(text):
        refs.add(m.group(1), f"{rel}:{line_of(text, m.start())}")
    for m in QML_FUNCTION.finditer(text):
        if not re.search(r"icon|glyph", m.group(1), re.I):
            continue
        body_at = text.find("{", m.end())
        if body_at < 0:
            continue
        body_end = match_brace(text, body_at)
        for r in re.finditer(r"\breturn\b", text[body_at:body_end]):
            at = body_at + r.end()
            take(expression(text, at), at)


# ---- C++ -----------------------------------------------------------------------------


def split_top(text: str, sep: str = ",") -> list[str]:
    parts, depth, cur, i = [], 0, [], 0
    while i < len(text):
        ch = text[i]
        if ch in "\"'":
            j = skip_string(text, i)
            cur.append(text[i:j])
            i = j
            continue
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur))
    return parts


def struct_tables(text: str):
    """(icon field index, table initialiser) for brace-initialised struct tables."""
    structs = {}
    for m in STRUCT.finditer(text):
        open_at = text.index("{", m.start())
        close = match_brace(text, open_at)
        members = [d.strip() for d in split_top(text[open_at + 1:close - 1], ";") if d.strip()]
        fields = []
        for d in members:
            if "(" in d:                     # a member function
                continue
            fields.append(re.sub(r"\s*=.*$", "", d).split()[-1].lstrip("*&"))
        if "icon" not in fields:
            continue
        index = fields.index("icon")
        if m.group(1):
            structs[m.group(1)] = index
        tail = re.match(r"\s*\w+\s*\[\s*\]\s*=\s*\{", text[close:])
        if tail:                             # struct { ... } table[] = { ... }
            yield index, text[close + tail.end() - 1:match_brace(text, close + tail.end() - 1)], close
    for name, index in structs.items():
        for m in re.finditer(rf"\b(?:{name}\s+\w+\s*\[\s*\]|QList<\s*{name}\s*>\s+\w+|QVector<\s*{name}\s*>\s+\w+|"
                             rf"std::array<\s*{name}\s*,\s*\w+\s*>\s+\w+)\s*=\s*\{{", text):
            open_at = m.end() - 1
            yield index, text[open_at:match_brace(text, open_at)], open_at


def scan_cpp(path: Path, rel: str, refs: Refs, dynamic: Refs):
    text = strip_comments(path.read_text(errors="replace"))

    def take(expr: str, at: int):
        for kind, value in value_literals(expr):
            (refs if kind == "value" else dynamic).add(value, f"{rel}:{line_of(text, at)}")

    for m in CPP_KEY.finditer(text):
        take(expression(text, m.end()), m.start())
    for m in CPP_CALL.finditer(text):
        args = text[m.end() - 1:match_brace(text, m.end() - 1)][1:-1]
        parts = split_top(args)
        if parts:
            # bindIcon(target, name, ...): the name is the second argument.
            first = parts[1] if m.group(1) == "bindIcon" and len(parts) > 1 else parts[0]
            take(first, m.start())
    for index, table, at in struct_tables(text):
        for row in split_top(table[1:-1]):
            row = row.strip()
            if not row.startswith("{"):
                continue
            fields = split_top(row[1:row.rfind("}")])
            if index < len(fields):
                lit = re.fullmatch(r"""\s*(?:QStringLiteral\()?u?"([^"]*)"(?:_s)?\)?\s*""", fields[index])
                if lit:
                    refs.add(lit.group(1), f"{rel}:{line_of(text, at)}")


# ---- main ----------------------------------------------------------------------------


def shipped(root: Path) -> set[str]:
    listing = root / "desktop/ui/icons/icons.list"
    names = {line.strip()[:-4] for line in listing.read_text().splitlines() if line.strip().endswith(".svg")}
    on_disk = {p.stem for p in (root / "desktop/ui/icons").glob("*.svg")}
    if names != on_disk:
        print(f"warning: icons.list and the SVG files differ: listed only {sorted(names - on_disk)}, "
              f"files only {sorted(on_disk - names)}", file=sys.stderr)
    return names & on_disk


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(ROOT), help="source tree (default: this one)")
    ap.add_argument("--log", action="append", default=[], help="log file or directory with run-time nxicon warnings")
    ap.add_argument("--verbose", "-v", action="store_true", help="also list every referenced name")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()
    if not (root / "desktop/ui/icons/icons.list").is_file():
        print(f"{root}: not a Shadow OS source tree (no desktop/ui/icons/icons.list)", file=sys.stderr)
        return 2

    refs, dynamic = Refs(), Refs()
    for top in ("desktop", "applications"):
        for path in sorted((root / top).rglob("*")):
            if not path.is_file() or SKIP_DIRS & set(path.relative_to(root).parts):
                continue
            rel = path.relative_to(root).as_posix()
            if rel.startswith(THEME_CONTEXTS) or rel.startswith("desktop/ui/icons/"):
                continue
            if path.suffix in (".qml", ".js", ".mjs"):
                scan_qml(path, rel, refs, dynamic)
            elif path.suffix in (".cpp", ".h", ".hpp", ".cc"):
                scan_cpp(path, rel, refs, dynamic)
    for item in args.log:
        p = Path(item)
        for f in ([p] if p.is_file() else sorted(x for x in p.rglob("*") if x.is_file())):
            for m in RUNTIME.finditer(f.read_text(errors="replace")):
                refs.add(m.group(1), f"run time: {f}")

    have = shipped(root)
    missing = sorted(n for n in refs.where if n not in have)
    if args.verbose:
        for name in sorted(refs.where):
            print(f"{'ok     ' if name in have else 'MISSING'} {name}: {', '.join(refs.where[name][:3])}"
                  + (f" (+{len(refs.where[name]) - 3})" if len(refs.where[name]) > 3 else ""))
    for name in sorted(dynamic.where):
        print(f"note: part of a computed name: \"{name}\" ({', '.join(dynamic.where[name][:2])})")
    print(f"{len(refs.where)} symbolic icon names referenced, {len(have)} shipped, {len(missing)} missing")
    for name in missing:
        print(f"MISSING {name}: {', '.join(refs.where[name])}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
