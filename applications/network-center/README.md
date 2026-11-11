# NEXUS Network Center

`nexus-network-center` (id `org.nexus.networkcenter`) shows the state of the
network, changes the configuration through NetworkManager and carries the
tools needed to find out why a connection does not work.

| Page | What it does |
|---|---|
| Overview | connectivity verdict, default gateway, primary address, DNS, exposed listeners, throughput of the primary interface |
| Wi-Fi | radio switch, scan, networks in range, connect (password dialog, hidden networks), current link, saved networks, forget |
| Interfaces | every kernel interface: state, addresses, MAC, MTU, speed, driver, counters, live rx/tx rates with a 60 s history; connect / disconnect through NetworkManager |
| IP configuration | per profile: method, addresses, gateway, DNS, search domains for IPv4 and IPv6; validated, reviewed, then applied |
| DNS | resolver configuration in effect (resolv.conf, systemd-resolved), DNS per device, DNS settings and DNS-over-TLS mode per profile |
| Routing table | all routes of all tables (view only), neighbours |
| Active connections | TCP/UDP sockets with owning processes, filter and sort; listeners bound beyond loopback are highlighted |
| VPN | WireGuard and plug-in VPN profiles: connect, disconnect, autoconnect, remove; import WireGuard / OpenVPN files |
| Diagnostics | troubleshooting checklist; ping, route trace, DNS lookup, TCP port check, path-MTU probe — live output, cancellable |
| Packet capture | capture to file with filter, size/time limits, ring buffer, live counters, stop; open in Wireshark |

`nexus-network-center <page>` (or `--page <page>`) opens a page directly:
`overview wifi interfaces ip dns routes connections vpn diagnostics capture`.

## Data sources

| Data | Source |
|---|---|
| Interfaces, addresses, counters | `ip -j -s -details address show`; `/sys/class/net/<if>/…` (speed, duplex, driver, wireless, statistics sampled once per second for the rates); `ip monitor` as change trigger |
| Routes, neighbours | `ip -j -4/-6 -details route show table all`, `ip -j neigh show` |
| Resolver | `/etc/resolv.conf`; `org.freedesktop.resolve1` on the system bus when systemd-resolved runs |
| Radio kill switches | `/sys/class/rfkill` |
| Sockets | `ss -H -O -t -u -n -a -p` (one socket per line, numeric); the same call as root through the helper for the processes of other users |
| NetworkManager | D-Bus, see below |
| Diagnostics | `ping -n -c N -W 2 [-4/-6] -- HOST`; `traceroute` (or `tracepath`); `dig` (else `resolvectl query`, else the C library resolver); port check and the HTTPS step are connections made by the application itself (QTcpSocket / QSslSocket) |
| Capture | `dumpcap` as the user when the account may use it (group `wireshark`), otherwise `tcpdump` through the helper |

Every host name, address, port, interface name and UUID that reaches a tool is
validated first (`netutil.cpp`) and passed as one element of an argument
vector; nothing goes through a shell. Tools run with `LC_ALL=C.UTF-8`.

### NetworkManager (`nmclient.cpp`)

All of it goes through NetworkManager's D-Bus API on the system bus
(`org.freedesktop.NetworkManager`); the application holds no privilege and
NetworkManager's own polkit policy decides what the user may do.

* **Read:** `org.freedesktop.DBus.ObjectManager.GetManagedObjects` (manager,
  devices, access points, IP configurations, active connections) and
  `Settings.Connection.GetSettings` per profile (cached, re-read after the
  profile's `Updated` signal).
* **Follow:** `PropertiesChanged`, `InterfacesAdded/Removed`,
  `Device.StateChanged`, `Connection.Active.StateChanged`.
* **Change:** `ActivateConnection`, `AddAndActivateConnection` (new Wi-Fi
  network; a device without profile), `DeactivateConnection`,
  `Device.Disconnect`, `Device.Wireless.RequestScan`,
  `Settings.Connection.Update` (IP/DNS settings, autoconnect),
  `Settings.Connection.Delete` (forget / remove), property `WirelessEnabled`.
  The result of an activation is taken from the active connection's
  `StateChanged(state, reason)` and the device's failure reason. A Wi-Fi
  profile created for a connection attempt that fails is deleted again.
* **Two operations use `nmcli`,** because NetworkManager has no D-Bus method
  for them and implements them in its client: `nmcli connection import type
  wireguard|openvpn file …` (VPN import; OpenVPN needs the plug-in), and
  `nmcli connection up uuid … passwd-file /dev/stdin` when an activation
  failed with "no secrets" and the user typed the missing secret (nmcli acts
  as the secret agent for that one activation).
* **Secrets:** the passphrase of a new Wi-Fi network is part of the profile
  sent to NetworkManager over the system bus; a secret for an existing profile
  goes to nmcli's standard input. They are never put into an argument vector,
  logged or stored by this application.
* **Unavailable states:** no system bus, NetworkManager not on the bus (not
  running / not installed) and a failing call each produce their own message;
  the kernel-based pages keep working and the overview says that its
  connectivity figure is then only derived from the routing table.

## Privileged operations

One helper, `/usr/libexec/nexus/network-helper` (Python 3, standard library),
started through `pkexec`. Policy: `org.nexus.networkcenter.policy`, generated
by CMake so that the authorised path is the installed path; pkexec selects the
action by the helper's first argument (`org.freedesktop.policykit.exec.argv1`).

| Action | Sub-command | Authorisation |
|---|---|---|
| `org.nexus.networkcenter.sockets` | `sockets` — `ss` as root, to see the processes of other users | `auth_admin_keep` (active session), `auth_admin` otherwise |
| `org.nexus.networkcenter.capture` | `capture --interface NAME [--filter EXPR] [--snaplen N]` — `tcpdump -Z USER -i NAME -U -w - [-s N] [-- EXPR]` | `auth_admin` every time, active local session only (`allow_any`/`allow_inactive` = `no`) |

Properties of the helper (reviewed and tested, see below):

* closed set of sub-commands and options; unknown, repeated or value-less
  options are refused; `sockets` takes no arguments;
* the interface must match `^[A-Za-z0-9_][A-Za-z0-9_.:-]{0,14}$` **and** exist
  in `/sys/class/net` (or be `any`);
* the filter may only contain the characters of the pcap filter language, is
  at most 1024 characters long, must not begin with `-`, and is handed to
  tcpdump as a single argument after `--` — it cannot become an option (`-w`,
  `-r`, `-F`, `-z`, which name files and commands). The application applies the
  same rule and additionally compiles the filter unprivileged
  (`tcpdump -d -r <empty pcap>`) before asking for authentication;
* the helper never opens a file for the caller: the capture is a pcap stream
  on standard output and the *unprivileged* application writes the files
  (`~/Captures`, mode 0700, files 0600, created with `O_EXCL`), counts packets,
  enforces limits and rotates the ring buffer;
* no shell, tools located in fixed system directories, fixed minimal
  environment, `python3 -I`, umask 077;
* before running a tool it removes everything else from the capability
  bounding set and sets `no_new_privs`: `ss` keeps
  CAP_NET_ADMIN/DAC_READ_SEARCH/SYS_PTRACE; `tcpdump` keeps
  CAP_NET_RAW/NET_ADMIN/SETUID/SETGID and then drops to the `tcpdump` (else
  `nobody`) account itself. If the restriction cannot be applied nothing runs;
* the capture ends when the helper's standard input closes (the application
  stops it, or exits), so a root tcpdump is never left behind.

The capture page carries a permanent note that only traffic one is authorised
to see may be captured.

## Run-time dependencies (Debian packages)

Required: `network-manager` (daemon and `nmcli`), `iproute2`, `iputils-ping`,
`python3` (helper; needs `ctypes`, i.e. not only `python3-minimal`), `pkexec`
and `polkitd` with a polkit agent in the session, `libqt6dbus6`,
`libqt6network6` (with its TLS backend for the HTTPS check).

Recommended: `traceroute` (or `iputils-tracepath`), `bind9-dnsutils` (`dig`),
`tcpdump` (capture; without it and without usable `dumpcap` the page says
capture is not possible).

Optional: `wireshark` ("Open in Wireshark"), `wireshark-common` (`dumpcap`,
capture without authentication for members of group `wireshark`),
`network-manager-openvpn` (OpenVPN import and activation).

## Limitations

* Wi-Fi: WPA/WPA2/WPA3 personal, OWE and open networks can be set up.
  Enterprise (802.1X) networks need an identity and certificates and can only
  be used if a profile already exists; WEP is refused.
* The application does not register a permanent NetworkManager secret agent.
  A secret is only asked for after an activation the user started here failed
  for lack of it.
* IP configuration edits method, addresses, gateway, DNS servers, search
  domains, "ignore automatic DNS" and DNS-over-TLS. Static routes, MTU and
  other profile settings are not editable; the routing table is view-only.
* VPN: import for WireGuard and OpenVPN only; profiles of other plug-ins are
  listed and can be activated, deactivated and removed.
* Capture through the helper produces classic pcap; pcapng only with dumpcap.
* Sockets: `ss` has no JSON output; the fixed-column `-H -O` form is parsed.
* `tests/` holds developer test aids (not built, not installed).

## What was tested, and how

Everything below was run in the Debian build chroot (root, no systemd, no
polkit, no hardware).

* **Build:** `NEXUS_JOBS=2 scripts/dev.sh build --dir netcenter --only network-center` — no compiler warnings.
* **Smoke test:** `tests/desktop/smoke.sh --dir netcenter network-center` — 10 of 10 launches pass (one per page), no QML warnings. The same ten launches were repeated with a live NetworkManager (below): no QML warnings.
* **Kernel data:** `--dump interfaces|routes|sockets|overview|dns|capture` compared with `ip -j address`, `ip -j route show table all` and `ss` (same interfaces, MAC, MTU, route and socket counts; a listener started as user `nobody` appears with its process).
* **Unavailable state:** without a system bus every NetworkManager page and `--dump wifi|connections|vpn` report "NetworkManager cannot be reached: the system message bus is not available (…)"; screenshots `netcenter-overview.png`, `netcenter-wifi.png`.
* **Diagnostics** (`tests/diagnostics-loopback.sh`, back-end driven by `tests/harness`): ping 127.0.0.1 (live output, 3/3), unanswered ping, cancelling a running ping, traceroute to 127.0.0.1, port check open / refused / invalid port, MTU probe on lo, DNS lookup (dig; the sandbox has no DNS server, reported as such), troubleshooting checklist (link, address, gateway ping all real; DNS and HTTPS failures reported correctly). Hostile targets (`-c 1 127.0.0.1`, `127.0.0.1;id`, `$(id)`, `-h`, `localhost -x`) are refused before anything is started.
* **Helper** (`tests/helper-hostile-arguments.sh`): 28 hostile invocations (unknown sub-commands and options, `-w`/`--write`, interface `lo;id`, `../../etc`, `-w`, newline, non-existent interface, filters beginning with `-`, containing `;`, `$()`, backticks, quotes, newlines, over-long, bad snaplen) all exit with status 2 and a JSON error before any tool is started, and create no file; a syntactically wrong but harmless filter reaches tcpdump and fails there; not-root exits 77; `sockets` returns the same lines as `ss`; a real capture on `lo` with a filter produced a valid pcap, with tcpdump running as uid `tcpdump`, effective capabilities 0, bounding set `0x30c0`, `NoNewPrivs 1`.
* **Capture from the GUI** (nested KWin session, `scripts/dev.sh nested`, xdotool): invalid filter `tcp; id` refused with the rule shown (`netcenter-n-capture1-invalid.png`); capture on eth0 with filter `icmp`, live counters (`netcenter-n-capture2-running.png`), Stop; resulting file read back with `tcpdump -r`; directory 0700, file 0600.
* **NetworkManager, real daemon (1.52.1):** `tests/nm-sandbox.sh` starts NetworkManager and a private system bus inside a private network and mount namespace (lo and a veth pair only), so the machine's network is not touched.
  * read side compared with `nmcli` (`--dump connections|wifi|vpn|overview|dns`): devices, states, addresses, profiles, IPv4/IPv6 settings incl. DNS in both wire formats;
  * operations through the D-Bus code (`tests/nm-operations.sh`): activate, deactivate, connect/disconnect device, delete, autoconnect, IP/DNS/search/DoT change with and without re-activation (verified with `nmcli connection show`, `ip addr`, `ip route`, ping over the veth), update of a Wi-Fi profile keeps its stored passphrase, Wi-Fi radio off/on, WireGuard import, refused imports, failure messages (no suitable device, DHCP timeout, kernel without WireGuard);
  * GUI against it (Xvfb + xdotool): IP page edit → review dialog → "Save and re-activate" → verified with `nmcli` (`netcenter-nm-ip-edit.png`, `-review.png`, `-applied.png`); pages `netcenter-nm-overview|interfaces|ip|dns|vpn|wifi.png`.

## Not tested

* **Wi-Fi against hardware:** scan (`RequestScan`, access-point list), connecting to a new network with a password (`AddAndActivateConnection` with a `802-11-wireless-security` setting), wrong-password handling, OWE/SAE selection, hidden networks, the current-link section. There is no wireless device in the chroot; only saved Wi-Fi profiles (list, update, forget) and the radio switch were exercised.
* **The secret prompt:** `secretsRequired` → password dialog → `nmcli … passwd-file /dev/stdin`. Nothing in the sandbox can ask for a secret; only its failure path ran.
* **OpenVPN** import and activation (plug-in not installed); **WireGuard activation** (kernel of the build machine has no WireGuard; import, listing, autoconnect, removal were tested, activation showed NetworkManager's error).
* **pkexec / polkit:** the chroot runs as root, so the helper was started directly. The policy file is well-formed and names the installed helper path, but no authentication dialog, `argv1` action selection or `auth_admin_keep` behaviour was observed.
* **NetworkManager as an unprivileged user** (its polkit prompts, permission errors).
* **dumpcap** back-end and **Wireshark** hand-over (neither is installed in the chroot).
* **systemd-resolved** (`org.freedesktop.resolve1`) part of the DNS page.
* In the namespace used for the NetworkManager tests `/sys` still shows the outer namespace, so per-interface counters of the test interfaces read "—" there; on a normal system the counters were verified on eth0.
