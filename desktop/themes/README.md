# NEXUS themes (`nexus-themes`)

One NEXUS look outside NEXUS's own applications — third-party Qt/KDE and GTK
applications, window decorations, icons — plus the login screen (SDDM) and the
lock screen (KScreenLocker). Data only: nothing is compiled.

Everything visual is derived from `tokens/nexus-tokens.json` (read-only source
of truth) through `scripts/gen-tokens.py render`. `tools/generate.py` decides
which template becomes which file; generated files are committed and installed
as they are.

    desktop/themes/tools/generate.py            regenerate after changing a template or the tokens
    desktop/themes/tools/generate.py --check    fail if a committed generated file is stale
    desktop/themes/tools/generate.py --list     every output and its source

## Contents and install paths

| Source | Installed as | Generated from |
|---|---|---|
| `colorschemes/Nexus{Dark,Light}[Accent].colors` (12) | `/usr/share/color-schemes/` | `colorschemes/Nexus.colors.in` |
| `defaults/kdeglobals` | `/etc/xdg/kdeglobals` | `defaults/kdeglobals.in` + NexusDark |
| `defaults/gtk-3.0/settings.ini`, `defaults/gtk-4.0/settings.ini` | `/etc/xdg/gtk-{3,4}.0/settings.ini` | `*.ini.in` |
| `defaults/90_nexus-desktop.gschema.override` | `/usr/share/glib-2.0/schemas/` | `*.override.in` |
| `tools/nexus-apply-appearance` | `/usr/bin/nexus-apply-appearance` | hand-written |
| `icons/nexus/`, `icons/nexus-light/` (`index.theme`, `16/places`, `64/places`, 37 icons each) | `/usr/share/icons/nexus{,-light}/` | `icons/src/folder*.svg.in` + glyphs of `desktop/ui/icons` |
| `assets/icons/apps/org.nexus.*.svg`, `nexus-os.svg` | `/usr/share/icons/hicolor/scalable/apps/` | (artwork component's sources) |
| `assets/icons/categories/*.svg` | `/usr/share/icons/hicolor/scalable/categories/` | (artwork component's sources) |
| `sddm/nexus/` | `/usr/share/sddm/themes/nexus/` | `tokens.js`, `icons/` from `tools/templates/tokens.js.in`, `desktop/ui/icons` |
| `sddm/10-nexus.conf` | `/usr/lib/sddm/sddm.conf.d/10-nexus.conf` | hand-written |
| `lockscreen/org.nexus.horizon/` | `/usr/share/plasma/shells/org.nexus.horizon/` | `tokens.js`, `icons/` as above |
| `lockscreen/plasmashellrc` | `/etc/xdg/plasmashellrc` | hand-written |
| `lockscreen/pam.d/kde`, `kde-fingerprint`, `kde-smartcard` | `/etc/pam.d/` | hand-written |

Not installed: `tokens/`, templates (`*.in`), `tools/generate.py`, `tests/`.
Wallpapers and logos belong to `assets/` (`nexus-artwork`).

## Qt / KDE applications and window decorations

`/etc/xdg/kdeglobals` is read by every Qt application through the `kde`
platform theme (`QT_QPA_PLATFORMTHEME=kde`, package plasma-integration) and by
KWin: colour scheme NexusDark (colour groups inline, all sets including `[WM]`),
icon theme `nexus`, Inter Variable 10 / JetBrains Mono 10, widget style Breeze.
Token mapping: Window/Header = bgSurface, View = bgBase, Button/Tooltip =
bgOverlay, Selection = accent, Complementary = dark bgBase.

`nexus-apply-appearance [--mode dark|light] [--accent NAME]` switches
everything outside NEXUS's own applications. Without arguments it applies
`~/.config/nexus/appearance.conf` (`[Appearance] ColorScheme=`, `Accent=`). It
replaces the colour groups in `~/.config/kdeglobals` with those of the chosen
scheme, sets the icon theme (`nexus`/`nexus-light`), writes the scheme name
and a few keys through `kwriteconfig6 --notify` (KWin and the Breeze tool-bar
area listen for that), updates `~/.config/gtk-{3,4}.0/settings.ini` and
GSettings, and emits `org.kde.KGlobalSettings.notifyChange`. **Nothing calls it
yet** — see "Needed outside this directory".

## GTK 3 / GTK 4 / libadwaita

* `settings.ini`: dark preference, icon theme `nexus`, cursor theme
  `breeze_cursors` 24, Inter Variable 10. Used by X11/Xwayland clients and when
  no portal answers.
* On Wayland GTK reads `org.gnome.desktop.interface` from GSettings / the GTK
  portal back-end: the gschema override sets the same defaults there
  (`color-scheme='prefer-dark'`, icon, cursor, fonts).
* **libadwaita ignores `gtk-application-prefer-dark-theme`.** It follows
  `org.freedesktop.appearance` → `color-scheme` (1 = dark, 2 = light) of the
  settings portal (`org.freedesktop.portal.Settings`). In the NEXUS session
  (`XDG_CURRENT_DESKTOP=NEXUS:KDE`, portals `kde;gtk`) xdg-desktop-portal-kde
  answers: it derives the value from the brightness of the Window background of
  the palette in kdeglobals, and `accent-color` from the Selection colour, and
  emits `SettingChanged` when kdeglobals changes with a notification. So
  libadwaita follows the NEXUS colour scheme through kdeglobals;
  `nexus-apply-appearance` is what changes it. xdg-desktop-portal-gtk, when it
  answers instead, reports the GSettings `color-scheme` key, which the script
  sets as well.

## Icon themes

`nexus` (dark, default) inherits `breeze-dark,hicolor`; `nexus-light` inherits
`breeze,hicolor`. Places (folder) icons are original NEXUS artwork generated
from two templates and glyphs of the NEXUS symbolic set — no Breeze file is
copied or recoloured. Application and category icons go to hicolor so every
theme and toolkit finds them.

## Login screen (SDDM theme `nexus`)

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

`10-nexus.conf` selects the theme and `DisplayServer=wayland` with
`kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1` as greeter
compositor: NEXUS OS has no Xorg server, so SDDM's default `x11` (and
`x11-user`) cannot start; the greeter then uses the same compositor and input
stack as the session.

## Lock screen (shell package `org.nexus.horizon`)

KWin locks through KScreenLocker; `kscreenlocker_greet` loads
`contents/lockscreen/LockScreen.qml` of the "Plasma/Shell" package named by
`plasmashellrc` `[Shell] ShellPackage` (`/etc/xdg/plasmashellrc` here).
`metadata.json` has `X-Plasma-APIVersion` 2 — below that the greeter falls
back to a built-in screen whose Plasma modules NEXUS does not ship.
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

## Run-time Debian packages

Required: `plasma-integration`, `kde-style-breeze`, `kwin-style-breeze`,
`breeze-icon-theme`, `breeze-cursor-theme`, `fonts-inter-variable`,
`fonts-jetbrains-mono`, `hicolor-icon-theme`, `sddm`,
`qml6-module-qtquick`, `libkscreenlocker6`, `plasma-desktoptheme` (the
"Plasma/Shell" package structure), **`layer-shell-qt`** (see limitations),
`kwin-wayland`, `libpam-modules`, `libkf6config-bin` (kwriteconfig6),
`dbus-bin` (dbus-send), `nexus-artwork` (wallpaper).
Recommended: `xdg-desktop-portal-kde`, `xdg-desktop-portal-gtk`,
`gsettings-desktop-schemas`, `libglib2.0-bin` (gsettings),
`adwaita-icon-theme`. Optional: `libpam-fprintd`, `libpam-pkcs11`.

## Tests

All in the build chroot (`scripts/dev.sh`), 11 Nov 2026:

* `tools/generate.py --check` — 187 files up to date.
* Install: `scripts/dev.sh exec --dir themes -- sh -c 'cmake -S /src -B /build/themes-data -DNEXUS_DATA_ONLY=ON -DCMAKE_INSTALL_PREFIX=/usr && DESTDIR=/tmp/themes-root cmake --install /build/themes-data --component nexus-themes && find /tmp/themes-root -type f | sort'` — 236 files at the paths above.
* Qt: `tests/qt-test.sh` in the nested KWin Wayland session with
  `QT_QPA_PLATFORMTHEME=kde` and the staged install: `tests/widgets-demo`
  reports style breeze, icons nexus, Inter Variable 10, JetBrains Mono 10 and
  the token palette; screenshots `themes-qt-dark-{controls,menu,views}.png`;
  live switch with `nexus-apply-appearance --mode light --accent violet`
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
  power actions, `10-nexus.conf` (SDDM starting KWin as greeter compositor)
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
  gschema override, the GSettings part of `nexus-apply-appearance` and the
  libadwaita behaviour are untested.
* User pictures are shown on the login screen (`themes-sddm-avatar-*.png`);
  the lock screen shows initials only.
* Strings not covered by SDDM's `TextConstants` ("Switch user", "Other
  user…", the confirmation, all lock-screen texts) are English only.
* `/etc/pam.d/kde` may also be shipped by Debian's Plasma packages; if those
  are ever installed next to nexus-themes the file needs a diversion.
