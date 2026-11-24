# Network kill switch and panic

Two root tools in the `shadow-hardening` package:

* **`shadow-killswitch`** — while it is on, no packet leaves this computer
  except through Tor or a VPN tunnel. It can stay on across restarts and is
  loaded before any network interface is configured.
* **`shadow-panic`** — cuts this computer off every network at once, until
  an administrator releases it or the computer restarts.

Both are nftables tables of their own. They never run `nft flush ruleset`
and never touch the host firewall (`inet shadow_filter`, see
[hardening.md](hardening.md#firewall-nftables)) or anyone else's tables.

| Table | Tool | Hooks and priority | Policy |
|---|---|---|---|
| `inet shadow_panic` | shadow-panic | input, output, forward at -300 | drop; only loopback passes |
| `bridge shadow_panic` | shadow-panic | forward at -300 | drop: every bridged frame |
| `inet shadow_egress` | shadow-killswitch | output, forward at -50; postrouting at -50 | output, forward: drop, plus the exceptions of the mode; postrouting: accept, but a loopback destination leaving by any device other than `lo` is dropped |
| `bridge shadow_egress` | shadow-killswitch | forward at -50 | drop, except frames to a local guest's port (kind `veth` or `tun`) |
| `inet shadow_filter` | shadow-firewall | input, forward, output at 0 | the host firewall |

A packet must pass every base chain on its hook; an accept in one table does
not skip another. The kill switch therefore never weakens the firewall, and
the panic table beats both. The kill switch runs at -50, after the NAT
redirect of Hide Me's transparent Tor proxy (-100). A redirected packet keeps
its original output device in the output hook's state, so the kill switch
does not recognise it by `oif "lo"`; its output chain accepts it by its new
destination, 127.0.0.0/8 or `::1`. Such a destination does not prove the
loopback device: a socket bound to the network card (`SO_BINDTODEVICE`,
which needs no privilege since Linux 5.7) sends a packet addressed to 127.x
out of the card, with the real address, and a redirected packet of such a
socket is re-routed out of the card too. The postrouting chain sees the
device the packet really leaves by and drops every loopback destination that
is not on `lo` (counter "loopback destination on a real device").

Frames that a Linux bridge forwards between its ports — a virtual machine or
container bridged to the network card (libvirt "bridged", a NetworkManager or
LXC bridge with the card as a port) — never reach the inet hooks unless
`br_netfilter` is loaded with `bridge-nf-call-iptables=1`, which Debian does
not do by default. Both tools therefore also load a `bridge` table, in the
same transaction as the inet one. On a kernel without the nftables bridge
family (`CONFIG_NF_TABLES_BRIDGE`) they load the inet table alone and say so
in a warning: bridged guests are then not covered.

## Kill switch

### Modes

| Mode | Allowed out | Forwarded (virtual machines, containers) |
|---|---|---|
| `off` | everything (the table is removed) | everything the firewall allows |
| `tor` | loopback — which carries Tor's SOCKS port, TransPort and DNSPort — and sockets of the user `debian-tor` | nothing |
| `vpn` | loopback; anything leaving through a tunnel interface (`wg*`, `tun*` and the devices of active NetworkManager WireGuard/tun connections); packets to a VPN endpoint (address, protocol and port); sockets of the user `nm-openvpn` | only into a tunnel, and the replies |
| `tor-or-vpn` | both of the above | as `vpn` |

"Forwarded" covers routed and NAT guests (libvirt's default network,
Docker's and Podman's bridges with NAT) through the inet forward chain, and
guests bridged to the network card through the bridge table: in every mode a
bridged guest's frame may go only to another local guest on the same bridge
(a `veth` or TAP port), never out of the card — a bridged guest cannot reach
a tunnel. Guests on macvtap/macvlan/ipvlan devices, SR-IOV virtual functions
and PCI-passthrough network cards are not covered (their frames pass no hook
of this computer; see [What it can break](#what-it-can-break)).

A tunnel interface is trusted by name **and** type: its name must match
`tunnel_ifaces` and it must be a layer-3 tunnel device (device type "none":
WireGuard, tun, OpenVPN's DCO, xfrm) or a TAP device (kind `tun`). Their
packets go to Tor/OpenVPN's sockets or are encapsulated and pass the output
chain again. A VLAN, macvlan or other Ethernet-type device that is only
*named* like a tunnel (`tun7` on the network card, which a program allowed to
change NetworkManager profiles could create) is dropped like any other
device; its sockets are closed at engage like any other.

Every mode except `off` also lets out DHCP (UDP 68→67, DHCPv6 546→547),
IPv6 router/neighbour solicitations and advertisements, and multicast
listener reports, so that the computer keeps its address.

`--lan` additionally allows the local networks: 10.0.0.0/8, 172.16.0.0/12,
192.168.0.0/16, 169.254.0.0/16, 224.0.0.0/24, fc00::/7, fe80::/10,
ff02::/16 — but never 10.192.0.0/10, Tor's range for automapped `.onion`
addresses: a packet to it means the Tor redirect is missing, and it must not
reach the LAN. The LAN is off by default: on large networks (a company
10/8) "LAN" is a lot of the world.

Everything else is dropped and counted. Nothing about blocked or allowed
traffic is logged; the status shows the number of dropped packets only.

### Commands

Must run as root (Control Center and Hide Me use `pkexec`, polkit action
`org.shadow.killswitch.manage`, administrator authentication kept for a
short time).

```
shadow-killswitch status [--json]
shadow-killswitch set {off|tor|vpn|tor-or-vpn} [--lan|--no-lan] [--runtime]
shadow-killswitch vpn-endpoint {add|remove} <ip>:<port>/<udp|tcp>
shadow-killswitch sync
shadow-killswitch boot
shadow-killswitch reassert [--watch]
```

| Command | Effect |
|---|---|
| `status` | the mode in force, whether its table is really loaded (read back from the kernel), the LAN setting, the number of VPN endpoints and dropped packets, any problem. Exit status 0: consistent; 3: a problem (for example on but not loaded); 4: run without root, so the kernel was not re-checked (the last recorded state is shown) |
| `set MODE` | validates, loads and saves the mode; it is also the mode after a restart. `--runtime`: until the next restart only. Without `--lan`/`--no-lan` the LAN setting in force is kept |
| `vpn-endpoint add/remove` | allows or forgets a VPN server. IP addresses only — `[2001:db8::1]:51820/udp` for IPv6; host names are refused |
| `sync` | re-reads tunnel interfaces and VPN endpoints from NetworkManager (the dispatcher runs it) |
| `boot` | used by `shadow-killswitch.service` at start-up; honours `shadow.killswitch=off` |
| `reassert` | loads the mode in force again without closing connections, unless the kernel already has exactly that table (used after `nftables.service` starts). `--watch`: as the [watchdog](#the-watchdog) runs it |

Examples:

```sh
sudo shadow-killswitch set tor                         # Tor only, also after a restart
sudo shadow-killswitch set vpn --runtime               # VPN only, until the next restart
sudo shadow-killswitch vpn-endpoint add 192.0.2.10:51820/udp
sudo shadow-killswitch status
sudo shadow-killswitch set off
```

Other programs change the kill switch only through this command and its
exit status, never with rules of their own; Hide Me (`shadow-anon`) uses
`set tor --no-lan --runtime` (`set tor --no-lan` with `--persist`) for Ghost
Mode, its kill switch switch, its transparent proxy and the "maximum" threat
level — always `--no-lan`, so a LAN setting saved for another mode does not
let the local network out while Hide Me says "Tor only" (mode `tor` with the
LAN allowed is not Hide Me's Tor only) — checks `status --json` afterwards,
and puts back the mode that was in force before it, with its LAN setting
(see [Hide Me](hardening.md#hide-me-shadow-anon)).

### How a change is made

1. The complete tables (`inet` and `bridge shadow_egress`) are written as
   nftables text, with the tables' comment carrying the mode and a checksum
   of the rules.
2. `nft --check` validates them. If they are rejected, or the load in step 3
   fails, nothing changes and the previous mode stays in force: the command
   exits with status 1 and names the mode that could not be set, and the
   status describes the mode still in force, checked against the kernel, with
   the failure in `change_error` (and in `warnings`) — not in `error`, which
   is only set if what is in force is itself not right. The desktop therefore
   keeps showing the old mode as protected, and the Control Center shows the
   failed change.
3. `nft --file` loads one batch — create the tables if missing, delete them,
   create them with the new rules — which the kernel applies atomically. On a
   kernel without the nftables bridge family the batch is the inet table
   alone, with a warning.
4. The tables are read back (`nft --json list table inet shadow_egress`,
   `… bridge shadow_egress`): the kill switch counts as loaded only if the
   output and forward chains have priority -50 and policy drop, the
   postrouting chain priority -50 and policy accept, the bridge table's
   forward chain priority -50 and policy drop, and both comments match what
   this configuration produces. The result goes to
   `/run/shadow/killswitch.json`. The configuration file is saved only after
   a successful load.
5. When the kill switch is turned on, or a mode becomes stricter (for
   example `tor-or-vpn` → `tor`, or the LAN is removed), connection tracking
   is flushed (`conntrack -F`) and every connected TCP/UDP socket that the
   new mode would block is closed (`ss -K`). Sockets of Tor, of OpenVPN, to
   VPN endpoints, through a tunnel, on loopback and the DHCP client are
   kept. Loosening a mode closes nothing.

Messages and the status never contain addresses: nft's error context
(which quotes rules, and therefore endpoint addresses) is cut to its
`Error:` lines.

### Files

`/etc/shadow-os/killswitch.json` (root, 0644) — the mode after a restart:

```json
{"version": 1, "mode": "off", "lan": false,
 "vpn_endpoints": [{"address": "192.0.2.10", "port": 51820, "protocol": "udp"}],
 "tunnel_ifaces": ["tun*", "wg*"]}
```

`tunnel_ifaces` holds interface names or prefixes ending in `*` (at least two
characters before it; `lo` is refused). A missing file means `off`. A file
that exists but cannot be used makes the kill switch **fail closed** in mode
`tor-or-vpn`, with the reason in the status; `shadow-killswitch set …`
replaces it.

`/run/shadow/killswitch.json` (0644, read by the desktop) — the state in this
boot:

| Field | Meaning |
|---|---|
| `mode` | the mode in force |
| `active` | the mode is not `off` |
| `loaded` | the table was found in the kernel, as configured, when the file was written |
| `since` | when the mode was engaged (UTC), or null |
| `lan` | local networks allowed |
| `endpoints` | number of VPN endpoints in force (configured plus NetworkManager's) |
| `error` | null, or what is wrong with what is in force — the desktop must show it |
| `change_error` | only after a change (`set`, `vpn-endpoint`) that failed while the previous state stayed in force: what could not be done. It is not a protection problem (`error` says whether the state in force is right); it is kept, like the warnings, until the next change |
| `escape_hatch` | `shadow.killswitch=off` kept it off in this boot |
| `persistent_mode`, `runtime_override` | the mode after a restart; whether a `--runtime` mode is in force |
| `tunnel_ifaces`, `skipped_hostnames`, `dropped_packets` | counts |
| `warnings`, `conntrack_flushed`, `sockets_closed` | what the last change did |

Only `active && loaded && !error` means protected. The file is rewritten
whenever root runs `shadow-killswitch status`, and by `reassert` and the
[watchdog](#the-watchdog) when they put the table back or when the recorded
mode, `loaded` or `error` no longer matches the kernel (an error recorded
while something was wrong is replaced once the kernel matches); otherwise
they write nothing. `shadow-security-status` (section `killswitch`) also
checks the live table.

### At start-up

`shadow-killswitch.service` runs `shadow-killswitch boot` with
`DefaultDependencies=no`, after `local-fs.target` and before
`network-pre.target`, which NetworkManager waits for: the table exists before
any interface gets an address. If loading fails, the unit fails and the
reason is in the status file; the boot continues.

Within one boot, `boot` keeps the mode in force (a restart of the unit or a
package upgrade does not drop a `--runtime` mode). Installing or upgrading
the package never turns the kill switch on; an upgrade reloads one that is
already loaded.

### VPNs and NetworkManager

The NetworkManager dispatcher script
`/usr/lib/NetworkManager/dispatcher.d/50-shadow-killswitch` (linked from
`pre-up.d`) runs `shadow-killswitch sync` on `up`, `down`, `pre-up`,
`vpn-up`, `vpn-down` and `vpn-pre-up`. In `vpn` and `tor-or-vpn` mode it
adds:

* the devices of active WireGuard and tun connections as tunnel interfaces
  (for names that do not match `wg*`/`tun*`);
* the peers of active WireGuard devices, read from the kernel
  (`wg show <device> endpoints` from wireguard-tools, which the image
  installs; nmcli does not show peers). Without `wg`, add WireGuard
  servers with `vpn-endpoint add`;
* the servers of every saved OpenVPN profile, active or not — openvpn
  connects as root before it drops to `nm-openvpn`, so its server must be
  allowed before the tunnel exists.

Only IP literals are used. **Host names are never resolved**: with the kill
switch on there is no DNS outside the tunnel, and resolving would leak. A
profile whose server is a host name is counted in `skipped_hostnames`;
add its address with `shadow-killswitch vpn-endpoint add`. The VPN's own DNS
servers are reached through the tunnel.

A WireGuard profile's first handshake can only go out after the dispatcher
has added its peer (at `pre-up`); the kernel retries the handshake every
5 seconds, so the tunnel comes up a few seconds later. Adding the endpoint
with `vpn-endpoint add` avoids the wait.

### What it can break

| With the kill switch on | Why |
|---|---|
| Nothing works when Tor is not running, not installed, or not yet bootstrapped (`tor` mode); or when the VPN is down (`vpn` mode) | that is the point; turn it off or use `tor-or-vpn` |
| Routed and NAT virtual machines and containers lose the network (`tor` mode), or reach it only through the tunnel (`vpn`); guests bridged to the network card lose it in every mode (they cannot reach a tunnel) and keep only the other guests on their bridge | forwarded and bridged traffic would otherwise leave with the real address |
| Guests on macvtap/macvlan/ipvlan devices, SR-IOV virtual functions or a PCI-passthrough network card keep the network | **not covered**: their frames pass no netfilter hook of this computer (a macvlan guest was measured to keep its network); use a routed, NAT or bridged network for guests while the kill switch is on |
| A program that binds a socket to the network card and sends to 127.x | dropped in postrouting (`curl --interface`, `ping -I` to a loopback address fail) |
| An interface named like a tunnel that is not a tunnel device (a VLAN or macvlan `tun7`) | dropped: only layer-3 tunnel and TAP devices count as tunnels |
| Captive portals (hotel, train) cannot be logged into | the portal is not Tor or a VPN; turn the kill switch off for the log-in |
| Printers, file shares, casting, mDNS | local network: use `--lan` |
| Time synchronisation (systemd-timesyncd) | its NTP traffic is not Tor or VPN traffic; a clock that drifts far enough can stop Tor from bootstrapping (not measured) |
| Servers on this computer answer only on loopback, through the tunnel or (with `--lan`) the LAN | replies are outgoing packets too |
| IPv6 when the VPN carries only IPv4 | dropped, which is intended (no IPv6 leak) |

## Panic

`shadow-panic engage`, in this order:

1. loads `inet shadow_panic`: input, output and forward with policy drop at
   priority -300; only loopback passes. In the same transaction
   `bridge shadow_panic` drops every frame a bridge forwards (guests bridged
   to the network card, and guests on one bridge among themselves), which the
   inet hooks never see without `br_netfilter`. This is the cut and depends on
   no other step;
2. flushes connection tracking and closes every connected socket that is not
   on loopback;
3. turns NetworkManager networking off (`nmcli networking off`): no DHCP, no
   reconnects — the DHCP client uses packet sockets, which nftables does not
   see;
4. soft-blocks every radio that was not blocked (Wi-Fi, Bluetooth, WWAN …)
   through `/sys/class/rfkill`, if `rfkill` is true in the configuration;
5. locks every session (`loginctl lock-sessions`), if `lock_session` is true.

The command exits with status 1 only when the table could not be loaded; a
failed later step is reported as a warning. Engaging again re-asserts
everything and keeps the record of the first engage.

### Starting it

| From | How |
|---|---|
| the desktop (panic shortcut **Ctrl+Alt+Shift+Escape**, [../desktop/horizon.md](../desktop/horizon.md#panic)) | D-Bus call `org.freedesktop.systemd1.Manager.StartUnit("shadow-panic.service", "replace")` on the system bus. The polkit rule `/usr/share/polkit-1/rules.d/49-shadow-panic.rules` allows exactly this — action `org.freedesktop.systemd1.manage-units`, unit `shadow-panic.service`, verb `start` — without a password for a local, active session of a member of `sudo`. Stopping, restarting, enabling or masking the unit, and every other unit, keep their normal rules |
| a terminal or TTY | `sudo systemctl start shadow-panic.service` or `sudo shadow-panic engage` |

Engaging is one-way: it can only cut the network. Clearing the clipboard is
left to the desktop, because root cannot reach a Wayland clipboard:
`clear_clipboard` (in `/etc/shadow-os/panic.json` and copied to
`/run/shadow/panic.json`) tells it whether to.

What the desktop (HORIZON, `desktop/horizon-shell/src/panic.h`) does with the
shortcut, all at once: the `StartUnit` call above, without allowing an
interactive password prompt; the screen lock (`org.freedesktop.ScreenSaver.Lock`,
logind's `Session.Lock` if that fails), unless `lock_session` is false; and
`wl-copy --clear` / `wl-copy --primary --clear` (KWin's data-control
protocol, which needs no keyboard focus) checked with `wl-paste`, unless
`clear_clipboard` is false. If systemd refuses the start, it turns
NetworkManager networking off (`Enable(false)`) and says that this is not a
firewall cut. Its banner shows the state from `/run/shadow/panic.json`, and
its "Restore network…" button runs the `pkexec` release below (also offered
in quick settings, for the keyboard). A release made any other way (the
Control Center, `sudo shadow-panic release`) takes the banner away once
`panic.json` says released. A panic pressed while the desktop's own release
is still running wins: a release still waiting for the administrator is
cancelled; one already authorised runs as root and cannot be stopped, so
the desktop starts `shadow-panic.service` again as soon as it has ended
(engage and release both take `/run/shadow/panic.lock`, so that engage runs
after the release). The panel's
kill switch indicator and quick-settings tile read `/run/shadow/killswitch.json`
(status module, `desktop/horizon-shell/modules/status/README.md`): a lock only
for `active && loaded && !error`, a warning for anything else that is not off.

### Releasing it

```sh
pkexec /usr/sbin/shadow-panic release      # polkit action org.shadow.panic.release: administrator, every time
sudo shadow-panic release                  # from a terminal
```

Release reverses exactly what engage recorded: it deletes the panic table,
turns NetworkManager networking back on only if engage turned it off, and
unblocks only the radios engage blocked (a radio that was off before stays
off). The kill switch is not touched. If a step fails, the panic stays
recorded as engaged and a second release finishes the job.

### After a restart

A panic never outlives the boot: the table lives in kernel memory. But
NetworkManager (`NetworkingEnabled` in
`/var/lib/NetworkManager/NetworkManager.state`) and systemd-rfkill (the
radio states in `/var/lib/systemd/rfkill/`) keep their own state across a
restart. So engage also writes what it changed to
`/var/lib/shadow-os/panic-pending.json` (root only), and at the next start
`shadow-panic-reset.service` — before NetworkManager and systemd-rfkill —
puts both back as they were before the panic. Release deletes that file.

Removing `shadow-hardening` takes `shadow-panic` and the reset unit away, so
the package's prerm releases an engaged panic first (an upgrade does not:
the new version takes the panic over), and postrm then deletes the kill
switch and panic tables. If that release fails, postrm says so: run
`nmcli networking on` and `rfkill unblock all`. Inside a chroot (installer,
recovery) neither script touches the running system's panic or kill switch
([hardening.md](hardening.md#overview)).

### Files

`/etc/shadow-os/panic.json` (optional; a missing or broken file means all
true — a panic must still work):

```json
{"rfkill": true, "lock_session": true, "clear_clipboard": true}
```

`/run/shadow/panic.json` (0644): `engaged`, `loaded` (read back from the
kernel), `since`, `error`, `steps` (`table`, `conntrack_flushed`,
`sockets_closed`, `networking_off`, `rfkill_blocked`, `sessions_locked`),
`undo`, `warnings`, `clear_clipboard`, `lock_session`, `dropped_packets`.
`shadow-panic status [--json]` re-checks the kernel when run as root (exit
status 0 consistent, 3 problem, 4 not re-checked).

### Limits

* Only this computer's own network namespace is cut. Network namespaces of
  containers or future personas have their own rules; their traffic that is
  forwarded or bridged through the host is dropped, traffic on their own
  interfaces (macvlan, a WireGuard device moved into them) is not.
* Virtual machines on macvtap/macvlan/ipvlan devices, SR-IOV virtual
  functions and PCI-passthrough network cards are not cut by the table: their
  frames pass no netfilter hook of this computer. NetworkManager networking
  off (step 3) and the radios (step 4) stop them only where those devices
  depend on a connection NetworkManager manages or on a radio.
* On a kernel without the nftables bridge family only the inet table is
  loaded, with a warning; bridged guests are then not cut by the table.
* Radio frames below IP (Wi-Fi association, Bluetooth advertising) stop only
  with the rfkill step.

## nftables.service

Debian's `nftables.service` stops with `nft flush ruleset`, and the nftables
package restarts it on every upgrade. That would silently remove the kill
switch, a panic, Hide Me's chains and Docker's or libvirt's NAT.
`shadow-hardening` ships `/usr/lib/systemd/system/nftables.service.d/shadow.conf`:

```ini
[Service]
ExecStop=
ExecStop=-/usr/sbin/nft delete table inet shadow_filter
ExecStartPost=-/usr/sbin/shadow-panic reassert
ExecStartPost=-/usr/sbin/shadow-killswitch reassert
```

Stopping or restarting `nftables.service` now removes only the host firewall
table it loaded. The two `reassert` lines put an engaged panic and the kill
switch back at once in case `/etc/nftables.conf` was edited to flush the
ruleset; both do nothing when their table is intact.

## The watchdog

Anything else that runs `nft flush ruleset` — an `nftables.conf` loaded by
hand with `nft -f`, a VPN client, container tool or script that flushes,
an administrator — removes the kill switch and a panic along with every other
table. `shadow-killswitch-watch.timer` (enabled, `WantedBy=timers.target`)
starts `shadow-killswitch-watch.service` every 20 seconds, which runs:

```ini
ExecStart=-/usr/sbin/shadow-panic reassert
ExecStart=/usr/sbin/shadow-killswitch reassert --watch
```

* `shadow-panic reassert` loads the panic's tables (`inet` and `bridge
  shadow_panic`) again only while a panic is recorded as engaged
  (`/run/shadow/panic.json`) and a table is missing or not exactly what
  engage loaded. It does not repeat the other steps (connections,
  NetworkManager, radios, session lock).
* `shadow-killswitch reassert --watch` loads the kill switch's tables (`inet`
  and `bridge shadow_egress`) again only if a mode was applied in this boot
  (by `shadow-killswitch.service`, `set` or Hide Me), in exactly that mode
  and LAN setting, and a table is missing or changed. It never closes
  connections.
* "Exactly": the tables are compared, from `nft --json list table`, with a
  fingerprint taken right after the last successful load — every set,
  element, chain, priority, policy and rule of both tables, without rule
  handles and counter values. A rule inserted by hand (`nft insert rule inet
  shadow_egress output accept`), a changed policy or a deleted bridge table
  is undone as well.
* Hide Me's own nat redirects (`SHADOWTOR`, `SHADOWDNS`, iptables-nft) are
  **not** watched: after a flush they stay removed, and the Hide Me page may
  show DNS-over-Tor or the transparent proxy as on until the next Hide Me
  action or restart (see [../limitations.md](../limitations.md)).
* Neither ever engages anything by itself: nothing before the boot unit ran,
  nothing while the mode in force is `off` (`set off --runtime`, the escape
  hatch `shadow.killswitch=off`), nothing after a release. If the
  kill switch's own table is found while it is off, it is removed.
* A load that failed (at boot, by a panic's engage, or by the watchdog
  itself) is reported once — the unit fails and the error is in
  `/run/shadow/*.json`, so the desktop shows "not protected" — and not
  retried every 20 s; an administrator's `shadow-killswitch reassert`
  (without `--watch`), `set` or a new `shadow-panic engage` retries it. A
  `set` that fails leaves the previous mode in force, and watched. A check
  that finds another
  `shadow-killswitch` or `shadow-panic` holding its lock skips that round
  instead of waiting.
* While everything is in place, a run reads the four tables and writes
  nothing, not even the status files; the unit logs nothing
  (`LogLevelMax=notice` hides systemd's "Starting/Finished" lines). When it
  put something back it logs one warning line, for example `shadow-killswitch:
  the kill switch table had been removed (for example by 'nft flush
  ruleset'); it is loaded again (mode tor).`
* The service runs with `CapabilityBoundingSet=CAP_NET_ADMIN`,
  `NoNewPrivileges=yes`, `ProtectSystem=full` and only `AF_UNIX`/`AF_NETLINK`
  sockets.

Timing: `OnBootSec=30s` (the boot itself loads the tables, the first check
waits for the start-up rush), `OnUnitActiveSec=20s` and `AccuracySec=2s`
(systemd's default accuracy of one minute would stretch the gap to about 80
seconds). So after a flush the computer is open for at most about 22
seconds — a period in which a leak is possible. A run where nothing changed
costs about 0.15 s of CPU. To stop the watchdog:
`sudo systemctl disable --now shadow-killswitch-watch.timer`.

## In the Control Center

**Control Center → Privacy** has a "Network kill switch" and a "Panic"
section (`applications/control-center/backends/privacykillswitchbackend.h`,
details in `applications/control-center/README-security.md`):

* the state in force, read from `/run/shadow/killswitch.json` and
  `/run/shadow/panic.json` with the rule above (only `active && loaded &&
  !error` is "Protected"), plus a check against the kernel through
  `pkexec shadow-security-status --section killswitch,panic` (no password for
  an active local session): a status file that says "loaded" while the kernel
  has no table is shown as **not protected**. Errors, the escape hatch, "no
  status recorded" and "not installed" are shown as such;
* **Mode** (Off / Tor only / VPN only / Tor or VPN), **Allow the local
  network** (only while it is on) and **Keep after a restart**:
  `pkexec /usr/sbin/shadow-killswitch set MODE --lan|--no-lan [--runtime]`.
  Turning it on, or a stricter mode, asks first, because connections the mode
  does not allow are closed. "Keep after a restart" off makes the next changes
  `--runtime`; switched on while a runtime mode is in force it saves that mode
  for start-up. The banner of a problem says "Traffic may leave outside Tor
  or the VPN" only when the mode's table is not in force (otherwise "Kill
  switch problem", for example a broken configuration while the kill switch
  fails closed). "Load again" runs `reassert`, and reports success only if
  the status it then reads has no problem left. A change that failed while
  the previous mode stayed in force is shown as that change's failure, with
  the previous mode still "Protected";
* **Panic**: its state, **Reconnect…** (`pkexec /usr/sbin/shadow-panic
  release`, administrator every time), the shortcut bound in the session, and
  the options of `/etc/shadow-os/panic.json` — read-only, because shadow-panic
  has no command that changes them (an administrator edits the file).

## Turning it off when there is no network

From a root shell (a terminal, a TTY with **Ctrl+Alt+F3**, or recovery):

```sh
sudo shadow-killswitch set off        # off now and after restarts (set off --runtime: until the next restart)
sudo shadow-panic release             # if a panic is engaged
```

Both work as root without polkit, and both record the change, so the
[watchdog](#the-watchdog) leaves the network open. With a working desktop,
Control Center → Privacy → Network kill switch → Off does the same
(administrator password).

If the desktop does not start, boot once with the kill switch off: at the
boot menu press **e**, add `shadow.killswitch=off` at the end of the `linux`
line, and press **Ctrl+X**. The saved configuration is not changed; the
status shows `escape_hatch: true`. Then turn it off for good with
`shadow-killswitch set off`, or fix the VPN/Tor problem and restart normally.

Without the tool (for example on a system mounted from the live medium,
where nothing is loaded anyway), the configuration is a plain file: set
`"mode": "off"` in `/etc/shadow-os/killswitch.json`. Do not remove the tables
of a running system with `nft delete table` while the tools work: the
recorded state still says "on" (or "engaged"), so the watchdog loads them
again within about 20 seconds. Only if the tools themselves cannot run, stop
the watchdog first, then remove the tables by hand (the status then says
"not loaded"):

```sh
sudo systemctl stop shadow-killswitch-watch.timer      # until the next restart
sudo nft delete table inet shadow_egress; sudo nft delete table bridge shadow_egress
sudo nft delete table inet shadow_panic; sudo nft delete table bridge shadow_panic
```

See also [../troubleshooting/recovery.md](../troubleshooting/recovery.md).

## Tests

* `python3 tests/security/test_killswitch_unit.py` — any user: rules per
  mode, configuration checks, status honesty against a fake nft that fails
  or lies, flush decisions, NetworkManager parsing, panic engage/release and
  the restart reset, both `reassert` commands as the watchdog runs them
  (silent and without writes while intact, one line after a repair, a
  changed table, the escape hatch, a held lock, a failed reload reported
  once), the bridge tables and a kernel without the bridge family, a failed
  change reported as `change_error` while the old mode stays protected, a
  stale error replaced once the kernel matches, the polkit rule (with node),
  the polkit action file (with xmllint), units and packaging lists.
* `sudo python3 tests/network/test_killswitch_netns.py` — root; re-executes
  itself in a private network namespace and sends real packets through real
  nftables in every mode: Tor user versus other users, loopback, DHCP-shaped
  UDP, a layer-3 tunnel `wg0` (two TUN devices joined by a relay, type
  "none" like WireGuard), a real `tun0`, a TAP `tun8`, a macvlan named `tun7`
  on the network card (dropped), VPN endpoints, the LAN and the onion range,
  an unprivileged socket bound to the network card sending to 127.x (with and
  without Hide Me's nat redirect to a stand-in TransPort/DNSPort; nothing
  reaches the upstream side), a routed "virtual machine", guests bridged to
  the network card with `bridge-nf-call-iptables=0` (kill switch and panic),
  connection tracking flush and
  `ss -K`, panic and exact release, the nftables.service drop-in, the escape
  hatch, a NetworkManager sync through a fake nmcli, a status that
  notices a table removed behind its back, and the watchdog: the
  `ExecStart=` lines of `shadow-killswitch-watch.service` run with its
  capability bounding set after `nft flush ruleset` and after a hand-inserted
  accept rule (the same table, chains and counters come back and the packets
  are dropped again), with negative controls (nothing loaded in this boot,
  `set off --runtime`, the escape hatch, a released panic).
* `applications/control-center/tests/test_killswitch_backend.py` — the
  Control Center section against stand-in tools (every kind of status
  document, the exact `pkexec` command lines of its controls, pkexec refusals,
  "Load again" that leaves a problem in place) and against documents the real
  tools published in a network namespace.

## Not verified

On real hardware or a booted image, none of this has run yet: boot ordering
against NetworkManager, real WireGuard (the test kernel has none) and
OpenVPN, NetworkManager's dispatcher and nmcli 1.52's exact output, IPv6
packets (the test kernel runs with `ipv6.disable=1`; the IPv6 rules,
including the postrouting drop of `::1` on a real device, are only checked
to load), real virtual machines on a libvirt/NetworkManager bridge (a veth
pair stood in for the guest, a macvlan for the network card), a kernel
without the nftables bridge family (only the fake nft of the unit tests),
the postrouting chain, the bridge table and the tunnel type checks (`meta
oiftype`, `meta oifkind`) with trixie's nft 1.1.3 (they were loaded and
tested with nft 1.0.9 on kernel 6.18 only), radios, `loginctl lock-sessions` locking KWin's screen,
the effect of blocked NTP on Tor, and the watchdog timer firing under a
real systemd with its logging settings (its units were checked with
`systemd-analyze verify`; its commands ran with the unit's capability
bounding set in a network namespace). The list is kept in
[../limitations.md](../limitations.md).
