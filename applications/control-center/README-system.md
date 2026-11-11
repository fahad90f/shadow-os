# Control Center — Personalisation and System pages

This file covers six pages of `shadow-control-center`: Appearance,
Accessibility, Startup Applications, Region & Language, Date & Time and
Storage (`pages/{Appearance,Accessibility,Startup,Language,DateTime,Storage}Page.qml`,
`backends/{appearance,accessibility,startup,language,datetime,storage}backend.{h,cpp}`,
`helpers/language-helper`, `helpers/org.shadow.controlcenter.language.policy`).
The other pages are documented by their authors (`README-network-hardware.md`, …).

Open a page with `shadow-control-center <key>`; the data behind it is printed
by `shadow-control-center --dump <key>` (keys: `appearance accessibility
startup language datetime storage`, plus `storage-home`, which runs the walk
of the home folder to its end).

## What each page does and where its data comes from

| Page | Functions | Sources and sinks |
|---|---|---|
| Appearance (`appearance`) | dark / light, accent colour, transparency, reduce motion, text size; wallpaper for the dark and for the light appearance (bundled images, "choose a file", placement: fill, fit, stretch, centre, tile) with a preview of the real image in the real placement; dock auto-hide and icon size; panel clock options | `Appearance` singleton of `org.shadow.ui` (`~/.config/shadow/appearance.conf`); `~/.config/shadow/horizon.conf` — `[Desktop] Wallpaper, WallpaperLight, WallpaperFill`, `[Dock] AutoHide, IconSize`, `[Panel] Clock24h, ClockSeconds, ClockDate` — written with `QSettings` exactly as the shell reads it (the shell watches the file); bundled wallpapers from `/usr/share/backgrounds/shadow` |
| Accessibility (`accessibility`) | text size, reduce motion (same `Appearance` singleton); pointer size; KWin effects: screen magnifier (zoom), magnifying glass, invert colours, colour-blindness correction (with the kind of correction), shake to find the pointer, locate the pointer, show mouse clicks — each with buttons for its shortcuts and the keys bound to them | `org.kde.KWin /Effects` (`org.kde.kwin.Effects`: `listOfEffects`, `loadedEffects`, `areEffectsSupported`, `loadEffect`, `unloadEffect`, `reconfigureEffect`); `org.kde.KWin /Compositor` (`compositingType`); `~/.config/kwinrc [Plugins] <effect>Enabled` and `[Effect-colorblindnesscorrection] Mode` through KConfig; `org.kde.kglobalaccel /component/kwin` (`allShortcutInfos`, `invokeShortcut`); `~/.config/kcminputrc [Mouse] cursorSize` (KConfig with change notification) plus the signal `org.kde.KGlobalSettings.notifyChange(5, 0)` |
| Startup Applications (`startup`) | list of autostart entries with icon, name, description, command; enable / disable; add an installed application (searchable list) or a command; remove; system entries can be switched off per user | `~/.config/autostart/*.desktop`; `<$XDG_CONFIG_DIRS>/autostart/*.desktop` (normally `/etc/xdg/autostart`); installed applications from the XDG `applications` directories |
| Region & Language (`language`) | system language; formats for dates and times, numbers, currency and measurement units (`LC_TIME`, `LC_NUMERIC`, `LC_MONETARY`, `LC_MEASUREMENT`) separately, each with an example; generation of a locale that is not installed yet; one "Apply" for the whole selection | `org.freedesktop.locale1` (`Locale`, `SetLocale`); `/usr/share/i18n/SUPPORTED` and `locale -a`; `/usr/libexec/shadow/language-helper` through `pkexec`; fallback for reading: `/etc/locale.conf`, `/etc/default/locale` |
| Date & Time (`datetime`) | current time and date; time zone (searchable list with region filter, UTC offsets); automatic time on/off; manual date and time while automatic time is off; hardware clock in UTC or local time (display only); panel clock: 24-hour, seconds, date | `org.freedesktop.timedate1` (`Timezone`, `LocalRTC`, `CanNTP`, `NTP`, `NTPSynchronized`; `SetTimezone`, `SetNTP`, `SetTime`, all interactive through polkit); `/usr/share/zoneinfo/zone1970.tab` and `iso3166.tab`; without the service: `/etc/localtime`, `/etc/adjtime`; `[Panel]` keys of `horizon.conf` |
| Storage (`storage`) | mounted file systems with used / total / available; home folder by kind (Documents, Downloads, Pictures, Music, Videos, Desktop, application data, caches, rubbish bin, other) measured in the background with progress and cancel; disks with their partitions, file systems, mount points and usage; SMART health; mount, unmount and "remove safely" for removable drives; link to SHADOW Disk Manager when `shadow-disks` is installed | `statvfs` via `QStorageInfo`, re-read when `/proc/self/mounts` changes; `fts(3)` walk in a worker thread; `org.freedesktop.UDisks2` (`GetManagedObjects`; `Block`, `Partition`, `Filesystem`, `Swapspace`, `Loop`, `Drive`, `Drive.Ata`, `NVMe.Controller`; `Filesystem.Mount/Unmount`, `Drive.Eject/PowerOff`, `Drive.Ata.SmartUpdate`); fallback `lsblk -J -b`; `smartctl --json -H -A` when UDisks2 has no SMART data |

### Details worth knowing

* **Wallpaper slots.** The shell shows `Wallpaper` in dark and `WallpaperLight`
  in light appearance; the chooser edits the slot selected at the top right of
  the section ("Use for light too" copies it). Files chosen from elsewhere are
  remembered in `horizon.conf` as `[ControlCenter] RecentWallpapers` (at most
  eight; a key the shell ignores and preserves) so they stay in the grid.
  Developer override for an uninstalled tree: `SHADOW_WALLPAPER_DIR=/src/assets/wallpapers`.
* **Text size.** The value is stored by `Appearance.fontScale`, but the UI
  library does not use it yet (see "Changes needed outside this directory").
  Both pages detect that at run time — the token `Theme.fontSizeBody` does not
  move when the scale changes — and then say so under the slider instead of
  pretending. The note disappears by itself once the tokens follow.
* **Startup entries.** Disabled = `Hidden=true` plus
  `X-GNOME-Autostart-enabled=false`. Switching a system entry off writes a copy
  of it to `~/.config/autostart/` with those keys and
  `X-SHADOW-Autostart-Override=true`; switching it on again deletes that copy.
  Entries the session will skip are marked with the reason (`TryExec` or the
  program missing, `OnlyShowIn` / `NotShowIn` against `$XDG_CURRENT_DESKTOP`).
  Commands are stored verbatim in `Exec=` (file name `shadow-startup-<name>.desktop`)
  and must start with a program that exists. Desktop files are edited line by
  line, so comments, translations and other groups survive.
* **Effects and the software renderer.** KWin's effects need OpenGL. When
  `compositingType` is `qpainter` (the session script sets `KWIN_COMPOSE=Q`
  when there is no GPU render node) the page says so, offers no effect for
  switching on and hides the shortcut buttons; effects that are already loaded
  can still be switched off. Reason: with the software renderer KWin 6.3.6
  reports zoom, trackmouse and mouseclick as "supported", but zoom draws
  nothing and triggering the trackmouse shortcut crashed the compositor in the
  nested test session.
* **Zoom and magnifying glass** share shortcuts and exclude each other;
  switching one on switches the other off.
* **Locale changes on Debian.** `SetLocale` is tried only where the system
  allows it. Debian's systemd package installs
  `/usr/share/dbus-1/system.d/systemd-localed-read-only.conf`, which denies
  `SetLocale` to everyone, root included (verified: the bus answers "Access
  denied"). Where that file exists, or when the bus rejects the call with its
  policy, the helper writes the locale with `update-locale --reset` instead;
  generation and writing then happen in one helper call (one authentication).
  The page always says that changes apply at the next login, and tells when
  the running session differs from the system setting.
* **Home folder figures** are allocated sizes (`st_blocks × 512`); the walk is
  physical (symbolic links are not followed), stays on the file system of the
  home folder, counts a file with several hard links once, and reports how
  many folders could not be read. Kinds are decided by location: the XDG user
  directories, `~/.cache`, `~/.local/share/Trash`, other dot-entries
  ("application data and settings"), everything else. Nothing is cached; a
  cancelled walk is labelled as partial.
* **Usage bars** use df's formula (used / (used + available to users)),
  the text gives used, total and available bytes.

## Privileged operations

| What | How |
|---|---|
| Time zone, automatic time, clock | `org.freedesktop.timedate1` with `interactive = true`; systemd's own polkit actions (`org.freedesktop.timedate1.set-timezone`, `.set-ntp`, `.set-time`) |
| System locale where systemd-localed may set it | `org.freedesktop.locale1.SetLocale` (`org.freedesktop.locale1.set-locale`) |
| Generate a locale; write the system locale on Debian | `pkexec /usr/libexec/shadow/language-helper [generate <locale>…] [set LANG=… LC_…=…]`, polkit action `org.shadow.controlcenter.language.configure` (`auth_admin`, `auth_admin_keep` for the active session) |
| Mount, unmount, eject, power off | UDisks2 with its own polkit actions |

`language-helper` (Python 3, standard library only) accepts exactly two
sub-commands. `generate` takes up to eight names that must match a strict
pattern **and** be listed with charset UTF-8 in `/usr/share/i18n/SUPPORTED`;
it enables their lines in `/etc/locale.gen` (atomic rewrite) and runs
`locale-gen`. `set` takes assignments whose variable is one of `LANG`,
`LANGUAGE`, `LC_*` and whose value is a locale that `locale -a` reports as
generated; it runs `update-locale --reset` (or writes `/etc/locale.conf`
where that tool does not exist). Everything is validated before anything is
changed; nothing is passed to a shell; the result is one JSON document.

## Run-time dependencies (Debian packages)

`kwin-wayland` (effects, cursor), `libkf6configcore6` (kwinrc, kcminputrc),
`systemd` (`systemd-timedated`, `systemd-localed`), a time synchronisation
service for "automatic time" (`systemd-timesyncd` or `chrony`), `tzdata`,
`locales` (`SUPPORTED`, `locale-gen`, `update-locale`), `libc-bin` (`locale`),
`python3`, `pkexec`, `polkitd` plus a polkit agent in the session, `udisks2`,
`util-linux` (`lsblk`, fallback), `qml6-module-qtquick-dialogs` (file chooser),
Qt image format plug-ins for the wallpapers (`qt6-image-formats-plugins` for
WebP and others). Optional: `smartmontools` (`smartctl`), SHADOW Disk Manager
(`shadow-disks`), a cursor theme with large sizes (`breeze-cursor-theme`).

## Limitations

* Not offered, on purpose: a high-contrast scheme (the tokens in
  `desktop/themes/tokens/shadow-tokens.json` have only dark and light, and
  `Appearance` has no such property); sticky, slow and bounce keys (not
  verified with this KWin, so omitted as the brief demands).
* "Reduce motion" covers SHADOW applications and the shell; KWin's own window
  animations are not changed.
* Pointer size: KWin follows at once. Programs that are already running keep
  their size, and see "Changes needed outside this directory" for the next
  login.
* The time-zone list comes from `zone1970.tab` (313 zones plus UTC); legacy
  alias names are not listed. If the system is set to one, it is shown but
  not highlighted in the list.
* Manual date and time are typed (`yyyy-MM-dd`, `HH:mm[:ss]`); there is no
  calendar widget in ShadowUI.
* The hardware-clock setting (UTC / local time) is displayed, not changed.
* Storage: encrypted volumes are listed but not unlocked here, there is no
  formatting or partitioning (that is Disk Manager's job), unallocated space
  is not shown, and in the `lsblk` fallback there are no actions.
* Format examples are produced with Qt's locale data (CLDR), which can differ
  in detail from glibc's.
* Locales are offered only in UTF-8.

## What was tested, and how

Build: `SHADOW_JOBS=2 scripts/dev.sh build --dir ccsys --only control-center
--cc-pages "appearance;accessibility;startup;language;datetime;storage"` —
no compiler warnings in these files.

**In the build chroot (no services):** every page with
`scripts/dev.sh run --dir ccsys --smoke --shot … shadow-control-center <key>`
in dark and in light appearance (zero QML warnings, screenshots
`ccsys-<key>-1.png`, `ccsys-<key>-light.png`); every `--dump` topic prints
valid JSON; `tests/desktop/smoke.sh --dir ccsys control-center` passes.
Storage listed the VM's virtio disks through the `lsblk` fallback; Date & Time
and Region & Language showed their read-only states. The home-folder walk was
compared with `du -s --block-size=1` on a prepared tree (XDG folders, cache,
rubbish bin, a hard link, a symbolic link to `/usr`): the total and every
folder matched to the byte.

**In the nested session (real KWin 6.3.6, real `horizon-shell`):**

* Appearance: accent, dark/light, wallpaper selection for both slots, dock
  auto-hide, 12-hour clock and seconds — each click changed `appearance.conf`
  / `horizon.conf` and the running shell followed (wallpaper, accent, panel
  clock, dock). Thumbnails and preview are the real images.
* Accessibility: zoom switched off and on through the page (KWin's
  `loadedEffects` and `kwinrc [Plugins] zoomEnabled` both changed); "Zoom in"
  / "Actual size" buttons made KWin report zoom as active / inactive;
  shortcut keys read from kglobalaccel. Pointer size 24 → 48 → 96 changed the
  cursor image KWin sets (measured with XFixes) once `cursorTheme` named an
  installed theme. The nested session composites with QPainter, so nothing
  could be *seen* zooming, and the OpenGL-only effects were reported as
  unsupported by KWin itself — see "Effects and the software renderer".
* Appearance, continued: placement "Stretch" and dock icon size 64 px were
  written and the dock grew.
* Startup: add a command, add an application (search, pick), disable/enable
  a user entry, remove one (with the confirmation), switch a system entry
  (from an `XDG_CONFIG_DIRS` test directory) off and on again, condition
  badges — checked against the files written.
* A second `shadow-control-center <key>` switched the running window to that
  page.

**Against the real systemd services on a private system bus** (one
`scripts/dev.sh exec` in a private mount namespace with throw-away overlays
over `/etc`, `/usr/lib/locale`, `/usr/libexec`, `/usr/share/polkit-1`; the
shared chroot was verified untouched afterwards):

* `systemd-timedated`: properties read; `SetTimezone` through the page
  (search "Abidjan", click) changed `/etc/localtime` in the overlay and the
  page followed.
* `systemd-localed`: `Locale` read; `SetLocale` found to be denied by Debian's
  bus policy; the helper path (real `pkexec` + `polkitd` + the shipped policy
  file, as root) generated `de_DE.UTF-8` and `en_GB.UTF-8` and wrote the
  locale; the page then showed the new system setting.
* `language-helper`: rejects unknown locales, shell metacharacters, path
  tricks, non-UTF-8 names and unknown sub-commands.
* `udisksd` 2.10.1 on the same private bus: the page used the service
  (`source: udisks`), but without a udev database the daemon exposes no block
  devices in the chroot, so only the empty answer was exercised.

The scaffolding for these runs is in `build/work/shots/ccsys-test/`
(`sysbus.sh` and the phase scripts); it is not part of the product.

## Not tested

* `SetTime` (deliberately: it would have changed the clock of the shared
  build machine) and `SetNTP` (needs systemd as PID 1 and a time service).
* polkit prompts for an ordinary user: every privileged call ran as root in
  the chroot, where polkit authorises without asking.
* `SetLocale` succeeding on a distribution that allows it.
* UDisks2 with real devices: the mapping of drives, partitions, encrypted
  containers and logical volumes to the list; mount, unmount, eject, power
  off; SMART from `Drive.Ata` / `NVMe.Controller`; `smartctl` on real
  hardware (in the VM it reports nothing for virtio disks).
* Cancelling the home-folder walk: the test trees were measured faster than a
  click could be sent.
* KWin effects with OpenGL compositing (visible zoom, magnifying glass,
  invert, colour-blindness correction, shake cursor) and the trackmouse /
  mouseclick shortcuts.
* The wallpaper file chooser (QtQuick.Dialogs) was not driven in the nested
  session; `setWallpaper()` with a path outside the bundled directory is the
  same code path as a thumbnail click.
* Whether the session really starts XDG autostart entries is the session's
  business (`shadow-horizon-session.target`), not verified here.

## Changes needed outside this directory

1. **`desktop/ui` — text size has no effect.** `Appearance.fontScale` is
   stored and broadcast, but nothing consumes it: the generated
   `Theme.fontSize*` tokens are fixed integers and `shadowapp.cpp` sets a fixed
   application font. The token generator should emit
   `Math.round(<size> * Appearance.fontScale)` for the `fontSize*` tokens (and
   ideally `controlHeight`, `rowHeight*`), and `Shadow::App` should scale the
   application font and follow `fontScaleChanged`.
2. **`desktop/horizon-shell/session/shadow-horizon-session` — pointer size is
   lost at login.** The script exports `XCURSOR_SIZE="${XCURSOR_SIZE:-24}"`
   before KWin starts, and KWin prefers the environment over `kcminputrc`. It
   should read the saved value first, e.g.
   `size="$(kreadconfig6 --file kcminputrc --group Mouse --key cursorSize)"`
   and export `XCURSOR_SIZE="${size:-24}"` (same for `cursorTheme`).
3. **`applications/control-center/smoke-tests.list`** (shared file, not
   edited): add `shadow-control-center appearance`, `… accessibility`,
   `… startup`, `… language`, `… datetime`, `… storage`.
4. **Packaging** — `/usr/libexec/shadow/language-helper` and the policy are
   installed by the existing CMake globs; the package must depend on
   `python3`, `pkexec`, `locales`. If the distribution prefers systemd-localed
   to be writable instead, it would have to drop Debian's
   `systemd-localed-read-only.conf`; the page works either way.
5. **KWin with the software renderer** (`KWIN_COMPOSE=Q`): no screen
   magnifier is available on machines without a GPU driver. That is a
   compositor limitation; mentioned here because it is an accessibility gap.
