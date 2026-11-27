#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The boot menu theme (GRUB gfxmenu) keeps the rules GRUB is strict about.

    python3 tests/build/test_grub_theme.py [-v]

GRUB gives no second chance: one picture it cannot read, one stray '%' in a
label or one font name that is not loaded, and the menu is drawn wrong or
replaced by the plain text menu after a 10 second wait. None of that shows up
in a package build, so this test reads assets/boot/grub/shadow/ the way GRUB
does and checks:

  * every file theme.txt names exists; boxes have either all nine slices or
    only the centre one; the row pictures have exactly the size of a row and
    draw nothing where an entry's title can be;
  * every picture is a PNG with 8 bits per channel, RGB or RGBA, not
    interlaced, and at least one PNG and all fonts sit in the top directory
    (update-grub looks only there);
  * label texts hold no '%' except the one "%02d" of the countdown number,
    and only countdown elements (id "__timeout__") hold countdown text;
  * every font name is the exact name inside one shipped .pf2 file, and the
    alphabetically last .pf2 (GRUB's fallback font) is z-mono-14.pf2;
  * the layout fits 640x480 with four entries, nothing touches the corner
    brackets, and the command-line box is large enough for 80x24 characters
    from 720 px screen height up;
  * the theme directory stays within 300 kB;
  * the live medium's grub.cfg template loads the fonts, exports the theme and
    still has the automated-test hook, unchanged;
  * the package script that refreshes the boot loader also rewrites grub.cfg;
  * where the tools are installed: the generator produces the same set of
    pictures and fonts.

It does not start GRUB. docs/architecture/boot-menu.md says how the theme was
looked at in a real GRUB.

Standard library only; runs as any user and writes only to a temporary
directory.
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
THEME = os.path.join(ROOT, "assets", "boot", "grub", "shadow")
GENERATOR = os.path.join(ROOT, "assets", "src", "boot", "gen-grub-theme.sh")
LIVE_CFG = os.path.join(ROOT, "system", "boot", "grub", "live-grub.cfg.in")
DEFAULT_GRUB = os.path.join(ROOT, "system", "boot", "shadow-boot", "grub")
POSTINST = os.path.join(ROOT, "debian", "shadow-boot.postinst")

BUILTIN_FONT = "GNU Unifont Regular 16"         # part of GRUB itself, 8x16 cells
SIZE_LIMIT = 300 * 1000
SCREENS = ((640, 480), (1024, 768), (1280, 720), (1280, 800), (1920, 1080))

# The hook the QEMU test harness relies on (tests/lib/qemu.py). It must stay
# exactly like this.
AUTOTEST_BLOCK = '''\
smbios --type 1 --get-string 7 --set shadow_smbios_serial
if [ "$shadow_smbios_serial" = "SHADOW-AUTOTEST" ]; then
    set default="advanced>live-serial"
    set timeout=1
fi
if [ "$shadow_smbios_serial" = "SHADOW-AUTOTEST-INSTALLED" ]; then
    set default="advanced>localboot"
    set timeout=1
fi
'''


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_comments(text):
    """Remove '#' comments; a '#' inside double quotes (a colour) stays."""
    out = []
    for line in text.splitlines():
        kept, quoted = [], False
        for ch in line:
            if ch == '"':
                quoted = not quoted
            if ch == "#" and not quoted:
                break
            kept.append(ch)
        out.append("".join(kept))
    return "\n".join(out)


def parse_theme(text):
    """Return (global properties, [(component kind, properties)])."""
    text = strip_comments(text)
    head = re.split(r"^\+", text, maxsplit=1, flags=re.M)[0]
    globals_ = dict(re.findall(r'^([a-z-]+)\s*:\s*"([^"]*)"', head, re.M))
    components = []
    for kind, body in re.findall(r"\+\s*([a-z_]+)\s*\{([^}]*)\}", text):
        props = {}
        for key, value in re.findall(r'([a-z_]+)\s*=\s*("[^"]*"|\S+)', body):
            props[key] = value.strip('"')
        components.append((kind, props))
    return globals_, components


def png_info(path):
    """(width, height, bit depth, colour type, interlace) or None if not a PNG."""
    with open(path, "rb") as f:
        head = f.read(33)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", head[16:29])
    return (w, h, depth, ctype, interlace)


def png_alpha(path):
    """(width, height, rows of alpha values) of an 8-bit RGBA PNG whose rows use
    the filters None, Sub or Up (what the generator's own writer uses)."""
    with open(path, "rb") as f:
        data = f.read()
    w, h, depth, ctype = struct.unpack(">IIBB", data[16:26])
    assert (depth, ctype) == (8, 6), path
    pos, idat = 8, b""
    while pos < len(data):
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + size]
        pos += 12 + size
    raw, stride = zlib.decompress(idat), w * 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        base = y * (stride + 1)
        row = bytearray(raw[base + 1:base + 1 + stride])
        if raw[base] == 1:
            for i in range(4, stride):
                row[i] = (row[i] + row[i - 4]) & 255
        elif raw[base] == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 255
        else:
            assert raw[base] == 0, "%s: PNG filter %d" % (path, raw[base])
        rows.append(row[3::4])
        prev = row
    return (w, h, rows)


def pf2_name(path):
    """The NAME section of a GRUB PF2 font file."""
    with open(path, "rb") as f:
        data = f.read(4096)
    pos = 0
    while pos + 8 <= len(data):
        kind = data[pos:pos + 4]
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        if kind == b"NAME":
            return data[pos + 8:pos + 8 + length].rstrip(b"\0").decode()
        if kind == b"DATA":
            break
        pos += 8 + length
    return None


def length(expr, total):
    """A GRUB theme length: "40", "50%", "50%-308", "4%+18"."""
    m = re.fullmatch(r"(?:(\d+)%)?([+-]?\d+)?", expr)
    if not m or not (m.group(1) or m.group(2)):
        raise ValueError("not a length: %r" % expr)
    value = int(m.group(1)) * total // 100 if m.group(1) else 0
    return value + (int(m.group(2)) if m.group(2) else 0)


def all_files(top):
    for folder, _, names in os.walk(top):
        for name in names:
            yield os.path.join(folder, name)


class ThemeFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = read(os.path.join(THEME, "theme.txt"))
        cls.globals, cls.components = parse_theme(cls.text)

    def of_kind(self, kind):
        return [props for k, props in self.components if k == kind]

    def slices(self, pattern):
        """Which of the nine slices of a styled box ("menu_*.png") exist."""
        self.assertEqual(pattern.count("*"), 1, pattern)
        return [s for s in ("nw", "n", "ne", "w", "c", "e", "sw", "s", "se")
                if os.path.isfile(os.path.join(THEME, pattern.replace("*", s)))]

    def box_patterns(self):
        """{pattern: where it is used} for every styled box of the theme."""
        found = {}
        if "terminal-box" in self.globals:
            found[self.globals["terminal-box"]] = "terminal-box"
        for kind, props in self.components:
            for key in ("menu_pixmap_style", "item_pixmap_style", "selected_item_pixmap_style",
                        "scrollbar_frame", "scrollbar_thumb", "bar_style", "highlight_style"):
                if key in props:
                    found[props[key]] = "%s %s" % (kind, key)
        return found

    def test_the_parser_sees_the_whole_theme(self):
        kinds = sorted(k for k, _ in self.components)
        self.assertEqual(kinds.count("boot_menu"), 1)
        self.assertEqual(kinds.count("circular_progress"), 1)
        self.assertEqual(kinds.count("progress_bar"), 1)
        self.assertEqual(len(re.findall(r"^\+", strip_comments(self.text), re.M)), len(self.components),
                         "a component of theme.txt was not understood by this test")
        self.assertEqual(self.globals.get("title-text"), "")

    def test_every_named_file_exists(self):
        names = [self.globals["desktop-image"]]
        for kind, props in self.components:
            for key in ("file", "center_bitmap", "tick_bitmap"):
                if key in props:
                    names.append(props[key])
        for name in names:
            self.assertTrue(os.path.isfile(os.path.join(THEME, name)), "theme.txt names %s, which is missing" % name)

    def test_boxes_are_whole_frames_or_single_centre_slices(self):
        frames = {"menu_pixmap_style", "terminal-box"}
        for pattern, where in self.box_patterns().items():
            have = self.slices(pattern)
            if where.split()[-1] in frames:
                self.assertEqual(len(have), 9, "%s (%s) needs all nine slices, has %s" % (pattern, where, have))
            else:
                # Rows, bars and the scrollbar: a box with edge slices would
                # move the selected row off its slot and change the row width.
                self.assertEqual(have, ["c"], "%s (%s) must be the centre slice only" % (pattern, where))

    def test_row_pictures_have_exactly_the_size_of_a_row(self):
        menu = self.of_kind("boot_menu")[0]
        west = png_info(os.path.join(THEME, menu["menu_pixmap_style"].replace("*", "w")))
        east = png_info(os.path.join(THEME, menu["menu_pixmap_style"].replace("*", "e")))
        row = (int(menu["width"]) - west[0] - east[0], int(menu["item_height"]))
        for key in ("item_pixmap_style", "selected_item_pixmap_style"):
            info = png_info(os.path.join(THEME, menu[key].replace("*", "c")))
            self.assertEqual(info[:2], row, "%s is stretched by GRUB unless it is %dx%d" % (menu[key], row[0], row[1]))
        # Constant row width, with or without a scrollbar.
        self.assertEqual(menu.get("scrollbar_slice"), "east")
        self.assertEqual(menu.get("scrollbar_left_pad"), "0")
        self.assertEqual(menu.get("item_padding"), "0")
        self.assertLessEqual(int(menu["scrollbar_width"]), east[0])

    def test_every_picture_is_a_png_grub_can_read(self):
        pictures = 0
        for path in all_files(THEME):
            name = os.path.relpath(path, THEME)
            ext = os.path.splitext(name)[1].lower()
            self.assertIn(ext, (".png", ".pf2", ".txt"), "unexpected file %s (the live medium loads only PNG)" % name)
            if ext != ".png":
                continue
            pictures += 1
            info = png_info(path)
            self.assertIsNotNone(info, "%s is not a PNG file" % name)
            self.assertEqual(info[2], 8, "%s: %d bits per channel, GRUB needs 8" % (name, info[2]))
            self.assertIn(info[3], (2, 6), "%s: colour type %d; a palette or grey PNG is drawn wrong" % (name, info[3]))
            self.assertEqual(info[4], 0, "%s is interlaced" % name)
        self.assertGreater(pictures, 20)

    def test_row_pictures_draw_nothing_where_a_title_can_be(self):
        # GRUB cuts a long title at the row's right edge and has no padding
        # there: the recovery entry of every installed system reaches the
        # edge. So right of the icon, at text height, a row picture may hold
        # only its faint fill (and the 1 px outline in the last column).
        menu = self.of_kind("boot_menu")[0]
        text_left = int(menu["icon_width"]) + int(menu["item_icon_space"])
        for key in ("item_pixmap_style", "selected_item_pixmap_style"):
            name = menu[key].replace("*", "c")
            w, h, rows = png_alpha(os.path.join(THEME, name))
            for y in range(6, h - 13):
                strongest = max(rows[y][text_left:w - 1])
                self.assertLess(strongest, 64, "%s draws over the title's place in row %d" % (name, y))

    def test_png_and_fonts_sit_in_the_top_directory(self):
        top = os.listdir(THEME)
        self.assertTrue([n for n in top if n.endswith(".png")],
                        "update-grub writes 'insmod png' only for a PNG in the theme's top directory")
        everywhere = [os.path.relpath(p, THEME) for p in all_files(THEME) if p.endswith(".pf2")]
        self.assertEqual(sorted(everywhere), sorted(n for n in top if n.endswith(".pf2")))

    def test_icons_exist_for_the_live_entries_and_have_the_icon_size(self):
        menu = self.of_kind("boot_menu")[0]
        size = (int(menu["icon_width"]), int(menu["icon_height"]))
        icons = os.path.join(THEME, "icons")
        for name in os.listdir(icons):
            self.assertEqual(png_info(os.path.join(icons, name))[:2], size, "icons/%s is scaled by GRUB" % name)
        entries = re.findall(r"^\s*(?:menuentry|submenu)\s.*$", read(LIVE_CFG), re.M)
        self.assertGreaterEqual(len(entries), 9)
        for line in entries:
            classes = re.findall(r"--class\s+(\S+)", line)
            self.assertTrue([c for c in classes if os.path.isfile(os.path.join(icons, c + ".png"))],
                            "no icon for any class of: %s" % line.strip())

    def test_percent_signs_and_countdown_text(self):
        number_labels = 0
        for kind, props in self.components:
            text = props.get("text", "")
            countdown = props.get("id") == "__timeout__"
            if kind == "label":
                rest = text.replace("%02d", "", 1) if countdown else text
                self.assertNotIn("%", rest, "a '%%' in a label makes GRUB drop the theme: %r" % text)
                number_labels += text.count("%02d")
            elif text:
                self.assertNotIn("%", text)
            if "%" in text or "TIMEOUT" in text:
                self.assertTrue(countdown, "countdown text outside a __timeout__ element stays after a key press: %r" % text)
            if kind == "label" and not countdown:
                self.assertEqual(text, "", "static words belong into pictures, and none of them may mention the countdown")
        self.assertEqual(number_labels, 1)

    def test_the_countdown_dots_step_evenly(self):
        ticks = int(self.of_kind("circular_progress")[0]["num_ticks"])
        live = int(re.search(r"^set timeout=(\d+)$", read(LIVE_CFG), re.M).group(1))
        installed = int(re.search(r"^GRUB_TIMEOUT=(\d+)$", read(DEFAULT_GRUB), re.M).group(1))
        for timeout in (live, installed):
            self.assertEqual(ticks % timeout, 0, "%d dots cannot step evenly over %d seconds" % (ticks, timeout))

    def test_font_names_are_the_names_inside_the_font_files(self):
        shipped = {}
        for name in sorted(os.listdir(THEME)):
            if name.endswith(".pf2"):
                shipped[pf2_name(os.path.join(THEME, name))] = name
        asked = {self.globals["terminal-font"]}
        for _, props in self.components:
            for key in ("font", "item_font", "selected_item_font"):
                if key in props:
                    asked.add(props[key])
        self.assertIn("font", self.of_kind("progress_bar")[0])
        for font in asked - {BUILTIN_FONT}:
            self.assertIn(font, shipped, "theme.txt asks for the font %r; the files hold %s. "
                          "GRUB silently uses the last loaded font instead." % (font, sorted(shipped)))
        # The terminal font must be the built-in one: it is the only font under
        # Secure Boot, and a name GRUB does not know gives a full-screen terminal.
        self.assertEqual(self.globals["terminal-font"], BUILTIN_FONT)

    def test_the_last_font_is_the_small_monospace_one(self):
        fonts = sorted(n for n in os.listdir(THEME) if n.endswith(".pf2"))
        self.assertEqual(fonts, ["a-sans-16.pf2", "b-monob-28.pf2", "z-mono-14.pf2"])
        self.assertEqual(pf2_name(os.path.join(THEME, fonts[-1])), "Shadow Mono Regular 14")
        # It also has to sort after the font files of the previous theme, which
        # shadow-grub-install's "cp -r" leaves in /boot/grub/themes/shadow.
        for old in ("mono-14.pf2", "sans-16.pf2", "sans-24.pf2"):
            self.assertGreater(fonts[-1], old)
            self.assertFalse(os.path.exists(os.path.join(THEME, old)), "%s belongs to the previous theme" % old)

    def test_the_directory_stays_small(self):
        total = sum(os.path.getsize(p) for p in all_files(THEME))
        self.assertLessEqual(total, SIZE_LIMIT, "the theme is %d bytes; it is read from /boot before the menu shows" % total)

    # ---- layout -----------------------------------------------------------------

    def rectangles(self, width, height):
        """[(name, left, top, right, bottom)] of every component on a screen."""
        out = []
        for kind, props in self.components:
            if kind == "image":
                w, h = png_info(os.path.join(THEME, props["file"]))[:2]
                name = props["file"]
            else:
                w = length(props["width"], width)
                h = length(props["height"], height) if "height" in props else 30   # a label: one line of text
                name = kind
            left, top = length(props["left"], width), length(props["top"], height)
            out.append((name, left, top, left + w, top + h))
        return out

    def test_everything_is_on_screen_and_clear_of_the_corner_brackets(self):
        for width, height in SCREENS:
            rects = self.rectangles(width, height)
            corners = [r for r in rects if r[0].startswith("corner_")]
            self.assertEqual(len(corners), 4)
            for name, left, top, right, bottom in rects:
                where = "%s at %dx%d" % (name, width, height)
                self.assertTrue(0 <= left and 0 <= top and right <= width and bottom <= height, where + " is partly off screen")
                if name.startswith("corner_"):
                    continue
                for cname, cl, ct, cr, cb in corners:
                    apart = right <= cl or cr <= left or bottom <= ct or cb <= top
                    self.assertTrue(apart, "%s touches %s" % (where, cname))

    def test_four_entries_fit_on_a_640x480_screen(self):
        menu = self.of_kind("boot_menu")[0]
        north = png_info(os.path.join(THEME, menu["menu_pixmap_style"].replace("*", "n")))[1]
        south = png_info(os.path.join(THEME, menu["menu_pixmap_style"].replace("*", "s")))[1]
        inside = length(menu["height"], 480) - north - south
        row, gap = int(menu["item_height"]), int(menu["item_spacing"])
        self.assertGreaterEqual(inside, 4 * row + 3 * gap)

    def test_the_instrument_and_its_dots_share_one_box(self):
        ring = self.of_kind("circular_progress")[0]
        plate = [p for p in self.of_kind("image") if p["file"] == "reticle.png"][0]
        self.assertEqual((plate["left"], plate["top"]), (ring["left"], ring["top"]))
        self.assertEqual(png_info(os.path.join(THEME, "reticle.png"))[:2], (int(ring["width"]), int(ring["height"])))
        self.assertEqual(ring["id"], "__timeout__")

    def test_the_command_line_box_holds_80x24_characters(self):
        g = self.globals
        border = int(g["terminal-border"])
        north = png_info(os.path.join(THEME, g["terminal-box"].replace("*", "n")))[1]
        south = png_info(os.path.join(THEME, g["terminal-box"].replace("*", "s")))[1]
        plate = [p for p in self.of_kind("image") if p["file"] == "reticle.png"][0]
        menu = self.of_kind("boot_menu")[0]
        hints = [p for p in self.of_kind("image") if p["file"] == "hints.png"][0]
        for width, height in SCREENS:
            if height < 720:
                # No room for 24 lines under the instrument. GRUB then draws
                # its own centred box over the header (seen at 800x600) or
                # uses the whole screen (seen at 640x480).
                continue
            w, h = length(g["terminal-width"], width), length(g["terminal-height"], height)
            left, top = length(g["terminal-left"], width), length(g["terminal-top"], height)
            where = " at %dx%d" % (width, height)
            # Otherwise GRUB replaces the box with a centred one over the header.
            self.assertGreaterEqual(w, 80 * 8 + 2 * border, "terminal too narrow" + where)
            self.assertGreaterEqual(h, 24 * 16 + 2 * border, "terminal too low" + where)
            self.assertLessEqual(top + h + south, height, "terminal frame leaves the screen" + where)
            # The frame starts below the instrument ...
            self.assertGreaterEqual(top - north, length(plate["top"], height) + 176, "terminal frame covers the instrument" + where)
            # ... and hides the entry frame and the key hints completely.
            self.assertLessEqual(left, length(menu["left"], width))
            self.assertGreaterEqual(left + w, length(menu["left"], width) + int(menu["width"]))
            self.assertGreaterEqual(top + h, length(hints["top"], height) + 16, "key hints stick out under the terminal" + where)


class LiveMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cfg = read(LIVE_CFG)

    def test_the_automated_test_hook_is_unchanged(self):
        self.assertIn(AUTOTEST_BLOCK, self.cfg)
        self.assertIn("set default=live\nset timeout=8\n", self.cfg)
        self.assertIn("\nset gfxmode=1280x800,1280x720,1024x768,auto\nset gfxpayload=keep\n", self.cfg)

    def test_the_built_in_font_is_found_under_secure_boot(self):
        self.assertIn("if [ x$feature_default_font_path = xy ] ; then\n    font=unicode\n", self.cfg)
        self.assertIn("if loadfont $font ; then\n    insmod gfxterm\n    insmod png\n    terminal_output gfxterm\nfi\n", self.cfg)

    def test_theme_fonts_are_loaded_in_order_and_the_theme_is_exported(self):
        lines = [line.strip() for line in strip_comments(self.cfg).splitlines() if line.strip()]
        start = lines.index("if [ -f $prefix/themes/shadow/theme.txt ]; then")
        block = lines[start:lines.index("else", start)]
        fonts = sorted(n for n in os.listdir(THEME) if n.endswith(".pf2"))
        self.assertEqual(block[1:], ['if [ "$lockdown" != "y" ]; then']
                         + ["loadfont $prefix/themes/shadow/" + name for name in fonts]
                         + ["fi", "set theme=$prefix/themes/shadow/theme.txt", "export theme"])
        self.assertNotIn("@LOADFONTS@", self.cfg)


class PackageScript(unittest.TestCase):
    def test_the_boot_loader_refresh_also_rewrites_grub_cfg(self):
        text = read(POSTINST)
        subprocess.run(["sh", "-n", POSTINST], check=True)
        refresh = text[text.index("refresh() {"):text.index('case "$1" in')]
        ok = refresh.index("if shadow-grub-install; then")
        self.assertIn("if command -v update-grub >/dev/null 2>&1 && [ -f /boot/grub/grub.cfg ]; then\n"
                      '            update-grub >/dev/null 2>&1 || echo "shadow-boot: WARNING: update-grub failed" >&2\n',
                      refresh[ok:refresh.index("    else\n", ok)])


class Generator(unittest.TestCase):
    """Runs gen-grub-theme.sh where its tools and fonts are installed."""

    def run_generator(self, option, tools):
        for tool in ("bash", "fc-list") + tools:
            if not shutil.which(tool):
                self.skipTest("%s is not installed" % tool)
        families = subprocess.run(["fc-list", ":", "family"], capture_output=True, text=True).stdout
        if "JetBrains Mono" not in families or "Inter" not in families:
            self.skipTest("the Inter and JetBrains Mono fonts are not installed")
        out = tempfile.mkdtemp(prefix="shadow-grub-theme-")
        self.addCleanup(shutil.rmtree, out, True)
        done = subprocess.run(["bash", GENERATOR, option, out], capture_output=True, text=True, timeout=300)
        self.assertEqual(done.returncode, 0, done.stderr)
        return out

    def test_the_script_is_valid_and_explains_itself(self):
        subprocess.run(["bash", "-n", GENERATOR], check=True)
        done = subprocess.run(["bash", GENERATOR], capture_output=True, text=True)
        self.assertEqual(done.returncode, 2)
        self.assertIn("--images-only", done.stderr)

    def test_it_makes_the_same_pictures(self):
        out = self.run_generator("--images-only", ("rsvg-convert",))
        made = sorted(os.path.relpath(p, out) for p in all_files(out) if p.endswith(".png"))
        kept = sorted(os.path.relpath(p, THEME) for p in all_files(THEME) if p.endswith(".png"))
        self.assertEqual(made, kept)
        for name in kept:
            self.assertEqual(png_info(os.path.join(out, name)), png_info(os.path.join(THEME, name)), name)
        # The background is computed by the script itself (no renderer whose
        # version could matter), so it must come out byte for byte.
        for name in ("background.png", "blank.png"):
            with open(os.path.join(out, name), "rb") as a, open(os.path.join(THEME, name), "rb") as b:
                self.assertEqual(a.read(), b.read(), "%s differs from what the generator makes" % name)
        self.assertEqual(read(os.path.join(out, "theme.txt")), read(os.path.join(THEME, "theme.txt")))

    def test_it_makes_the_same_fonts(self):
        out = self.run_generator("--fonts-only", ("grub-mkfont",))
        made = sorted(n for n in os.listdir(out) if n.endswith(".pf2"))
        self.assertEqual(made, sorted(n for n in os.listdir(THEME) if n.endswith(".pf2")))
        for name in made:
            self.assertEqual(pf2_name(os.path.join(out, name)), pf2_name(os.path.join(THEME, name)))


if __name__ == "__main__":
    unittest.main()
