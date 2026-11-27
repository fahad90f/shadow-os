# Shadow Horizon architecture

How the desktop is built: the session, the shell process, the protocols and
services it uses, rendering, theming of third-party applications, and the
login and lock screens. For using the desktop see [horizon.md](horizon.md);
for writing shell modules see [shell-modules.md](shell-modules.md); for the
UI library see [shadowui.md](shadowui.md).

Sources: `desktop/horizon-shell/` (shell and session), `desktop/ui/`
(ShadowUI), `desktop/themes/` (themes, login and lock screen).

## Components

| Component | Package | Role |
|---|---|---|
| KWin (`kwin_wayland`) + Xwayland | Debian `kwin-wayland`, `xwayland` | Wayland compositor and window manager; runs X11 applications |
| `shadow-horizon-session` | `shadow-horizon` | session start script (`/usr/bin`) |
| `horizon-session-leader` | `shadow-horizon` | session supervisor (`/usr/libexec/shadow`) |
| `horizon-shell` | `shadow-horizon` | the shell: one process |
| ShadowUI (`libshadowui`, QML module `org.shadow.ui`, style `org.shadow.style`) | `shadow-ui` | shared UI library of the shell and all SHADOW applications |
| Themes, SDDM theme, lock screen, `shadow-apply-appearance` | `shadow-themes` | look of everything that is not a SHADOW application |
| SDDM | Debian `sddm` | display manager (login screen) |
| KScreenLocker (`kscreenlocker_greet`) | Debian `libkscreenlocker6` | lock screen process, started by KWin |
| xdg-desktop-portal with the KDE and GTK back-ends | Debian | portals (file chooser, screenshots, screen sharing, settings) |

Shadow Horizon does not run `plasmashell` or a Plasma session. The
`plasma-workspace` package is installed only because the KDE portal's dialogs
(the screen-sharing picker, screenshot and access prompts) import its QML
modules; its Plasma login sessions are hidden (see below), and the activity
manager that KWin starts from it keeps no history
(`/etc/xdg/kactivitymanagerd-pluginsrc`).

## Session files

Installed by `desktop/horizon-shell/CMakeLists.txt`:

| File | Purpose |
|---|---|
| `/usr/share/wayland-sessions/shadow-horizon.desktop` | the "Shadow Horizon" entry of the login screen; `Exec=/usr/bin/shadow-horizon-session`, `DesktopNames=SHADOW;KDE` |
| `/usr/bin/shadow-horizon-session` | sets the environment, chooses the rendering mode, starts KWin |
| `/usr/libexec/shadow/horizon-session-leader` | started by KWin; publishes the environment, starts the session target, supervises the shell |
| `/usr/lib/systemd/user/shadow-horizon-session.target` | binds `graphical-session.target`; wants `graphical-session-pre.target` and `xdg-desktop-autostart.target` |
| `/usr/lib/systemd/user/plasma-xdg-desktop-portal-kde.service.d/50-shadow.conf` | gives only the KDE portal back-end `XDG_CURRENT_DESKTOP=KDE`: it offers ScreenCast, Screenshot, RemoteDesktop, GlobalShortcuts, Background and InputCapture only when the value is exactly `KDE` |
| `/usr/share/applications/org.shadow.horizon.shell.desktop` | hidden entry of the shell; its `X-KDE-Wayland-Interfaces` line is what makes KWin grant the shell the restricted protocols listed below |
| `/usr/share/xdg-desktop-portal/shadow-portals.conf` | portal back-ends for `XDG_CURRENT_DESKTOP=SHADOW`: `kde`, then `gtk`; GlobalShortcuts, RemoteDesktop and InputCapture switched off (`none`) |
| `/etc/xdg/menus/shadow-applications.menu` | the application menu (flat: every installed application); selected with `XDG_MENU_PREFIX=shadow-` |
| `/etc/xdg/menus/applications.menu` | the same menu for programs started without `XDG_MENU_PREFIX`, so a rebuild of the application database by them keeps every application (KWin finds the restricted-protocol permissions of the shell and the portal there) |
| `/usr/share/shadow/sddm/{wayland-sessions,xsessions}/plasma*.desktop` | hidden entries that keep the Plasma sessions of `plasma-workspace` off the login screen (SDDM reads this directory first, `SessionDir` in `10-shadow.conf`) |
| `/etc/xdg/kwinrc` | compositor defaults |
| `/etc/xdg/kscreenlockerrc` | lock-screen defaults |

### Environment set by `shadow-horizon-session`

| Variable | Value | Why |
|---|---|---|
| `XDG_CURRENT_DESKTOP` | `SHADOW:KDE` | SHADOW first (the portal front-end finds `shadow-portals.conf` through it); `KDE` makes KDE Frameworks applications and xdg-utils choose their KDE behaviour. xdg-desktop-portal-kde needs exactly `KDE` and gets it from its drop-in (above) |
| `XDG_SESSION_DESKTOP`, `XDG_SESSION_TYPE` | `shadow`, `wayland` | |
| `XDG_MENU_PREFIX` | `shadow-` | selects `shadow-applications.menu` |
| `KDE_FULL_SESSION`, `KDE_SESSION_VERSION` | `true`, `6` | KDE Frameworks integration |
| `QT_QPA_PLATFORM` | `wayland;xcb` | native Wayland, X11 as fallback |
| `QT_QPA_PLATFORMTHEME` | `kde` | third-party Qt applications read `kdeglobals` through `plasma-integration` |
| `GDK_BACKEND`, `SDL_VIDEODRIVER`, `CLUTTER_BACKEND`, `MOZ_ENABLE_WAYLAND`, `ELECTRON_OZONE_PLATFORM_HINT` | Wayland first | toolkit selection |
| `_JAVA_AWT_WM_NONREPARENTING` | `1` | Java Swing/AWT tools lay out correctly through Xwayland |
| `XCURSOR_THEME`, `XCURSOR_SIZE` | `breeze_cursors`, `24` (unless already set) | cursor |
| `PLASMA_DEFAULT_SHELL` | `org.shadow.horizon` | where KScreenLocker finds the lock-screen QML |
| `KWIN_COMPOSE`, `QT_QUICK_BACKEND` | `Q`, `software` — only with software rendering | see below |

It also creates `$XDG_RUNTIME_DIR/shadow` for per-session state and runs
`shadow-apply-appearance` before starting the compositor:

```
exec kwin_wayland_wrapper --xwayland --exit-with-session=/usr/libexec/shadow/horizon-session-leader
```

### The session leader

1. Guards against a second copy: its PID is kept in
   `$XDG_RUNTIME_DIR/shadow/session-leader.pid`. KWin starts the leader again
   when it restarts after a crash; that copy waits for the first one instead
   of starting a second shell.
2. Publishes the display and desktop variables with
   `dbus-update-activation-environment` (with `--systemd` when the systemd
   user manager runs), so D-Bus-activated services, portals and autostart
   entries see the display. Then it stops the portal services and starts
   them again: KWin's own start-up already starts the portal front-end, and a
   portal keeps the environment it started with (without it the front-end
   finds no `shadow-portals.conf` and offers no screen sharing). They are
   started explicitly because a D-Bus start that the stop interrupted would
   keep every portal call waiting for up to two minutes.
3. Runs `kbuildsycoca6`, so the application database exists before the
   launcher opens.
4. Starts `shadow-horizon-session.target` (XDG autostart applications).
5. Runs `/usr/bin/horizon-shell` in a loop: exit status 0 means "log out";
   any other status is a crash and the shell is restarted — at most five
   crashes within a minute, then the session ends.
6. On exit stops the target and unsets the published variables. KWin exits
   with the leader.

## The shell process

`horizon-shell` (`src/main.cpp`) is a `Shadow::App` like every SHADOW
application, without a main window. It creates these services and exposes
them to QML as context properties:

| Class (`src/`) | Property | Back-end |
|---|---|---|
| `ShellSettings` | `shellSettings` | `~/.config/shadow/horizon.conf` (QSettings, file watcher) |
| `Applications` | `applications` | freedesktop desktop-entry database through KService; launches with `KIO::ApplicationLauncherJob` and an xdg-activation token |
| `WindowModel` (`tasks.*`, `plasmawindowmanagement.*`) | `windows` | Wayland protocol `org_kde_plasma_window_management` |
| `Workspaces` | `workspaces` | D-Bus `org.kde.KWin.VirtualDesktopManager` |
| `Session` | `session` | logind (power actions), `org.freedesktop.ScreenSaver` (lock), `/run/shadow/live-session` and `/run/shadow/autostart` (live medium), program launching |
| `Shortcuts` | `shortcuts` | KGlobalAccel (`org.kde.kglobalaccel`), component `shadow-horizon` |
| `PolkitAgent` | `polkitAgent` | polkit authentication agent (PolkitQt6) |
| `ShellEffects` | (used by `BlurBehind`) | `KWindowEffects` blur-behind |
| `Horizon::ShellModules` (`module.*`) | `shellModules` | registry of compiled-in modules |

The QML root is `qml/Shell.qml`. It instantiates:

* one `Wallpaper` per screen (layer `background`);
* on the primary screen the `Panel`, the `Dock`, and the overlays
  `Launcher`, `SessionMenu`, `DockMenu`, `AuthDialog` and a toast for the
  shell's own error messages;
* the panel items and surfaces registered by modules.

Only one overlay is open at a time. Named actions — from a global shortcut,
a panel button or a module — go through `shell.trigger(name)`: the core
actions start programs, a name with a registered overlay toggles it, and
anything else is passed to the modules (`unhandledAction`).

Surfaces are layer-shell windows (`qml/ShellSurface.qml`, using the
`org.kde.layershell` QML module of layer-shell-qt): `ShellSurface` for
passive surfaces sized to their content, `OverlaySurface` for full-screen
surfaces that take the keyboard, `PopupSurface` for cards hanging under the
panel.

### Start of the session, and showing and hiding panel and dock

Wallpaper, panel and dock are created in the same moment and are there, and
take input, from their first frame. What follows in the first 0.8 s is drawing
only (`qml/StartClock.qml`, one instance in each of the three surfaces; the
clock starts after two drawn frames, so a slow first frame cannot swallow the
beginning):

| Time | What |
|---|---|
| 0–0.46 s | a scan line (`StartScan.qml`) crosses the screen from the top; the wallpaper picture appears above it (it sits in a clip that follows the line). A picture that is decoded later fades in when it arrives |
| 0–0.4 s | panel: the background fades in; clock, left group and right group light up one after the other, each dropping 6 px into place; a hairline in the accent colour grows from the centre along the lower edge and then fades into the border |
| 0.08–0.4 s | dock: the bar fades in, the row of icons rises 12 px, the icons light up from the centre outwards and grow from 85 % to full size |
| 0.4–0.8 s | four brackets (`StartBrackets.qml`) close on the dock, hold and fade |

There is no sequence with "Reduce motion" (the durations are 0) and when an
application window is already open — then the shell was restarted under a
running session and everything is simply there.

A panel or dock that hides (`panelHide`, `dockHide`: the default "smart"
hides them while a window is open) is a layer-shell window that shrinks to a
2 px / 4 px strip at the screen edge. Coming back, the window has its full
height at once, the background fades in and the contents slide 8 px into
place (140 ms). Leaving, everything fades (80 ms) and only then the window
shrinks. With "Reduce motion" both are instant.

One rule holds in both cases: the item that carries the blur (`BlurBehind`:
the panel background, the dock bar) is never moved, scaled or clipped. It
tells the compositor its rectangle only when its size or visibility changes,
so only its opacity is animated and what moves are its contents.

One departure from the design: the icons grow from 85 % as whole dock
entries (`scale` on the `DockItem` in `Dock.qml`), not through the icon
inside the entry, because `DockItem.qml` belongs to the shell's own files and
does not expose that inner item. For the first 0.4 s the entry's hover and
click area is therefore up to 15 % smaller, and the rectangle the dock tells
KWin for the minimise animation is sent again once the sequence is over.
Exposing the inner icon of `DockItem` would let the scale move there.

Verified in the lab (KWin nested in Xvfb, llvmpipe and the software
renderer, no blur): the sequence frame by frame, the restart under an open
window, showing and hiding. Not verified: a real session start after a
login, and anything with a GPU and blur.

### Modules

`modules/<name>/` is compiled into the same binary; the core does not
reference it. Present with a README: `notifications`, `tray`, `status`. A
`power` module directory exists but is not integrated. The mechanism and
rules are in [shell-modules.md](shell-modules.md); each module's README
documents its D-Bus names, limits on untrusted input and tests.

| Module | Owns on the session bus | Adds |
|---|---|---|
| `notifications` | `org.freedesktop.Notifications` (not replacing another daemon) | popups, notification centre, bell |
| `tray` | `org.kde.StatusNotifierWatcher` (or registers as a host of an existing watcher) | tray icons and their menus |
| `status` | nothing | status item, quick settings, calendar, on-screen display |

## Protocols and interfaces used

| Interface | Kind | Used for |
|---|---|---|
| layer-shell (`zwlr_layer_shell_v1`, via layer-shell-qt) | Wayland | every shell surface: wallpaper, panel, dock, overlays, popups |
| `org_kde_plasma_window_management` | Wayland, restricted by KWin | window list, active window, activate/close/minimise |
| `org_kde_plasma_virtual_desktop_management`, `org_kde_plasma_activation_feedback` | Wayland, restricted by KWin | requested in the shell's desktop file |
| xdg-activation | Wayland | passing focus to applications the shell starts; tokens for notification and tray actions |
| blur (through `KWindowEffects`) | Wayland | "glass" surfaces |
| `org.kde.KWin.VirtualDesktopManager` | D-Bus, session | workspaces |
| `org.kde.kglobalaccel` | D-Bus, session | global shortcuts |
| `org.freedesktop.ScreenSaver` | D-Bus, session | lock |
| `org.kde.KWin` (`/org/kde/KWin/NightLight`, `/org/kde/KWin/InputDevice`), `org.kde.keyboard` | D-Bus, session | Night Light, input devices and keyboard layouts (status module, Control Center, installer) |
| `org.freedesktop.login1` | D-Bus, system | suspend, hibernate, restart, power off; brightness |
| polkit authority | D-Bus, system | authentication agent registration |
| `org.freedesktop.Notifications`, `org.kde.StatusNotifierWatcher`, `com.canonical.dbusmenu`, MPRIS2 | D-Bus, session | notifications, tray, media control |
| NetworkManager, BlueZ, UPower, power-profiles-daemon | D-Bus, system | status module |

The restricted Wayland interfaces are KWin-specific. They are the main thing
that ties the shell to KWin; see "Design decisions" in
[../architecture/overview.md](../architecture/overview.md).

## Rendering and the software fallback

KWin and Qt Quick normally render with OpenGL on the GPU. On a machine
without hardware 3D — a basic virtual machine, a display adapter without a
driver — OpenGL falls back to llvmpipe, which is slow and, in the QEMU test
of the desktop image, **crashed the compositor**. The session therefore
checks for a GPU render node:

```
SHADOW_SOFTWARE_RENDERING unset or "auto":
    /dev/dri/renderD* exists  → GPU rendering
    otherwise                 → software rendering
SHADOW_SOFTWARE_RENDERING=1    → software rendering
SHADOW_SOFTWARE_RENDERING=0    → GPU rendering
```

Software rendering sets `KWIN_COMPOSE=Q` (KWin composites with QPainter on
the CPU) and `QT_QUICK_BACKEND=software` (Qt Quick's software renderer for
the shell and applications). Consequences:

* no blur and no other OpenGL effects: `ShellEffects` reports blur as
  unavailable and every "glass" surface is drawn opaque;
* animations and large redraws cost CPU time;
* applications that require OpenGL may not work.

`SHADOW_SOFTWARE_RENDERING` must be in the environment of
`shadow-horizon-session`, that is, set before the session starts. How to do
that when the desktop does not come up is described in
[../troubleshooting/README.md](../troubleshooting/README.md).

Verified: the automatic fallback in QEMU without a GPU. Not verified:
GPU rendering on any real graphics card.

## Theming chain

Everything visual derives from one file,
`desktop/themes/tokens/shadow-tokens.json` (colours, typography, spacing,
radii, motion), through `scripts/gen-tokens.py`.

```
shadow-tokens.json
 ├─ gen-tokens.py qml    → Theme singleton of ShadowUI (generated at build time)
 │                         → shell and every SHADOW application
 ├─ gen-tokens.py render → colour schemes, kdeglobals, GTK settings, gschema override,
 │                         SDDM and lock-screen tokens.js   (generated files are committed;
 │                         desktop/themes/tools/generate.py)
 └─ gen-tokens.py render → Terminal style sheet and colour schemes (at build time)
```

**SHADOW applications and the shell** use ShadowUI: the `Theme` singleton and
the Qt Quick Controls style `org.shadow.style`. The live state — dark/light,
accent, reduce motion, transparency, font scale — is `Shadow::Appearance`,
stored in `~/.config/shadow/appearance.conf` and watched by every process, so
a change applies everywhere at once.

**Third-party applications** cannot use ShadowUI. The chain for them
(`desktop/themes/README.md`):

| Toolkit | Mechanism |
|---|---|
| Qt and KDE applications, KWin window decorations | `QT_QPA_PLATFORMTHEME=kde` (`plasma-integration`) reads `/etc/xdg/kdeglobals`: SHADOW colour scheme, icon theme `shadow`, Inter and JetBrains Mono fonts, widget style Breeze |
| GTK 3 / GTK 4 | `/etc/xdg/gtk-{3,4}.0/settings.ini` and a GSettings schema override: dark preference, icon and cursor theme, fonts |
| libadwaita | follows the settings portal's `color-scheme`; in this session the KDE portal back-end derives it from `kdeglobals` |
| Icons | themes `shadow` (dark) and `shadow-light`, inheriting Breeze and hicolor; SHADOW application icons are installed into hicolor |

`shadow-apply-appearance` switches all of these for the user: it rewrites the
colour groups in `~/.config/kdeglobals`, sets the icon theme, updates the
GTK settings files and GSettings, and notifies running applications over the
session bus. It is called by the session script at start and by the shell
whenever `Appearance.dark` or `Appearance.accent` changes.

It also carries "Reduce motion" (`ReduceMotion` in `appearance.conf`) to the
two places that cannot read that file: KDE's animation speed factor
(`kdeglobals` `[KDE] AnimationDurationFactor`, written with a change
notification: KWin's effects, the Alt+Tab switcher and KDE applications) and
the lock screen's own setting (`kscreenlockerrc` `[Greeter][LnF][General]
reduceMotion`). The factor is set to 0 while Reduce motion is on; when it
is off, only a 0 is removed again (KDE's default applies) and a speed the
user chose in KDE's own settings is left alone. The shell runs the tool when
that switch changes (as it does for mode and accent), so both follow it then;
SHADOW's own applications and the shell follow at once. The run on a switch
has not been seen in a real session.

Tested in the build chroot: Qt widgets with the staged theme, including a
live switch. Not tested: GTK and libadwaita (no GTK in the chroot), and the
whole chain in the booted image.

## Login screen (SDDM)

* Theme `shadow` (`/usr/share/sddm/themes/shadow`), plain Qt Quick, Qt 6
  greeter.
* `/usr/lib/sddm/sddm.conf.d/10-shadow.conf` selects the theme and
  `DisplayServer=wayland`. The greeter is composited by
  `kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1`.
  Shadow OS ships no Xorg server, so SDDM's default X11 greeter could not
  start.
* `--locale1` makes the greeter use the system keyboard layout
  (`/etc/default/keyboard`), so passwords are typed in the layout chosen at
  installation.
* The greeter needs `layer-shell-qt` (a dependency of `shadow-themes`).
* Features: user list and "Other user…", password field with show/hide and
  Caps Lock warning, session selector, suspend/restart/shut down when SDDM
  allows them.
* Entrance (0.7 s): the first frame is the near-black field the boot splash
  ends in, with the password field already there, focused and usable. A scan
  line then resolves the wallpaper from the top while the rest lights up:
  the hexagon of the SHADOW symbol around the user picture, a tick ring whose
  two nodes show the real time, brackets around the password row, the clock,
  the buttons of the bottom bar one after the other. While a login is checked
  a comet circles the ring; a refused login turns ring and brackets red once,
  next to the message and the shake. All of this decoration is one file
  behind a `Loader` (`components/LoginDecor.qml`); the form works without it.
* `reduceMotion=true` in the theme's `theme.conf` (or `theme.conf.user`)
  switches every movement off. It is one setting for the whole computer:
  the greeter runs before anyone is logged in and cannot know a user's
  "Reduce motion" switch.
* Live session: `system/live/2000-shadow-session` writes
  `/etc/sddm.conf.d/10-shadow-live.conf` with auto-login for the live user
  into the `shadow-horizon` session, unless `noautologin` is on the kernel
  command line.
* Installed system with automatic login: the installer writes
  `/etc/sddm.conf.d/20-autologin.conf`.

Verified: the theme in SDDM's test mode (build chroot, and frame by frame
in the lab with OpenGL on llvmpipe and with the software renderer);
auto-login of the live user in QEMU. Not verified: an interactive login
through the greeter on a booted system, the greeter on DRM, and what the
screen shows between the end of the boot splash and the greeter's first
frame (the splash is not kept; nothing here changes that).

## Lock screen

Locking is done by the compositor, not by the shell: only the compositor can
guarantee that nothing is displayed and no input reaches clients.

1. `Session::lock()` calls `Lock` on `org.freedesktop.ScreenSaver`; KWin
   also locks by itself after the idle time-out and on suspend
   (`/etc/xdg/kscreenlockerrc`: 10 minutes, lock on resume, 5 s grace).
2. KWin's KScreenLocker starts `kscreenlocker_greet`.
3. The greeter loads its QML from a "Plasma/Shell" package. Which one is
   named by `/etc/xdg/plasmashellrc` (`ShellPackage=org.shadow.horizon`) and
   `PLASMA_DEFAULT_SHELL`; the package is
   `/usr/share/plasma/shells/org.shadow.horizon/contents/lockscreen/LockScreen.qml`
   — SHADOW's own QML, using only QtQuick. Its decoration (scan line,
   ring, brackets; a 0.4 s entrance each time the screen is shown) is a
   separate file behind a `Loader`, `components/LockDecor.qml`: when that
   file cannot be loaded the form is complete and unlocks as before. There
   is no exit animation: the right password ends the greeter at once. The
   user's "Reduce motion" switch reaches it as `reduceMotion` in
   `kscreenlockerrc` (see the theming chain above).
4. Authentication is PAM, service `kde` (`/etc/pam.d/kde`, including
   Debian's `common-auth`). Two further non-interactive PAM services,
   `kde-fingerprint` and `kde-smartcard`, exist because the greeter always
   starts them; they contain only `pam_fprintd` / `pam_pkcs11` (skipped when
   not installed) and `pam_deny`, so they can never ask for a password.
5. On success the greeter exits and KWin unlocks.

KScreenLocker is designed to keep the screen locked if the greeter crashes
or cannot start (the safe failure mode); this has not been exercised here.
Recovery is described in
[../troubleshooting/README.md](../troubleshooting/README.md).

Verified: the lock-screen QML with the real `kscreenlocker_greet --testing`
and real PAM in the build chroot, as an X11 client; in the lab also with the
software renderer, with the decoration file removed, and with
`reduceMotion` set. Not verified: a real lock from KWin on Wayland, the
grace time, multiple screens.

## Authentication agent

The shell registers as the polkit authentication agent of the session
(`src/polkitagent.*`). It relays the PAM conversation between polkit's
helper and `AuthDialog.qml`, which is on the overlay layer with exclusive
keyboard focus. The secret goes to polkit's setuid helper and is neither
stored nor logged by the shell. If the shell is not running, no agent is
available and `pkexec` falls back to its text-mode prompt or fails.

## Application runtime

Every SHADOW QML application goes through `Shadow::App`
(`desktop/ui/src/app/shadowapp.h`): SHADOW style, icon providers,
`--help`/`--version`/`--dump <topic>`, optional single-instance activation,
and the test hooks `SHADOW_SMOKE_TEST`, `SHADOW_SCREENSHOT`,
`SHADOW_SCREENSHOT_DELAY`, `SHADOW_WINDOW_SIZE`. External programs are run
with `Shadow::Command` (asynchronous, argument vector). The contract is in
[../applications/development.md](../applications/development.md).

## Logs

| What | Where |
|---|---|
| Session script, KWin, session leader, shell (standard error) | `~/.local/share/sddm/wayland-session.log` |
| User services, portals, autostart | `journalctl --user -b` |
| SDDM and the greeter | `journalctl -b -u sddm` |
| Lock-screen greeter | user journal |
