# SHADOW themes (`shadow-themes`)

One SHADOW look outside SHADOW's own applications — third-party Qt/KDE and GTK
applications, window decorations, icons — plus the login screen (SDDM) and the
lock screen (KScreenLocker). Data only: nothing is compiled.

Everything visual is derived from `tokens/shadow-tokens.json` (read-only source
of truth) through `scripts/gen-tokens.py render`. `tools/generate.py` decides
which template becomes which file; generated files are committed and installed
as they are.

    desktop/themes/tools/generate.py            regenerate after changing a template or the tokens
    desktop/themes/tools/generate.py --check    fail if a committed generated file is stale
    desktop/themes/tools/generate.py --list     every output and its source

## Contents and install paths

| Source | Installed as | Generated from |
|---|---|---|
| `colorschemes/Shadow{Dark,Light}[Accent].colors` (12) | `/usr/share/color-schemes/` | `colorschemes/Shadow.colors.in` |
| `defaults/kdeglobals` | `/etc/xdg/kdeglobals` | `defaults/kdeglobals.in` + ShadowDark |
| `defaults/gtk-3.0/settings.ini`, `defaults/gtk-4.0/settings.ini` | `/etc/xdg/gtk-{3,4}.0/settings.ini` | `*.ini.in` |
| `defaults/90_shadow-desktop.gschema.override` | `/usr/share/glib-2.0/schemas/` | `*.override.in` |
| `tools/shadow-apply-appearance` | `/usr/bin/shadow-apply-appearance` | hand-written |
| `icons/shadow/`, `icons/shadow-light/` (`index.theme`, `16/places`, `64/places`, 37 icons each) | `/usr/share/icons/shadow{,-light}/` | `icons/src/folder*.svg.in` + glyphs of `desktop/ui/icons` |
| `assets/icons/apps/org.shadow.*.svg`, `shadow-os.svg` | `/usr/share/icons/hicolor/scalable/apps/` | (artwork component's sources) |
| `assets/icons/categories/*.svg` | `/usr/share/icons/hicolor/scalable/categories/` | (artwork component's sources) |
| `sddm/shadow/` | `/usr/share/sddm/themes/shadow/` | `tokens.js`, `icons/` from `tools/templates/tokens.js.in`, `desktop/ui/icons`; the shared decoration in `components/` (see below) from `tools/templates/components/` |
| `sddm/10-shadow.conf` | `/usr/lib/sddm/sddm.conf.d/10-shadow.conf` | hand-written |
| `lockscreen/org.shadow.horizon/` | `/usr/share/plasma/shells/org.shadow.horizon/` | `tokens.js`, `icons/`, `components/` as above |
| `lockscreen/plasmashellrc` | `/etc/xdg/plasmashellrc` | hand-written |
| `lockscreen/pam.d/kde`, `kde-fingerprint`, `kde-smartcard` | `/etc/pam.d/` | hand-written |

Not installed: `tokens/`, templates (`*.in`), `tools/generate.py`, `tests/`.
Wallpapers and logos belong to `assets/` (`shadow-artwork`).

## Qt / KDE applications and window decorations

`/etc/xdg/kdeglobals` is read by every Qt application through the `kde`
platform theme (`QT_QPA_PLATFORMTHEME=kde`, package plasma-integration) and by
KWin: colour scheme ShadowDark (colour groups inline, all sets including `[WM]`),
icon theme `shadow`, Inter Variable 10 / JetBrains Mono 10, widget style Breeze.
Token mapping: Window/Header = bgSurface, View = bgBase, Button/Tooltip =
bgOverlay, Selection = accent, Complementary = dark bgBase.

`shadow-apply-appearance [--mode dark|light] [--accent NAME]` switches
everything outside SHADOW's own applications. Without arguments it applies
`~/.config/shadow/appearance.conf` (`[Appearance] ColorScheme=`, `Accent=`,
`ReduceMotion=`). It replaces the colour groups in `~/.config/kdeglobals` with
those of the chosen scheme, sets the icon theme (`shadow`/`shadow-light`),
writes the scheme name and a few keys through `kwriteconfig6 --notify` (KWin
and the Breeze tool-bar area listen for that), updates
`~/.config/gtk-{3,4}.0/settings.ini` and GSettings, and emits
`org.kde.KGlobalSettings.notifyChange`.

"Reduce motion" is carried to the two places that cannot read
`appearance.conf`:

| Written | Value | Read by |
|---|---|---|
| `kdeglobals` `[KDE] AnimationDurationFactor` (with a change notification) | `0` when Reduce motion is on; when it is off, a `0` is removed (KDE's default, 1, applies) and any other value is left alone | KWin's effects, the Alt+Tab switcher, KDE applications |
| `kscreenlockerrc` `[Greeter][LnF][General] reduceMotion` | `true` / `false` | the lock screen (`config.reduceMotion`, declared in its `config.xml`), each time the screen is locked |

Without `kwriteconfig6` both are written into the files directly, in place
(repeated runs leave one entry). The animation factor is also a setting of
KDE's own, so the tool only ever owns the `0`: a speed the user chose
elsewhere (say `0.5`) survives every run while Reduce motion is off. Turning
Reduce motion on and off again leaves KDE's default, not that earlier speed.

The tool is run by the session script before the compositor starts
(`shadow-horizon-session`) and by the shell when dark/light, the accent or
"Reduce motion" changes (`Shell.qml`). SHADOW's own applications and the
shell follow the Reduce motion switch at once; the lock screen and KWin
follow it as soon as the shell has run the tool. That run on a switch was
added last and has not been seen in a real session.

## GTK 3 / GTK 4 / libadwaita

* `settings.ini`: dark preference, icon theme `shadow`, cursor theme
  `breeze_cursors` 24, Inter Variable 10. Used by X11/Xwayland clients and when
  no portal answers.
* On Wayland GTK reads `org.gnome.desktop.interface` from GSettings / the GTK
  portal back-end: the gschema override sets the same defaults there
  (`color-scheme='prefer-dark'`, icon, cursor, fonts).
* **libadwaita ignores `gtk-application-prefer-dark-theme`.** It follows
  `org.freedesktop.appearance` → `color-scheme` (1 = dark, 2 = light) of the
  settings portal (`org.freedesktop.portal.Settings`). In the SHADOW session
  (`XDG_CURRENT_DESKTOP=SHADOW:KDE`, portals `kde;gtk`) xdg-desktop-portal-kde
  answers: it derives the value from the brightness of the Window background of
  the palette in kdeglobals, and `accent-color` from the Selection colour, and
  emits `SettingChanged` when kdeglobals changes with a notification. So
  libadwaita follows the SHADOW colour scheme through kdeglobals;
  `shadow-apply-appearance` is what changes it. xdg-desktop-portal-gtk, when it
  answers instead, reports the GSettings `color-scheme` key, which the script
  sets as well.

## Icon themes

`shadow` (dark, default) inherits `breeze-dark,hicolor`; `shadow-light` inherits
`breeze,hicolor`. Places (folder) icons are original SHADOW artwork generated
from two templates and glyphs of the SHADOW symbolic set — no Breeze file is
copied or recoloured. Application and category icons go to hicolor so every
theme and toolkit finds them.

## Login screen (SDDM theme `shadow`)

SDDM 0.21, Qt 6 greeter (`QtVersion=6`), plain Qt Quick + `SddmComponents`
(only `TextConstants`, for SDDM's translated strings). Wallpaper from
`theme.conf` (`background=`; solid `bgBase` when missing), clock/date, user
with picture or initials, user list for any number of users plus "Other
user…" (typed login name; also the only mode when no user is listed),
password field with show/hide, Caps Lock warning, session selector, keyboard
layout indicator/selector, suspend/restart/shut down only when `sddm.can*`,
confirmation before restart/shut down, failed-login message. Only the primary
screen shows the form. Keyboard: Return logs in; Tab/Shift+Tab reach every
control; lists take arrows, Home/End, PageUp/Down, Return, Escape.

Entrance, decoration and the `reduceMotion` key: see "Entrance and decoration
of both screens" below.

`10-shadow.conf` selects the theme and `DisplayServer=wayland` with
`kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1` as greeter
compositor: Shadow OS has no Xorg server, so SDDM's default `x11` (and
`x11-user`) cannot start; the greeter then uses the same compositor and input
stack as the session.

## Lock screen (shell package `org.shadow.horizon`)

KWin locks through KScreenLocker; `kscreenlocker_greet` loads
`contents/lockscreen/LockScreen.qml` of the "Plasma/Shell" package named by
`plasmashellrc` `[Shell] ShellPackage` (`/etc/xdg/plasmashellrc` here).
`metadata.json` has `X-Plasma-APIVersion` 2 — below that the greeter falls
back to a built-in screen whose Plasma modules SHADOW does not ship.
QtQuick only. API used (kscreenlocker 6.3.5 source, `greeter/pamauthenticators.h`):
`authenticator.startAuthenticating()`, `respond()`, `state`, `busy`,
`prompt`, `promptForSecret`, `infoMessage`, `errorMessage`, signals
`succeeded`, `failed(kind, …)`; `kscreenlocker_userName`; `config.wallpaper`
(`contents/lockscreen/config.xml`, stored in kscreenlockerrc
`[Greeter][LnF][General] wallpaper=`); `Qt.quit()` after `succeeded`.
`state.js` (one instance per QML engine) makes sure a PAM prompt is answered
once although every screen has its own copy of the form.

PAM: `/etc/pam.d/kde` is the service compiled into the greeter
(`@include common-auth`, plus account/password/session-noninteractive for
completeness; only `auth` is used). `kde-fingerprint` and `kde-smartcard` are
two further, non-interactive conversations the greeter always starts; they
must never ask for a password, so they contain only `pam_fprintd` /
`pam_pkcs11` (skipped when not installed) and `pam_deny`. Without these two
files PAM would fall back to `/etc/pam.d/other`, i.e. a password prompt nobody
can answer.

## Entrance and decoration of both screens

Both screens build up behind a scan line instead of appearing in one frame,
and carry the same decoration: the hexagon of the SHADOW symbol as an outline
around the user picture, a tick ring around it, brackets around the password
row and, on the login screen, a hairline with a node to each side.

**What protects the user**

* The password field exists, is enabled and has the focus from the first
  frame, exactly as before. The entrance changes opacity (the field starts at
  55 %) and a few pixels of offset, nothing else: no item is hidden,
  disabled, loaded later or re-parented. Keys typed while the entrance runs
  arrive.
* The focus ring means exactly "keys arrive here": it is drawn only while the
  field really has the keyboard, which needs the window to have it. The
  window usually gets the keyboard one or two frames after its first one, so
  the very first frame shows the field without the ring; it is never drawn in
  advance (with two monitors the window may not get the keyboard at all).
* Everything decorative is one file behind a `Loader`:
  `components/LoginDecor.qml` and `components/LockDecor.qml`. `Main.qml` and
  `LockScreen.qml` read a single value from it, `ent` (progress 0..1), bound
  as `decor.status === Loader.Ready ? decor.item.t : 1`. When the file does
  not load, `ent` is 1: a complete, working form without decoration. This
  matters most on the lock screen, where a `LockScreen.qml` that fails to
  load locks the user out; `LockScreen.qml` itself still imports QtQuick,
  `tokens.js` and `state.js` only. The brackets of the login screen's
  shut-down question sit behind a `Loader` of their own; no decoration file
  is a static dependency of `Main.qml`.
* Plain items only: Rectangle, Text, Image (SVG), opacity, Translate. No
  shader, no effect, no particle, no rotated item (positions on the ring are
  computed with sin/cos), no new package. The software renderer draws the
  same picture.
* Nothing loops when the screen is idle and nothing moves per key press. The
  lock screen has no exit animation: the right password ends the greeter at
  once.
* The decoration states nothing that is not a fact. The two nodes on the
  tick ring are the real time (cyan: minute, violet: hour, moved by the
  clock timer that existed before). The comet on the ring exists while
  `root.busy` (login) or `authenticator.busy` (lock) is true, and the texts
  "…" and "Checking…" stay. A refused password turns ring and brackets red
  once (they return to their colours over 0.9 s) next to the message, the red
  border and the shake that were there before. Brackets are slate, the accent
  while the field has the keyboard, amber while Caps Lock is on.

**Timeline** (milliseconds after the entrance starts; every item takes its
opacity from one window of the single progress value)

| Login screen, 700 ms | Lock screen, 420 ms | What |
|---|---|---|
| 0–420 | 0–260 | scan line from top to bottom; the wallpaper appears above it |
| 0–200 | 0–100 | password field and button from 55 % to 100 % |
| 160–420 (picture 160–320) | 90–260 (picture 90–210) | hexagon, drawn from its top corner downwards, and the user picture: from the moment the scan line reaches the top of the ring |
| 100–280 | 60–200 | brackets arrive from 20u outside the password row |
| 220–420 | 130–320 | name, "Switch user" / the "Locked" line, behind the scan line |
| 180–460 | 0–240 | clock (on the login screen it settles 12u downwards), date |
| 200–420 | 120–320 | tick ring and time nodes |
| 240–520 | – | the two wings grow away from the ring |
| 420–700 | – | buttons of the bottom bar, 40 ms apart |

A second monitor of the login screen gets the scan line and the clock only.
The entrance starts two frames after the first one was drawn
(`components/Entrance.qml`), so a slow first frame cannot swallow its
beginning; the lock screen runs it again each time the greeter shows the
view. A wallpaper that is decoded only after the entrance has started fades
in over 0.3 s when it arrives. The first frame is a gradient `#04060B` to
`#020306`, the colours of the boot splash and the boot menu.

While the user list or the shut-down question covers the card, ring, wings
and the brackets of the password row step back (opacity only, 100 ms) and
return when it closes, so no line runs out from under the list and the only
brackets on screen are those around the question.

Lists of the login screen (users, sessions, keyboard layouts) unfold from the
button that opened them (160 ms) and close with a 100 ms fade; rows exist and
take keys from the first frame, the highlight still jumps, and the focus is
back at the opener the moment the list closes. The restart / shut-down
question dims the screen in 160 ms with brackets closing on the card; Cancel
has the focus from the first frame.

**Departures from the design brief**, each for a reason found while testing:

* The focus ring is not there "from frame one": it appears when the window
  really gets the keyboard (see above). A ring drawn in advance would claim
  that keys arrive where they may not (two monitors).
* The user picture, the hexagon, the name and "Switch user" (lock screen:
  the "Locked" line) start when the scan line reaches them, not before it;
  the field and its brackets keep their early windows.
* The head of the comet is pale cyan `#A5F3FC`, not the hot white that
  belongs to the scan line only; the wing lines are slate at 40 % instead of
  the unlit trace colour, which could hardly be seen at 1024×768.
* Ring, wings and field brackets step back under the user list and the
  shut-down question (see above).
* The lock screen has 6u more room between the "Locked" line and the
  password row, so the brackets keep clear of the text.

**Reduce motion**

| Screen | Setting | Who sets it |
|---|---|---|
| Login | `reduceMotion=true` in `/usr/share/sddm/themes/shadow/theme.conf.user` (default `false` in `theme.conf`) | the administrator. **One setting for every user of the computer**: the greeter runs before anyone is logged in and cannot know a user's choice |
| Lock | `reduceMotion` in `kscreenlockerrc` `[Greeter][LnF][General]` (entry in `config.xml`, default `false`) | `shadow-apply-appearance`, from the user's "Reduce motion" switch (see above for when) |

With it on, the screen appears in one frame, there is no scan line, no comet
and no shake, and lists and the question open in one frame. Colours and
texts still change.

**Files**

The decoration exists once, in `tools/templates/components/`, and
`tools/generate.py` copies it into both themes; `generate.py --check` fails
when a copy differs from its source. Edit the sources, never the copies.

| File | What |
|---|---|
| `Entrance.qml` | the clock of an entrance: `t` 0..1, started after two frame ticks (a 600 ms timer starts it should no tick come), `reduce` = 1 at once. The only file with a versionless `import QtQuick`: `FrameAnimation` does not exist under `import QtQuick 2.15` |
| `Fx.js` | `lin` / `ph` (a window of a progress value, linear / slowing down), `mix`, `alpha` |
| `HexRing.qml` | hexagon (revealed through a clip), tick ring, time nodes, comet |
| `Brackets.qml` | four square-ended corners around a rectangle |
| `ScanSweep.qml` | the scan line: bright core, fading tail above it |
| `Wing.qml` | hairline with a node at its end |
| `LoginDecor.qml`, `LockDecor.qml` | compose the above for one screen and own its `Entrance` |
| `ring-hex.svg`, `ring-hex-alert.svg` | the hexagon in the gradient of the symbol and in the danger colour, from `ring-hex.svg.in` |
| `ring-ticks.svg`, `ring-ticks-lit.svg` | the tick ring at rest and lit, from `ring-ticks.svg.in` (the 72 lines are computed by `generate.py`) |

`tokens.js` gained the durations of both entrances and the colours of the
decoration that are not design tokens (`motionEntrance` … `pale`), as
literals in `tools/templates/tokens.js.in`.

## Run-time Debian packages

Required: `plasma-integration`, `kde-style-breeze`, `kwin-style-breeze`,
`breeze-icon-theme`, `breeze-cursor-theme`, `fonts-inter-variable`,
`fonts-jetbrains-mono`, `hicolor-icon-theme`, `sddm`,
`qml6-module-qtquick`, `libkscreenlocker6`, `plasma-desktoptheme` (the
"Plasma/Shell" package structure), **`layer-shell-qt`** (see limitations),
`kwin-wayland`, `libpam-modules`, `libkf6config-bin` (kwriteconfig6),
`dbus-bin` (dbus-send), `shadow-artwork` (wallpaper).
Recommended: `xdg-desktop-portal-kde`, `xdg-desktop-portal-gtk`,
`gsettings-desktop-schemas`, `libglib2.0-bin` (gsettings),
`adwaita-icon-theme`. Optional: `libpam-fprintd`, `libpam-pkcs11`.

## Tests

All in the build chroot (`scripts/dev.sh`), 11 Nov 2026:

* `tools/generate.py --check` — 187 files up to date.
* Install: `scripts/dev.sh exec --dir themes -- sh -c 'cmake -S /src -B /build/themes-data -DSHADOW_DATA_ONLY=ON -DCMAKE_INSTALL_PREFIX=/usr && DESTDIR=/tmp/themes-root cmake --install /build/themes-data --component shadow-themes && find /tmp/themes-root -type f | sort'` — 236 files at the paths above.
* Qt: `tests/qt-test.sh` in the nested KWin Wayland session with
  `QT_QPA_PLATFORMTHEME=kde` and the staged install: `tests/widgets-demo`
  reports style breeze, icons shadow, Inter Variable 10, JetBrains Mono 10 and
  the token palette; screenshots `themes-qt-dark-{controls,menu,views}.png`;
  live switch with `shadow-apply-appearance --mode light --accent violet`
  (`themes-qt-light-violet-{live,controls}.png`: title bar, menus, content
  follow while the application runs).
* SDDM: `tests/sddm-test.sh` — real `sddm-greeter-qt6 --test-mode` under Xvfb
  at 1024×768, 1600×900, 3840×2160; 0, 1, 3, 14, 30 users; 0–3 sessions; with
  and without wallpaper; keyboard-only walk through every control
  (`themes-sddm-*.png`). Also once as a Wayland client of the nested KWin
  (`themes-sddm-wayland.png`).
* Lock screen: `tests/lockscreen-test.sh` — real `kscreenlocker_greet
  --testing` under Xvfb with this directory's PAM files and real `pam_unix`:
  wrong password → error and new prompt; right password → greeter prints
  "Unlocked" and exits 0; via `--shell` and via `plasmashellrc`; 1024×768,
  1600×900, 3840×2160; Caps Lock, show/hide, unlock button by keyboard
  (`themes-lock-*.png`, `.log`, `.out`).
* `tests/desktop/smoke.sh --dir themes themes`: 0 passed, 0 failed — there is
  no application in this component.

Entrance and decoration, 26 Nov 2026, in the lab chroot (Debian trixie, Qt
6.8.2, sddm 0.21, kscreenlocker 6.3.5; private Xvfb whose frame buffer is
sampled about 100 times a second from before the program starts):

* `tools/generate.py --check` — 211 files up to date; `tokens.js` and the
  shared files of `components/` identical in both themes.
* Login and lock screen with OpenGL on llvmpipe and with
  `QT_QUICK_BACKEND=software`: no QML warning; the first drawn frame already
  shows the password field (at 55 %); its focus ring follows when the window
  gets the keyboard, 50–60 ms later in these runs.
* No element gets dimmer once it has appeared (mean brightness of clock,
  date, ring, wings, name, field and bottom bar over every captured frame,
  leaving out the frames in which the scan line crosses the region), with
  `QSG_FIXED_ANIMATION_STEP` so that each frame is one 16 ms step. At
  640×400 the capture keeps nearly every step (25 of the 27 steps of the
  login screen's scan line); at 1600×900 it skips about every second one.
* Time to the first drawn frame, before and after, runs alternating (median
  of 8 with OpenGL, of 3 with the software renderer; the lab was busy and
  single runs spread by several hundred ms): login 447 → 503 ms and 155 →
  161 ms, lock 673 → 717 ms and 230 → 277 ms.
* Login: five keys sent 0.75 s after the greeter was started all arrive, and
  so do five keys typed right after choosing a user from the list, while the
  list still fades;
  wrong password with the fake daemon: "…" and the comet, then red border,
  "Login failed", shake; lists and the shut-down question with three users
  and two sessions; the greeter as a layer-shell client of a nested KWin,
  with one and with two outputs.
* 27 Nov 2026, after a review: the decoration of the shut-down question with
  `components/Brackets.qml` removed (the question works, the log names the
  missing file and nothing else); ring, wings and field brackets gone while
  the user list and the question are open, back afterwards.
* Lock: wrong password, then the right one — the greeter prints "Unlocked"
  and exits.
* With `components/LockDecor.qml` (and, separately, `LoginDecor.qml`)
  removed: the screen loads, the form works, the log has the Loader's "No
  such file or directory" and nothing else.
* With `reduceMotion=true` in `theme.conf`, and with `reduceMotion=true` in
  the lock test's `kscreenlockerrc`: one-frame appearance, no shake.
* `shadow-apply-appearance` with a private `XDG_CONFIG_HOME`: writes both
  keys for `ReduceMotion=true` and writes them back for `false`, with
  `kwriteconfig6` and without it; a factor of `0.5` already in `kdeglobals`
  survives runs with `false`, the tool's own `0` is removed again, and
  repeated runs leave a single entry.

## Limitations and what was not tested

* **`layer-shell-qt` is not installed in the build chroot and is not a
  dependency of `libkscreenlocker6`.** Without it `kscreenlocker_greet` cannot
  start on Wayland ("No shell integration named layer-shell"), so the lock
  screen was tested as an X11 client only, and the SDDM greeter with
  `QT_WAYLAND_SHELL_INTEGRATION=layer-shell` not at all. The package must be
  installed on the system.
* SDDM test mode has no daemon: a login is never answered and `sddm.can*` are
  false. The failed-login message and the power buttons were tested with
  `--fake-daemon`, which patches a temporary copy of the theme (emits the
  daemon's `loginFailed` signal, reads `can*` as true). A real login, real
  power actions, `10-shadow.conf` (SDDM starting KWin as greeter compositor)
  and multi-screen behaviour need a booted system.
* On the Wayland greeter SDDM reports no keyboard layouts, so the layout
  indicator is hidden there; it shows under X11. Caps Lock is detected from
  `keyboard.capsLock` where SDDM provides it and otherwise from the case of
  typed letters, i.e. only after the first letter. The lock screen has only
  the latter.
* Lock screen from a real lock (KWin → KScreenLocker → greeter with
  `--ksldfd`), grace time, multiple screens, fingerprint and smart card: not
  tested. The greeter logs "Error loading the wallpaper, no valid package
  loaded" (KScreenLocker's own wallpaper plugin, not installed) — harmless.
* GTK: no GTK, portal or GSettings schema in the chroot — `settings.ini`, the
  gschema override, the GSettings part of `shadow-apply-appearance` and the
  libadwaita behaviour are untested.
* The login screen's `reduceMotion` is system-wide (see "Reduce motion"). The
  lock screen and KWin follow a user's switch when the shell runs
  `shadow-apply-appearance`; that was not tried in a real session.
* With two outputs under the nested KWin the password field of the primary
  screen has no keyboard focus until that screen is clicked, and typed keys
  are lost until then. This was so before the entrance existed and is
  unchanged. The screen does not pretend otherwise: no focus ring at any
  time, also not during the entrance, and slate brackets; ring and accent
  brackets appear with the click. Whether real monitors behave the same is
  not known.
* Nothing here changes what the screen shows between the boot splash and the
  greeter: Plymouth quits without keeping its picture and SDDM is not ordered
  after it. Both sides only meet in the same near-black.
* What an accepted login looks like could not be shown: in test mode the
  greeter is gone in the frame in which `loginSucceeded` arrives, and how
  long the real greeter stays is not known. The form is therefore not faded
  out; the tick ring lights up and the brackets close for as long as there
  is.
* Not tested for the entrance: the greeter on DRM under SDDM's own KWin, a
  real lock from KWin (the test greeter is an X11 client), a real GPU. Times
  measured on llvmpipe say nothing about real hardware.
* User pictures are shown on the login screen (`themes-sddm-avatar-*.png`);
  the lock screen shows initials only.
* Strings not covered by SDDM's `TextConstants` ("Switch user", "Other
  user…", the confirmation, all lock-screen texts) are English only.
* `/etc/pam.d/kde` may also be shipped by Debian's Plasma packages; if those
  are ever installed next to shadow-themes the file needs a diversion.
