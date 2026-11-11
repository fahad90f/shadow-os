# Control Center — Network and Hardware pages

This file covers nine pages of `shadow-control-center`: Wi-Fi, Wired & Proxy,
Bluetooth, VPN, Displays, Sound, Power & Battery, Keyboard, Mouse & Touchpad
(`pages/{Wifi,Network,Bluetooth,Vpn,Display,Sound,Power,Keyboard,Mouse}Page.qml`,
`backends/{wifi,network,networkcommon,networkmanagerutil,bluetooth,vpn,display,sound,power,keyboard,mouse}*`).
The other pages of the Control Center are documented by their own authors;
the directory is shared, which is why this is not `README.md`.

Open a page directly with `shadow-control-center <key>`; the data behind it is
printed by `shadow-control-center --dump <key>` (keys: `wifi network bluetooth
vpn display sound power keyboard mouse`).

## What each page does and where its data comes from

| Page | Functions | Sources and sinks |
|---|---|---|
| Wi-Fi (`wifi`) | radio on/off, scan, list (signal, security, band), connect with password dialog, hidden network, disconnect, forget, auto-connect, MAC address policy, connection details | `nmcli -t` for reading and for changes without a secret; `org.freedesktop.NetworkManager` on the system bus for `AddAndActivateConnection` (the password travels in the D-Bus message, never on a command line), `Device.StateChanged`, `Settings.Connection.Delete`, change signals; `/sys/class/rfkill` |
| Wired & Proxy (`network`) | wired interfaces: state, speed, addresses, connect/disconnect, IPv4/IPv6 method, addresses, gateway, DNS; proxy: none / manual (HTTP, HTTPS, SOCKS, exceptions) / automatic (PAC URL) | `nmcli -t device show`, `connection show/modify/up`; proxy written to `~/.config/environment.d/90-shadow-proxy.conf` (read by `systemd --user` at login) and, for none/automatic, to `proxy.method` / `proxy.pac-url` of the active NetworkManager profiles |
| Bluetooth (`bluetooth`) | power, discoverability, scan, pair (PIN / passkey / confirmation prompts through an own agent), connect, disconnect, trust, remove, battery level | KF6 BluezQt (`org.bluez` on the system bus), `/sys/class/rfkill` |
| VPN (`vpn`) | list, connect, disconnect, delete, connect automatically, import WireGuard `.conf` and OpenVPN `.ovpn` | `nmcli connection show/up/down/delete/modify/import`; plug-ins found in `/usr/lib/NetworkManager/VPN/*.name` and `/etc/NetworkManager/VPN/*.name` |
| Displays (`display`) | per output: on/off, resolution, refresh rate, scale, orientation, primary, VRR and HDR when the output has them; arrangement by dragging; apply with "Keep these settings?" and automatic revert after 15 s; Night Light | `kscreen-doctor -j` to read, one `kscreen-doctor output.<name>.…` call to apply; `~/.config/kwinrc [NightColor]` and `org.kde.KWin /org/kde/KWin/NightLight` |
| Sound (`sound`) | output and input devices, volume, mute, default device, ports, card profiles, per-application streams, input level meter, test tone (left, then right), volumes above 100 % | KF6 PulseAudioQt (PipeWire's PulseAudio service or PulseAudio); test tone played with `paplay` (fallback `pw-play`), level meter with `parec` |
| Power & Battery (`power`) | batteries and peripherals, power source, power profile, panel brightness, idle timeouts, sleep support and lid behaviour (display only) | `org.freedesktop.UPower`; `org.freedesktop.UPower.PowerProfiles` (or `net.hadess.PowerProfiles`); `org.freedesktop.login1` (`CanSuspend`, `CanHibernate`, lid settings, `Session.SetBrightness`); `/sys/class/backlight`; `logind.conf` when logind is unreachable; `~/.config/shadow/power.conf` |
| Keyboard (`keyboard`) | layouts and variants (add, remove, order), layout-switch / Compose / Caps Lock options, repeat delay and rate, test field, list of desktop shortcuts | `/usr/share/X11/xkb/rules/evdev.lst`; `~/.config/kxkbrc [Layout]` and `~/.config/kcminputrc [Keyboard]` through KConfig; signal `org.kde.keyboard.reloadConfig` and `org.kde.KWin /KWin reconfigure`; `org.kde.KeyboardLayouts`; `org.freedesktop.locale1` or `/etc/default/keyboard` for the system default; `org.kde.kglobalaccel` (`allComponents`, `allShortcutInfos`) |
| Mouse & Touchpad (`mouse`) | per device: enable, pointer speed, acceleration profile, left-handed, middle-click emulation, natural scrolling, scroll speed, scroll method, tap-to-click, tap-and-drag, drag lock, click method, disable while typing | `org.kde.KWin /org/kde/KWin/InputDevice[/<sysName>]` (`org.kde.KWin.InputDevice` properties, written with `Properties.Set`; KWin stores them itself) |

### `~/.config/shadow/power.conf`

Written only by the Power page, read by the shell. INI, group `[Power]`, whole
minutes 0–1440, 0 = never; a missing key has the default in brackets:
`IdleScreenOffMinutes` (10), `IdleLockMinutes` (15), `IdleSuspendMinutes` (0),
`IdleScreenOffMinutesOnBattery` (5), `IdleLockMinutesOnBattery` (10),
`IdleSuspendMinutesOnBattery` (20). Details are in `backends/powerbackend.h`.
Nothing in the repository reads the file yet; the shell has to.

## Privileged operations

None of these pages has a helper or polkit policy of its own. Changes go to
NetworkManager, BlueZ, power-profiles-daemon and logind, which ask polkit
themselves through the session's agent.

## Run-time dependencies (Debian packages)

`network-manager` (daemon and `nmcli`), `bluez`, `libkf6bluezqt6`,
`libkf6pulseaudioqt5` (KF6 PulseAudioQt), `pipewire`, `pipewire-pulse`,
`wireplumber`, `pulseaudio-utils` (`paplay`, `parec`; `pipewire-bin`'s
`pw-play` is the fallback for the test tone), `upower`,
`power-profiles-daemon`, `libkscreen-bin` (`kscreen-doctor`), `kwin-wayland`, `libkf6configcore6`, `xkb-data`,
`polkitd` plus a polkit agent in the session. Optional:
`network-manager-openvpn` (OpenVPN import and connections), a kernel with
WireGuard.

## Limitations

* Wi-Fi: no profiles for enterprise (802.1X) or WEP networks are created; such
  networks are listed with the reason and can be used once a profile exists.
  No hotspot.
* Manual proxies exist only in the environment file (NetworkManager can only
  express none/automatic); running applications see a proxy change after the
  next login. Proxy credentials are not supported on purpose.
* VPN: profiles are imported, not edited. Plug-in VPNs "connect
  automatically" by being added to `connection.secondaries` of the networks
  connected at that moment.
* Lid and power-button behaviour is shown, not changed: `logind.conf` needs
  root and there is no helper for it.
* Desktop shortcuts are listed read-only (the first chord of each binding, as
  KGlobalAccel reports it); they cannot be edited here.
* Mouse settings exist only where KWin exposes libinput devices, i.e. in a
  real session, not in KWin nested in X11.
* Night Light is offered only when KWin's nightlight plugin is loaded.
* The "Apply" button of the Displays page is in the page header and scrolls
  away with it on a long page.

## What was tested, and how

Build: `scripts/dev.sh build --dir ccnet --only control-center --cc-pages
"wifi;network;bluetooth;vpn;display;sound;power;keyboard;mouse"`.

Smoke: `tests/desktop/smoke.sh --dir ccnet control-center` and
`scripts/dev.sh run --dir ccnet --smoke shadow-control-center <key>` for each
of the nine keys: no QML warnings; without services every page shows a named
unavailable state. `--dump <key>` returns valid JSON for each key.

Against the real thing, driven through the GUI with xdotool and checked with
the service's own tool:

* **Displays** — KWin 6 nested in Xvfb (`dev.sh nested`, and a two-output
  variant): scale 100 → 125 % applied, reverted by itself after 15 s without
  an answer; kept after "Keep"; orientation; second output dragged below the
  first; primary moved to the second output; second output switched off — each
  confirmed with `kscreen-doctor`.
* **Keyboard** — same session: German and French added through the dialog,
  `kxkbrc` written and KWin's `getLayoutsList` reporting them; 151 shortcuts
  read from KWin's KGlobalAccel, 43 with keys. Not exercised through the GUI:
  the option combo boxes and the repeat sliders.
* **Sound** — PipeWire + WirePlumber + pipewire-pulse with null sinks: volume,
  mute, default output, per-application stream listed, test tone visible as a
  stream while it played — confirmed with `pactl`; a server started after the
  page shows up without a restart. No capture device existed, so inputs and
  the level meter were not exercised. Nothing was heard: there is no audio
  hardware.
* **Wired & Proxy, VPN** — real NetworkManager 1.52 on a private system bus in
  a network namespace with a veth pair: interface off; address changed in the
  dialog and seen in `ip addr` and the profile; manual and automatic proxy
  written (file, `proxy.method`, `proxy.pac-url`); WireGuard file "My Office
  VPN.conf" imported through the file dialog (interface `my-office-vpn`,
  auto-connect off). Activating it was attempted and NetworkManager's error
  shown — the kernel there has no WireGuard, so an established tunnel was not
  seen. OpenVPN: plug-in not installed; only the "not installed" state.
* **Power** — real upowerd and power-profiles-daemon (with polkitd) on that
  bus: profile switched to power-saver and read back; idle timeout written to
  `power.conf`. No battery, lid, backlight or logind there.

Against test doubles only (the real service cannot run in the chroot):

* **Bluetooth** — a scripted `org.bluez` stand-in: adapter and devices listed,
  discoverable, scan, pair, trust, connect, disconnect reached the service.
  Pairing prompts, removal, rfkill and a real adapter: not exercised.
* **Mouse & Touchpad** — a stand-in for KWin's input-device objects: devices
  listed, pointer speed, acceleration profile, left-handed, natural scrolling,
  tap-to-click written as the right properties.
* **Wi-Fi** — real NetworkManager without a Wi-Fi adapter gives "no adapter".
  With `nmcli` answers replaced by fixtures the list, details, password dialog
  and scan worked, and NetworkManager accepted the `AddAndActivateConnection`
  call and rejected the made-up device. No real scan, join, wrong-password
  handling, forget or hidden network was ever performed.

Not tested at all: Night Light, VRR/HDR, real hardware of any kind, a polkit
agent prompting a non-root user, the pages inside a full SHADOW session.
