# security/hardening

Configuration files of the `shadow-hardening` package (the security
baseline, [docs/security/hardening.md](../../docs/security/hardening.md)) and
`shadow-netprivacy`, the tool for encrypted DNS and MAC address privacy
([docs/security/private-dns.md](../../docs/security/private-dns.md)).

| File | Installed as | What |
|---|---|---|
| `sysctl.d/60-shadow-hardening.conf` | `/usr/lib/sysctl.d/` | kernel settings |
| `modprobe.d/shadow-hardening.conf` | `/usr/lib/modprobe.d/` | no implicit loading of rare protocols and filesystems |
| `grub.d/20-shadow-hardening.cfg` | `/etc/default/grub.d/` | kernel command line |
| `journald.conf.d/shadow.conf` | `/usr/lib/systemd/journald.conf.d/` | journal limits |
| `sudoers.d/shadow-hardening` | `/etc/sudoers.d/` | sudo defaults |
| `NetworkManager/80-shadow-privacy.conf` | `/usr/lib/NetworkManager/conf.d/` | DNS through systemd-resolved; a private MAC address per network and per boot; no host name in DHCP; no LLMNR/mDNS |
| `resolved.conf.d/80-shadow-privacy.conf` | `/usr/lib/systemd/resolved.conf.d/` | DNS-over-TLS to Quad9 (opportunistic), every name to the provider (`Domains=~.`), DNSSEC allow-downgrade, no fallback servers |
| `shadow-netprivacy` | `/usr/sbin/shadow-netprivacy` | `status [--json]`, `dns {dot,dot-strict,doh,off}`, `mac {per-boot,per-network,hardware}`, `mac-exception {add,remove} <uuid>`, `boot` |
| `systemd/shadow-netprivacy.service` | `/usr/lib/systemd/system/` | the escape hatch: with `shadow.netprivacy=off` on the kernel command line, that boot uses the network's own DNS and the devices' own MAC addresses |

`shadow-netprivacy` follows the conventions of `../firewall/`: Python
standard library, argparse, a `Failure` exception, `run()` that checks every
return code, atomic writes, validation before anything changes, and a
`status --json` that reports what systemd-resolved and NetworkManager have in
force. It writes only `/etc/systemd/resolved.conf.d/90-shadow-user.conf`,
`/etc/NetworkManager/conf.d/90-shadow-mac.conf` and, in DNS-over-HTTPS mode,
dnscrypt-proxy's configuration and service drop-in. Its polkit action
`org.shadow.netprivacy.manage` is in `../permissions/org.shadow.network.policy`.
`ROOT` (a path prefix) is only changed by the tests.

NetworkManager 1.52 reads most `[connection]` defaults as integers: write
`0`/`1`, never `no`/`yes`, or the value is silently ignored
(`tests/security/test_netprivacy.py` checks every key).

Tests: `tests/security/test_netprivacy.py`, `tests/network/resolved-lab-test.sh`
(real systemd-resolved in namespaces), `tests/security/test_hardening_packaging.py`.
