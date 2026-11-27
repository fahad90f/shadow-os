#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Plymouth splash theme is complete, small and free of known traps.

    python3 tests/build/test_plymouth_theme.py [-v]

The theme (assets/boot/plymouth/shadow) is one script and about 175 images.
Plymouth reports a mistake in it only on its own error output: a script that
does not parse leaves the screen empty at boot, at the disk passphrase prompt
and at shutdown, and a missing image stops the statement that loads it. This
test cannot run the script (that needs the real daemon), but it checks what
can be read from the files:

  * every image the script loads exists, and no file is left in the directory
    that the script does not load (the whole directory is copied into every
    initramfs);
  * the directory stays within its size limit and every image is plain 8-bit
    RGBA;
  * the script does not use the constructs that were seen to kill or blank
    the splash (see the list at the top of shadow.script);
  * the tables of the script and of the image generator agree.

Standard library only; runs as any user and writes nothing.
"""
import os
import re
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
THEME = os.path.join(ROOT, "assets/boot/plymouth/shadow")
SCRIPT = os.path.join(THEME, "shadow.script")
DESCRIPTOR = os.path.join(THEME, "shadow.plymouth")
GENERATOR = os.path.join(ROOT, "assets/src/boot/gen-plymouth-theme.py")

SIZE_LIMIT = 800 * 1000          # bytes, the whole theme directory
PREFIXES = ("s_", "m_", "l_")    # the three image sets
# Images that exist only in some sets: the small set has no moving bezel.
ONLY_IN = {"bezel": ("m_", "l_")}
# How far the counting loops of the script go for each family of images.
COUNTS = {"seg": 6, "ping": 4, "bezel": 6}
# Names the theme file may give to the script, and nothing else may read them.
OPTIONS = ("motion", "status", "gov", "probe")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def code_of(script):
    """The script without comments (a '#' inside a string is kept)."""
    lines = []
    for line in script.splitlines():
        out, quoted = [], False
        for ch in line:
            if ch == '"':
                quoted = not quoted
            if ch == "#" and not quoted:
                break
            out.append(ch)
        lines.append("".join(out))
    return "\n".join(lines)


def string_lists(code):
    """Arrays of strings defined in the script: name -> list."""
    lists = {}
    for name, body in re.findall(r"^(\w+)\s*=\s*\[([^\]]*)\];", code, re.M):
        items = re.findall(r'"([^"]*)"', body)
        if items and len(items) == body.count(",") + 1:
            lists[name] = items
    return lists


def images_of(code):
    """Every file name the script can pass to Image() or img().

    The argument is a chain of '+' terms: string literals, PRE (the set
    prefix), a loop counter, or an element of a list of strings. Anything
    else is not understood and fails the test, so that a new way of naming
    images cannot slip past it.
    """
    lists = string_lists(code)
    names, problems = set(), []
    for expr in re.findall(r'\b(?:Image|img)\(([^()]*?\.png")\)', code):
        variants = [""]
        stem = "".join(re.findall(r'"([^"]*)"', expr))
        family = next((f for f in COUNTS if stem.startswith(f)), None)
        prefixes = PREFIXES
        for key, only in ONLY_IN.items():
            if stem.startswith(key):
                prefixes = only
        for term in [t.strip() for t in expr.split("+")]:
            literal = re.fullmatch(r'"([^"]*)"', term)
            indexed = re.fullmatch(r"(\w+)\[\w+\]", term)
            if literal:
                choices = [literal.group(1)]
            elif term == "PRE":
                choices = list(prefixes)
            elif indexed and indexed.group(1) in lists:
                choices = lists[indexed.group(1)]
            elif re.fullmatch(r"[a-z]", term) and family:
                choices = [str(n) for n in range(COUNTS[family])]
            else:
                problems.append("%s: cannot tell what '%s' stands for" % (expr, term))
                choices = []
            variants = [v + c for v in variants for c in choices]
        names.update(variants)
    return names, problems


def png_header(path):
    with open(path, "rb") as fh:
        head = fh.read(33)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">IIBBBBB", head[16:29])


class PlymouthTheme(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = read(SCRIPT)
        cls.code = code_of(cls.script)
        cls.files = sorted(os.listdir(THEME))
        cls.pngs = {f for f in cls.files if f.endswith(".png")}

    def test_the_directory_holds_only_the_theme(self):
        other = [f for f in self.files if not f.endswith(".png")]
        self.assertEqual(sorted(other), ["shadow.plymouth", "shadow.script"],
                         "everything in this directory ships in every initramfs")

    def test_every_image_the_script_loads_exists_and_no_other(self):
        wanted, problems = images_of(self.code)
        self.assertEqual(problems, [])
        self.assertGreater(len(wanted), 100, "the script's Image() calls were not found")
        self.assertEqual(sorted(wanted - self.pngs), [], "loaded by the script but missing")
        self.assertEqual(sorted(self.pngs - wanted), [], "in the directory but never loaded")

    def test_every_set_is_complete(self):
        # Apart from the documented exceptions the three sets hold the same images.
        per_set = {p: {f[len(p):] for f in self.pngs if f.startswith(p)} for p in PREFIXES}
        for prefix in PREFIXES:
            skipped = {f for f in per_set["l_"] for key, only in ONLY_IN.items()
                       if f.startswith(key) and prefix not in only}
            self.assertEqual(sorted(per_set[prefix]), sorted(per_set["l_"] - skipped), prefix)

    def test_size_limit(self):
        total = sum(os.path.getsize(os.path.join(THEME, f)) for f in self.files)
        self.assertLessEqual(total, SIZE_LIMIT, "theme directory is %d bytes" % total)

    def test_images_are_8_bit_rgba_without_interlacing(self):
        for name in sorted(self.pngs):
            head = png_header(os.path.join(THEME, name))
            self.assertIsNotNone(head, "%s is not a PNG" % name)
            _w, _h, depth, colour, _c, _f, interlace = head
            self.assertEqual((depth, colour, interlace), (8, 6, 0), name)

    def test_no_number_with_an_exponent(self):
        # "1e5" is a parse error, and a parse error means an empty screen.
        without_strings = re.sub(r'"[^"]*"', '""', self.code)
        found = re.findall(r"(?<![\w.])\d+(?:\.\d+)?[eE][+-]?\d+", without_strings)
        self.assertEqual(found, [])

    def test_refresh_rate_is_a_literal(self):
        calls = re.findall(r"SetRefreshRate\s*\(([^)]*)\)", self.code)
        self.assertEqual(len(calls), 1)
        self.assertRegex(calls[0].strip(), r"^\d+$")

    def test_forbidden_calls_are_not_used(self):
        for name, why in (("GetCapslockState", "kills the daemon after a mode change"),
                          ("SetKeyboardInputFunction", "sees the keys of the passphrase"),
                          ("Math.Random", "returns nearly zero in this Plymouth"),
                          ("special://", "another distribution's logo")):
            self.assertNotIn(name, self.script, why)

    def test_options_are_only_compared_as_strings(self):
        # Arithmetic on a value from the theme file killed the daemon.
        without_strings = re.sub(r'"[^"]*"', '""', self.code)
        for option in OPTIONS:
            for use in re.finditer(r"(?<![\w.])%s(?![\w.(])([^;\n]{0,6})" % option, without_strings):
                self.assertRegex(use.group(1), r'^\s*==\s*""',
                                 "option '%s' is used outside a string comparison" % option)

    def test_descriptor(self):
        text = read(DESCRIPTOR)
        self.assertRegex(text, r"(?m)^ModuleName=script$")
        self.assertRegex(text, r"(?m)^ImageDir=/usr/share/plymouth/themes/shadow$")
        self.assertRegex(text, r"(?m)^ScriptFile=/usr/share/plymouth/themes/shadow/shadow\.script$")
        variables = text.split("[script-env-vars]")
        self.assertEqual(len(variables), 2, "no [script-env-vars] group")
        self.assertRegex(variables[1], r"(?m)^motion=(full|calm)$")
        self.assertRegex(variables[1], r"(?m)^status=[01]$")

    def test_script_and_generator_use_the_same_sizes(self):
        generator = read(GENERATOR)
        for table, key in (("T_u", "u"), ("T_ringr", "ring_r"), ("T_nseg", "nseg"), ("T_tile", "tile")):
            row = re.search(r"^%s\s*=\s*\[([^\]]*)\];" % table, self.code, re.M)
            self.assertIsNotNone(row, table)
            in_script = [float(v) for v in row.group(1).split(",")]
            in_generator = []
            for prefix in PREFIXES:
                one = re.search(r'Set\(prefix="%s".*?\b%s=([\d.]+)' % (prefix, key), generator, re.S)
                self.assertIsNotNone(one, "%s of set %s" % (key, prefix))
                in_generator.append(float(one.group(1)))
            self.assertEqual(in_script, in_generator, "%s / %s" % (table, key))


if __name__ == "__main__":
    unittest.main()
