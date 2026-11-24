# Security baseline

Every security setting Shadow OS ships, what it protects against, and what
it can break. All of it is in one removable package, `shadow-hardening`
(source: `security/`), plus the installer's disk-encryption parameters and
the boot chain.

> **Scope and verification.** `shadow-hardening` is part of the desktop
> image, not of the minimal image. Verified: `shadow-firewall` in a
> functional test inside an isolated network namespace; during the first
> boot of the desktop image a bug in the audit rules was found and fixed.
> Everything else is implemented and has not been verified as a whole on an
> installed system. Nothing has run on real hardware.

Principle from the package description: none of these settings disables a
security mechanism to make software work. Where something can break, the
table says how to change the one setting concerned.

## Hardening level

Three sysctl settings trade compatibility for security, so they are chosen by a
**hardening level** instead of being fixed. Set it in the Control Center under
Privacy → *OS Integrity & Kernel Lockdown*, or on the command line:

```bash
shadow-harden level            # show the current level and what each changes
sudo shadow-harden level hardened
```

| Level | `unprivileged_userns_clone` | `unprivileged_bpf_disabled` | `perf_event_paranoid` | Effect |
|---|---|---|---|---|
| `standard` | 1 | 0 | 2 | Everyday desktop; nothing that commonly breaks apps. |
| `hardened` *(default)* | 1 | 1 | 3 | Strong limits, but browser and Flatpak sandboxes still work. |
| `paranoid` | 0 | 2 | 4 | Maximum. **Breaks** web-browser and Flatpak sandboxes and most container tools. |

The level is written to `/etc/sysctl.d/99-shadow-level.conf` and recorded in
`/etc/shadow-os/hardening-level`; it is applied at once and again at every boot.
The default is `hardened` with unprivileged user namespaces **on**, so browsers
and Flatpak work out of the box — the one setting most likely to break apps is
only disabled at `paranoid`. Everything in `91-shadow-extreme.conf` is applied
at every level.

## Overview

| Area | File on the system | Source |
|---|---|---|
| Kernel settings (sysctl) | `/usr/lib/sysctl.d/60-shadow-hardening.conf` | `security/hardening/sysctl.d/` |
| Kernel command line | `/etc/default/grub.d/20-shadow-hardening.cfg` | `security/hardening/grub.d/` |
| Module auto-loading | `/usr/lib/modprobe.d/shadow-hardening.conf` | `security/hardening/modprobe.d/` |
| Firewall | `/etc/nftables.conf`, `/usr/sbin/shadow-firewall`, `/etc/shadow-os/firewall.json`, `/etc/nftables.d/` | `security/firewall/` |
| Audit rules | `/etc/audit/rules.d/50-shadow.rules` | `security/audit/` |
| Journal | `/usr/lib/systemd/journald.conf.d/shadow.conf` | `security/hardening/journald.conf.d/` |
| sudo | `/etc/sudoers.d/shadow-hardening` | `security/hardening/sudoers.d/` |
| NetworkManager privacy | `/usr/lib/NetworkManager/conf.d/80-shadow-privacy.conf` | `security/hardening/NetworkManager/` |
| Status report | `/usr/sbin/shadow-security-status` | `security/audit/` |
| Hardening level, AIDE, AppArmor, boot and filesystem hardening (Control Center → Privacy) | `/usr/bin/shadow-harden` | `distro/base/rootfs/usr/bin/` |
| polkit actions | `/usr/share/polkit-1/actions/org.shadow.security.policy` | `security/permissions/` |

The package depends on `nftables`, `apparmor`, `auditd` and `sudo`, and
recommends `apparmor-profiles`, `apparmor-profiles-extra` and
`apparmor-utils` (Recommends are not installed in the image).

On first installation the package enables `nftables.service`, applies the
sysctl settings on a running system, and reloads an active firewall. Debian's
`/etc/nftables.conf` is kept as `/etc/nftables.conf.debian` through a dpkg
diversion. The Shadow ruleset in its place is a configuration file of the
package: like every configuration file it stays when the package is removed
(and the diversion with it) and goes when the package is purged, which puts
Debian's file back.

`shadow-harden` is in the package (in 0.2.0 it was only copied into the
image, so upgrades never replaced it). An upgrade rewrites a
`proc-shadow-hidepid.service` that `shadow-harden fs-harden` of 0.2.0 wrote
(`hidepid=2` without the group `proc` and without the escape hatch
`shadow.hidepid=off`, which broke pkexec at every start) the way fs-harden
writes it now, and on the running system remounts `/proc` with the group at
once (`shadow-harden migrate-hidepid`). If polkitd cannot be put into the
group, the unit is removed instead and nothing is hidden.

Removing the package on the running system first releases an engaged panic
(networking and radios come back), then deletes the kill switch and panic
tables and Hide Me's redirects, so nothing keeps cutting or redirecting the
network once the programs that undo it are gone. Inside a chroot (the
installer, the recovery tools and `docs/troubleshooting/recovery.md` bind-mount
the live `/run` into theirs) the maintainer scripts touch none of the running
system's network state and reload nothing: they check that `/` is the root
of process 1 (the test `systemctl` uses), not only `/run/systemd/system`. A
container that shares the computer's network and boots its own systemd
(`systemd-nspawn -b` without `--private-network`) counts as a running system.

## Kernel settings (sysctl)

To change a value, do not edit the shipped file. Create
`/etc/sysctl.d/90-local.conf` with the keys you want — files in `/etc`
override it — and run `sudo sysctl --system`.

Settings marked ° are prefixed with `-` in the file: they are ignored if the
running kernel does not have the key.

### Information leaks

| Setting | Protects against | Can break |
|---|---|---|
| `kernel.kptr_restrict = 2` | kernel addresses in `/proc` and logs, which defeat KASLR | profilers and debuggers that need kernel symbols (`perf` with kernel symbols) show zeros, even as root |
| `kernel.dmesg_restrict = 1` | unprivileged users reading the kernel log | `dmesg` needs `sudo` |

### Attack surface reachable by unprivileged users

| Setting | Protects against | Can break |
|---|---|---|
| `kernel.unprivileged_bpf_disabled = 1` | unprivileged eBPF, a recurring source of kernel privilege escalations | eBPF tools need root |
| `net.core.bpf_jit_harden = 2` | JIT-spraying through the BPF JIT, for privileged users too | some BPF tracing output is less detailed; small overhead |
| `kernel.perf_event_paranoid = 3` ° | `perf_event_open(2)` for users without `CAP_PERFMON` (Debian carries the patch for level 3) | `perf` and other profilers need root or the capability |
| `vm.unprivileged_userfaultfd = 0` ° | `userfaultfd` as an exploitation primitive | unprivileged programs using userfaultfd (some VM live-migration and checkpoint tools) need privileges |
| `dev.tty.ldisc_autoload = 0` ° | automatic loading of TTY line-discipline modules | exotic serial protocols need their module loaded by root first |
| `dev.tty.legacy_tiocsti = 0` ° | keystroke injection into a terminal (`TIOCSTI`) | very old tools that push characters into the terminal |
| `kernel.kexec_load_disabled = 1` | replacing the running kernel without a reboot | `kexec` and kdump do not work until the next boot with a different setting |

### Process isolation

| Setting | Protects against | Can break |
|---|---|---|
| `kernel.yama.ptrace_scope = 1` | one process of a user reading another's memory (credentials in a browser, an agent) | `gdb -p`, `strace -p` on a process that is not a descendant need root. Debuggers that start the program themselves work |
| `fs.suid_dumpable = 0` | secrets from setuid programs in core dumps | no core dumps of setuid programs |
| `fs.protected_symlinks = 1`, `fs.protected_hardlinks = 1` | link-based races in world-writable directories | — |
| `fs.protected_fifos = 2`, `fs.protected_regular = 2` | opening an attacker's FIFO or file in `/tmp` | programs that deliberately share FIFOs or files in sticky directories between users |

### Memory

| Setting | Protects against | Can break |
|---|---|---|
| `kernel.randomize_va_space = 2` | predictable addresses | — |
| `vm.mmap_min_addr = 65536` | NULL-pointer dereference exploits | very old emulators that map low memory |
| `vm.mmap_rnd_bits = 32` °, `vm.mmap_rnd_compat_bits = 16` ° | maximum ASLR entropy the architecture offers | rarely, programs that need large contiguous address ranges |

### Magic SysRq

| Setting | Effect |
|---|---|
| `kernel.sysrq = 176` | only sync (16), remount read-only (32) and reboot (128) are allowed. Process killing, memory dumps and the other functions are off |

### Network: this machine is a host, not a router

| Setting | Protects against | Can break |
|---|---|---|
| `net.ipv4.conf.{all,default}.rp_filter = 2` | spoofed source addresses. Loose mode is used because strict mode breaks multi-homed and VPN set-ups | asymmetric routing over an interface that has no route to the source at all |
| `accept_redirects = 0`, `secure_redirects = 0` (IPv4), `accept_redirects = 0` (IPv6) | ICMP redirects rerouting your traffic | networks that rely on redirects instead of proper routes |
| `send_redirects = 0` | this host sending redirects | only relevant for routers |
| `accept_source_route = 0` (IPv4 and IPv6) | source-routed packets | — |
| `net.ipv4.tcp_syncookies = 1` | SYN floods | — |
| `net.ipv4.tcp_rfc1337 = 1` | TIME-WAIT assassination | — |
| `net.ipv4.icmp_echo_ignore_broadcasts = 1`, `icmp_ignore_bogus_error_responses = 1` | smurf amplification, log noise | — |
| `net.ipv6.conf.{all,default}.use_tempaddr = 2` | tracking through a stable IPv6 address: temporary addresses are preferred for outgoing connections | services that expect connections from a stable IPv6 address |

IP forwarding is not changed by this file. Docker and libvirt switch it on
when they need it.

## Kernel command line

`/etc/default/grub.d/20-shadow-hardening.cfg` appends to
`GRUB_CMDLINE_LINUX`. It takes effect after `update-grub` and a reboot; the
installer runs `update-grub`. To drop a parameter, override
`GRUB_CMDLINE_LINUX` in a later file such as
`/etc/default/grub.d/90-local.cfg` and run `sudo update-grub`.

| Parameter | Protects against | Cost |
|---|---|---|
| `slab_nomerge` | heap overflows reaching objects of another slab cache | slightly more kernel memory |
| `init_on_alloc=1` | leaks and use of uninitialised kernel memory | small allocation overhead |
| `page_alloc.shuffle=1` | predictable page-allocator layout | negligible |
| `randomize_kstack_offset=on` | stack-based attacks that rely on a fixed kernel stack layout | negligible |
| `vsyscall=none` | the legacy fixed-address vsyscall page as an exploitation aid | decades-old statically linked binaries crash |
| `audit=1`, `audit_backlog_limit=8192` | missing audit records for processes started before auditd | a little memory for the backlog |

These parameters apply to Debian's kernel as shipped. The additional
build-time options of the SHADOW kernel configuration are **not** in effect,
because that kernel is not shipped ([../kernel/README.md](../kernel/README.md)).

The live medium's boot menu does not use this file; the live session runs
without these parameters.

## Module auto-loading

`/usr/lib/modprobe.d/shadow-hardening.conf` uses `blacklist`, which stops a
module from being loaded **implicitly** — for example when an unprivileged
program opens a socket of an exotic protocol family, which is how several
kernel privilege escalations were reached. It does not make the module
unavailable: root can still load any of them by name, which matters on a
security workstation where such protocols and filesystems are sometimes
exactly what is being examined.

| Group | Modules |
|---|---|
| Network protocol families with a history of vulnerabilities and no desktop use | `dccp`, `dccp_ipv4`, `dccp_ipv6`, `rds`, `tipc`, `n-hdlc`, `ax25`, `netrom`, `x25`, `rose`, `decnet`, `econet`, `af_802154`, `ipx`, `appletalk`, `psnap`, `p8023`, `p8022`, `atm` |
| Legacy filesystems that are never mounted automatically on a desktop | `cramfs`, `freevxfs`, `jffs2`, `bfs`, `minix`, `qnx4`, `qnx6`, `sysv`, `ufs` |

Can break: software that expects one of these to load on demand (amateur
radio AX.25, IEEE 802.15.4, mounting an old UFS or Minix disk). Load the
module first: `sudo modprobe ufs`. To re-enable auto-loading permanently,
override the file with one of the same name in `/etc/modprobe.d/`.

## AppArmor

* `shadow-hardening` depends on `apparmor`, so the AppArmor service and the
  profiles that Debian packages ship are installed. Debian's kernel enables
  AppArmor by default.
* **Shadow OS ships no AppArmor profiles of its own.** SHADOW applications,
  the shell and the helpers run unconfined.
* `apparmor-profiles`, `apparmor-profiles-extra` and `apparmor-utils` are
  recommended by the package but not installed in the image. Install them
  if you want more profiles and the `aa-status`, `aa-enforce`,
  `aa-complain` tools.
* `shadow-security-status --section apparmor` reports whether the module is
  enabled, the number of profiles per mode and the number of confined
  processes. `/etc/apparmor.d/` is watched by the audit rules.

If a profile blocks a program, look for `apparmor="DENIED"` in
`sudo journalctl -k` and put that one profile into complain mode
(`sudo aa-complain <profile>`, from `apparmor-utils`). Do not disable
AppArmor as a whole.

## Firewall (nftables)

### Default policy

`/etc/nftables.conf` defines one table, `inet shadow_filter`:

| Direction | Policy | Exceptions |
|---|---|---|
| Inbound | **drop** | replies to connections this machine opened; loopback; ICMP and ICMPv6 errors; echo requests, rate-limited to 10 per second; IPv6 neighbour discovery and multicast listener discovery from link-local addresses; DHCPv6 client replies; DNS and DHCP from local virtual machines and containers (`virbr*`, `docker0`, `br-*`, `podman*`, `lxcbr*`); ports you opened |
| Forwarded | **drop** | traffic of local virtual machines and containers leaving through this host, and connections published to them (DNAT) |
| Outbound | **accept** | — |

Only the SHADOW table is replaced when the file is loaded. Tables owned by
Docker, libvirt or a VPN client are left alone.

Can break: anything that expects unsolicited inbound connections — an SSH
or web server on this machine, file sharing, KDE Connect, device discovery
(mDNS, SSDP), listeners for reverse connections during a test. Open the port
you need.

### `shadow-firewall`

Must run as root.

```
shadow-firewall status [--json]
shadow-firewall list [--json]
shadow-firewall enable
shadow-firewall disable
shadow-firewall allow <port>[-<port>]/<tcp|udp> [--name TEXT]
shadow-firewall remove <port>[-<port>]/<tcp|udp>
shadow-firewall reload
shadow-firewall reset
```

| Command | Effect |
|---|---|
| `status` | active or not, starts at boot or not, policies, rule count, open ports, other nftables tables present. Exit status 0 when active, 3 when not |
| `list` | the opened ports |
| `enable` | validates and loads the ruleset and enables `nftables.service` |
| `disable` | disables the service and deletes the SHADOW table: inbound connections are no longer filtered |
| `allow` | opens a port or a port range for inbound connections, with an optional name |
| `remove` | closes it again |
| `reload` | regenerates and loads the ruleset |
| `reset` | closes all opened ports |

Examples:

```sh
sudo shadow-firewall status
sudo shadow-firewall allow 22/tcp --name "SSH"
sudo shadow-firewall allow 8000-8010/tcp --name "lab web servers"
sudo shadow-firewall remove 22/tcp
```

How it works:

* The opened ports are recorded in `/etc/shadow-os/firewall.json`. From it the
  tool generates `/etc/nftables.d/50-shadow-allowed.nft`, which fills the
  sets `allowed_tcp` and `allowed_udp`.
* Every change is validated against the complete ruleset
  (`nft --check --file /etc/nftables.conf`) before it replaces anything. If
  validation fails, the previous file is restored and nothing is applied.
* Loading replaces the table atomically.

Your own rules belong in additional files `/etc/nftables.d/*.nft`; they
survive updates of the shipped file. Use the chains `input_local` and
`forward_local`, which exist as hooks for this:

```
# /etc/nftables.d/60-local.nft
add rule inet shadow_filter input_local iifname "tun0" tcp dport 4444 accept
```

Graphical tools call `shadow-firewall` through `pkexec` (polkit action
`org.shadow.firewall.manage`, administrator authentication). A Control Center
firewall page exists in the source tree but is not part of the image yet.

Limits of this firewall: it filters by port only (no per-application
rules), it does not filter outbound traffic, and `allow` opens a port for
every source address and interface. Per-source rules go into your own
`.nft` file.

Outbound traffic is restricted by the opt-in kill switch
(`shadow-killswitch`: nothing leaves except through Tor or a VPN), and
`shadow-panic` cuts every network at once. Both are tables of their own and
are described in [kill-switch.md](kill-switch.md). The per-application
firewall (`shadow-appfw`, a third table) takes the network away from single
applications: [app-firewall.md](app-firewall.md). `nftables.service`
carries a drop-in from this package: stopping or restarting it deletes only
`inet shadow_filter` instead of flushing every table, so an nftables upgrade
no longer removes the kill switch, Hide Me's chains or Docker's NAT.

A second firewall does not mix with this one: a packet must pass every
table's input chain, so a port opened with `shadow-firewall` stays closed when
firewalld (which Debian enables and starts on installation, with its own
input and forward rejection) or an enabled ufw rejects it, and firewalld also
cuts the network of virtual machines and containers. Neither is installed by
Shadow OS: the Blue Team profile only *suggests* them, and `iptables-persistent`,
which would restore saved iptables rules at boot, is no longer in the image.
firewalld is not masked either: the remaining ways to get it (`firewall-config`,
`firewall-applet`, `freedombox`) are explicit choices of a firewall manager.
Whoever installs one should turn the other off (`sudo shadow-firewall disable`,
or `sudo systemctl disable --now firewalld`); `shadow-firewall status` lists
other tables in `other_tables`.

## Audit rules

`/etc/audit/rules.d/50-shadow.rules`, loaded by `augenrules`. A deliberately
small, low-noise set: changes to identity, privilege and kernel code, which
is what an investigation needs first.

| Key | Watches |
|---|---|
| `identity` | writes and attribute changes to `/etc/passwd`, `shadow`, `group`, `gshadow`, `/etc/security/opasswd` |
| `privilege` | `/etc/sudoers`, `/etc/sudoers.d/`, `/etc/polkit-1/`, `/usr/share/polkit-1/rules.d/` |
| `auth-config` | `/etc/pam.d/`, `/etc/ssh/sshd_config`, `/etc/ssh/sshd_config.d/` |
| `firewall` | `/etc/nftables.conf`, `/etc/nftables.d/` |
| `mac-policy` | `/etc/apparmor.d/` |
| `audit-config` | `/etc/audit/` |
| `modules` | the system calls `init_module`, `finit_module`, `delete_module` by logged-in users; `/etc/modprobe.d/` |
| `time-change` | `adjtimex`, `settimeofday`, `clock_settime` by logged-in users |
| `boot` | `/boot/grub/grub.cfg`, `/etc/default/grub`, `/etc/default/grub.d/`, `/etc/shadow-os/boot.conf` |
| `apt-sources` | `/etc/apt/sources.list.d/`, `/etc/apt/trusted.gpg.d/` |

* The rules start with `-i`: a watched path that does not exist (no SSH
  server, no installed boot loader) is logged and skipped instead of making
  the whole rule set fail to load. That failure was seen on the first boot
  of the desktop image and fixed this way.
* The rules are **not locked** (`-e 2` is not set), so they can be extended
  without a reboot — and an attacker with root can change them too.
* System-call rules cover the 64-bit ABI (`arch=b64`) only.

Search the log with, for example, `sudo ausearch -k identity`.

Cost: audit records in `/var/log/audit/`; the small rule set keeps the
volume low.

## Journal

`/usr/lib/systemd/journald.conf.d/shadow.conf`:

| Setting | Effect |
|---|---|
| `Storage=persistent` | logs survive reboots — needed to investigate anything after the fact |
| `Compress=yes` | compressed |
| `SystemMaxUse=1G`, `SystemKeepFree=2G` | at most 1 GiB of journal, and 2 GiB of disk kept free |
| `MaxRetentionSec=3month` | older entries are deleted |
| `ForwardToSyslog=no` | no second copy through syslog |

Consider the privacy side: three months of logs stay on disk. On an
unencrypted installation they can be read by anyone with physical access.

## sudo

`/etc/sudoers.d/shadow-hardening`:

| Setting | Effect | Can break |
|---|---|---|
| `Defaults use_pty` | commands run in a pseudo-terminal, so a compromised command cannot inject keystrokes into your terminal after it exits | rarely, programs under `sudo` that need the caller's terminal device directly |
| `Defaults passwd_tries=3` | three password attempts | — |
| `Defaults badpass_message=…` | a plain message for a wrong password | — |

Related, set by the installer and the image build rather than this
package: the root account is locked; the first user is a member of `sudo`.
In the live session the live user has passwordless `sudo`.

## NetworkManager privacy and encrypted DNS

`/usr/lib/NetworkManager/conf.d/80-shadow-privacy.conf` and
`/usr/lib/systemd/resolved.conf.d/80-shadow-privacy.conf`. Change them with
`shadow-netprivacy` (encrypted DNS provider and mode, MAC policy, per-network
exceptions); everything about them, including the status JSON and the escape
hatch `shadow.netprivacy=off`, is in [private-dns.md](private-dns.md).
Override single connections with `nmcli` or Control Center.

| Setting | Protects against | Can break |
|---|---|---|
| `dns=systemd-resolved`; resolved with `DNS=` Quad9 (TLS name `dns.quad9.net`), `DNSOverTLS=opportunistic`, `Domains=~.`, `FallbackDNS=` empty | the network (and anyone on it) reading every name looked up; the network's DNS server seeing every name (it answers only for its own domains) | strict mode on networks that block port 853; captive portals that allow only their own DNS (see private-dns.md) |
| `DNSSEC=allow-downgrade` | forged answers for signed domains | — (`yes` breaks behind many routers, through Tor and with a wrong clock) |
| `LLMNR=no`, `MulticastDNS=no`, `connection.llmnr=0`, `connection.mdns=0` | the computer announcing and answering its name on the local network | finding other computers by `name.local` (Avahi is not installed either) |
| `wifi.scan-rand-mac-address=yes` | tracking of the device while it scans for networks | — |
| `connection.stable-id=${CONNECTION}/${BOOT}` with `wifi.cloned-mac-address=stable`, `ethernet.cloned-mac-address=stable` | networks correlating the device with each other (a different address on every network) or across days (a new address every boot); the address stays the same for the whole boot, so DHCP leases and captive-portal log-ins keep working | networks that filter by hardware address: `nmcli connection modify <name> wifi.cloned-mac-address permanent`; networks that should remember the device across boots: `shadow-netprivacy mac-exception add <uuid>`. Drivers that cannot change the address (`wl`, `eagle_sdio`) keep theirs |
| `ipv6.dhcp-duid=stable-uuid` | the DHCPv6 client identifier linking those private addresses: unset, NetworkManager sends one DUID made from `/etc/machine-id` on every network and at every boot (even with only the router's "other configuration" flag, and through the kill switch); now it follows the stable-id like the MAC address, and changes with Hide Me's "rotate identity" | — (a DHCPv6 server sees a new client per network and boot, as with the MAC address) |
| `ipv4/ipv6.dhcp-send-release=1` | an old per-boot address keeping a lease in the router's pool | — |
| `ipv4/ipv6.dhcp-send-hostname=0` | the router learning the computer's name | — (until this release the value was written as `no`, which NetworkManager 1.52 ignores for this integer setting, so the name *was* sent) |
| `ipv6.ip6-privacy=2` | tracking through a stable IPv6 address | services expecting a stable source address |

## Hide Me (`shadow-anon`)

Hide Me (`/usr/bin/shadow-anon`, Control Center → Privacy, or the text panel
`pkexec shadow-anon panel`) is the identity protection engine. It runs as root
under its AppArmor profile `/etc/apparmor.d/usr.bin.shadow-anon`.

### What each protection does

| Protection | How | Command |
|---|---|---|
| Kill switch | Hide Me does not filter traffic itself: it asks `shadow-killswitch` for mode `tor` with the local network blocked (`set tor --no-lan`; see [kill-switch.md](kill-switch.md)), so nothing leaves except Tor's own traffic, loopback, DHCP and IPv6 neighbour discovery — IPv6 and the traffic of routed, NAT and bridged virtual machines and containers included (guests on macvtap/macvlan devices, SR-IOV or passthrough network cards are not covered; see [kill-switch.md](kill-switch.md#what-it-can-break)), and the local network too even when "Allow the local network" was saved for another mode. Mode `tor` with the local network allowed (set in Control Center → Privacy → Network kill switch) does not count as Hide Me's "Tor only": turning Hide Me's kill switch on makes it strict. The mode that was in force before, with its LAN setting, is recorded and put back when Hide Me turns the kill switch off. Until the next restart, unless `--persist` | `kill-switch-on [--persist]`, `kill-switch-off` |
| Transparent Tor proxy | iptables-nft chain `SHADOWTOR` in nat `OUTPUT`: every new TCP connection and every DNS query of every program except Tor goes to Tor's TransPort (TCP `127.0.0.1:40`) and DNSPort (UDP `127.0.0.1:26`); DNS over TCP (port 53) is refused, because Tor's DNSPort has no TCP (see DNS-over-Tor below). Both ports are below 1024, so no local user can bind them, also not with `SO_REUSEADDR` next to Tor's socket or while Tor restarts; earlier versions used 9040 and 5353, where any user (a Flatpak app included) could take every redirected connection and query. So is Tor's SOCKS port (see "Tor's SOCKS port" below): Tor exits when it cannot bind any one of its ports, so a user who held Debian's 9050 while Tor was stopped kept these from opening. Tor binds them as root before it becomes `debian-tor` and keeps the capability for reloads (`KeepBindCapabilities 1` in Hide Me's torrc.d files; Tor is restarted, never reloaded, when those change). Turning the proxy on fails when `tor@default` does not run afterwards (Debian's `tor.service` is a oneshot running `/bin/true`, so it does not count); `status --json`, `status`, `leak-test` (as root) and `boot` report the proxy and Ghost Mode as not in force, with an error, while the redirect is in place and Tor does not run, and a Hide Me action whose Tor restart fails in Ghost Mode (also "Rotate identity" and `tor-new`) exits with status 3 and records that error. What was asked for is kept (also when Tor stopped by itself, a crash or `systemctl stop`, with the redirect and the kill switch in place): once Tor runs again they report Ghost Mode (threat level `maximum`) again, without the old error, and so does the record of the next Hide Me action that succeeds (Rotate identity, `tor-new`, the DNS switch, the timer's `tor-newnym`: while a redirect to Tor is hooked, a success records the re-checked state, so one while Tor is still down keeps the error). `.onion` names resolve to Tor's range 10.192.0.0/10, which is redirected before the private ranges (10/8, 172.16/12, 192.168/16, 169.254/16) are let through, so onion services work. The proxy only sees IPv4 TCP and DNS, so it engages the kill switch first: UDP (QUIC, WebRTC) and IPv6 are dropped instead of going around it. Turning the proxy off releases a kill switch that only the proxy needed | `transparent-proxy-on [--persist]`, `transparent-proxy-off` |
| DNS-over-Tor | iptables-nft chain `SHADOWDNS`: every IPv4 port-53 query (to resolved's stub 127.0.0.53 or any server) goes to Tor's DNSPort, except Tor's own — systemd-resolved's included, so names under a network's own search domains (which resolved sends to that network's server, e.g. `printer.lan`) go to Tor too and do not resolve while DNS protection is on; Tor's DNSPort is UDP `127.0.0.1:26`, a port only root may bind: on the old 5353 any local user could bind a socket with `SO_REUSEADDR` next to Tor's (Tor sets it too) and get every query, resolved's own included, and answer for the whole system. DNS over TCP is refused, never sent to the DNSPort, which answers UDP only (on 5353 a user could also listen on its TCP side; resolved's opportunistic DNS-over-TLS checks no certificate). TCP port 53, resolved's DNS-over-TLS to a network's server (port 853, NetworkManager's per-connection setting; resolved then falls back to UDP port 53, i.e. Tor) and resolved's TCP to Tor's DNSPort (its retry after a truncated answer, or its fallback when Tor does not answer yet) are redirected to port 1 on loopback, where nothing listens and which only root may bind (it stays privileged even where `net.ipv4.ip_unprivileged_port_start` is lowered, unless to 0 or 1). Over IPv6 nothing is sent to Tor (its DNSPort is IPv4 only): the chain's IPv6 side (`ip6tables`, wherever the kernel has IPv6) refuses every port-53 query and resolved's DNS-over-TLS the same way (port 1 on `::1`), so resolved asks the network's IPv4 server, which goes to Tor. Without it, names resolved sends to a network's IPv6 server (its search domains, from router advertisements or DHCPv6) left in plain text or over DNS-over-TLS without a certificate check; on a network whose only DNS servers are IPv6, such names do not resolve while DNS protection is on. DNS protection counts as on only with both sides in place. `boot` (also run on an upgrade) replaces rules of an older version that still sent TCP to the DNSPort, used the old ports 9040/5353 or had no IPv6 side, rewrites Tor's torrc.d files and resolved's drop-in for the new ports, and only then restarts Tor (on a running system it waits for Tor, starts and waits for one that does not run while DNS protection or the proxy needs it, and records a failure; only at start-up, as `systemctl is-system-running` reports it, is Tor's start queued); it removes a `SHADOWDNS` that nothing wants (DNS protection off: an older version's chain restored by netfilter-persistent, which sent every lookup to 5353). While Tor is not running, queries and connections sent to its ports fail: `status --json`, `status` and `leak-test` (as root) say so (DNS protection stays reported on: nothing leaks); see [limitations](../limitations.md). With systemd-resolved, the runtime drop-in `/run/systemd/resolved.conf.d/95-shadow-anon.conf` (`DNS=` and `Domains=` emptied first — they are lists that add up across files, and the shipped provider would otherwise stay in front of Tor and get every name in plain text — then `DNS=127.0.0.1:26`, `Domains=~.`, `FallbackDNS=`, `DNSOverTLS=no`, `DNSSEC=no`; Tor's DNSPort has no DNSSEC) makes Tor resolved's only global server, and `/etc/resolv.conf` stays resolved's stub link. Turning it on checks `resolvectl dns` afterwards: if resolved uses any other global server (a file read after `95-shadow-anon.conf` can add one), it fails (exit status 3) and is rolled back; the state's `dns_protected`, `status` and `leak-test` are likewise taken from resolved's live server list, not from the file being there. Without systemd-resolved (older installs), `/etc/resolv.conf` points at `127.0.0.1` and `/etc/NetworkManager/conf.d/90-shadow-dns.conf` tells NetworkManager to leave it alone. The file is never made immutable any more | `dns-protect-on`, `dns-protect-off` |
| New network address | "Rotate identity" gives every connected NetworkManager connection a new MAC address: a salt (`/run/shadow/mac-salt`, this boot only) is added to the connection's stable-id (`${CONNECTION}/${BOOT}/<salt>`, `cloned-mac-address=stable`, `nmcli --temporary`), the DHCP lease is released (`ipv4.dhcp-send-release`), and the connection reconnects. At most once in 300 s; never on a timer, never by a threat level, never the `random` policy (which used up DHCP pools). A connection with an address setting of its own (`permanent`, `preserve`, a fixed address, its own stable-id) keeps it; interfaces NetworkManager does not manage are left alone | `rotate` (also `mac`) |
| New Tor circuits | `SIGNAL NEWNYM` on Tor's control socket `/run/tor/control` (cookie authentication), or a reload of `tor@default` | `tor-new`; the timer runs `tor-newnym` |
| Tor's SOCKS port | `/etc/tor/torrc.d/00-shadow-socks.conf`, always present (written by `boot` at every start and on upgrade, by the image build, and before Hide Me changes Tor), replaces the SocksPort list of Debian's defaults (`SocksPort 9050`) with `SocksPort unix:/run/tor/socks WorldWritable` and `SocksPort 127.0.0.1:39`, plus `KeepBindCapabilities 1`. 9050 is a port any local user may bind: holding it while Tor was stopped (any restart, or at boot before `tor@default` starts) made Tor exit, so DNS protection and Ghost Mode failed, and every SOCKS client (torsocks, proxychains4, `fix-network`'s Tor check) talked to that user, who saw each destination and could relay it from the real address. Port 39 is below 1024. `/etc/tor/torsocks.conf` (`TorPort 39`) and `/etc/proxychains4.conf` (`socks5 127.0.0.1 39`) are pointed at it while they still name 127.0.0.1:9050; other settings are left alone. 9050 itself stays a port any user may bind, so every new TCP connection to 9050 on a loopback address is sent to 39 (iptables-nft chain `SHADOWSOCKS` in nat `OUTPUT`, loaded by `boot` and `tor-socks` whatever Hide Me does): programs that use 9050 with no setting to change (`tor-resolve`, `sqlmap --tor`, a user's own scripts) reach Tor, and are refused while Tor does not run, and a user's listener on 9050 gets nothing. Its IPv6 side (`ip6tables`, wherever the kernel has IPv6) refuses `[::1]:9050` (port 1 on `::1`: nothing listens, only root may bind it), so clients that name the proxy `localhost:9050` (curl, requests through PySocks), which try `::1` first, move on to 127.0.0.1:9050, i.e. Tor, instead of reaching a user who binds `[::1]:9050`; see [limitations](../limitations.md). Without iptables (only recommended) nothing redirects 9050: `boot` and `tor-socks` exit with status 3 and record that as the state's error. A running Tor is restarted, and waited for, once for the change (by `boot` and by `tor-socks`: until then it still serves 9050, and the file's `KeepBindCapabilities` makes it exit at its next reload); a stopped one is not started for it. The image build runs `tor-socks --offline` (the files only: its chroot shares the build machine's network and services). The leak test and `status` ask Tor through 127.0.0.1:39 directly | `tor-socks [--offline]` (`boot` does it too) |

Ghost Mode (`ghost-on [--persist]`, threat level `maximum`) engages the kill
switch **first**, then changes the address (Ghost Mode only; the `maximum`
threat level does not), IPv6 privacy, host name, time zone, kernel settings,
Bluetooth, the transparent proxy and DNS-over-Tor, and wipes logs (the journal;
the shell histories and thumbnail caches of every home, handled in a child
process with the uid and gid of the home's owner, so a link such as
`~/.bash_history -> /etc/...` reaches nothing its owner could not change;
links, hard links and files of another owner are skipped; old `/tmp` files). If the kill
switch cannot be engaged nothing else happens; if a later network step fails,
the kill switch stays on (fail closed) and Ghost Mode is reported as off.
`ghost-off` removes the proxy and DNS-over-Tor before it puts the kill switch
back. The threat levels `basic` and `enhanced` turn the proxy off and put back
a kill switch that Hide Me engaged.

Every step that adds a rule or changes a service checks its exit status. A
command that could not do its job exits with status 3, prints the reason and
records it as `error` in `/run/shadow-anon-state.json`; the state file only
ever says `true` for what is really in force (`kill_switch` is read back from
`shadow-killswitch status --json`, the chains from iptables). The fields the
Privacy page reads (`ghost_mode`, `threat_model`, `transparent_proxy`,
`dns_protected`, `bt_killed`, `usb_killed`, `kernel_hardened`) are unchanged;
new are `kill_switch`, `killswitch` (mode, loaded, error), `dns_method`,
`error` and `escape_hatch`. `shadow-anon status --json` prints the record,
re-checked when run as root.

The rotation timer (`shadow-anon start --interval N`, `shadow-anon stop`)
only renews Tor circuits. Its minimum is 60 s: a shorter interval is raised to
60 s, and an interval below that left by an older version is raised at the
next start.

### Across a restart

| Feature | Kept in | After a restart |
|---|---|---|
| DNS-over-Tor | the choice in `/etc/shadow-os/hide-me.json` (with systemd-resolved), or the `resolv.conf`/NetworkManager files (without); `/etc/tor/torrc.d/shadow-dns.conf` | stays on: `shadow-anon-boot.service` puts the redirect and the resolved drop-in back before the network starts. If Tor is no longer installed it hands DNS back instead. A system that still has the `resolv.conf` method but now has systemd-resolved is moved to the drop-in |
| Kill switch | `shadow-killswitch` | off, unless it was switched on with `--persist` (then `shadow-killswitch.service` applies it at boot) |
| Transparent Tor proxy, Ghost Mode | iptables rules only | off; turn Ghost Mode on again |
| Kernel hardening, USB storage block | files in `/etc/sysctl.d`, `/etc/udev/rules.d` | stay on |
| New network address | `/run/shadow/mac-salt`, `nmcli --temporary` | gone: each network gets its usual address again |
| Tor circuit timer | `shadow-anon-rotate.timer` | stays on if it was started |

`shadow-anon-boot.service` runs `shadow-anon boot` at every start (after
`shadow-killswitch.service`, and when the package is installed or upgraded).
It also replaces what older versions left behind: their iptables kill switch
chain `SHADOWKILL` (the new kill switch is engaged first, so nothing leaks
in between; if it cannot be, the old chain stays), a transparent proxy
without the `.onion` rule, a DNS redirect without DNS protection, and timer
intervals below 60 s, and it writes Tor's SOCKS ports file and loads the
redirect of 9050 to it (see above). Then it writes
`/run/shadow-anon-state.json` from what is actually in effect. After any
other command that succeeds, the record is re-checked the same way while a
redirect to Tor is hooked (or a Ghost Mode that Tor's failure interrupted
waits); otherwise only its `error` is cleared.

Turning DNS protection off (`sudo shadow-anon dns-protect-off`, or the
switch on the Privacy page) removes the drop-in, restarts systemd-resolved
and points `/etc/resolv.conf` at its stub again
(`../run/systemd/resolve/stub-resolv.conf`). Without systemd-resolved it
lets NetworkManager write the network's own DNS servers again; if
NetworkManager does not, it falls back to 9.9.9.9 and 1.1.1.1.

### Recovery

* Kernel command line `shadow.hideme=off`: for that boot, `boot` does not put
  DNS-over-Tor back and hands DNS to the system resolver (the choice is kept
  with systemd-resolved, and forgotten with the `resolv.conf` method).
  `shadow.killswitch=off` keeps the kill switch off (see
  [kill-switch.md](kill-switch.md)).
* From a text console as root: `shadow-anon ghost-off` (or
  `dns-protect-off`, `kill-switch-off`); `shadow-anon status` shows what is in
  force and the last error.
* By hand: `iptables -t nat -D OUTPUT -j SHADOWTOR`, the same for
  `SHADOWDNS` (and `ip6tables -t nat -D OUTPUT -j SHADOWDNS`),
  `rm /run/systemd/resolved.conf.d/95-shadow-anon.conf`,
  `systemctl restart systemd-resolved`, `shadow-killswitch set off`.

## Disk encryption (LUKS2)

Set up by the installer engine when you choose encryption
(`system/installer/engine/shadow_installer/executor.py`).

| Parameter | Value |
|---|---|
| Format | LUKS2 |
| Cipher | `aes-xts-plain64` |
| Key size | 512 bits (AES-256 in XTS mode) |
| Hash | SHA-512 |
| Key derivation | Argon2id, with cryptsetup's default cost (benchmarked on the installing machine) |
| Label | `shadow-crypt` |
| Inside | LVM volume group (`shadow`) with `root`, optionally `home` and `swap` |
| Unlocking | passphrase at boot (`/etc/crypttab`: `none luks,discard`) |
| Passphrase rules | at least 8 characters; passed to cryptsetup on standard input only |

What it protects: data at rest on a powered-off or locked-out machine —
theft, disposal.

What it does not protect, and trade-offs:

* `/boot` and the EFI System Partition are not encrypted. Kernel and
  initramfs are protected against tampering only as far as Secure Boot goes:
  the kernel is signature-checked, the initramfs is not.
* A running or suspended machine has the key in memory.
* `discard` (TRIM) is enabled for SSD performance and lifetime; it reveals
  which blocks are unused.
* With a swap **file** swap is inside the encrypted root. With a swap
  partition it is a logical volume in the same container. Without
  encryption, swap is not encrypted.
* A forgotten passphrase cannot be recovered. There is no recovery key, key
  file or TPM unlocking.
* Encryption is not available with manual partitioning.

Verified in QEMU: an encrypted whole-disk installation boots after the
passphrase is entered, and `cryptsetup luksDump` reports version 2 with
Argon2id.

## Secure Boot

* The system boots through Debian's Microsoft-signed shim, Debian-signed
  GRUB and Debian's signed kernel; no key enrolment is needed
  ([../architecture/boot.md](../architecture/boot.md)).
* With Secure Boot enabled the kernel is in lockdown: unsigned kernel
  modules are refused, `/dev/mem` and similar interfaces are restricted and
  hibernation is disabled. The boot test checks that lockdown is active.
* GRUB and shim updates are copied to the ESP by a dpkg trigger
  ([updates.md](updates.md)).
* If Secure Boot is disabled in the firmware the system still installs and
  boots; the installer shows a warning and the system stays "Secure Boot
  ready".
* The GRUB menu is **not** password-protected: anyone at the keyboard can
  edit kernel parameters or choose recovery mode. Recovery mode's root shell
  asks for the root password, and the root account is locked
  ([../troubleshooting/recovery.md](../troubleshooting/recovery.md)).
* The SHADOW project has no Secure Boot key of its own.

## Security status report

```
sudo shadow-security-status [--section NAME[,NAME…]] [--pretty]
```

A read-only JSON report; every field is the result of a check made when the
program runs — nothing is cached, estimated or scored. A check that cannot
be performed reports `{"available": false, "reason": "…"}`.

Sections: `firewall`, `apparmor`, `encryption`, `secureboot`, `updates`,
`listening`, `connections`, `services`, `logins`, `audit`, `hardening`,
`killswitch`, `panic`, `recommendations`.

`killswitch` and `panic` combine what the tools recorded in
`/run/shadow/*.json` with a live check of their nftables tables:
`protected` is true only when the kill switch is on, recorded as loaded and
its table is in the kernel now ([kill-switch.md](kill-switch.md)).

The `hardening` section compares the running kernel's settings with the
shipped baseline files, so it shows which of the settings on this page are
actually in effect. `secureboot` includes the marker of a failed boot-loader
refresh.

Graphical tools run it through `pkexec` (polkit action
`org.shadow.security.read-status`). An active local session may do so
without a password, because the program changes nothing and takes no input
beyond the section names; remote and inactive sessions need administrator
authentication, since the report reveals listening services and login
activity. The Security Center application that would display the report is
not part of the image yet.

## polkit actions shipped by SHADOW

| Action | Program | Default for an active local session |
|---|---|---|
| `org.shadow.security.read-status` | `/usr/sbin/shadow-security-status` | allowed without authentication |
| `org.shadow.firewall.manage` | `/usr/sbin/shadow-firewall` | administrator authentication, kept for a short time |
| `org.shadow.killswitch.manage` | `/usr/sbin/shadow-killswitch` | administrator authentication, kept for a short time |
| `org.shadow.panic.release` | `/usr/sbin/shadow-panic` (release) | administrator authentication every time |
| `org.shadow.netprivacy.manage` | `/usr/sbin/shadow-netprivacy` (encrypted DNS, MAC policy) | administrator authentication, kept for a short time; `status` needs none |
| `org.freedesktop.systemd1.manage-units`, unit `shadow-panic.service`, verb `start` only | systemd (`49-shadow-panic.rules`) | allowed without authentication for members of `sudo`: engaging a panic only cuts the network |
| `org.shadow.networkcenter.sockets` | `network-helper sockets` | administrator authentication, kept |
| `org.shadow.networkcenter.capture` | `network-helper capture` | administrator authentication every time; never for remote or inactive sessions |
| `org.shadow.installer.run` | `/usr/sbin/shadow-installer-engine` | administrator authentication every time; in the live session a rule allows it without a password. The package is removed from installed systems |

## Documented exceptions

Places where the baseline is deliberately weaker than it could be, and why:

| Exception | Reason |
|---|---|
| Module signatures are not enforced without Secure Boot (`MODULE_SIG_FORCE` off, also in the SHADOW kernel configuration) | enforcing would block DKMS drivers such as the proprietary NVIDIA module. Under Secure Boot, lockdown enforces signatures |
| `blacklist` instead of `install … /bin/false` for kernel modules | root must be able to load these protocols and filesystems on a security workstation |
| Reverse-path filtering in loose mode, not strict | strict mode breaks multi-homed and VPN set-ups |
| `kernel.yama.ptrace_scope = 1`, not 2 or 3 | debuggers started normally must keep working on a development system |
| Audit rules not locked | they must be extendable without a reboot |
| Firewall forwards traffic of local virtual machines and containers and lets them use the host's DNS and DHCP | lab virtual machines and containers are a core use of the system |
| Outbound traffic is not filtered unless the kill switch is turned on (it is off by default) | a workstation that starts scans and connections by design |
| ICMP echo requests are answered (rate-limited) | a host that cannot be pinged is hard to diagnose; the limit prevents use as an amplifier |
| Passwordless `sudo` and passwordless installer authorisation in the live session | the live user has no password to type; both exist only on the live medium |
| Lock screen grace time of 5 seconds | a just-locked screen can be dismissed without the password, as a convenience default (`/etc/xdg/kscreenlockerrc`, `LockGrace`) |
| `/boot` unencrypted; `discard` enabled on LUKS | boot loader and kernel must be readable before unlocking; SSD performance |
| IOMMU for Intel integrated graphics left at Debian's default (off) | long-standing firmware and driver quirks on some laptops; `intel_iommu=on` forces it |
| Non-free firmware and microcode in the image | hardware does not work without it; the same choice Debian's official images make |
| Build-time APT mirrors use plain HTTP | APT verifies signed Release files; the shipped system uses HTTPS |

## Not in the baseline

So that nothing is assumed: no outbound firewall by default (the kill switch
is opt-in), no per-application
network rules, no automatic security updates, no USB device authorisation
(USBGuard is in the Blue Team profile, not installed), no intrusion
detection, no file-integrity monitoring, no SHADOW AppArmor profiles, no
`hidepid` on `/proc`, no kernel module signature enforcement without Secure
Boot, no GRUB password, no TPM use, no SSH server (only the client is
installed).
