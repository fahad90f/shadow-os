# Encrypted DNS and private MAC addresses

Package `shadow-hardening`. Program `shadow-netprivacy` (root; the graphical
tools call it through `pkexec`, polkit action `org.shadow.netprivacy.manage`,
administrator authentication kept for a short time). `shadow-netprivacy status`
works for every user.

## What is on by default

| What | Default | Where |
|---|---|---|
| Resolver | systemd-resolved; `/etc/resolv.conf` is its stub link (`127.0.0.53`) | `systemd-resolved` package, `/usr/lib/NetworkManager/conf.d/80-shadow-privacy.conf` (`dns=systemd-resolved`) |
| Encryption | DNS-over-TLS (port 853) to the provider whenever it answers there; otherwise plain DNS to the same provider ("opportunistic") | `/usr/lib/systemd/resolved.conf.d/80-shadow-privacy.conf` |
| Provider | Quad9 (`9.9.9.9`, `149.112.112.112`, `2620:fe::fe`, `2620:fe::9`, TLS name `dns.quad9.net`) | same |
| Who sees your names | the provider, except names under the search domains of the network you are on, which go to that network's DNS server (encrypted only if that server offers DNS-over-TLS): `Domains=~.` keeps every other name from it. A network chooses its own search domains (DHCP), normally `lan`, `home.arpa` or a company domain, but a hostile one can claim any suffix, even a whole top-level domain such as `com` | same |
| DNSSEC | checked where the answer is signed; unsigned answers and networks that break DNSSEC are accepted (`allow-downgrade`) | same |
| Fallback servers | none: nothing else is asked when the provider cannot be reached | same |
| LLMNR, multicast DNS | off (the computer does not answer or announce its name on the local network) | resolved file and NetworkManager file |
| MAC address | a private address per network and per boot: `connection.stable-id=${CONNECTION}/${BOOT}` with `cloned-mac-address=stable`; random addresses while scanning | NetworkManager file |
| DHCP | the host name is not sent; the lease is released when a connection goes down | NetworkManager file |

Not installed, on purpose: `libnss-resolve`. `/etc/nsswitch.conf` keeps
`hosts: files dns`, so every program looks names up through
`/etc/resolv.conf`, where Hide Me's DNS-over-Tor and its port-53 redirect see
every lookup. `shadow-hardening` declares `Conflicts: libnss-resolve` (APT
then never installs it as a recommendation of `systemd-resolved`, and a
package that depends on it, such as `openvpn-systemd-resolved`, cannot be
installed next to Shadow's baseline; NetworkManager does that job for OpenVPN
already). The image build fails if the package is in the image.

## Choosing

```sh
shadow-netprivacy status                       # what is in force (also --json)
sudo shadow-netprivacy dns dot                 # the default
sudo shadow-netprivacy dns dot-strict          # DNS-over-TLS only
sudo shadow-netprivacy dns doh                 # DNS-over-HTTPS (needs dnscrypt-proxy)
sudo shadow-netprivacy dns off                 # the network's own DNS servers
sudo shadow-netprivacy dns dot --provider mullvad --dnssec yes
sudo shadow-netprivacy dns dot-strict --provider custom:192.0.2.53#dns.example.net
sudo shadow-netprivacy mac per-boot|per-network|hardware
sudo shadow-netprivacy mac-exception add <connection UUID>   # nmcli -f NAME,UUID connection show
```

| DNS mode | Encrypted | When the network blocks port 853 | Notes |
|---|---|---|---|
| `dot` | when the provider answers on 853 | plain DNS to the provider | an attacker on the network can force the plain fallback |
| `dot-strict` | always | no name resolves (log-in pages too) | `scripts/fix-network.sh` can let that one network's server answer (below) |
| `doh` | always (HTTPS, port 443) | works | `dnscrypt-proxy` on `127.0.2.1:53` behind systemd-resolved; opt-in |
| `off` | only if the network's server offers DNS-over-TLS | — | DHCP-provided servers |

Providers: `quad9` (filters known malicious domains), `mullvad` (no
filtering), `cloudflare`, or `custom:<ip>#<TLS name>[,…]` (DNS-over-TLS only;
the server is given by address, never by name). Choosing a provider decides
who sees every name this computer looks up; it is a trust decision.

`--dnssec yes` rejects unsigned and broken answers. It fails behind many
routers and captive portals, through Tor (Hide Me) and while the clock is
wrong; `allow-downgrade` is the default for that reason.

What the program writes, and nothing else:

| File | Written by | Purpose |
|---|---|---|
| `/etc/systemd/resolved.conf.d/90-shadow-user.conf` | `dns` (removed when the default is chosen again) | the choice; wins over the shipped file |
| `/etc/NetworkManager/conf.d/90-shadow-mac.conf` | `mac` (removed for `per-boot`) | the MAC policy |
| `/etc/shadow-os/dnscrypt-proxy.toml`, `/etc/systemd/system/dnscrypt-proxy.service.d/50-shadow-netprivacy.conf` | `dns doh` (removed when leaving DoH) | dnscrypt-proxy's configuration: the provider's DoH servers by address (no resolver list is downloaded), no query or NX log |
| `/run/systemd/resolved.conf.d/93-shadow-escape.conf`, `/run/NetworkManager/conf.d/93-shadow-escape.conf` | `boot`, only with `shadow.netprivacy=off` | the escape hatch |

A DNS change restarts systemd-resolved and is then read back from it over
D-Bus; if resolved does not come back with the new setting, the previous
file is put back and the command fails. DoH is switched on only after
dnscrypt-proxy has answered a query; otherwise nothing changes. Entering DoH
turns Debian's `dnscrypt-proxy-resolvconf.service` off (resolved forwards to
the proxy instead); leaving it disables `dnscrypt-proxy.socket`/`.service`.
Note that Debian's own `dnscrypt-proxy.toml` (used when the package is
installed but DoH mode is not chosen) logs every query to
`/var/log/dnscrypt-proxy/query.log`.

## MAC addresses

| Mode | Address a network sees | Across reboots | Effect on DHCP |
|---|---|---|---|
| `per-boot` (default) | private, different for every network | new each boot | at most one new lease per network and boot; leases are released on disconnect |
| `per-network` | private, different for every network | the same (from `/var/lib/NetworkManager/secret_key`) | one lease per network |
| `hardware` | the real address | the same | — |

None of the modes uses a new random address per connection: routers block
the "new device" after each reconnect, DHCP pools run out, log-in pages forget
the device, and some Wi-Fi chips stop receiving. Nothing is reconnected when a
mode changes; each network takes its new address the next time it connects.
Drivers that cannot change the MAC address (`wl`, `eagle_sdio`, Debian's
`no-mac-addr-change.conf`) keep their own address.

The DHCPv6 client identifier follows the same policy:
`ipv6.dhcp-duid=stable-uuid` derives it from the same stable-id, so it is
different for every network and new each boot (per network in `per-network`
mode and with an exception, new with Hide Me's "rotate identity"). Left
unset, NetworkManager would send one identifier made from `/etc/machine-id`
on every network and at every boot, to every router that offers DHCPv6
(also with only the "other configuration" flag) and through the kill
switch, which lets DHCPv6 out: it would link all the private addresses above.
The DHCPv4 client identifier is the (private) MAC address already.

`mac-exception add <uuid>` sets `connection.stable-id=${CONNECTION}` on that
profile: one private address for that network across boots (a MAC allowlist,
a log-in page that should remember the device). It refuses a profile that
already has its own stable-id. Networks that only accept the real hardware
address: `nmcli connection modify <name> wifi.cloned-mac-address permanent`.
In a live session `/var/lib/NetworkManager/secret_key` is new at every boot,
so even `per-network` and exceptions change per boot there. Hide Me's
"rotate identity" changes the address only when asked, through a per-boot salt
in the stable-id ([hardening.md](hardening.md#hide-me-shadow-anon)); profiles with an exception
keep their address.

## In Network Center

**Network Center → Privacy** (`applications/network-center/netprivacy.h`,
`pages/PrivacyPage.qml`) shows what `shadow-netprivacy status --json`
reports — the mode, provider and DNSSEC in force, whether lookups are
encrypted (from systemd-resolved, not from the files), the server in use, the
tool's errors and warnings, Hide Me's override and the escape hatch — and
changes it with `pkexec /usr/sbin/shadow-netprivacy`:

| On the page | Command |
|---|---|
| Encrypted DNS: Automatic DoT / Strict DoT / DoH / Off, provider (Quad9, Mullvad, Cloudflare, your own `address#TLS name` servers), DNSSEC, then **Apply** | `dns dot\|dot-strict\|doh\|off [--provider …] --dnssec …` |
| Hardware address: New every boot / Stable per network / Real address | `mac per-boot\|per-network\|hardware` |
| Keep a network's address (per Wi-Fi or wired profile) | `mac-exception add\|remove <uuid>` |

The page states the trade-off in one line: Strict DoT finds no names where
port 853 is blocked or before a hotel or train Wi-Fi log-in page; Automatic
DoT falls back to plain DNS there, which someone on that network can force.
The DNS page links to it.

## Hide Me, VPNs, captive portals

* **Hide Me (DNS over Tor)** writes `/run/systemd/resolved.conf.d/95-shadow-anon.conf`
  while its DNS protection is on: `DNS=` and `Domains=` emptied first (they
  are lists that add up across files; without that, the provider chosen here
  stays first and gets every name in plain text), then Tor's DNSPort
  `127.0.0.1:26` as the only server, `Domains=~.`, no fallback, no TLS, no
  DNSSEC. The port is below 1024, so only root may bind it: earlier versions
  used 5353, where any local user could bind a socket next to Tor's (Tor
  sets `SO_REUSEADDR`, so may anyone else) and read and answer every name. It wins over every setting here. `status` shows
  `"override": "hide-me"` and `"encrypted": "tor"` only while Tor's DNSPort is
  the only global server in the files and the server resolved reports in use
  (if any); otherwise it reports what is in force and an error. Hide Me's
  redirect sends resolved's queries to a network's own server (names under
  its search domains) to Tor as well, so such names (`printer.lan`) do not
  resolve while it is on. Tor's DNSPort answers UDP only, so DNS over TCP
  is refused while it is on: resolved's DNS-over-TLS to a network's server
  (the per-connection setting; resolved then falls back to plain DNS, i.e.
  Tor), its TCP retries to Tor's DNSPort and every program's TCP port 53 go
  to a closed loopback port only root may bind (port 1). Over IPv6 nothing
  goes to Tor (its DNSPort is IPv4 only): every IPv6 port-53 query and
  resolved's DNS-over-TLS to a network's IPv6 server are refused the same way
  (port 1 on `::1`), so resolved asks the network's IPv4 server, i.e. Tor. A
  network whose only DNS servers are IPv6 cannot resolve names under its own
  domains while Hide Me's DNS protection is on.
* **VPNs**: NetworkManager gives each VPN's DNS server its search domains;
  with `Domains=~.` names outside those domains go to the provider (through the
  tunnel when the VPN carries the default route). A VPN that must answer every
  name needs a negative DNS priority on its profile
  (`nmcli connection modify <vpn> ipv4.dns-priority -50`).
* **Captive portals** ("log in to this network" pages): NetworkManager's
  connectivity check resolves through the network's own server and still
  detects the portal. With `dot` the browser reaches the portal through the
  plain fallback on most networks; with `dot-strict` it does not: run
  `bash scripts/fix-network.sh`, or switch to `dot` until you are logged in.
* **A network that blocks port 853** (or every DNS server but its own):
  `scripts/fix-network.sh` adds the routing domain `~.` to that one profile,
  only when a connection to the provider's port 853 fails and only if that
  makes names work; the change is in its undo script.

## Escape hatch and recovery

* Kernel command line `shadow.netprivacy=off` (GRUB: `e`, add it to the
  `linux` line): for that boot the network's own DNS servers are used without
  encryption or DNSSEC, and every device keeps its own MAC address.
  `shadow-netprivacy.service` writes overrides into `/run`, before
  systemd-resolved and NetworkManager start; `status` shows
  `"escape_hatch": true`. Network Center → Privacy and Control Center →
  Privacy → Network identity both say "Real addresses for this boot" (the
  latter reads `[connection-shadow-escape]` of
  `/run/NetworkManager/conf.d/93-shadow-escape.conf`, which applies to every
  device before the `[connection]` defaults).
* From a text console as root: `shadow-netprivacy dns off` and
  `shadow-netprivacy mac hardware`, or delete
  `/etc/systemd/resolved.conf.d/90-shadow-user.conf` and
  `/etc/NetworkManager/conf.d/90-shadow-mac.conf` and run
  `systemctl restart systemd-resolved; nmcli general reload conf`.
* `systemctl restart systemd-resolved` when names stop resolving;
  `resolvectl status` shows the servers and settings per network.

## Status JSON (`shadow-netprivacy status --json`)

Read by Network Center. It describes what is in force, not what was asked
for: the `live` values come from systemd-resolved over D-Bus.

```json
{
  "version": 1,
  "escape_hatch": false,
  "dns": {
    "mode": "dot",                       // dot | dot-strict | doh | off | plain (fixed servers, no TLS: Hide Me)
    "provider": "quad9",                 // quad9 | mullvad | cloudflare | custom | network | unknown
    "servers": ["9.9.9.9#dns.quad9.net", "..."],
    "dns_over_tls": "opportunistic",     // from the files: yes | opportunistic | no
    "dnssec": "allow-downgrade",         // yes | allow-downgrade | no
    "all_names_to_provider": true,       // Domains=~.
    "fallback_servers": [],
    "llmnr": "no",
    "mdns": "no",
    "source": "default",                 // default | user | none
    "override": null,                    // null | "hide-me" | "escape"
    "resolver_installed": true,
    "resolver_active": true,
    "resolv_conf": {"kind": "stub", "target": "../run/systemd/resolve/stub-resolv.conf", "dangling": false},
                                         // kind: stub | stub-copy | resolved-uplinks | networkmanager | other-link | file | missing
    "nss_resolve": false,
    "files": ["/etc/systemd/resolved.conf", "/usr/lib/systemd/resolved.conf.d/80-shadow-privacy.conf"],
    "live": {"dns_over_tls": "opportunistic", "dnssec": "allow-downgrade", "llmnr": "no", "mdns": "no",
             "current_server": "9.9.9.9#dns.quad9.net"},          // null when resolved does not answer
    "doh": {"installed": false, "configured": false, "active": false, "listen": "127.0.2.1:53"},
    "encrypted": "opportunistic"         // always | opportunistic | partly | no | tor | unknown
  },
  "mac": {
    "mode": "per-boot",                  // per-boot | per-network | hardware | random | custom
    "stable_id": "${CONNECTION}/${BOOT}",
    "wifi": "stable", "ethernet": "stable",
    "scan_randomization": true,
    "source": "default",                 // default | user | none
    "override": null,                    // null | "escape"
    "fixed_drivers": ["eagle_sdio", "wl"],
    "exceptions": ["<connection UUID>"],
    "exceptions_known": true,            // false when nmcli could not be asked
    "dns_plugin": "systemd-resolved",
    "files": ["..."]
  },
  "errors": [],                          // a protection that is not working (e.g. resolved stopped)
  "warnings": []                         // a bypass or a mismatch (plain resolv.conf, nss-resolve, ...)
}
```

## Tests

* `tests/security/test_netprivacy.py`: shipped files (every NetworkManager
  value is one 1.52 accepts), validation, rendering, DoH stamps, every
  command with a fake system, status honesty, escape hatch, packaging.
* `tests/network/resolved-lab-test.sh`: the real systemd-resolved 257 of
  trixie in private namespaces against a fake router and a fake provider with
  DNS-over-TLS: routing with `Domains=~.`, opportunistic and strict TLS,
  certificate checking, the per-network relaxation, and Hide Me's drop-in and
  redirect (Tor's DNSPort, a stand-in, as the only global server; the
  network's search domains and its DNS-over-TLS sent to Tor; an
  unprivileged user can bind Tor's DNSPort neither over UDP, next to Tor's
  socket, nor over TCP, and gets no name, also when Tor answers truncated or
  not at all; controls show that on the old port 5353 it gets them).
* `tests/network/fix-network-test.sh`: the repairs with systemd-resolved.
* `tests/build/in-chroot-resolver-test.sh`: name resolution in image-build
  hooks when `/etc/resolv.conf` is resolved's stub link.
* `applications/network-center/tests/test_netprivacy_backend.py`: the Network
  Center page's back-end against a stand-in tool (status documents, the exact
  `pkexec` command lines, input refused before anything runs) and against the
  real `shadow-netprivacy status --json`.
