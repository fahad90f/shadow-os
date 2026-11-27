# Boot menu (GRUB theme)

What the GRUB menu of Shadow OS looks like, how it is built, and the rules
that keep it working. The rest of the boot path is in [boot.md](boot.md).

Sources: `assets/boot/grub/shadow/` (the theme), `assets/src/boot/gen-grub-theme.sh`
(its generator), `system/boot/grub/live-grub.cfg.in` (the live medium's menu),
`system/boot/shadow-boot/` and `debian/shadow-boot.postinst` (installed
systems), `tests/build/test_grub_theme.py`.

What has been verified (QEMU only): the theme in the real GRUB of Debian
trixie (2.12-9+deb13u4) under legacy BIOS, UEFI, and UEFI with Secure Boot
enforcing (shim, then Debian's signed GRUB), at 1280x800, 1280x720, 1024x768,
1920x1080 and 640x480. Nothing has been looked at on physical hardware. The
section [What is checked, and what is not](#what-is-checked-and-what-is-not)
has the details.

## What GRUB can and cannot do

GRUB's graphical menu cannot animate. Screenshots taken four times a second
showed no changed pixel between two countdown steps. There are no timers, no
fades, nothing that moves or rotates. The only things that change by
themselves are the elements GRUB ties to the countdown: they step once a
second, and they all disappear together at the first key press.

So the menu is the boot splash's instrument *at rest*, with one stepped
element. The motion of the design happens in the splash, the login screen and
the desktop; here the same shapes stand still.

Showing a sequence of pictures before the menu (a `background_image` loop in
grub.cfg) is possible, but it delays every boot, cannot run while the menu is
shown, and stalled for several seconds when it was tried. It is not used.

## What is on the screen

Everything sits in one column 616 px wide, placed with "percent plus or minus
pixels", so it is centred on any screen and still fits 640x480.

```
┌                                                          ┐   corner brackets
                         . · ' ' · .
       SHADOW OS  o────o (  symbol  ) o────o  T-07             header
                 /       ' · . . · '         \
     ┌──────────/─────────────────────────────\──────────┐
     │▌ ⬡  Shadow OS - Live session                  ››› │     entries
     │  ⬡  Shadow OS - Live session (safe graphics)      │
     │  ⬡  Install Shadow OS                             │
     │  »  Advanced options                              │
     └───────────────────────────────────────────────────┘
                       7s remaining.
                  ━━━━━──────────────────
            [ENTER] BOOT  [E] EDIT  [C] CONSOLE  [ESC] BACK
└                                                          ┘
```

| Part | What it is | Changes? |
|---|---|---|
| Background | The field gradient of all boot surfaces (`#04060B` to `#020306`), a cyan glow behind the instrument, a horizon glow, three faint arcs. Soft shapes only, because GRUB stretches the picture to the screen. | no |
| Corner brackets | Four L shapes, 16 px from the screen corners. | no |
| Instrument | The Shadow symbol on its plate: 72 fine ticks, six cyan index marks, a violet dashed ring, and a thin empty track round it. | no |
| Conduit | Thin traces from the instrument outwards and down onto the entry frame, with small ring nodes. Lines only at multiples of 30 degrees. | no |
| Wordmark | "SHADOW OS" as a picture. | no |
| Countdown dots | 40 dots on the instrument's track, taken off clockwise from 12 o'clock: 5 a second on the live medium (timeout 8), 8 a second on an installed system (timeout 5). | once a second |
| Countdown number | `T-07`: the remaining seconds. | once a second |
| Countdown line | GRUB's own, translated "7s remaining." over a thin line that fills. | once a second |
| Entries | A framed list. Rows are 40 px high; the selected row has a cyan bar, a light fill, an outline with a cut corner and three short tabs on its top edge. A scrollbar appears in the frame's right edge when the entries do not fit. | on key press |
| Key hints | The keys that work in the menu, as a picture. | no |
| Command line, editor | `c` and `e` open GRUB's terminal in a framed box below the instrument. | - |

Two rules follow from "the countdown elements disappear at a key press":

* Nothing that stays may talk about the countdown. The wordmark, the hints and
  the instrument carry no countdown words; the track is just an empty ring
  once the dots are gone.
* What looks like data is data. The dots, the number and the line all show
  GRUB's real remaining seconds. There are no invented status lines.

Rows by screen height: the list is `50% - 30` px high, of which 24 px are the
frame. 480 px gives 4 rows, 720 px 6, 800 px 7.

## Rules that must be kept

Each of these was seen to break the menu in a real GRUB. The test
`tests/build/test_grub_theme.py` checks them without starting GRUB.

* **Pictures are PNG, 8 bits per channel, RGB or RGBA, not interlaced.**
  A palette or grey PNG is drawn wrong (this broke the selection box of the
  previous theme). One picture GRUB cannot read makes it drop the whole
  theme, wait 10 seconds and show its plain text menu. No JPEG: the live
  medium only loads the PNG module.
* **A PNG must sit in the theme's top directory**, and so must the fonts:
  `update-grub` writes `insmod png` and the `loadfont` lines only for files
  it finds there.
* **No `%` in a label**, except the one `%02d` of the countdown number.
  A label's text is a printf template; a stray `%` drops the theme.
* **Pictures are made at their final size.** GRUB never shrinks a picture,
  and it stretches the edges and the centre of a box, so those hold flat
  colour only.
* **Rows are single centre slices of exactly the row's size** (592x40:
  616 minus the frame's 12 px on each side). With edge slices the selected
  row sits a few pixels off its slot. `scrollbar_slice = "east"` and
  `scrollbar_left_pad = 0` keep the row width the same with and without a
  scrollbar.
* **Font names in `theme.txt` are the exact names inside the `.pf2` files.**
  A name GRUB does not know silently becomes the font that was loaded last.
* **The terminal box must hold 80x24 characters of its font plus the border.**
  Otherwise GRUB replaces it with a centred box of exactly that size, which
  covers the instrument. The box uses `GNU Unifont Regular 16` (8x16 cells,
  so 644x388 with the 2 px border), starts under the instrument and is
  `50% + 32` px high: large enough from 712 px screen height up, and there it
  covers the entry frame and the key hints completely. A lower screen has no
  room for 24 lines under the instrument (at 600 px the box would have to end
  at 606). GRUB then does what it likes: at 800x600 it puts its centred box
  over the lower half of the instrument and leaves the key hints visible
  below it; at 640x480 it uses the whole screen. The command line and the
  editor work in both cases. The three modes the menu asks for (1280x800,
  1280x720, 1024x768) are all high enough.
* **Nothing is drawn at text height right of a row's icon.** GRUB cuts a long
  title at the row's right edge and has no padding there, and the recovery
  entry of every installed system is that long. Decoration of the selected
  row therefore sits on its edges (bar, outline, tabs on the top edge).
* **Entry icons are 32x20 pictures with the glyph in the right 20 px.** GRUB
  draws an icon at the row's left edge, where the selection bar is.
* **Original artwork only.** The icons for other systems are neutral line
  glyphs (a prompt, a square, a window outline), not vendor marks.

## Fonts and Secure Boot

| File | Name inside | Used for |
|---|---|---|
| `a-sans-16.pf2` | `Shadow Sans Regular 16` (Inter) | entries, "Ns remaining." |
| `b-monob-28.pf2` | `Shadow Mono Bold 28` (JetBrains Mono Bold) | the countdown number |
| `z-mono-14.pf2` | `Shadow Mono Regular 14` (JetBrains Mono) | nothing by name |

`z-mono-14.pf2` exists because of its place in the alphabet. The font loaded
last is GRUB's fallback: for a name it does not know, for boot messages and
for the plain text menu. `update-grub` loads a theme's fonts in alphabetical
order, so the last file should be a small monospace face. `z-` also sorts
after the font files of the previous theme (`mono-14`, `sans-16`, `sans-24`),
which stay in `/boot/grub/themes/shadow` after an upgrade.

The fonts hold Latin letters and common punctuation only (5 to 10 kB each).
Other characters, for example a Cyrillic entry title, are drawn with GRUB's
own Unifont.

Under UEFI Secure Boot GRUB refuses every font file from disk ("prohibited by
secure boot policy"). Only the Unifont inside Debian's signed GRUB image is
available, at 16 px. The theme still loads; all real text is then Unifont. The
layout is made for that: rows are 40 px high whatever the font, and the two
pieces of designed text (wordmark and key hints) are pictures. The countdown
number is small there (16 px instead of the 28 px bold face); a theme cannot
ask whether Secure Boot is on, so the number is centred over its trace and
looks placed at both sizes.

## Live medium

`system/boot/grub/live-grub.cfg.in`:

* loads the built-in font by name (`loadfont unicode`) where GRUB supports it.
  With the old path form the font was refused under Secure Boot, no graphical
  terminal started, and the medium showed the plain menu;
* loads the three theme fonts with explicit `loadfont` lines, the monospace
  one last, unless `$lockdown` is `y` (Secure Boot);
* exports `theme`. Without the export the "Advanced options" submenu was
  drawn as the plain menu.

The automated-test hook (SMBIOS serial `SHADOW-AUTOTEST`) and the entries are
unchanged.

## Installed systems

The theme is installed to `/usr/share/grub/themes/shadow` (package
`shadow-artwork`) and copied to `/boot/grub/themes/shadow` by
`shadow-grub-install`, because `/boot` is readable before any disk is
unlocked. `update-grub` then writes the lines that load it into `grub.cfg`:
one `loadfont` per `.pf2`, `insmod png`, `set theme`, `export theme`.

Those lines name the font files. When a new theme brings other font files,
copying it is not enough: `grub.cfg` must be written again. So
`debian/shadow-boot.postinst` runs `update-grub` after a successful
`shadow-grub-install`, only where `/boot/grub/grub.cfg` already exists, and
never fails the package run over it.

The copy stays a plain `cp -r`: old font files are not removed from `/boot`,
because a `grub.cfg` that has not been rewritten yet still names them.

A new theme reaches `/boot` when `shadow-grub-install` runs. That happens
when `shadow-boot` itself is upgraded, and through the dpkg triggers in
`debian/shadow-boot.triggers` when GRUB or shim change or when
`shadow-artwork` installs files under `/usr/share/grub/themes/shadow`; the
postinst's `triggered` branch runs the same refresh as an upgrade. This trigger has not
been tried on an installed system. By hand: `sudo shadow-grub-install &&
sudo update-grub`.

The theme is not shown when the installer configured a serial console
(`GRUB_TERMINAL="console serial"`); the plain menu is used then.

## Regenerating the theme

`theme.txt` is written by hand. Everything else in the directory is made by
`assets/src/boot/gen-grub-theme.sh`:

```sh
assets/src/boot/gen-grub-theme.sh OUTDIR                 # pictures and fonts
assets/src/boot/gen-grub-theme.sh --images-only OUTDIR   # needs python3, rsvg-convert
assets/src/boot/gen-grub-theme.sh --fonts-only OUTDIR    # needs grub-mkfont
```

`OUTDIR` receives a complete theme (with a copy of `theme.txt`); look at it,
then copy it over `assets/boot/grub/shadow/`. Both parts need the Inter and
JetBrains Mono fonts. The pictures are drawn as SVG and rasterised with
`rsvg-convert`; the background is computed by the script itself; every PNG is
written by the script's own small encoder, which guarantees the format GRUB
needs. No ImageMagick and no Pillow.

The build chroot has `rsvg-convert` but not `grub-mkfont`, so there:

```sh
scripts/dev.sh exec -- /src/assets/src/boot/gen-grub-theme.sh --images-only /tmp/grub-theme
```

and the fonts are built with `--fonts-only` on a Debian trixie system that
has the package `grub-common`. `assets/src/boot/gen-boot-theme.sh` is the
common entry point for the splash and the menu; it runs this script for the
menu, with `--images-only` when `grub-mkfont` is not installed.

On Debian trixie the result is byte for byte the committed theme (pictures
checked in the build chroot, pictures and fonts in a second trixie chroot).
Another version of librsvg or FreeType may give slightly different bytes for
the drawn pictures and the fonts; the background does not depend on either.

Two things to know about `grub-mkfont`:

* it builds the name as "`-n` value, Regular or Bold, size" and takes "Bold"
  from the font file. Its `-b` option must not be added for the countdown
  number: on top of the bold face it smears every glyph one pixel to the
  right;
* it prints many "unsupported font feature" warnings for Inter; the script
  hides them.

## What is checked, and what is not

`tests/build/test_grub_theme.py` (part of `tests/run-suites.sh`) reads the
theme the way GRUB does: files named by `theme.txt`, PNG formats, box slices,
row sizes, `%` in labels, font names against the `.pf2` files, the last font,
the layout at five screen sizes, the terminal box, the 300 kB size limit, the
live template and the package script. Where `rsvg-convert` or `grub-mkfont`
are installed it also runs the generator and compares the result.

Looked at in QEMU, in the real GRUB, on a medium laid out like the live ISO
but without a kernel (the preview script that builds this medium is not part
of the repository):

* live menu under BIOS at 1280x800, 1920x1080, 1024x768, 1280x720, 800x600,
  640x480;
  under UEFI; under Secure Boot enforcing;
* a stand-in for the installed system's `grub.cfg` (header lines as
  `update-grub` writes them, kernel titles, an entry found by os-prober)
  under BIOS and Secure Boot;
* the submenu, the command line and the editor in each of them;
* a menu with 13 entries, long titles and entries without a class, for the
  scrollbar and the clipping;
* the selected recovery entry of the installed stand-in (a title longer than
  the row), under BIOS and Secure Boot;
* `lsfonts`, to see that the three fonts are loaded under the names
  `theme.txt` uses;
* 24 screenshots 0.25 s apart, to see that the screen changes once a second
  and only in the countdown elements.

Not checked:

* a real computer: firmware mode lists, the time it takes to read the theme
  from a slow medium;
* a very high resolution. If the firmware offers none of 1280x800, 1280x720
  and 1024x768, GRUB takes its "auto" mode. The art has a fixed pixel size,
  so on a 4K mode the menu is small;
* arm64 media (another signed GRUB image);
* `update-grub` itself and the package script on an installed system: the
  `update-grub` step in `debian/shadow-boot.postinst` has not been run;
* `tests/boot` with the changed live template (it needs a built ISO).

Known limits:

* a title longer than the row is cut at the row's right edge, in the middle
  of a letter. GRUB has no ellipsis and no right padding;
* on a screen lower than 712 px the command line and the editor are not in
  the framed box below the instrument (see the terminal box rule above);
* the background is 67 kB, a little over the 60 kB it was planned at: the
  three arcs were made bright enough to be seen. The whole theme is 138 kB.
