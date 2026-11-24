# Shell module: status

Status indicators in the panel, the quick settings popup, the on-screen
display for the volume / brightness / media keys, the calendar popup, and
the network kill switch and panic state (panel indicator, quick-settings
tile, panic banner) of the Shadow Horizon shell. Compiled into `horizon-shell`; removing this
directory removes these features and nothing else.

| Part | File | Registered as |
|---|---|---|
| Panel item (order 30): network, VPN, kill switch, volume, battery; opens the quick settings | `qml/StatusPanelItem.qml` | panel item |
| Quick settings popup (Meta+A) | `qml/StatusQuickSettings.qml` (+ `StatusTile`, `StatusSliderRow`, `StatusSegmented`, `StatusMediaCard`) | overlay `quick-settings` |
| Calendar popup (the panel clock becomes a button) | `qml/StatusCalendar.qml` | overlay `calendar` |
| On-screen display, handles `shell.unhandledAction` for `volume-up/-down/-mute`, `mic-mute`, `brightness-up/-down`, `media-play/-next/-previous` | `qml/StatusOsd.qml` | surface (overlay layer, no keyboard, no pointer input, sized to its content) |
| Level glyphs (Wi-Fi arcs, battery charge) drawn on the icon set's grid | `qml/StatusGlyph.qml` | — |
| Panic banner: full-width, high-contrast strip while a panic is engaged or failed, with "Restore network…" (also in quick settings, for the keyboard) | `qml/StatusPanicBanner.qml` | surface (overlay layer, exclusive zone, keyboard on demand) |

Context property: **`status`** (`statusservice.h`), with one object per
back-end: `status.audio`, `.network`, `.bluetooth`, `.radio`, `.battery`,
`.powerProfiles`, `.brightness`, `.media`, `.nightLight`, `.calendar`,
`.killSwitch`. Every
back-end has `ready`, `available` and `unavailableReason`; a failed request
is reported through `status.failed(message)` and shown with `shell.notify()`.
The module owns no D-Bus name.

Diagnostics: **`horizon-shell --dump status`** (no compositor needed) prints
every back-end's availability, the reason when unavailable, and its values;
it waits until each back-end has had its answer (at most 8 s).

## Back-ends and data sources

All asynchronous; nothing blocks the shell's start, nothing is polled.

| Back-end | Source | Writes |
|---|---|---|
| `audio` (`statusaudio.*`) | PulseAudio protocol through KF6 PulseAudioQt (pipewire-pulse on SHADOW): default sink and default source. PipeWire's `auto_null` placeholder is not a device; a monitor source is not a microphone. Reconnects when the sound server appears later. | sink/source volume (never above 100 %) and mute |
| `network` (`statusnetwork.*`) | NetworkManager, system bus: manager, devices, active connections, access point in use; `org.freedesktop.DBus.Properties` + `PropertiesChanged`. The saved VPN and WireGuard profiles: `Settings.ListConnections` and the `connection` group of each one's `GetSettings` (no secrets), read again when one is added or removed | `WirelessEnabled` (polkit decides); `ActivateConnection` / `DeactivateConnection` for a VPN profile (polkit: `network-control`) |
| `bluetooth` (`statusbluetooth.*`) | BlueZ through KF6 BluezQt: adapters, connected devices, rfkill block | adapter `Powered`; lifts a soft rfkill block first |
| `radio` (`statusradio.*`) | `/dev/rfkill` events. Airplane mode = every radio blocked. Available only with at least one radio and write access (logind `uaccess`). | one `RFKILL_OP_CHANGE_ALL` event |
| `battery` (`statuspower.*`) | UPower display device + `OnBattery` | — |
| `powerProfiles` (`statuspower.*`) | power-profiles-daemon as `org.freedesktop.UPower.PowerProfiles`, falling back to `net.hadess.PowerProfiles` | `ActiveProfile` |
| `brightness` (`statusbrightness.*`) | `/sys/class/backlight/*` (firmware > platform > raw), file watcher | `org.freedesktop.login1.Session.SetBrightness` on session `auto` |
| `media` (`statusmedia.*`) | MPRIS2: every `org.mpris.MediaPlayer2.*` name on the session bus; the playing (most recently started) player wins. Cover art only from local files. | `PlayPause`/`Stop`, `Next`, `Previous` |
| `nightLight` (`statusnightlight.*`) | KWin `/org/kde/KWin/NightLight` (`org.kde.KWin.NightLight`) | `kwinrc [NightColor] Active` through KConfig with change notification |
| `calendar` (`statuscalendar.*`) | `QDate` / `QLocale` / `QTimeZone`: locale's first day of the week and names, ISO 8601 week numbers | — |
| `killSwitch` (`statuskillswitch.*`) | `/run/shadow/killswitch.json` (shadow-killswitch) and `/run/shadow/panic.json` (shadow-panic), both root-written, 0644. `QFileSystemWatcher` on the directory (creation, atomic replacement, removal) and on the files (writes in place); while `/run/shadow` does not exist its nearest parent is watched. Changes coalesced for 50 ms; a 5 s poll only if watching fails. `SHADOW_STATUS_RUN_DIR` overrides the directory (tests). | — (read only) |

What the UI leaves out rather than showing dead: the Bluetooth tile without
an adapter or BlueZ, airplane mode without a writable `/dev/rfkill`, Do Not
Disturb without the `notifications` context property, Night Light unless
KWin reports it, the microphone and brightness sliders, the battery row
without a battery, the power-profile row without the daemon, the media card
without a player, the lock button unless `session.canLock`, and the
Control Center buttons unless `shadow-control-center` is installed. Without
Wi-Fi hardware (or without NetworkManager) the Wi-Fi tile is replaced by a
read-only "Network" tile that says what is known and why it cannot switch.

## Kill switch and panic

The formats are those of [docs/security/kill-switch.md](../../../../docs/security/kill-switch.md).
The parser (`NetGuard::parseKillSwitch` / `parsePanic`) keeps its rule: only
*active, loaded and no error* means the kill switch protects anything.

| `status.killSwitch.state` | When | Panel | Quick-settings tile |
|---|---|---|---|
| `unknown` | no `killswitch.json` (tool not installed, or not run in this boot) | nothing | hidden |
| `off` | mode `off`, table not loaded, no error | nothing | "Off" |
| `on` | mode `tor`, `vpn` or `tor-or-vpn`, loaded, no error | lock glyph | lock, active, "Tor only" / "VPN only" / "Tor or VPN" (+ local network) |
| `bypassed` | off in this boot because of `shadow.killswitch=off`, another mode saved | open lock, warning colour | "Off for this boot", warning (icon circle and border; the text stays in a text colour) |
| `error` | `error` set; or a mode other than off whose table is not loaded; or off with the table still loaded; or a file that cannot be read, is not JSON, names no or an unknown mode, has a field of the wrong type or contradicts itself (`mode` vs `active`) | warning shield, danger colour | "Not in force" (a mode on whose table is not loaded) / "Problem" (anything else, for example the tool's error with the table loaded), danger (icon circle and border), and the full message under the tiles |

A tile's tone colours its icon circle and border, not its caption-size
subtitle: the warning and danger colours as text are below WCAG AA (4.5:1)
on the tile (light theme warning 3.2:1, danger 4.2:1; dark theme danger
3.7:1 while the tile is on, just under 4.5:1 hovered), so the subtitle stays in a text
colour (semibold when toned). `tests/tst_status_tile_contrast.qml` checks
title and subtitle in both themes, every tone, on and off, every accent,
with and without hover.

The tile never switches anything: changing the kill switch needs an
administrator, so a click opens the Control Center's privacy page
(`shadow-control-center privacy`). Without the Control Center the tile only
shows the state and says how to change it from a terminal.

`panicState`: `none` (no file, or never engaged), `engaged` (cut: engaged,
loaded, no error), `released`, or `error` (an error, engaged without its
table, released with the table still loaded, or an unreadable file).

The panic **action** is not in this module: the global shortcut "panic"
(**Ctrl+Alt+Shift+Escape**) is handled by the shell core (`src/panic.h`,
context property `panic`): it starts `shadow-panic.service` over D-Bus,
locks the screen and clears the clipboard. The banner here shows both
sides: what the core asked for and was answered (service start, NetworkManager
fallback, screen lock, clipboard), and what `panic.json` says is true. Its
colours do not follow the theme: white on deep red while the computer is cut
off, near-black on amber whenever the panic is NOT what was asked for (the
service did not start, the status reports an error, or no confirmation within
15 s). "Restore network…" runs `pkexec /usr/sbin/shadow-panic release`
(administrator, every time); if only the NetworkManager fallback was used it
turns NetworkManager networking back on instead. "Dismiss" appears only when
nothing was cut.

A release made elsewhere (the Control Center's "Reconnect…", `sudo
shadow-panic release`) ends the panic as well: the banner passes `panic.json`'s
state on to the core (`panic.setServiceState()`), which forgets its request
once the service, having answered it with "engaged" (or a problem), publishes
"released". The banner then goes at once, without the "has not confirmed the
cut" alarm 15 s later. An old "released" record from before the request does
not end it. A panic pressed while the banner's own release still runs wins:
see `src/panic.h` (a release still waiting for the administrator is killed;
one already authorised runs as root, so the shell leaves it to end, never
waits for it, and starts the panic service again once it has).

Keyboard: the banner takes the keyboard only when it is clicked (a layer
surface with on-demand keyboard). Its buttons are therefore also at the top
of quick settings while they are shown: **Meta+A**, **Tab**, then **Space**
or **Return**. The banner says so in a line of its own and in its
accessible description (with the key actually bound to quick settings).

Limits: with `[Panel] Hide=smart` (the default) the panel, and with it the
kill switch indicator, is hidden while a window is open; the banner (overlay
layer) is not. The lock screen covers the banner. The shell never learns
of a kill switch or panic change that the tools do not publish to these
files.

## Run-time dependencies (Debian packages)

`libkf6pulseaudioqt5`, `libkf6bluezqt6`, `libkf6configcore6` (linked);
`pipewire-pulse` + `wireplumber` (or `pulseaudio`), `network-manager`,
`bluez`, `upower`, `power-profiles-daemon`, `systemd` (logind),
`kwin-wayland` — each optional at run time: without it the corresponding
part reports "unavailable" and is hidden or explained. The kill switch
parts need `shadow-hardening` (shadow-killswitch, shadow-panic) to have
anything to show. Build:
`libkf6pulseaudioqt-dev`, `libkf6bluezqt-dev`, `libkf6config-dev`.
`shadow-control-center` is opened for the Wi-Fi, network and Bluetooth pages
(`shadow-control-center wifi|network|bluetooth`) and by the footer button.

## Tests

`tests/` is not part of the product build.

* `tests/CMakeLists.txt` builds two helpers into `/build/modstatus-tests/bin`:
  `status-test-mpris-player` (a minimal MPRIS2 player that logs the commands
  it receives) and `status-test-dbus-fixture` (answers
  `org.freedesktop.DBus.Properties` for the objects of a JSON file on a
  private bus; refuses to run without `DBUS_SYSTEM_BUS_ADDRESS`).
* `tests/session-test.sh` — nested session with a real PipeWire
  (pipewire, wireplumber, pipewire-pulse, a null sink and a virtual source)
  and the MPRIS test player.
* `tests/session-test-fixture.sh` — nested session whose "system bus" is a
  private `dbus-daemon` with `tests/fixture-laptop.json` standing in for
  NetworkManager, UPower and power-profiles-daemon.
* `tests/check-calendar.py` — compares the calendar part of the dump with
  Python's date arithmetic.
* `tests/killswitch-selftest.cpp` (`status-test-killswitch`, Qt Core only,
  any user) — the kill switch / panic parser with sound, missing, empty,
  malformed, oversized, contradicting and wrongly typed documents, every
  mode, the error and not-loaded cases, the escape hatch; and the watcher on
  a real temporary directory: created late, files replaced atomically and
  written in place, broken and repaired, removed, the directory removed and
  re-created.
* `tests/vpn-selftest.sh` + `tests/vpn-selftest.cpp` (`status-test-vpn`) — the
  VPN part of the network back-end against the fixture on a private bus: only
  VPN and WireGuard connections are profiles, the most recently used first;
  connecting the most recent and a named one; the state while one is up;
  disconnecting; NetworkManager's refusal reported with the profile's name;
  and what the fixture was asked to do. No compositor needed.
* `tests/session-test-killswitch.sh` + `tests/fake-panic-services.py` —
  nested session with a real KWin, horizon-shell and wl-clipboard; systemd,
  NetworkManager, logind and the screen locker are stand-ins on a private
  "system" bus, `pkexec` a stand-in on the shell's PATH, and the state files
  are written by the script into `SHADOW_STATUS_RUN_DIR`. Whether the panic
  banner is on the screen is read from the screenshots (`tests/png-pixel.py`:
  the banner's fixed red or amber at the top edge).
* `tests/tst_status_tile_contrast.qml` — `qmltestrunner` test of the tile's
  text contrast (see the tile table above); needs the built `org.shadow.ui`.
* The panic core itself (`src/panic.h`) has its own test without a
  compositor: `desktop/horizon-shell/tests/panic/` (`run.sh` as root).

```
SHADOW_JOBS=2 scripts/dev.sh build --dir modstatus --only horizon-shell --modules status
tests/desktop/smoke.sh --dir modstatus horizon-shell
scripts/dev.sh run --dir modstatus horizon-shell --dump status
scripts/dev.sh exec --dir modstatus -- cmake -S /src/desktop/horizon-shell/modules/status/tests -B /build/modstatus-tests -G Ninja
scripts/dev.sh exec --dir modstatus -- ninja -C /build/modstatus-tests
SHADOW_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- '. /src/desktop/horizon-shell/modules/status/tests/session-test.sh'
SHADOW_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- '. /src/desktop/horizon-shell/modules/status/tests/session-test-fixture.sh'
scripts/dev.sh exec --dir modstatus -- /build/modstatus-tests/bin/status-test-killswitch
SHADOW_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- '. /src/desktop/horizon-shell/modules/status/tests/session-test-killswitch.sh'
# (dev.sh nested's "shot" takes no crop: a cropped shot of the kill switch
# session test becomes a full-screen shot. The test was also run in a
# standalone Qt 6.8 session of KWin and horizon-shell, where crops are kept.)
scripts/dev.sh exec --dir modstatus -- bash -c 'LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 TZ=America/New_York QT_QPA_PLATFORM=offscreen horizon-shell --dump status | python3 /src/desktop/horizon-shell/modules/status/tests/check-calendar.py 7'
# The tile contrast test: qmltestrunner with the build directory's org.shadow.ui
# (dev.sh exec puts its qml/ on QML_IMPORT_PATH and lib/ on LD_LIBRARY_PATH)
scripts/dev.sh exec --dir modstatus -- bash -c 'HOME=$(mktemp -d) QT_QPA_PLATFORM=offscreen /usr/lib/qt6/bin/qmltestrunner -input /src/desktop/horizon-shell/modules/status/tests/tst_status_tile_contrast.qml'
```

Results land in `build/work/shots/nested-logs/modstatus/` (`session-test.log`,
`session-test-fixture.log`, `fixture.log`, `dump-status*.json`) and
`build/work/shots/modstatus-*.png`. The click coordinates in the scripts
assume the default 1600×900 nested screen.

### Tested (build chroot, 2026-11-11)

* Smoke test of the shell with this module: passes, no QML warnings.
* `--dump status` without any service: every back-end reports unavailable
  with its reason, within about 2 s.
* **Against a real PipeWire** (`session-test.sh`, 26 checks pass): dump shows
  the default sink/source and volume; XF86AudioRaiseVolume / LowerVolume /
  Mute / MicMute change the real sink and source (verified with `pactl`) and
  show the OSD; raising the volume unmutes; the popup's volume slider follows
  a change made with `pactl`, a click on it sets the volume, the mute button
  mutes; Meta+A and a click on the panel item open the popup; the dark-mode
  tile switches `appearance.conf`; brightness key without a backlight and
  media key without a player show an honest OSD.
* **Against the MPRIS test player** (same script): player found, media card
  shown, its pause button and the XF86AudioPlay / Next / Prev keys reach the
  player (its log) and the new state comes back.
* **Against the D-Bus fixture, not the real services**
  (`session-test-fixture.sh`, 31 checks pass): NetworkManager object
  tracking (devices, active connections, access point, SSID bytes), live
  updates through `PropertiesChanged` (signal strength, a WireGuard
  connection as VPN, captive portal, Wi-Fi off, wired, service gone), UPower
  (discharging, critical, charging, service gone), power profiles (list,
  choosing one writes `ActiveProfile`), the Wi-Fi tile writing
  `WirelessEnabled`; BluezQt on a bus without BlueZ; a fabricated backlight
  directory is read (and reported as not settable without logind).
* Calendar: popup opened from the clock, month navigation with the keyboard;
  `check-calendar.py` passes for en_US (Sunday first), de_DE, en_GB, ar_EG
  (Saturday first) and four time zones; the December 2026 grid with week 53 was
  checked by eye in the popup (`modstatus-14-calendar-next-month.png`).

### Not tested

* **NetworkManager, UPower, power-profiles-daemon: only against the
  fixture.** polkit prompts for `WirelessEnabled`, real timing and real
  hardware are unverified.
* **BlueZ with an adapter** (power on/off, rfkill unblock, connected
  devices), **airplane mode** (`/dev/rfkill` does not exist in the chroot),
  **brightness writes through logind** and the backlight file watcher on a
  real sysfs, the `net.hadess.PowerProfiles` fallback: not exercised at all.
* **Night Light**: the nested KWin has no `/org/kde/KWin/NightLight` object,
  so the tile never appeared; reading the state and switching through
  `kwinrc` are untested.
* **Do Not Disturb tile**: this build contained only the status module, so
  the `notifications` property did not exist and the tile was never shown.
* The lock button (`session.canLock` is false in the nested session) and the
  footer's Control Center button beyond "it starts the program".
* Cover art in the media card; several players at once; right-to-left
  locales (the grid is correct, the layout is not mirrored); HiDPI.

All of these need the VM image: compare `horizon-shell --dump status` there
with `nmcli`, `upower -d`, `powerprofilesctl`, `bluetoothctl show`,
`brightnessctl`/sysfs and `pactl`.

### Tested: kill switch and panic (osf lab, trixie chroot, KWin 6.3.6, 2026-11-23)

* `status-test-killswitch`: 149 checks pass (as uid 1000).
* `session-test-killswitch.sh`: 34 checks pass, no QML warnings; screenshots
  looked at one by one: the lock glyph (on), the warning shield (error, not
  loaded, unreadable file), nothing (off, no file), the tile in each state in
  the dark and the light theme (also with the software scene graph), the full
  error text under the tiles, and the banner in every state: cut; release
  cancelled by the administrator; release failed (the tool's message); the
  NetworkManager fallback; the fallback failed too (and the screen lock
  refused by both lockers); dismissed; not confirmed after 15 s; recorded
  but its table not loaded; panic with `lock_session`/`clear_clipboard`
  false.
* The real key: `xdotool key ctrl+alt+shift+Escape` into the nested KWin
  reaches the shell through kglobalaccel; `StartUnit("shadow-panic.service",
  "replace")` arrives without the allow-interactive-authorisation flag; the
  screen locker's `Lock` is called (logind's `Session.Lock` when it refuses);
  text put on the clipboard and the primary selection with wl-copy is gone
  afterwards ("Nothing is copied"). The registry (`kglobalshortcutsrc`) shows
  `panic=Ctrl+Alt+Shift+Esc` and no other holder.
* Clearing through data-control also works in a second KWin started
  **without** `KWIN_WAYLAND_NO_PERMISSION_CHECKS`: KWin 6.3.6 offers
  `zwlr_data_control_manager_v1` (version 2) to an ordinary client without a
  desktop file, `set_selection(nil)` empties the clipboard.
* The tile starts `shadow-control-center privacy`, which opens that page.

Round-1 fixes (osf lab, 2026-11-24), each test run against the build before
the fix (fails) and after it (passes):

* `session-test-killswitch.sh`, now 42 checks (no QML warnings): the banner
  is gone after its own release; after a release made elsewhere (the file
  `shadow-panic release` publishes, written while the shell's panic is
  engaged) it is gone at once and still gone 17 s later (before the fix: red
  "cutting…", then amber "has not confirmed the cut" with Dismiss); and the
  keyboard route: Meta+A, Tab, Space in quick settings runs `pkexec
  shadow-panic release` and the banner goes (before the fix the first Tab
  reached a tile and nothing was released).
* `tests/tst_status_tile_contrast.qml`: 12 cases pass (before the fix 6
  failed: light warning 3.21:1 and 2.93:1, light danger 4.19:1 and 3.82:1,
  dark danger just under 4.5:1 hovered and 3.69:1 while on).
* `desktop/horizon-shell/tests/panic` (core, no compositor): 27 checks pass
  (before the fix 7 failed).

### Not tested: kill switch and panic

* Against the real services: systemd starting `shadow-panic.service` with
  the polkit rule (no password), the real NetworkManager `Enable`, logind's
  `Session.Lock` making KWin's screen locker lock, and the real screen locker
  answering `org.freedesktop.ScreenSaver.Lock` (the lab's KWin runs with
  `--no-lockscreen`; a stand-in answered).
* `pkexec /usr/sbin/shadow-panic release` with a real polkit dialog (a
  stand-in `pkexec` played allow, cancel and fail).
* The shortcut while the screen is locked, or while an application inhibits
  global shortcuts (a virtual machine viewer, a remote desktop): KWin does
  not deliver global shortcuts then, so the panic key does nothing.
* The files written by the real shadow-killswitch and shadow-panic: the test
  files follow their `build_status()` field for field, but no tool ran.

## Limitations

* One output and one input device (the defaults); choosing devices is the
  Control Center's job. Volume is capped at 100 %.
* Only the built-in backlight; no DDC/CI for external monitors.
* The panel item has no tooltip: the panel surface is only as high as the
  panel, so a tooltip would cover the panel. The summary is exposed as the
  accessible description.
* Wi-Fi shows the first device that has an access point; no network list
  (that is the Control Center's Wi-Fi page).
* No calendar events; days are not interactive.
