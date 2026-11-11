# Shell module: status

Status indicators in the panel, the quick settings popup, the on-screen
display for the volume / brightness / media keys, and the calendar popup of
the NEXUS HORIZON shell. Compiled into `horizon-shell`; removing this
directory removes these features and nothing else.

| Part | File | Registered as |
|---|---|---|
| Panel item (order 30): network, VPN, volume, battery; opens the quick settings | `qml/StatusPanelItem.qml` | panel item |
| Quick settings popup (Meta+A) | `qml/StatusQuickSettings.qml` (+ `StatusTile`, `StatusSliderRow`, `StatusSegmented`, `StatusMediaCard`) | overlay `quick-settings` |
| Calendar popup (the panel clock becomes a button) | `qml/StatusCalendar.qml` | overlay `calendar` |
| On-screen display, handles `shell.unhandledAction` for `volume-up/-down/-mute`, `mic-mute`, `brightness-up/-down`, `media-play/-next/-previous` | `qml/StatusOsd.qml` | surface (overlay layer, no keyboard, no pointer input, sized to its content) |
| Level glyphs (Wi-Fi arcs, battery charge) drawn on the icon set's grid | `qml/StatusGlyph.qml` | — |

Context property: **`status`** (`statusservice.h`), with one object per
back-end: `status.audio`, `.network`, `.bluetooth`, `.radio`, `.battery`,
`.powerProfiles`, `.brightness`, `.media`, `.nightLight`, `.calendar`. Every
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
| `audio` (`statusaudio.*`) | PulseAudio protocol through KF6 PulseAudioQt (pipewire-pulse on NEXUS): default sink and default source. PipeWire's `auto_null` placeholder is not a device; a monitor source is not a microphone. Reconnects when the sound server appears later. | sink/source volume (never above 100 %) and mute |
| `network` (`statusnetwork.*`) | NetworkManager, system bus: manager, devices, active connections, access point in use; `org.freedesktop.DBus.Properties` + `PropertiesChanged` | `WirelessEnabled` (polkit decides) |
| `bluetooth` (`statusbluetooth.*`) | BlueZ through KF6 BluezQt: adapters, connected devices, rfkill block | adapter `Powered`; lifts a soft rfkill block first |
| `radio` (`statusradio.*`) | `/dev/rfkill` events. Airplane mode = every radio blocked. Available only with at least one radio and write access (logind `uaccess`). | one `RFKILL_OP_CHANGE_ALL` event |
| `battery` (`statuspower.*`) | UPower display device + `OnBattery` | — |
| `powerProfiles` (`statuspower.*`) | power-profiles-daemon as `org.freedesktop.UPower.PowerProfiles`, falling back to `net.hadess.PowerProfiles` | `ActiveProfile` |
| `brightness` (`statusbrightness.*`) | `/sys/class/backlight/*` (firmware > platform > raw), file watcher | `org.freedesktop.login1.Session.SetBrightness` on session `auto` |
| `media` (`statusmedia.*`) | MPRIS2: every `org.mpris.MediaPlayer2.*` name on the session bus; the playing (most recently started) player wins. Cover art only from local files. | `PlayPause`/`Stop`, `Next`, `Previous` |
| `nightLight` (`statusnightlight.*`) | KWin `/org/kde/KWin/NightLight` (`org.kde.KWin.NightLight`) | `kwinrc [NightColor] Active` through KConfig with change notification |
| `calendar` (`statuscalendar.*`) | `QDate` / `QLocale` / `QTimeZone`: locale's first day of the week and names, ISO 8601 week numbers | — |

What the UI leaves out rather than showing dead: the Bluetooth tile without
an adapter or BlueZ, airplane mode without a writable `/dev/rfkill`, Do Not
Disturb without the `notifications` context property, Night Light unless
KWin reports it, the microphone and brightness sliders, the battery row
without a battery, the power-profile row without the daemon, the media card
without a player, the lock button unless `session.canLock`, and the
Control Center buttons unless `nexus-control-center` is installed. Without
Wi-Fi hardware (or without NetworkManager) the Wi-Fi tile is replaced by a
read-only "Network" tile that says what is known and why it cannot switch.

## Run-time dependencies (Debian packages)

`libkf6pulseaudioqt5`, `libkf6bluezqt6`, `libkf6configcore6` (linked);
`pipewire-pulse` + `wireplumber` (or `pulseaudio`), `network-manager`,
`bluez`, `upower`, `power-profiles-daemon`, `systemd` (logind),
`kwin-wayland` — each optional at run time: without it the corresponding
part reports "unavailable" and is hidden or explained. Build:
`libkf6pulseaudioqt-dev`, `libkf6bluezqt-dev`, `libkf6config-dev`.
`nexus-control-center` is opened for the Wi-Fi, network and Bluetooth pages
(`nexus-control-center wifi|network|bluetooth`) and by the footer button.

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

```
NEXUS_JOBS=2 scripts/dev.sh build --dir modstatus --only horizon-shell --modules status
tests/desktop/smoke.sh --dir modstatus horizon-shell
scripts/dev.sh run --dir modstatus horizon-shell --dump status
scripts/dev.sh exec --dir modstatus -- cmake -S /src/desktop/horizon-shell/modules/status/tests -B /build/modstatus-tests -G Ninja
scripts/dev.sh exec --dir modstatus -- ninja -C /build/modstatus-tests
NEXUS_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- '. /src/desktop/horizon-shell/modules/status/tests/session-test.sh'
NEXUS_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- '. /src/desktop/horizon-shell/modules/status/tests/session-test-fixture.sh'
scripts/dev.sh exec --dir modstatus -- bash -c 'LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 TZ=America/New_York QT_QPA_PLATFORM=offscreen horizon-shell --dump status | python3 /src/desktop/horizon-shell/modules/status/tests/check-calendar.py 7'
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
