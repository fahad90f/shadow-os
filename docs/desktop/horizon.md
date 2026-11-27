# Using Shadow Horizon

Shadow Horizon is the desktop of Shadow OS: a top panel, a dock, an
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
| SHADOW button | opens the launcher |
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
**Alt+F1**, or the SHADOW button.

* Type to search; the grid narrows as you type. You can start typing at
  once: the search field has the keyboard while the launcher is still
  being drawn.
* Arrow keys move, **Return** starts the selected application. **Escape**
  clears the search text; on an empty field it closes the launcher.
* The list on the left narrows the grid to one category; "All applications"
  shows everything.
* The footer shows `user@host` (and "live session" on the live medium), the
  number of applications, a button for the Control Center and one for the
  session menu.

The launcher lists every installed application that has a desktop entry.

## Workspaces and windows

Windows are managed by KWin with the defaults SHADOW ships in
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
SHADOW does not change or define them, so they are not listed here; the
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
| **Ctrl+Alt+T**, **Meta+Return** | Open a terminal | starts `shadow-terminal` |
| **Meta+E** | Open the file manager | starts `shadow-files` (SHADOW Files) |
| **Meta+I** | Open the Control Center | starts `shadow-control-center` |
| **Meta+A** | Show quick settings | toggles the quick settings (status module) |
| **Meta+N** | Show notifications | toggles the notification centre (notifications module) |
| **Ctrl+Alt+Delete** | Show the power and session menu | toggles the session menu |
| **Ctrl+Shift+Escape** | Open the task manager | starts `shadow-monitor --page processes` |
| **Print** | Take a screenshot | starts `shadow-screenshot` |
| **Meta+Shift+S** | Take a screenshot of a region | starts `shadow-screenshot --region` |
| **Volume Up** / **Volume Down** / **Mute** keys | Increase / decrease volume, mute | handled by the status module, with an on-screen display |
| **Microphone Mute** key | Mute the microphone | status module |
| **Brightness Up** / **Brightness Down** keys | Screen brightness | status module |
| **Play/Pause**, **Next**, **Previous** media keys | Media control | status module (MPRIS players) |
| **Ctrl+Alt+Shift+Escape** | Panic | cuts every network connection, locks the screen, clears the clipboard (see [Panic](#panic)) |

When a shortcut names a program that is not installed, the shell shows a
short message such as "shadow-terminal is not installed." This currently
applies to `shadow-terminal` and `shadow-screenshot`, which are not part of
the image yet.

The shortcuts need KDE's global-shortcut service on the session bus; in the
Shadow Horizon session KWin provides it.

`horizon-shell --dump shortcuts` prints the registered shortcuts and their
current keys as JSON.

The panic key was chosen because it is free in KWin 6.3's own registry
(checked in a nested session's `kglobalshortcutsrc`; the shortcuts of
KWin's effects are registered only with GPU compositing and were not in it,
and the effects' QML handles Escape only while an effect is open) and in
this list, and needs three modifiers: the Escape combinations next to it are taken (KWin:
**Meta+Ctrl+Escape** kills a window, **Meta+Shift+Escape** disables input
capture; the shell: **Ctrl+Shift+Escape** opens the task manager). If
another program holds the key when the shell starts, the shell says so in a
message instead of leaving a panic key that does nothing.

## Panic

**Ctrl+Alt+Shift+Escape** (the shell core, `desktop/horizon-shell/src/panic.h`)
does three things at once:

1. asks systemd to start `shadow-panic.service`
   (`org.freedesktop.systemd1.Manager.StartUnit`, system bus). The service
   cuts every connection with an nftables table (see
   [security/kill-switch.md](../security/kill-switch.md#panic)). A polkit rule
   lets a local, active member of `sudo` start exactly this unit without a
   password; the shell never asks for one in a panic. If systemd refuses,
   the shell turns NetworkManager networking off instead and says plainly
   that this is not a firewall cut;
2. locks the screen (the screen locker, or logind if that fails);
3. clears the clipboard and the primary selection with `wl-copy --clear`
   (wl-clipboard) and checks with `wl-paste` that nothing is left. This uses
   KWin's data-control protocol, so it works whatever window has the focus.

`/etc/shadow-os/panic.json` can turn off the lock (`"lock_session": false`)
or the clipboard step (`"clear_clipboard": false`).

A full-width banner at the top of the screen shows what is true, from the
panic service's own status file: white on red while the computer is cut
off; black on amber when the panic is not what was asked for (the service
did not start, it reports a problem, or it has not confirmed the cut after
15 seconds). It also says whether the screen was locked and the clipboard
cleared. **Restore network…** runs `pkexec /usr/sbin/shadow-panic release`:
an administrator must authenticate, every time. A release made elsewhere
(the Control Center's **Reconnect…** on its Privacy page, or
`sudo shadow-panic release`) also takes the banner away as soon as the
panic service reports it. Pressing the panic key while a release from the
banner is still running cancels that release if the administrator has not
authenticated yet; if they have, the release (running as root) cannot be
stopped, so the shell starts the panic service again as soon as it has
finished: the panic, pressed last, wins. A panic also ends with the next
restart.

The banner takes the keyboard only when it is clicked. Its buttons are also
at the top of **quick settings** while they are shown: **Meta+A**, **Tab**,
then **Space** or **Return**; the banner says so.

The key does nothing while the screen is locked or while an application
that grabs the keyboard (a virtual machine, a remote desktop) has inhibited
global shortcuts; use `sudo systemctl start shadow-panic.service` from a
terminal then.

## Quick settings

**Meta+A** or a click on the status item. Provided by the status module
(`desktop/horizon-shell/modules/status/README.md`). It shows only what
exists on the machine:

* tiles: Wi-Fi (or a read-only "Network" tile without Wi-Fi hardware), VPN,
  Bluetooth, airplane mode, kill switch, Do Not Disturb, Night Light, dark
  mode;
* sliders: volume with mute, microphone, screen brightness;
* battery state and power profile;
* a media card for the playing media player (play/pause, next, previous);
* a lock button and buttons that open the Control Center.

The **VPN** tile connects the saved VPN or WireGuard profile that was used
most recently (its name is under the tile) and disconnects whatever is up;
the arrow opens Control Center → VPN, where profiles are imported and one is
chosen among several. A profile that needs a password that is not saved
connects from there, not from the tile. With the kill switch in "VPN only"
mode the tile says that the network stays blocked until the VPN is on.

The **calendar** opens from the clock: a month view with week numbers and
keyboard navigation. It shows no events.

Volume, brightness and media keys show a small on-screen display.

**Kill switch.** While the network kill switch is on (see
[security/kill-switch.md](../security/kill-switch.md)), the panel shows a
lock next to the network icon. A warning shield in red means the kill switch
is not what it claims: its firewall table is not loaded, the tool reports an
error, or its status cannot be read; an open lock in amber means the escape
hatch `shadow.killswitch=off` kept it off in this boot. The quick-settings
tile shows the mode (Tor only, VPN only, Tor or VPN) and opens the Control
Center's privacy page: changing the kill switch needs an administrator, so
the tile does not switch it. With the panel set to hide while windows are
open (the default), the indicator is hidden with the panel; the tile and
the panic banner are not affected.

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
  `~/.config/shadow/notifications.conf`.

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

SHADOW applications and the shell share one appearance state:

| Setting | Values |
|---|---|
| Colour scheme | dark (default) or light |
| Accent colour | one of the named accents; default `cyan` |
| Reduce motion | off (default) or on |
| Transparency | on (default) or off |
| Font scale | 0.8–1.6, default 1.0 |

You can switch dark mode from the quick settings. When the colour scheme or
accent changes, the shell runs `shadow-apply-appearance`, which carries the
change over to third-party Qt/KDE and GTK applications, window decorations
and the icon theme. From a terminal:

```sh
shadow-apply-appearance --mode light --accent violet
```

A Control Center page for appearance exists in the source tree but is not
part of the image yet; until then, use the quick settings, the command
above, or edit `appearance.conf` (below).

Translucent "glass" surfaces need GPU compositing. With CPU rendering — a
virtual machine without 3D, or `SHADOW_SOFTWARE_RENDERING=1` — surfaces are
opaque.

Wallpapers shipped in `/usr/share/backgrounds/shadow/`: `shadow-horizon.jpg`
(default, dark), `shadow-daylight.jpg` (default for the light scheme),
`shadow-lattice.jpg`, `shadow-aurora.jpg`.

### How menus and popups open and close

The launcher, the popups under the panel, the session menu and the menus of
the dock and the tray open the same way, each from the place it belongs to:

1. a short line ties the menu to its origin (the panel edge, the icon);
2. the outline of the card is traced from there, both ways round;
3. a scan line crosses the card once, and rows and tiles appear behind it;
4. on the launcher, four brackets close onto the corners, and the outline
   settles from the accent colour most of the way to the ordinary border
   colour; a trace of the accent and the brackets stay while it is open.
   On the other surfaces the outline settles to the border colour.

| Surface | Opens in | Closes in |
|---|---|---|
| Launcher, session menu | 300 ms | 100 ms |
| Quick settings, calendar, notification centre | 220 ms | 100 ms |
| Dock menu, tray menus | 160 ms | 80 ms |
| Authentication dialog | 140 ms (background and brackets only) | at once |
| Notification popup | 220 ms; the others move up in 140 ms when one goes | at once |
| Volume and brightness display, messages of the shell | 140 ms | 140 ms |

Things that hold in every one of them:

* Nothing is moved, resized or re-arranged while a menu opens. Every row
  and button is in its place and can be clicked from the first frame;
  search and password fields have the keyboard from the first frame. Only
  the opacity of things changes, and what is inside a row shifts by a few
  pixels.
* The animation is decoration. It never shows a number, a word or a state
  that the system did not report.
* Closing does not play the opening backwards. The menu stays as it is
  and fades out as a whole, at an even pace; on the launcher, the tile of
  an application that was just started stays lit until the menu is gone.
* Closing is short on purpose. While a menu is still fading out, the
  compositor keeps the keyboard on it, so keys typed in that moment are
  lost: they reach neither the menu (which ignores them) nor the window
  underneath. The pointer already passes through a closing menu. Measured
  in the headless test session (KWin 6.3, software rendering): of twenty
  keys typed at about 10 ms per key straight after Escape, those sent in
  the first 90 to 120 ms were lost and every later one arrived in the
  window below; with Reduce motion none was lost. The authentication
  dialog closes without an exit for the same reason.
* A menu that is opened while another one is closing or open replaces it
  at once: two menus never hold the keyboard together.
* With **Reduce motion** every menu appears complete in one frame and
  disappears in one frame, and nothing pulses (the dock's attention pulse,
  the travelling piece of a progress bar, the attention pulse of a tray
  icon). The window switcher is meant to follow the same switch through
  KWin's animation speed, which `shadow-apply-appearance` sets (see below:
  not checked yet).
* Without a GPU (software rendering) the soft glow around the traced
  outline and the tail of the scan line are left out, and fewer rows are
  staggered; everything else is the same.

Popups inside applications (menus, drop-down lists, dialogs) fade in over
140 ms while a line grows along their top edge, and fade out in 80 ms.

Not seen yet: the glass (blur) look of these surfaces while they open and
KWin's own window effects next to them (both need a GPU), the
authentication dialog (the test session has no system bus), window
previews in the window switcher, and whether the switcher really becomes
instant with Reduce motion: its durations are multiples of Kirigami's
`longDuration`, which KWin scales by `AnimationDurationFactor`; that needs
a running KWin with the Plasma Kirigami units and has not been tried. The
tray menus were looked at with a made-up tray item only.

## Settings files

All under `~/.config/shadow/`. System-wide defaults can be placed under
`/etc/xdg/shadow/` with the same file names. The files are plain INI; the
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
# ~/.config/shadow/horizon.conf
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
