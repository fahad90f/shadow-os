# security/firewall

Root tools and files of the `shadow-hardening` package that own nftables
tables. Each tool owns its tables (the kill switch and the panic an `inet` and
a `bridge` table each, for bridged virtual machines), replaces them atomically
and never runs `nft flush ruleset`.

| File | Installed as | What |
|---|---|---|
| `nftables.conf` | `/etc/nftables.conf` | the host firewall, `inet shadow_filter` (inbound default-deny) |
| `shadow-firewall` | `/usr/sbin/shadow-firewall` | manages it: opened ports, enable/disable |
| `shadow-killswitch` | `/usr/sbin/shadow-killswitch` | the kill switch, `inet shadow_egress` (output/forward at -50, postrouting drops loopback destinations on a real device) and `bridge shadow_egress` (bridged guests): nothing leaves except through Tor or a VPN |
| `shadow-appfw` | `/usr/sbin/shadow-appfw` | the per-application firewall, `inet shadow_appfw` (output at -40): one rule rejects whatever is sent from a socket created in the system slice `shadowblock.slice`, loopback included. Which applications are blocked: `/etc/shadow-os/appfw.json` |
| `shadow-appfw-run` | `/usr/bin/shadow-appfw-run` | starts a blocked application: asks to be confined, then becomes the application. The launcher entries `shadow-appfw` writes into `/usr/local/share/applications` run it |
| `shadow-appfw-enter` | `/usr/sbin/shadow-appfw-enter` | the one request an ordinary user may make without a password ("confine my starting process"), reached through pkexec |
| `systemd/shadow-appfw.service` | `/usr/lib/systemd/system/` | at boot: writes the launcher entries again and loads the rule when something is blocked |
| `shadow-panic` | `/usr/sbin/shadow-panic` | panic, `inet shadow_panic` (input/output/forward at -300) and `bridge shadow_panic`: everything but loopback dropped, NetworkManager off, radios blocked |
| `systemd/shadow-killswitch.service` | `/usr/lib/systemd/system/` | applies the saved kill switch mode before `network-pre.target` |
| `systemd/shadow-panic.service` | `/usr/lib/systemd/system/` | `shadow-panic engage`; the desktop starts it over D-Bus |
| `systemd/shadow-panic-reset.service` | `/usr/lib/systemd/system/` | at boot, undoes what a panic left in NetworkManager's and systemd-rfkill's saved state |
| `systemd/shadow-killswitch-watch.timer`, `.service` | `/usr/lib/systemd/system/` | the watchdog: every 20 s, `shadow-panic reassert`, `shadow-appfw reassert` and `shadow-killswitch reassert --watch` put back a panic, per-application or kill switch table that something (for example `nft flush ruleset`) removed or changed; silent and without writes while they are intact |
| `systemd/nftables.service.d/shadow.conf` | `/usr/lib/systemd/system/nftables.service.d/` | stopping nftables.service deletes only `inet shadow_filter` instead of flushing every table; starting it reasserts a panic and the kill switch |
| `NetworkManager/50-shadow-killswitch` | `/usr/lib/NetworkManager/dispatcher.d/` (and `pre-up.d/`) | `shadow-killswitch sync` when interfaces and VPNs go up or down |

The polkit files are in `../permissions/`: `org.shadow.network.policy`
(actions `org.shadow.killswitch.manage`, `org.shadow.panic.release`),
`49-shadow-panic.rules` (starting `shadow-panic.service` without a password)
and, in `org.shadow.security.policy`, `org.shadow.appfw.manage` (an
administrator changes what is blocked) and `org.shadow.appfw.enter` (the
active local session may have its own starting process confined).

Conventions shared by the Python tools (standard library only): an
argparse CLI, a `Failure` exception, `run()` that checks every return code,
`status --json`, atomic writes (temporary file plus `os.replace`), and
`nft --check` before anything is loaded. The kill switch and panic also read
their table back from the kernel after every change and record the result in
`/run/shadow/*.json`; a status never claims a table that is not loaded.
Their path constants are only changed by the tests.

Documentation: [docs/security/hardening.md](../../docs/security/hardening.md)
(firewall) and [docs/security/kill-switch.md](../../docs/security/kill-switch.md)
(kill switch, panic, recovery).

Tests: `tests/security/test_killswitch_unit.py` (any user),
`tests/network/test_killswitch_netns.py` (root; real packets in a private
network namespace), `tests/security/test_security_status.py`.
