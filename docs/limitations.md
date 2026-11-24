# Known limitations

What does not work, is not finished, or has not been verified. This list is
meant to be complete and blunt; if you find something missing, it is a
documentation bug.

Shadow OS 0.1.0 is a development version. Do not use it where data loss or
downtime matters.

## Verification

* **Not tested on physical hardware.** Every boot and installation ran in
  QEMU with software emulation. The first boot on a real machine is the
  first such test. Wi-Fi, GPUs, laptops, suspend, Bluetooth, audio output
  and touchpads are untested.
* **arm64 is not supported.** Configuration for it exists (package lists,
  kernel fragment, boot paths, test matrix), package names resolve and
  `Architecture: all` packages are published for arm64, but no arm64 image
  has been built or booted. `SHADOW_ARCHES` in `distro/base/shadow.conf` lists
  it; that is an intention, not a fact.
* The desktop image has been booted under UEFI only. Its Secure Boot and
  BIOS boots, the graphical installer run, the lock screen and the login
  screen without auto-login are pending.
* Installer scenarios with a pending re-run: manual partition assignment
  (UEFI and BIOS), separate `/home`, and the refusal cases.
* Application back-ends were tested in a build chroot against real daemons
  on a private bus or against fixtures — not in the booted image. polkit
  authentication prompts have not been observed for any application.
* A CI workflow exists (`.github/workflows/build.yml`), but no run of it is
  on record, and as written its package build is expected to stop on missing
  build dependencies (next section). Its job `tests` runs the suites that
  need no build (`tests/run-suites.sh`, as a user and as root); the
  Control Center, Network Center and shell back-end tests need the programs
  built and are not in CI. There is no integration test suite inside a
  booted image.
* Images are not verified to be reproducible.

## Distribution and updates

* **No hosted SHADOW package repository exists.** `shadow.sources` is shipped
  disabled. SHADOW packages on an installed system receive **no updates**,
  including security fixes for SHADOW components.
* The SHADOW packages of a release ship as a signed repository on the disk
  (`/usr/share/shadow/repository`, desktop image). It is a snapshot.
* **The archive signing key used so far is a development key** generated on
  the build machine, without a passphrase. There is no release key and no
  release-signing procedure. Treat every existing image as a development
  build.
* Release channels (`dev`, `testing`, `stable`) are a label only.
* No automatic updates and no background update check.
* No rollback: no snapshots, no transactional updates, one version per
  package in the repository.
* No upgrade path to a later Shadow OS or Debian release is defined.
* Images contain whatever Debian package versions the mirror offered at
  build time; the base-layer cache does not notice Debian updates unless
  you build with `--fresh`.
* `build/chroot/build-deps.list` and `debian/control` are out of sync: a
  package build needs the chroot created with `--dev-tools`. The CI workflow
  creates it without.

## Kernel

* The image ships **Debian's signed kernel**, not a SHADOW kernel.
* The SHADOW kernel's configuration pipeline works; a **full build has never
  completed**. No SHADOW kernel package exists, none has been booted, and
  nothing would install one.
* The build-time hardening options of the SHADOW configuration that Debian
  does not already set (1000 Hz and full preemption, `UBSAN_BOUNDS`,
  `RANDOM_KMALLOC_CACHES`, `ZERO_CALL_USED_REGS`, strict IOMMU invalidation,
  always-on BPF JIT) are therefore **not in effect** on a Shadow OS system.
* A SHADOW kernel would be unsigned unless a Secure Boot key is supplied;
  the project has no such key.
* The kernel build keeps the temporary module-signing key in its build
  tree; it is "discarded" only when that tree is deleted.

## Boot

* The boot-loader directory on the ESP is `EFI/debian` (required by
  Debian's signed GRUB). Shadow OS cannot share an EFI System Partition with
  a Debian installation or another distribution that uses that directory.
* The boot-loader refresh trigger has not been exercised by a real GRUB or
  shim update. A failed refresh is reported only by
  `shadow-security-status`; the Updater does not show it.
* The GRUB menu has no password.
* GRUB's recovery-mode entries are kept, but the root account is locked, so
  the rescue shell is expected to be unusable unless you set a root
  password.
* The live medium's "Recovery environment" entry starts a program
  (`shadow-recovery`) that does not exist in the image.
* The Plymouth passphrase prompt on an encrypted installation of the
  desktop image is unverified.

## Installer

* The graphical installer has not performed a real installation.
* No partition editor: manual mode assigns existing partitions only.
* No resizing of existing partitions: "alongside" needs unallocated space
  on a GPT disk.
* No encryption in manual mode. No TPM unlocking, key files or recovery
  keys. `/boot` and the ESP are never encrypted.
* Only ext4 for new filesystems. No btrfs, no RAID, no LVM without
  encryption, no ZFS.
* Partition and swap sizes cannot be set in the graphical installer.
* **Tool profiles selected in the installer are installed without their
  tools**: the engine installs the metapackages with
  `--no-install-recommends`, and the tools are Recommends.
* The installer's own texts are English only; so are the shell and the
  applications (strings are marked for translation, no translations ship).
* The installed system is a copy of the live image: hardware-specific
  package selection does not happen.
* Dual boot with Windows is untested.
* The installer's README says locales that glibc lists without the
  `.UTF-8` suffix (`hi_IN`, `vi_VN`, …) cannot be selected; the engine has
  since gained handling for them, and the combination is untested.

## Desktop

* The compositor is KWin. Shadow Horizon is a shell and session on top of
  it, with dependencies on KWin-specific interfaces, KScreenLocker,
  `plasma-integration` and KDE's portal back-end. A native compositor is
  not implemented.
* **Blur and glass effects need GPU compositing.** With CPU rendering —
  any machine without a GPU render node, including basic virtual machines —
  surfaces are opaque, and animations cost noticeable CPU time.
* **XEmbed (legacy X11) tray icons are not supported.**
* Panel, dock, notifications, quick settings and the tray appear on the
  primary screen only. Multiple screens and HiDPI are untested.
* The lock screen and the SDDM greeter depend on `layer-shell-qt`; a real
  lock on Wayland and an interactive login are unverified.
* Window-management keyboard shortcuts are KWin's defaults and cannot be
  edited in the Control Center (they are listed read-only).
* Idle time-outs written by the Control Center (`power.conf`) are read by
  nothing. Screen locking follows KScreenLocker's own settings.
* Notifications: no sounds, no inline reply, history lost on shell restart.
* Quick settings: Bluetooth, airplane mode, brightness changes and Night
  Light are untested; NetworkManager, UPower and power profiles were tested
  against fixtures only.
* GTK and libadwaita theming is untested.
* No on-screen keyboard; no screen reader has been tried (accessibility
  roles are set in the UI).
* A crash in a shell module takes the whole shell down (it is restarted, up
  to five times a minute).
* Panic shortcut and kill switch indicator
  ([desktop/horizon.md](desktop/horizon.md#panic)): tested in a nested KWin
  6.3.6 with stand-ins for systemd, NetworkManager, logind, the screen locker
  and pkexec. **UNVERIFIED** on a booted image: the real
  `StartUnit("shadow-panic.service")` under the polkit rule, the real
  screen locker locking on `org.freedesktop.ScreenSaver.Lock` or logind's
  `Session.Lock`, the polkit dialog of `pkexec shadow-panic release`, and
  status files written by the real tools (the tests write the same fields).
  The panic key does nothing while the screen is locked or while an
  application has inhibited global shortcuts (virtual machines, remote
  desktops). With the default smart-hiding panel the kill switch indicator
  is hidden while windows are open (the tile and the panic banner are not).
  The panic banner takes the keyboard only when clicked; from the keyboard
  its buttons are in quick settings (Meta+A, Tab). The shell makes a panic
  pressed during its own release win (it starts the panic service again
  after an authorised release has ended); a release started at the same
  moment from another program (the Control Center, a terminal) is not
  ordered against the panic: whichever `shadow-panic` takes the lock last
  decides, and the banner then shows that state.

## Applications

Not in the image, although the desktop refers to them:

| Missing | Effect |
|---|---|
| Terminal (`shadow-terminal`) — has a README, excluded from package builds | **the desktop image contains no terminal emulator.** The terminal shortcuts and the Arsenal manager's "show usage in terminal" report "not installed". Use a text console (Ctrl+Alt+F3) or install a terminal from Debian |
| Screenshot (`shadow-screenshot`) | the Print shortcuts report "not installed" |
| Editor, Archive Manager, Disk Manager | not available; default dock pins for missing applications are not shown |
| Security Center | the status report exists only as the command `shadow-security-status` |
| Recovery tool | see "Boot" |
| Control Center pages: Appearance, Accessibility, Startup, Language, Date & Time, Storage, Users, Privacy, Firewall, Security, Updates | not available. In particular there is no graphical way to manage users, date and time, the firewall, or the appearance beyond the quick-settings dark-mode switch |
| Shell power module | not integrated |

In the image, with limits stated in their READMEs
([applications/README.md](applications/README.md)); the most important:

* **Arsenal manager:** no PackageKit transaction has ever been run through
  it.
* **Files:** mounting real drives through UDisks2, and SFTP, SMB, NFS and
  WebDAV locations, are untested (SFTP/SMB/NFS also need `kio-extras`, which
  is only recommended and not in the image); "Open terminal here" has no
  terminal to open. Whether Files was part of the desktop image that was
  boot-tested is not recorded.
* **Store:** DEB packages only (no Flatpak or Snap); repositories cannot be
  added; screenshot download unverified.
* **Updater:** periodic check not scheduled; no offline updates.
* **Network Center / Control Center:** Wi-Fi scan and join never performed
  against hardware; no enterprise Wi-Fi set-up; OpenVPN and WireGuard
  activation untested.
* **Control Center → Privacy (kill switch, panic) and Network Center →
  Privacy (encrypted DNS, MAC address):** tested against stand-in tools and
  against the status documents of the real `shadow-killswitch`,
  `shadow-panic`, `shadow-security-status` and `shadow-netprivacy` (lab
  chroot and a throw-away network namespace), and driven with xdotool in the
  lab's HORIZON session; **UNVERIFIED** on a running system: the polkit
  prompts of `org.shadow.killswitch.manage`, `org.shadow.panic.release` and
  `org.shadow.netprivacy.manage`, `org.shadow.security.read-status` answering
  without a prompt, a DNS or MAC change applied by the real tool from the
  page, and the per-network address switches with real NetworkManager
  profiles (the lab has no NetworkManager). The panic options (radios, lock,
  clipboard) are read-only in the Control Center: `shadow-panic` has no
  command that changes them; an administrator edits
  `/etc/shadow-os/panic.json`. The Hide Me and OS integrity controls of the
  same page were tested against stand-ins of `shadow-anon` and
  `shadow-harden` (command lines, exit status, error text) and with
  hand-written state records only; **UNVERIFIED**: the real `shadow-anon`
  or `shadow-harden` run from the page (and their polkit prompts).
* **Monitor:** sensors, batteries, GPUs and services tested against
  synthetic data only; no GPU utilisation for Intel, nouveau or virtual
  GPUs.
* **Assistant (voice):** English only; the offline voice engine needs about
  700 MB of data (in the image only when it was built with network access,
  otherwise `sudo shadow-voice-setup --system`) and about 850 MB of memory
  while it runs; its runtime is x86-64 only, so arm64 has no speech
  recognition or neural voice. Tested with a virtual microphone (in the build
  chroot and on the 0.2.0 image's root filesystem) and a stand-in for the
  Claude API, never with a real microphone, speakers or the real API
  ([applications/assistant-voice.md](applications/assistant-voice.md)).
* **The assistant's model team was never run against NVIDIA's real service.**
  No API key was available in the test environment. The model ids were read
  from NVIDIA's real model list; requests, streaming, tool calls, pictures
  and error answers are tested against a stand-in server only (53 tests).
  Which listed models accept tools or pictures is learnt at run time. A team
  sends one request per model for each message (eight for a question);
  whether an account's request limit allows that was not measured — the team
  shrinks by itself after a "too many requests" answer, and `max_models` in
  `[team]` of `assistant.conf` limits it for good
  ([applications/assistant/README.md](../applications/assistant/README.md#the-model-team)).

## Hardware and drivers

* **Proprietary GPU drivers (NVIDIA) are not included**; nouveau and Mesa
  only. Debian's `non-free` component is not enabled.
* No out-of-tree kernel modules; DKMS drivers are your responsibility and,
  under Secure Boot, need your own signing key.
* No printing or scanning support, no virtual-machine guest tools, no
  fingerprint support out of the box.
* Hibernation is untested and, under Secure Boot, disabled by the kernel.
* See [installation/hardware.md](installation/hardware.md).

## Security

* **Tools not in Debian main** (Metasploit, Burp Suite, Ghidra, …) are
  listed as external with links, **not installed**.
* The "security distribution" consists of on-demand profiles of Debian
  packages. No security tool profile is installed by default, whatever the
  variant file's comment says.
* SHADOW ships no AppArmor profiles; SHADOW applications and helpers run
  unconfined.
* The firewall filters inbound traffic by port only; no per-application
  rules. Outbound traffic is filtered only by the opt-in kill switch. There
  is no graphical firewall management in the image.
* Kill switch and panic ([security/kill-switch.md](security/kill-switch.md))
  are tested with real packets in a network namespace and with the trixie
  nft 1.1.3 binary, but **UNVERIFIED** on a booted image or real hardware:
  * that `shadow-killswitch.service` loads before NetworkManager brings up a
    link (only `systemd-analyze verify` was run on the units);
  * real WireGuard (the test kernel has none; two TUN devices joined by a
    relay, the same device type, stood in for `wg0`) and real OpenVPN under
    NetworkManager, including the first WireGuard handshake waiting for the
    dispatcher's `pre-up` sync;
  * NetworkManager's dispatcher calling the script, and nmcli 1.52's
    output on a real system (a fake nmcli built from the 1.52 source's
    escaping rules was used);
  * IPv6 packets: the test kernel runs with `ipv6.disable=1`; the IPv6
    rules (DHCPv6, neighbour discovery, IPv6 endpoints, LAN6, the
    postrouting drop of `::1` leaving by a real device) are only known to
    load;
  * real virtual machines on a bridge (a veth pair stood in for the guest
    and a macvlan for the network card, with `bridge-nf-call-iptables=0`),
    and a kernel without the nftables bridge family (fake nft only);
  * the effect of blocked NTP on Tor's bootstrap;
  * the panic's NetworkManager and rfkill steps against a real
    NetworkManager and radios, `loginctl lock-sessions` locking KWin's
    screen, and `shadow-panic-reset.service` running before NetworkManager
    and systemd-rfkill at boot (tested against fabricated files only);
  * the maintainer scripts generated by debhelper for the new units (no
    package was built). The hand-written prerm, postinst and postrm of
    `shadow-hardening` and `shadow-base-files` are run by dpkg on stand-in
    packages (`tests/security/test_hardening_maintscripts.py`: install,
    upgrade, remove, purge, the diverted configuration files, the network
    state on the running system and in a chroot), without the debhelper
    snippets and against stand-ins of the programs they call. A container
    that shares the computer's network and boots its own systemd
    (`systemd-nspawn -b` without `--private-network`) counts as the running
    system for them.
* Neither tool reaches other network namespaces: containers, or future
  personas, keep their own rules for traffic on their own interfaces.
* Neither tool covers virtual machines on macvtap/macvlan/ipvlan devices,
  SR-IOV virtual functions or PCI-passthrough network cards: their frames
  pass no netfilter hook of this computer (a macvlan guest was measured to
  keep its network under the kill switch). Routed, NAT and bridged guests are
  covered.
* Hide Me's nat redirects (DNS-over-Tor, the transparent proxy) are not
  watched: after `nft flush ruleset` they stay removed, and the state file
  and the Hide Me page keep showing them on until the next Hide Me action or
  restart (the kill switch itself is put back by the watchdog). systemd-resolved
  keeps asking Tor's DNSPort (its drop-in stays), also over TCP when it falls
  back to TCP: that port is below 1024, so no user can listen there either.
* Something that runs `nft flush ruleset` (or deletes or edits the tables)
  opens the computer until the watchdog, `shadow-killswitch-watch.timer`,
  puts the kill switch and an engaged panic back: at most about 22 seconds
  (every 20 s, accuracy 2 s; at once when it is `nftables.service` that
  starts). Traffic in that window leaves unfiltered. A load that fails is
  reported once and not retried by the watchdog. **UNVERIFIED**: the timer
  firing under a real systemd, and its logging settings (the commands it
  runs were tested in a network namespace with its capability set).
* Hide Me ([security/hardening.md](security/hardening.md#hide-me-shadow-anon))
  is tested with fakes for every program it runs, and its nat chains and
  kill switch hand-over with real packets in a network namespace, but
  **UNVERIFIED** against the real programs:
  * the new network address: `nmcli --temporary` with a salted
    `connection.stable-id`, and whether `nmcli device modify …
    ipv4.dhcp-send-release yes` makes NetworkManager 1.52 send a DHCP
    release on the next `connection down` (no NetworkManager or DHCP server
    was run); interfaces NetworkManager does not manage keep their address
    (the old `macchanger` fallback is gone);
  * Tor itself: `SIGNAL NEWNYM` on `/run/tor/control` (Debian's
    `tor-service-defaults-torrc` is expected to open it with cookie
    authentication; a fake control socket was used), the reload fallback,
    and onion services through the TransPort. Tor's ports below 1024 were
    tested with trixie's real Tor 0.4.9.15, started as root with a `User`
    in a network namespace (with the torrc.d files shadow-anon writes and its
    nat chains): it binds them, keeps `CAP_NET_BIND_SERVICE` with
    `KeepBindCapabilities 1` (without it, a reload that must bind a new such
    port makes Tor exit, and so does a reload that adds or drops the option),
    answers through the redirects, and an unprivileged user cannot bind either
    port (EACCES, with `SO_REUSEADDR` and `SO_BINDTODEVICE` too). Not run:
    Debian's `tor@default` unit under systemd and its AppArmor profile in
    enforce mode (no AppArmor in this kernel); `system_tor` compiles with
    trixie's apparmor_parser 4.1 and grants `capability net_bind_service`
    (abstractions/tor includes nameservice, which includes nis), and the
    unit's `CapabilityBoundingSet` has `CAP_NET_BIND_SERVICE`. Tor's SOCKS
    port is `127.0.0.1:39` and `/run/tor/socks` (`00-shadow-socks.conf`
    replaces the SocksPort list of Debian's defaults, whose 9050 any user
    could hold while Tor was stopped, so that Tor could not start): checked
    in review with the same real Tor (a user looping on 9050 and 39 gets 9050,
    not 39; Tor starts, restarts and survives a reload with 39, 40 and 26 and
    answers SOCKS on 39). torsocks and proxychains4 with their rewritten
    configuration files were not run;
  * DNS-over-Tor against real Tor (its DNSPort was a stand-in). With the real
    systemd-resolved 257 in network namespaces
    (`tests/network/resolved-lab-test.sh`) it is tested that Tor's DNSPort is
    resolved's only global server, also over a user's own provider, and that
    names under a network's search domains (resolved sends them to that
    network's server; a hostile network can claim a whole suffix such as
    `com`) are sent to Tor by the redirect, which no longer exempts
    systemd-resolved, and that resolved's DNS-over-TLS to that server is
    refused (it falls back to plain DNS, i.e. Tor). Consequence: while
    DNS protection is on, names only the network's or a VPN's own server knows
    (for example `printer.lan`, a company's internal names) do not resolve.
    DNS over TCP does not work while DNS protection or the proxy is on (Tor's
    DNSPort has no TCP): it is refused, sent to loopback port 1. Tor's DNSPort
    is UDP `127.0.0.1:26` and its TransPort TCP `127.0.0.1:40`, ports only
    root may bind. Earlier versions used 5353 and 9040: any local user (a
    Flatpak app included: it shares the network namespace) could bind UDP
    5353 next to Tor's socket (Tor sets `SO_REUSEADDR`, so a user's socket
    with `SO_REUSEADDR` may bind the same address; it gets the datagrams,
    always when it is also bound to `lo`) and read and answer every name
    looked up on the system, or take either port while Tor restarted (Tor
    then failed to start) and get every redirected query or connection. The lab test checks that such a user cannot bind the
    DNSPort and gets no name, over UDP or TCP, also when Tor answers truncated
    or not at all (controls: on 5353 it gets resolved's and the programs'
    names). `boot`, which also runs on an upgrade, moves the rules, Tor's
    torrc.d files and resolved's drop-in off the old ports and restarts Tor.
    Over IPv6 every port-53 query and resolved's DNS-over-TLS are refused
    (the chain's IPv6 side; loaded with the real ip6tables-nft in a network
    namespace, but no IPv6 packet was sent: this kernel has IPv6 disabled).
    That resolved then asks the network's IPv4 server is **UNVERIFIED**; on a
    network whose only DNS servers are IPv6, names under its own domains do
    not resolve while DNS protection is on;
  * the AppArmor profile in enforce mode against these code paths (it
    compiles with trixie's apparmor_parser 4.1,
    `tests/security/test_shadow_anon_apparmor.py` with `AA_ROOT` set to an
    extracted trixie apparmor package, but was not loaded).
* While Hide Me's DNS protection or transparent proxy is on and Tor is not
  running (stopped, restarting, failed, or not started yet at boot), the
  redirected queries and connections fail: nothing else may listen on Tor's
  ports. The transparent proxy and Ghost Mode are then not reported as in
  force (`status --json` as root and `boot` check that `tor@default` runs; a
  Hide Me action whose Tor restart fails records it too, and since Debian's
  `tor.service` is a oneshot running `/bin/true`, only `tor@default` counts),
  but nothing watches Tor in between: after a crash the state file and the
  Hide Me page show the last recorded state until the next Hide Me action
  that succeeds or fails, or the next start (`boot`). While a redirect to Tor
  is hooked (proxy or DNS protection), or a Ghost Mode that Tor's failure
  interrupted waits, every action that succeeds (Rotate identity, `tor-new`,
  the timer's `tor-newnym`, the DNS switch, anything else) records the
  re-checked state as `boot` does: a Tor that is still down stays recorded,
  and once Tor runs again Ghost Mode, the proxy and threat level `maximum`
  are recorded again. That holds whether Hide Me's own Tor restart failed or
  Tor stopped by itself (a crash, `systemctl stop`, a package upgrade): while
  the redirect and the kill switch are in place and only Tor is missing, the
  Ghost Mode asked for is kept (`ghost_wanted` in the state file) and only
  turning Ghost Mode, the proxy or the kill switch off, or another threat
  level, drops it. `status --json`, `status` and `leak-test` run as root
  re-check it too (the proxy and Ghost Mode not in force, DNS protection's
  lookups failing), but record nothing; an error recorded while Tor was down,
  also one that joins several problems (an upgrade's failed Tor restart and
  the lookups failing), no longer shows in them once Tor runs. On a running
  system (an install or upgrade, or `systemctl restart shadow-anon-boot`)
  `boot` starts a Tor that does not run while DNS protection or the proxy
  needs it, waits for it and records it if it does not come up; only at
  start-up (`systemctl is-system-running`: initializing or starting) does it
  just queue Tor's start, so a Tor that fails then is recorded by the next
  action or check, not by `boot`. Only the page's "Tor: running" / "Tor: stopped" (the
  Rotate identity row) asks `tor@default` itself, each time the page opens or
  finishes an action. A machine that lowers `net.ipv4.ip_unprivileged_port_start`
  to 40 or below (0 in some containers) makes Tor's ports bindable by anyone
  again: a port is privileged only while it is below that value, so at 40 any
  user may take the TransPort (TCP `127.0.0.1:40`) while Tor restarts and get
  every redirected connection, at 39 or below also Tor's SOCKS port and at 26
  or below also the DNSPort. Keep it above 40.
* Tor's SOCKS port is `127.0.0.1:39` (or the socket `/run/tor/socks`), not
  Debian's 9050. Any local user may still bind 9050, but every new TCP
  connection to 9050 on an IPv4 loopback address goes to Tor's port 39 (the
  iptables-nft chain `SHADOWSOCKS` in nat `OUTPUT`, loaded by `boot` at every
  start, install and upgrade, and by `shadow-anon tor-socks`, whatever Hide Me
  does): programs that use 9050 with no setting to change (`tor-resolve`,
  `sqlmap --tor`, a user's own scripts) reach Tor, and are refused while Tor
  does not run, never a user's listener on 9050 (tested with real packets in
  a network namespace; the same rule was checked in review against the real
  Tor 0.4.9.15 with `tor-resolve` and sqlmap's choice of port). Its IPv6
  side (`ip6tables`, wherever the kernel has IPv6) refuses `[::1]:9050`
  (sent to port 1 on `::1`, where nothing listens and only root may bind):
  clients that name the proxy `localhost:9050` (curl, Python's requests
  through PySocks, `ALL_PROXY=socks5h://localhost:9050`) try `::1` first
  (`::1 localhost` in `/etc/hosts`, and RFC 6724 puts it before 127.0.0.1)
  and move on to 127.0.0.1:9050, i.e. Tor, only when `::1` refuses; without
  it a user who bound `[::1]:9050` or `[::]:9050` got their names and
  traffic. A client that tries only `::1` fails instead. The IPv6 rule was
  loaded with the real ip6tables-nft in a network namespace, but no IPv6
  packet was sent here (this kernel has IPv6 disabled); the clients' fallback
  to 127.0.0.1 on a refused `::1` was checked in review in a VM with IPv6.
  Without iptables (shadow-hardening only recommends it) nothing redirects
  9050: `boot` and `tor-socks` then fail (exit status 3) and record it as the
  state's error, which the Hide Me page and `status` show until the next Hide
  Me action that succeeds (every start records it again). Not covered:
  sqlmap's fallback port 9150 when Tor does not run, and connections to the
  computer's own network addresses (IPv4 or IPv6) on 9050; a service of
  one's own on 127.0.0.1:9050 or `[::1]:9050` gets no local connection.
  torsocks and proxychains4 are pointed at 39 while their files still name
  127.0.0.1:9050 (a changed file of theirs makes dpkg ask on their next
  upgrade). Like Hide Me's other chains, `SHADOWSOCKS` is not watched: after a
  ruleset flush it is gone until the next start or `sudo shadow-anon
  tor-socks`. Removing shadow-hardening keeps it until the next restart;
  purging removes it (both sides), while Tor's SOCKS ports file stays (Tor keeps port 39):
  delete `/etc/tor/torrc.d/00-shadow-socks.conf` and restart Tor to give it
  9050 back.
* Encrypted DNS and MAC privacy ([security/private-dns.md](security/private-dns.md))
  are tested with the real systemd-resolved 257 of trixie in network
  namespaces (`tests/network/resolved-lab-test.sh`: the shipped file in force,
  routing with `Domains=~.`, opportunistic and strict DNS-over-TLS, the
  certificate check, the per-network relaxation, allow-downgrade against a
  forwarder without DNSSEC), with dnscrypt-proxy 2.1.8's `-check` on the
  generated configuration, and with fakes for every command; **UNVERIFIED**:
  * NetworkManager 1.52 applying the shipped defaults (no NetworkManager ran;
    the values were checked against its source, which showed that the old
    `dhcp-send-hostname=no` was ignored and the host name was sent):
    one address per network and boot from `${CONNECTION}/${BOOT}`, the
    DHCPv6 DUID from the same stable-id (`ipv6.dhcp-duid=stable-uuid`; no
    DHCPv6 ran, this kernel has IPv6 disabled), the DHCP
    release, the `wl`/`eagle_sdio` exception, `nmcli general reload conf`
    switching an upgraded system to `dns=systemd-resolved`, and the
    connectivity check still finding captive portals with `Domains=~.`;
  * DNS-over-HTTPS end to end (dnscrypt-proxy reaching a DoH server, its
    socket activation with the drop-in, the start-up probe's timing, DNSSEC
    through the proxy), and the real providers' certificates, IPv6 servers
    and DNSSEC answers (the lab's fake servers do not sign);
  * `shadow-netprivacy.service` at boot (`systemd-analyze verify` only; the
    command it runs is tested);
  * the image build with systemd-resolved in the base layer (desktop image
    only; the minimal image has none, since without shadow-hardening it
    would answer LLMNR and send plain DNS): `in_chroot`
    was tested on an overlay of a trixie chroot, the final `resolv.conf` and
    libnss-resolve checks were not run in a build;
  * the maintainer-script steps (restart of systemd-resolved, NetworkManager
    reload) — no package was built; Wi-Fi hardware.
* `shadow-harden fs-harden` mounts `/proc` with `hidepid=2,gid=<proc>` and
  adds `polkitd` to the group `proc` first: the kernel side is tested in a
  namespace (`tests/security/test_hidepid.py`); pkexec under hidepid on a
  running system is **UNVERIFIED** (no polkitd ran). Process monitors of
  other users still see only their own processes, as intended. The upgrade
  of a 0.2.0 system whose `proc-shadow-hidepid.service` has no group
  (`shadow-harden migrate-hidepid` from postinst) is tested with fakes and
  with dpkg on stand-in packages, not on an upgraded installation.
* The Arsenal's sizes (`profiles/sizes.json`): the Blue Team figures, without
  firewalld, ufw and the OpenSnitch UI, were recomputed on their own on
  2026-11-23; the other profiles' date from 2026-11-11, and those of Core
  Security, Network Security, Red Team and Complete Arsenal were computed for
  an earlier tool list. [arsenal/docs/profiles.md](../arsenal/docs/profiles.md)
  says both.
* `debian/control`'s `shadow-arsenal-pentest` stanza (43 tools) is older than
  the one `arsenal/packaging/control.in` generates (84 tools), and
  `shadow-arsenal-complete`'s description still counts 237 tools; both were
  left alone. The 84-tool profile depends on 13 packages that none of the
  image's package sources has (trixie `main`, `contrib`, `non-free-firmware`):
  commix, enum4linux, exploitdb, impacket-scripts, kismet, mitmproxy, netexec,
  nikto, radare2, seclists, theharvester, wordlists and zaproxy. The desktop
  image installs `shadow-arsenal-complete`, which already depends on
  `shadow-arsenal-pentest`, so copying the generated stanza into
  `debian/control` would make the image build's package installation fail.
  For the same reason `tools/profile-size.py` cannot size those two profiles,
  and profiles.md's "every tool below is a package in trixie `main`" does not
  hold for Penetration Testing.
* Audit rules are not locked.
* The lock screen has a 5-second grace period by default.
* No vulnerability in a SHADOW component can currently be fixed on installed
  systems through an update channel.
* The complete list of deliberate exceptions is in
  [security/hardening.md](security/hardening.md).

## Documentation

* Source comments refer to documents that do not exist under those names:
  `docs/architecture/build-environment.md`, `docs/architecture/image-build.md`,
  `docs/kernel/configuration.md`, `docs/kernel/patches.md`,
  `docs/security/secure-boot.md`, `docs/packaging/licensing.md`,
  `distro/release/channels.md`, `distro/signing/README.md`. The content is
  in [packaging/building.md](packaging/building.md),
  [packaging/iso.md](packaging/iso.md), [kernel/README.md](kernel/README.md),
  [architecture/boot.md](architecture/boot.md) and
  [packaging/repositories.md](packaging/repositories.md), as far as it
  exists.
* `build/iso/build.sh` tells you to run `make packages repo`; there is no
  Makefile. Use `build/packages/build.sh` and `build/packages/repo.sh`.
* Troubleshooting and recovery procedures are derived from the design and
  have not been carried out on broken systems.
