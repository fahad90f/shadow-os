# Using NEXUS HORIZON

NEXUS HORIZON is the desktop of NEXUS OS: a top panel, a dock, an
application launcher and a set of popups, drawn by one program
(`horizon-shell`) on the KWin Wayland compositor. This page describes what
you see and how to use it. How it works inside is in
[architecture.md](architecture.md).

What has been checked: in QEMU the desktop image logs the live user in and
shows panel, dock, wallpaper and launcher; in a nested test session the
launcher, search, dock, its context menu, the session menu and the global
shortcuts were exercised with injected input. The tray, notifications and
status parts were tested in nested sessions as their READMEs describe, not
yet in the booted image. Nothing has run on a real graphics card.

## The screen

```
┌────────────────────────────────────────────────────────────────────────┐
│ ⬡  ● ○ ○ ○  Application · window title     Tue 10 Nov  14:32   ▢ ▢ ▢ ⏻ │  panel
│                                                                        │
│                              wallpaper                                 │
│                                                                        │
│                     ╭──────────────────────────╮                       │
│                     │  ▣  ▣  ▣  ▣  ▣  ▣  ▣     │                       │  dock
└─────────────────────┴──────────────────────────┴───────────────────────┘
```

Panel and dock are on the primary screen. Every screen gets the wallpaper.

## Panel

Left to right:

| Item | What it does |
|---|---|
| NEXUS button | opens the launcher |
| Workspace pills | one per workspace, the current one is wider and in the accent colour; click a pill to switch. Shown when there is more than one workspace |
| Application name and window title | of the active window |
| Clock (centre) | date and time. When the status module is present, a click opens the calendar |
| System tray | icons of applications that provide a status item (see "Tray") |
| Status item | network, VPN, volume and battery indicators; a click opens the quick settings |
| Bell | opens the notification centre; shows the number of unread notifications |
| Install | only in the live session: starts the installer |
| Power button | opens the session menu |

The tray, status item and bell come from shell modules and are absent if a
module is not built in.

## Dock

Shows pinned applications and running applications.

| Action | Result |
|---|---|
| Click | start the application, or activate its window if it is running |
| Middle click | open a new window |
| Right click | context menu: the application's open windows, "Open"/"New window", "Pin to dock"/"Unpin from dock", "Close window"/"Close all windows" |
| Tab, then Return or Space | the same as a click, from the keyboard |

An entry pulses while one of its windows asks for attention. Applications
that are pinned but not installed are not shown. The dock can hide
automatically (`AutoHide`, below).

Default pins, as far as they are installed: Files, Firefox ESR, Terminal,
Editor, Store, Arsenal, Security Center, Monitor, Control Center. Several of
these applications are not in the image yet
([../limitations.md](../limitations.md)); their entries do not appear.

## Launcher

Open it with the **Meta** key (the Super/Windows key, tapped on its own),
**Alt+F1**, or the NEXUS button.

* Type to search; the grid narrows as you type.
* Arrow keys move, **Return** starts the selected application, **Escape**
  closes the launcher.
* The list on the left narrows the grid to one category; "All applications"
  shows everything.
* The footer shows `user@host` (and "live session" on the live medium), the
  number of applications, a button for the Control Center and one for the
  session menu.

The launcher lists every installed application that has a desktop entry.

## Workspaces and windows

Windows are managed by KWin with the defaults NEXUS ships in
`/etc/xdg/kwinrc`:

| Setting | Default |
|---|---|
| Workspaces (virtual desktops) | 4, in one row; switching wraps around |
| New windows | placed in the centre |
| Focus | click to focus |
| Dragging a window to a screen edge | top: maximise; sides: tile |
| Snapping | windows snap to screen edges and to each other |
| Title-bar buttons | minimise, maximise, close on the right; none on the left |
| Window borders | none |
| Window switcher (Alt+Tab) | thumbnail grid |
| Effects | workspace slide, scale on open/close, fade, overview, blur and background contrast (only with GPU rendering) |
| Xwayland applications | cannot read keystrokes typed into other windows (`XwaylandEavesdrops=None`) |

The keyboard shortcuts for window management — switching windows and
workspaces, moving, tiling, the overview — are **KWin's own defaults**.
NEXUS does not change or define them, so they are not listed here; the
Control Center's Keyboard page shows the shortcuts that are registered on
your system (read-only).

To change a KWin setting, write it to your own `~/.config/kwinrc`; it
overrides the system file.

## Keyboard shortcuts

These are the global shortcuts the shell registers
(`desktop/horizon-shell/src/shortcuts.cpp`). If you change a binding through
KDE's global-shortcut service, your binding is kept.

| Keys | Action | What happens |
|---|---|---|
| **Meta** (tapped alone), **Alt+F1** | Open the application launcher | toggles the launcher |
| **Ctrl+Alt+T**, **Meta+Return** | Open a terminal | starts `nexus-terminal` |
| **Meta+E** | Open the file manager | starts `nexus-files` (NEXUS Files) |
| **Meta+I** | Open the Control Center | starts `nexus-control-center` |
| **Meta+A** | Show quick settings | toggles the quick settings (status module) |
| **Meta+N** | Show notifications | toggles the notification centre (notifications module) |
| **Ctrl+Alt+Delete** | Show the power and session menu | toggles the session menu |
| **Ctrl+Shift+Escape** | Open the task manager | starts `nexus-monitor --page processes` |
| **Print** | Take a screenshot | starts `nexus-screenshot` |
| **Meta+Shift+S** | Take a screenshot of a region | starts `nexus-screenshot --region` |
| **Volume Up** / **Volume Down** / **Mute** keys | Increase / decrease volume, mute | handled by the status module, with an on-screen display |
| **Microphone Mute** key | Mute the microphone | status module |
| **Brightness Up** / **Brightness Down** keys | Screen brightness | status module |
| **Play/Pause**, **Next**, **Previous** media keys | Media control | status module (MPRIS players) |

When a shortcut names a program that is not installed, the shell shows a
short message such as "nexus-terminal is not installed." This currently
applies to `nexus-terminal` and `nexus-screenshot`, which are not part of
the image yet.

The shortcuts need KDE's global-shortcut service on the session bus; in the
NEXUS HORIZON session KWin provides it.

`horizon-shell --dump shortcuts` prints the registered shortcuts and their
current keys as JSON.

## Quick settings

**Meta+A** or a click on the status item. Provided by the status module
(`desktop/horizon-shell/modules/status/README.md`). It shows only what
exists on the machine:

* tiles: Wi-Fi (or a read-only "Network" tile without Wi-Fi hardware),
  Bluetooth, airplane mode, Do Not Disturb, Night Light, dark mode;
* sliders: volume with mute, microphone, screen brightness;
* battery state and power profile;
* a media card for the playing media player (play/pause, next, previous);
* a lock button and buttons that open the Control Center.

The **calendar** opens from the clock: a month view with week numbers and
keyboard navigation. It shows no events.

Volume, brightness and media keys show a small on-screen display.

Tested against a real PipeWire and a test media player; against fixtures for
NetworkManager, UPower and power profiles; not tested at all for Bluetooth,
airplane mode, brightness changes and Night Light — see the module's README.

## Notifications

Provided by the notifications module
(`desktop/horizon-shell/modules/notifications/README.md`). The shell is the
session's notification service.

* **Popups** appear at the top right under the panel, at most four at a
  time. Click a popup for its default action, use its buttons for other
  actions, or close it. Popups do not take the keyboard. Resting the pointer
  on them stops their time-outs. Critical notifications stay until
  dismissed.
* **Notification centre** (bell or **Meta+N**): history grouped by
  application; dismiss one, a group or all. Keys: Up/Down, Return (default
  action), Delete (dismiss), Escape.
* **Do Not Disturb** holds back popups except critical ones; held-back
  notifications are in the centre. The setting is stored in
  `~/.config/nexus/notifications.conf`.

Limits: no sounds, no inline reply, and the history is lost when the shell
restarts. Popups and the centre appear on the primary screen only.

## Tray

Provided by the tray module (`desktop/horizon-shell/modules/tray/README.md`).
It shows StatusNotifierItem icons — the protocol used by KDE applications,
libayatana-appindicator, Electron and Qt.

| Action | Result |
|---|---|
| Left click | the item's primary action (or its menu if it only has a menu) |
| Middle click | secondary action |
| Right click | the item's menu |
| Wheel | scroll action |
| Tab, Return/Space, Menu key | keyboard operation |

**Legacy X11 (XEmbed) tray icons are not supported.** Applications that only
provide those show no icon.

## Lock screen

* Lock from the session menu, the quick settings, or let it happen
  automatically.
* Defaults (`/etc/xdg/kscreenlockerrc`): the screen locks after 10 minutes
  without input and when the computer suspends. For 5 seconds after locking
  the screen can be dismissed without the password.
* Type your password and press Return to unlock.

Locking is done by the compositor's screen locker, which guarantees that
nothing is shown and no input reaches applications while locked. The lock
screen has not yet been verified in the booted image; see
[../troubleshooting/README.md](../troubleshooting/README.md) if it fails.

## Session menu

Power button in the panel, **Ctrl+Alt+Delete**, or the button in the
launcher.

| Entry | Notes |
|---|---|
| Lock | available when the screen locker is running |
| Log out | asks for confirmation; closes open applications |
| Suspend | if the system supports it |
| Hibernate | if the system supports it |
| Restart | asks for confirmation |
| Power off | asks for confirmation |

Entries the system cannot perform are disabled. Power actions go through
systemd-logind; if authorisation is needed (for example other users are
logged in), the authentication dialog appears.

## Authentication dialog

When an application asks for administrator rights (polkit), the shell shows
"Authentication required" with the requesting action, on top of everything
and with exclusive keyboard focus. Type the password of the shown account
and choose **Authenticate**, or press Escape to cancel. The password goes
directly to polkit and is not stored or logged by the shell.

## Appearance

NEXUS applications and the shell share one appearance state:

| Setting | Values |
|---|---|
| Colour scheme | dark (default) or light |
| Accent colour | one of the named accents; default `cyan` |
| Reduce motion | off (default) or on |
| Transparency | on (default) or off |
| Font scale | 0.8–1.6, default 1.0 |

You can switch dark mode from the quick settings. When the colour scheme or
accent changes, the shell runs `nexus-apply-appearance`, which carries the
change over to third-party Qt/KDE and GTK applications, window decorations
and the icon theme. From a terminal:

```sh
nexus-apply-appearance --mode light --accent violet
```

A Control Center page for appearance exists in the source tree but is not
part of the image yet; until then, use the quick settings, the command
above, or edit `appearance.conf` (below).

Translucent "glass" surfaces need GPU compositing. With CPU rendering — a
virtual machine without 3D, or `NEXUS_SOFTWARE_RENDERING=1` — surfaces are
opaque.

Wallpapers shipped in `/usr/share/backgrounds/nexus/`: `nexus-horizon.jpg`
(default, dark), `nexus-daylight.jpg` (default for the light scheme),
`nexus-lattice.jpg`, `nexus-aurora.jpg`.

## Settings files

All under `~/.config/nexus/`. System-wide defaults can be placed under
`/etc/xdg/nexus/` with the same file names. The files are plain INI; the
programs watch them and follow changes immediately.

| File | Written by | Content |
|---|---|---|
| `appearance.conf` | shell, applications | `[Appearance]` `ColorScheme=dark\|light`, `Accent=<name>`, `ReduceMotion=`, `Transparency=`, `FontScale=` |
| `horizon.conf` | shell | `[Desktop]` `Wallpaper=`, `WallpaperLight=`, `WallpaperFill=crop\|fit\|stretch\|center\|tile`; `[Dock]` `Pinned=<desktop ids, comma-separated>`, `AutoHide=true\|false`, `IconSize=32–72`; `[Panel]` `Clock24h=`, `ClockSeconds=`, `ClockDate=` |
| `notifications.conf` | notifications module | `[Notifications]` `DoNotDisturb=true\|false` |
| `power.conf` | Control Center, Power page | `[Power]` idle time-outs in minutes. **Nothing reads this file yet**: the shell part that would act on it is not integrated |
| `updater.conf` | Updater | `Updates/AutoCheck`, `Updates/CheckIntervalHours`, last check result |
| `arsenal.conf` | Arsenal manager | `[ResponsibleUse]` acknowledgement |
| `files.conf` | Files | `[View]`, `[Bookmarks]`, `[Servers]` |
| `terminal.conf` | Terminal (not in the image yet) | profiles, shortcuts, saved session |

Example — auto-hide the dock, larger icons, 12-hour clock with seconds:

```ini
# ~/.config/nexus/horizon.conf
[Dock]
AutoHide=true
IconSize=56

[Panel]
Clock24h=false
ClockSeconds=true
```

Other per-user files the desktop uses belong to KWin and friends and live
directly in `~/.config/`: `kwinrc` (compositor), `kscreenlockerrc` (lock
screen), `kxkbrc` (keyboard layouts), `kdeglobals` (colours for third-party
Qt applications), `kglobalshortcutsrc` (global shortcuts).

## Diagnostics

```sh
horizon-shell --dump settings      # effective shell settings and the file path
horizon-shell --dump apps          # applications the launcher knows
horizon-shell --dump workspaces
horizon-shell --dump session       # user, host, what the session menu can do
horizon-shell --dump shortcuts
horizon-shell --dump modules       # modules compiled in
horizon-shell --dump status        # status module back-ends
horizon-shell --dump notifications
horizon-shell --dump tray
```

Each prints JSON and exits without touching the running shell.
