# NEXUS HORIZON architecture

How the desktop is built: the session, the shell process, the protocols and
services it uses, rendering, theming of third-party applications, and the
login and lock screens. For using the desktop see [horizon.md](horizon.md);
for writing shell modules see [shell-modules.md](shell-modules.md); for the
UI library see [nexusui.md](nexusui.md).

Sources: `desktop/horizon-shell/` (shell and session), `desktop/ui/`
(NexusUI), `desktop/themes/` (themes, login and lock screen).

## Components

| Component | Package | Role |
|---|---|---|
| KWin (`kwin_wayland`) + Xwayland | Debian `kwin-wayland`, `xwayland` | Wayland compositor and window manager; runs X11 applications |
| `nexus-horizon-session` | `nexus-horizon` | session start script (`/usr/bin`) |
| `horizon-session-leader` | `nexus-horizon` | session supervisor (`/usr/libexec/nexus`) |
| `horizon-shell` | `nexus-horizon` | the shell: one process |
| NexusUI (`libnexusui`, QML module `org.nexus.ui`, style `org.nexus.style`) | `nexus-ui` | shared UI library of the shell and all NEXUS applications |
| Themes, SDDM theme, lock screen, `nexus-apply-appearance` | `nexus-themes` | look of everything that is not a NEXUS application |
| SDDM | Debian `sddm` | display manager (login screen) |
| KScreenLocker (`kscreenlocker_greet`) | Debian `libkscreenlocker6` | lock screen process, started by KWin |
| xdg-desktop-portal with the KDE and GTK back-ends | Debian | portals (file chooser, screenshots, screen sharing, settings) |

NEXUS HORIZON does not run `plasmashell` or any other Plasma process.

## Session files

Installed by `desktop/horizon-shell/CMakeLists.txt`:

| File | Purpose |
|---|---|
| `/usr/share/wayland-sessions/nexus-horizon.desktop` | the "NEXUS HORIZON" entry of the login screen; `Exec=/usr/bin/nexus-horizon-session`, `DesktopNames=NEXUS;KDE` |
| `/usr/bin/nexus-horizon-session` | sets the environment, chooses the rendering mode, starts KWin |
| `/usr/libexec/nexus/horizon-session-leader` | started by KWin; publishes the environment, starts the session target, supervises the shell |
| `/usr/lib/systemd/user/nexus-horizon-session.target` | binds `graphical-session.target`; wants `graphical-session-pre.target` and `xdg-desktop-autostart.target` |
| `/usr/share/applications/org.nexus.horizon.shell.desktop` | hidden entry of the shell; its `X-KDE-Wayland-Interfaces` line is what makes KWin grant the shell the restricted protocols listed below |
| `/usr/share/xdg-desktop-portal/nexus-portals.conf` | portal back-ends for `XDG_CURRENT_DESKTOP=NEXUS`: `kde`, then `gtk` |
| `/etc/xdg/menus/nexus-applications.menu` | the application menu (flat: every installed application); selected with `XDG_MENU_PREFIX=nexus-` |
| `/etc/xdg/kwinrc` | compositor defaults |
| `/etc/xdg/kscreenlockerrc` | lock-screen defaults |

### Environment set by `nexus-horizon-session`

| Variable | Value | Why |
|---|---|---|
| `XDG_CURRENT_DESKTOP` | `NEXUS:KDE` | NEXUS first; `KDE` makes portal back-ends, KDE Frameworks applications and xdg-utils choose their KDE behaviour |
| `XDG_SESSION_DESKTOP`, `XDG_SESSION_TYPE` | `nexus`, `wayland` | |
| `XDG_MENU_PREFIX` | `nexus-` | selects `nexus-applications.menu` |
| `KDE_FULL_SESSION`, `KDE_SESSION_VERSION` | `true`, `6` | KDE Frameworks integration |
| `QT_QPA_PLATFORM` | `wayland;xcb` | native Wayland, X11 as fallback |
| `QT_QPA_PLATFORMTHEME` | `kde` | third-party Qt applications read `kdeglobals` through `plasma-integration` |
| `GDK_BACKEND`, `SDL_VIDEODRIVER`, `CLUTTER_BACKEND`, `MOZ_ENABLE_WAYLAND`, `ELECTRON_OZONE_PLATFORM_HINT` | Wayland first | toolkit selection |
| `_JAVA_AWT_WM_NONREPARENTING` | `1` | Java Swing/AWT tools lay out correctly through Xwayland |
| `XCURSOR_THEME`, `XCURSOR_SIZE` | `breeze_cursors`, `24` (unless already set) | cursor |
| `PLASMA_DEFAULT_SHELL` | `org.nexus.horizon` | where KScreenLocker finds the lock-screen QML |
| `KWIN_COMPOSE`, `QT_QUICK_BACKEND` | `Q`, `software` — only with software rendering | see below |

It also creates `$XDG_RUNTIME_DIR/nexus` for per-session state and runs
`nexus-apply-appearance` before starting the compositor:

```
exec kwin_wayland_wrapper --xwayland --exit-with-session=/usr/libexec/nexus/horizon-session-leader
```

### The session leader

1. Guards against a second copy: its PID is kept in
   `$XDG_RUNTIME_DIR/nexus/session-leader.pid`. KWin starts the leader again
   when it restarts after a crash; that copy waits for the first one instead
   of starting a second shell.
2. Publishes the display and desktop variables with
   `dbus-update-activation-environment` (with `--systemd` when the systemd
   user manager runs), so D-Bus-activated services, portals and autostart
   entries see the display.
3. Runs `kbuildsycoca6`, so the application database exists before the
   launcher opens.
4. Starts `nexus-horizon-session.target` (XDG autostart applications).
5. Runs `/usr/bin/horizon-shell` in a loop: exit status 0 means "log out";
   any other status is a crash and the shell is restarted — at most five
   crashes within a minute, then the session ends.
6. On exit stops the target and unsets the published variables. KWin exits
   with the leader.

## The shell process

`horizon-shell` (`src/main.cpp`) is a `Nexus::App` like every NEXUS
application, without a main window. It creates these services and exposes
them to QML as context properties:

| Class (`src/`) | Property | Back-end |
|---|---|---|
| `ShellSettings` | `shellSettings` | `~/.config/nexus/horizon.conf` (QSettings, file watcher) |
| `Applications` | `applications` | freedesktop desktop-entry database through KService; launches with `KIO::ApplicationLauncherJob` and an xdg-activation token |
| `WindowModel` (`tasks.*`, `plasmawindowmanagement.*`) | `windows` | Wayland protocol `org_kde_plasma_window_management` |
| `Workspaces` | `workspaces` | D-Bus `org.kde.KWin.VirtualDesktopManager` |
| `Session` | `session` | logind (power actions), `org.freedesktop.ScreenSaver` (lock), `/run/nexus/live-session` and `/run/nexus/autostart` (live medium), program launching |
| `Shortcuts` | `shortcuts` | KGlobalAccel (`org.kde.kglobalaccel`), component `nexus-horizon` |
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
NEXUS_SOFTWARE_RENDERING unset or "auto":
    /dev/dri/renderD* exists  → GPU rendering
    otherwise                 → software rendering
NEXUS_SOFTWARE_RENDERING=1    → software rendering
NEXUS_SOFTWARE_RENDERING=0    → GPU rendering
```

Software rendering sets `KWIN_COMPOSE=Q` (KWin composites with QPainter on
the CPU) and `QT_QUICK_BACKEND=software` (Qt Quick's software renderer for
the shell and applications). Consequences:

* no blur and no other OpenGL effects: `ShellEffects` reports blur as
  unavailable and every "glass" surface is drawn opaque;
* animations and large redraws cost CPU time;
* applications that require OpenGL may not work.

`NEXUS_SOFTWARE_RENDERING` must be in the environment of
`nexus-horizon-session`, that is, set before the session starts. How to do
that when the desktop does not come up is described in
[../troubleshooting/README.md](../troubleshooting/README.md).

Verified: the automatic fallback in QEMU without a GPU. Not verified:
GPU rendering on any real graphics card.

## Theming chain

Everything visual derives from one file,
`desktop/themes/tokens/nexus-tokens.json` (colours, typography, spacing,
radii, motion), through `scripts/gen-tokens.py`.

```
nexus-tokens.json
 ├─ gen-tokens.py qml    → Theme singleton of NexusUI (generated at build time)
 │                         → shell and every NEXUS application
 ├─ gen-tokens.py render → colour schemes, kdeglobals, GTK settings, gschema override,
 │                         SDDM and lock-screen tokens.js   (generated files are committed;
 │                         desktop/themes/tools/generate.py)
 └─ gen-tokens.py render → Terminal style sheet and colour schemes (at build time)
```

**NEXUS applications and the shell** use NexusUI: the `Theme` singleton and
the Qt Quick Controls style `org.nexus.style`. The live state — dark/light,
accent, reduce motion, transparency, font scale — is `Nexus::Appearance`,
stored in `~/.config/nexus/appearance.conf` and watched by every process, so
a change applies everywhere at once.

**Third-party applications** cannot use NexusUI. The chain for them
(`desktop/themes/README.md`):

| Toolkit | Mechanism |
|---|---|
| Qt and KDE applications, KWin window decorations | `QT_QPA_PLATFORMTHEME=kde` (`plasma-integration`) reads `/etc/xdg/kdeglobals`: NEXUS colour scheme, icon theme `nexus`, Inter and JetBrains Mono fonts, widget style Breeze |
| GTK 3 / GTK 4 | `/etc/xdg/gtk-{3,4}.0/settings.ini` and a GSettings schema override: dark preference, icon and cursor theme, fonts |
| libadwaita | follows the settings portal's `color-scheme`; in this session the KDE portal back-end derives it from `kdeglobals` |
| Icons | themes `nexus` (dark) and `nexus-light`, inheriting Breeze and hicolor; NEXUS application icons are installed into hicolor |

`nexus-apply-appearance` switches all of these for the user: it rewrites the
colour groups in `~/.config/kdeglobals`, sets the icon theme, updates the
GTK settings files and GSettings, and notifies running applications over the
session bus. It is called by the session script at start and by the shell
whenever `Appearance.dark` or `Appearance.accent` changes.

Tested in the build chroot: Qt widgets with the staged theme, including a
live switch. Not tested: GTK and libadwaita (no GTK in the chroot), and the
whole chain in the booted image.

## Login screen (SDDM)

* Theme `nexus` (`/usr/share/sddm/themes/nexus`), plain Qt Quick, Qt 6
  greeter.
* `/usr/lib/sddm/sddm.conf.d/10-nexus.conf` selects the theme and
  `DisplayServer=wayland`. The greeter is composited by
  `kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1`.
  NEXUS OS ships no Xorg server, so SDDM's default X11 greeter could not
  start.
* `--locale1` makes the greeter use the system keyboard layout
  (`/etc/default/keyboard`), so passwords are typed in the layout chosen at
  installation.
* The greeter needs `layer-shell-qt` (a dependency of `nexus-themes`).
* Features: user list and "Other user…", password field with show/hide and
  Caps Lock warning, session selector, suspend/restart/shut down when SDDM
  allows them.
* Live session: `system/live/2000-nexus-session` writes
  `/etc/sddm.conf.d/10-nexus-live.conf` with auto-login for the live user
  into the `nexus-horizon` session, unless `noautologin` is on the kernel
  command line.
* Installed system with automatic login: the installer writes
  `/etc/sddm.conf.d/20-autologin.conf`.

Verified: the theme in SDDM's test mode (build chroot); auto-login of the
live user in QEMU. Not verified: an interactive login through the greeter on
a booted system.

## Lock screen

Locking is done by the compositor, not by the shell: only the compositor can
guarantee that nothing is displayed and no input reaches clients.

1. `Session::lock()` calls `Lock` on `org.freedesktop.ScreenSaver`; KWin
   also locks by itself after the idle time-out and on suspend
   (`/etc/xdg/kscreenlockerrc`: 10 minutes, lock on resume, 5 s grace).
2. KWin's KScreenLocker starts `kscreenlocker_greet`.
3. The greeter loads its QML from a "Plasma/Shell" package. Which one is
   named by `/etc/xdg/plasmashellrc` (`ShellPackage=org.nexus.horizon`) and
   `PLASMA_DEFAULT_SHELL`; the package is
   `/usr/share/plasma/shells/org.nexus.horizon/contents/lockscreen/LockScreen.qml`
   — NEXUS's own QML, using only QtQuick.
4. Authentication is PAM, service `kde` (`/etc/pam.d/kde`, including
   Debian's `common-auth`). Two further non-interactive PAM services,
   `kde-fingerprint` and `kde-smartcard`, exist because the greeter always
   starts them; they contain only `pam_fprintd` / `pam_pkcs11` (skipped when
   not installed) and `pam_deny`, so they can never ask for a password.
5. On success the greeter exits and KWin unlocks.

If the greeter crashes or cannot start, KWin keeps the screen locked; that
is the safe failure mode. Recovery is described in
[../troubleshooting/README.md](../troubleshooting/README.md).

Verified: the lock-screen QML with the real `kscreenlocker_greet --testing`
and real PAM in the build chroot, as an X11 client. Not verified: a real
lock from KWin on Wayland, the grace time, multiple screens.

## Authentication agent

The shell registers as the polkit authentication agent of the session
(`src/polkitagent.*`). It relays the PAM conversation between polkit's
helper and `AuthDialog.qml`, which is on the overlay layer with exclusive
keyboard focus. The secret goes to polkit's setuid helper and is neither
stored nor logged by the shell. If the shell is not running, no agent is
available and `pkexec` falls back to its text-mode prompt or fails.

## Application runtime

Every NEXUS QML application goes through `Nexus::App`
(`desktop/ui/src/app/nexusapp.h`): NEXUS style, icon providers,
`--help`/`--version`/`--dump <topic>`, optional single-instance activation,
and the test hooks `NEXUS_SMOKE_TEST`, `NEXUS_SCREENSHOT`,
`NEXUS_SCREENSHOT_DELAY`, `NEXUS_WINDOW_SIZE`. External programs are run
with `Nexus::Command` (asynchronous, argument vector). The contract is in
[../applications/development.md](../applications/development.md).

## Logs

| What | Where |
|---|---|
| Session script, KWin, session leader, shell (standard error) | `~/.local/share/sddm/wayland-session.log` |
| User services, portals, autostart | `journalctl --user -b` |
| SDDM and the greeter | `journalctl -b -u sddm` |
| Lock-screen greeter | user journal |
