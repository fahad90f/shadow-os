# Security baseline

Every security setting NEXUS OS ships, what it protects against, and what
it can break. All of it is in one removable package, `nexus-hardening`
(source: `security/`), plus the installer's disk-encryption parameters and
the boot chain.

> **Scope and verification.** `nexus-hardening` is part of the desktop
> image, not of the minimal image. Verified: `nexus-firewall` in a
> functional test inside an isolated network namespace; during the first
> boot of the desktop image a bug in the audit rules was found and fixed.
> Everything else is implemented and has not been verified as a whole on an
> installed system. Nothing has run on real hardware.

Principle from the package description: none of these settings disables a
security mechanism to make software work. Where something can break, the
table says how to change the one setting concerned.

## Overview

| Area | File on the system | Source |
|---|---|---|
| Kernel settings (sysctl) | `/usr/lib/sysctl.d/60-nexus-hardening.conf` | `security/hardening/sysctl.d/` |
| Kernel command line | `/etc/default/grub.d/20-nexus-hardening.cfg` | `security/hardening/grub.d/` |
| Module auto-loading | `/usr/lib/modprobe.d/nexus-hardening.conf` | `security/hardening/modprobe.d/` |
| Firewall | `/etc/nftables.conf`, `/usr/sbin/nexus-firewall`, `/etc/nexus/firewall.json`, `/etc/nftables.d/` | `security/firewall/` |
| Audit rules | `/etc/audit/rules.d/50-nexus.rules` | `security/audit/` |
| Journal | `/usr/lib/systemd/journald.conf.d/nexus.conf` | `security/hardening/journald.conf.d/` |
| sudo | `/etc/sudoers.d/nexus-hardening` | `security/hardening/sudoers.d/` |
| NetworkManager privacy | `/usr/lib/NetworkManager/conf.d/80-nexus-privacy.conf` | `security/hardening/NetworkManager/` |
| Status report | `/usr/sbin/nexus-security-status` | `security/audit/` |
| polkit actions | `/usr/share/polkit-1/actions/org.nexus.security.policy` | `security/permissions/` |

The package depends on `nftables`, `apparmor`, `auditd` and `sudo`, and
recommends `apparmor-profiles`, `apparmor-profiles-extra` and
`apparmor-utils` (Recommends are not installed in the image).

On first installation the package enables `nftables.service`, applies the
sysctl settings on a running system, and reloads an active firewall. Debian's
`/etc/nftables.conf` is kept as `/etc/nftables.conf.debian` through a dpkg
diversion and restored when the package is removed.

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

`/etc/default/grub.d/20-nexus-hardening.cfg` appends to
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
build-time options of the NEXUS kernel configuration are **not** in effect,
because that kernel is not shipped ([../kernel/README.md](../kernel/README.md)).

The live medium's boot menu does not use this file; the live session runs
without these parameters.

## Module auto-loading

`/usr/lib/modprobe.d/nexus-hardening.conf` uses `blacklist`, which stops a
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

* `nexus-hardening` depends on `apparmor`, so the AppArmor service and the
  profiles that Debian packages ship are installed. Debian's kernel enables
  AppArmor by default.
* **NEXUS OS ships no AppArmor profiles of its own.** NEXUS applications,
  the shell and the helpers run unconfined.
* `apparmor-profiles`, `apparmor-profiles-extra` and `apparmor-utils` are
  recommended by the package but not installed in the image. Install them
  if you want more profiles and the `aa-status`, `aa-enforce`,
  `aa-complain` tools.
* `nexus-security-status --section apparmor` reports whether the module is
  enabled, the number of profiles per mode and the number of confined
  processes. `/etc/apparmor.d/` is watched by the audit rules.

If a profile blocks a program, look for `apparmor="DENIED"` in
`sudo journalctl -k` and put that one profile into complain mode
(`sudo aa-complain <profile>`, from `apparmor-utils`). Do not disable
AppArmor as a whole.

## Firewall (nftables)

### Default policy

`/etc/nftables.conf` defines one table, `inet nexus_filter`:

| Direction | Policy | Exceptions |
|---|---|---|
| Inbound | **drop** | replies to connections this machine opened; loopback; ICMP and ICMPv6 errors; echo requests, rate-limited to 10 per second; IPv6 neighbour discovery and multicast listener discovery from link-local addresses; DHCPv6 client replies; DNS and DHCP from local virtual machines and containers (`virbr*`, `docker0`, `br-*`, `podman*`, `lxcbr*`); ports you opened |
| Forwarded | **drop** | traffic of local virtual machines and containers leaving through this host, and connections published to them (DNAT) |
| Outbound | **accept** | — |

Only the NEXUS table is replaced when the file is loaded. Tables owned by
Docker, libvirt or a VPN client are left alone.

Can break: anything that expects unsolicited inbound connections — an SSH
or web server on this machine, file sharing, KDE Connect, device discovery
(mDNS, SSDP), listeners for reverse connections during a test. Open the port
you need.

### `nexus-firewall`

Must run as root.

```
nexus-firewall status [--json]
nexus-firewall list [--json]
nexus-firewall enable
nexus-firewall disable
nexus-firewall allow <port>[-<port>]/<tcp|udp> [--name TEXT]
nexus-firewall remove <port>[-<port>]/<tcp|udp>
nexus-firewall reload
nexus-firewall reset
```

| Command | Effect |
|---|---|
| `status` | active or not, starts at boot or not, policies, rule count, open ports, other nftables tables present. Exit status 0 when active, 3 when not |
| `list` | the opened ports |
| `enable` | validates and loads the ruleset and enables `nftables.service` |
| `disable` | disables the service and deletes the NEXUS table: inbound connections are no longer filtered |
| `allow` | opens a port or a port range for inbound connections, with an optional name |
| `remove` | closes it again |
| `reload` | regenerates and loads the ruleset |
| `reset` | closes all opened ports |

Examples:

```sh
sudo nexus-firewall status
sudo nexus-firewall allow 22/tcp --name "SSH"
sudo nexus-firewall allow 8000-8010/tcp --name "lab web servers"
sudo nexus-firewall remove 22/tcp
```

How it works:

* The opened ports are recorded in `/etc/nexus/firewall.json`. From it the
  tool generates `/etc/nftables.d/50-nexus-allowed.nft`, which fills the
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
add rule inet nexus_filter input_local iifname "tun0" tcp dport 4444 accept
```

Graphical tools call `nexus-firewall` through `pkexec` (polkit action
`org.nexus.firewall.manage`, administrator authentication). A Control Center
firewall page exists in the source tree but is not part of the image yet.

Limits of this firewall: it filters by port only (no per-application
rules), it does not filter outbound traffic, and `allow` opens a port for
every source address and interface. Per-source rules go into your own
`.nft` file.

## Audit rules

`/etc/audit/rules.d/50-nexus.rules`, loaded by `augenrules`. A deliberately
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
| `boot` | `/boot/grub/grub.cfg`, `/etc/default/grub`, `/etc/default/grub.d/`, `/etc/nexus/boot.conf` |
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

`/usr/lib/systemd/journald.conf.d/nexus.conf`:

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

`/etc/sudoers.d/nexus-hardening`:

| Setting | Effect | Can break |
|---|---|---|
| `Defaults use_pty` | commands run in a pseudo-terminal, so a compromised command cannot inject keystrokes into your terminal after it exits | rarely, programs under `sudo` that need the caller's terminal device directly |
| `Defaults passwd_tries=3` | three password attempts | — |
| `Defaults badpass_message=…` | a plain message for a wrong password | — |

Related, set by the installer and the image build rather than this
package: the root account is locked; the first user is a member of `sudo`.
In the live session the live user has passwordless `sudo`.

## NetworkManager privacy

`/usr/lib/NetworkManager/conf.d/80-nexus-privacy.conf`. Override per
connection (`nmcli`, Control Center) or with a file in
`/etc/NetworkManager/conf.d/`.

| Setting | Protects against | Can break |
|---|---|---|
| `wifi.scan-rand-mac-address=yes` | tracking of the device while it scans for networks | — |
| `wifi.cloned-mac-address=stable` | networks correlating the device with each other: every Wi-Fi network sees a different, but for that network stable, address. DHCP leases and captive-portal log-ins keep working | networks that filter by hardware address: set `permanent` on that connection (`nmcli connection modify <name> wifi.cloned-mac-address permanent`) |
| `ethernet.cloned-mac-address=permanent` | — (wired interfaces keep their real address) | — |
| `ipv6.ip6-privacy=2` | tracking through a stable IPv6 address | services expecting a stable source address |

## Disk encryption (LUKS2)

Set up by the installer engine when you choose encryption
(`system/installer/engine/nexus_installer/executor.py`).

| Parameter | Value |
|---|---|
| Format | LUKS2 |
| Cipher | `aes-xts-plain64` |
| Key size | 512 bits (AES-256 in XTS mode) |
| Hash | SHA-512 |
| Key derivation | Argon2id, with cryptsetup's default cost (benchmarked on the installing machine) |
| Label | `nexus-crypt` |
| Inside | LVM volume group (`nexus`) with `root`, optionally `home` and `swap` |
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
* The NEXUS project has no Secure Boot key of its own.

## Security status report

```
sudo nexus-security-status [--section NAME[,NAME…]] [--pretty]
```

A read-only JSON report; every field is the result of a check made when the
program runs — nothing is cached, estimated or scored. A check that cannot
be performed reports `{"available": false, "reason": "…"}`.

Sections: `firewall`, `apparmor`, `encryption`, `secureboot`, `updates`,
`listening`, `connections`, `services`, `logins`, `audit`, `hardening`,
`recommendations`.

The `hardening` section compares the running kernel's settings with the
shipped baseline files, so it shows which of the settings on this page are
actually in effect. `secureboot` includes the marker of a failed boot-loader
refresh.

Graphical tools run it through `pkexec` (polkit action
`org.nexus.security.read-status`). An active local session may do so
without a password, because the program changes nothing and takes no input
beyond the section names; remote and inactive sessions need administrator
authentication, since the report reveals listening services and login
activity. The Security Center application that would display the report is
not part of the image yet.

## polkit actions shipped by NEXUS

| Action | Program | Default for an active local session |
|---|---|---|
| `org.nexus.security.read-status` | `/usr/sbin/nexus-security-status` | allowed without authentication |
| `org.nexus.firewall.manage` | `/usr/sbin/nexus-firewall` | administrator authentication, kept for a short time |
| `org.nexus.networkcenter.sockets` | `network-helper sockets` | administrator authentication, kept |
| `org.nexus.networkcenter.capture` | `network-helper capture` | administrator authentication every time; never for remote or inactive sessions |
| `org.nexus.installer.run` | `/usr/sbin/nexus-installer-engine` | administrator authentication every time; in the live session a rule allows it without a password. The package is removed from installed systems |

## Documented exceptions

Places where the baseline is deliberately weaker than it could be, and why:

| Exception | Reason |
|---|---|
| Module signatures are not enforced without Secure Boot (`MODULE_SIG_FORCE` off, also in the NEXUS kernel configuration) | enforcing would block DKMS drivers such as the proprietary NVIDIA module. Under Secure Boot, lockdown enforces signatures |
| `blacklist` instead of `install … /bin/false` for kernel modules | root must be able to load these protocols and filesystems on a security workstation |
| Reverse-path filtering in loose mode, not strict | strict mode breaks multi-homed and VPN set-ups |
| `kernel.yama.ptrace_scope = 1`, not 2 or 3 | debuggers started normally must keep working on a development system |
| Audit rules not locked | they must be extendable without a reboot |
| Firewall forwards traffic of local virtual machines and containers and lets them use the host's DNS and DHCP | lab virtual machines and containers are a core use of the system |
| Outbound traffic is not filtered | a workstation that starts scans and connections by design |
| ICMP echo requests are answered (rate-limited) | a host that cannot be pinged is hard to diagnose; the limit prevents use as an amplifier |
| Passwordless `sudo` and passwordless installer authorisation in the live session | the live user has no password to type; both exist only on the live medium |
| Lock screen grace time of 5 seconds | a just-locked screen can be dismissed without the password, as a convenience default (`/etc/xdg/kscreenlockerrc`, `LockGrace`) |
| `/boot` unencrypted; `discard` enabled on LUKS | boot loader and kernel must be readable before unlocking; SSD performance |
| IOMMU for Intel integrated graphics left at Debian's default (off) | long-standing firmware and driver quirks on some laptops; `intel_iommu=on` forces it |
| Non-free firmware and microcode in the image | hardware does not work without it; the same choice Debian's official images make |
| Build-time APT mirrors use plain HTTP | APT verifies signed Release files; the shipped system uses HTTPS |

## Not in the baseline

So that nothing is assumed: no outbound firewall, no per-application
network rules, no automatic security updates, no USB device authorisation
(USBGuard is in the Blue Team profile, not installed), no intrusion
detection, no file-integrity monitoring, no NEXUS AppArmor profiles, no
`hidepid` on `/proc`, no kernel module signature enforcement without Secure
Boot, no GRUB password, no TPM use, no SSH server (only the client is
installed).
