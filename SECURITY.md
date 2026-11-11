# Security policy

## Supported versions

| Version | Supported |
|---|---|
| Current development version (the `main` branch; 0.1.0 "obsidian", channel `dev`) | yes — fixes are made here |
| Anything else | no |

There has been no stable release. Shadow OS 0.1.0 is a development version
that has only been tested in virtual machines; do not rely on it to protect
anything important ([docs/limitations.md](docs/limitations.md)).

Be aware of how fixes reach users today: **no hosted SHADOW package
repository exists**, so a fix in a SHADOW component reaches a system only
through a newly built image or packages you build yourself. Fixes for the
Debian base system arrive through Debian's security archive, which is
enabled on every Shadow OS system
([docs/security/updates.md](docs/security/updates.md)).

## Reporting a vulnerability

**Do not open a public issue for a security vulnerability.**

Open a **private security advisory** on the project's GitHub repository:
on <https://github.com/fahad90fa/Custom-OS>, go to the *Security* tab and
choose *Report a vulnerability*. This creates a report that only the
maintainers can see.

Please include:

* the component and file (for example `security/firewall/shadow-firewall`,
  `applications/network-center/network-helper`), and the commit or image
  you examined;
* what an attacker can do, and what they need for it (local user, network
  position, physical access, a malicious file or D-Bus client);
* steps to reproduce, or a proof of concept. Keep it minimal, and do not
  include other people's data;
* whether the problem is in SHADOW's own code or configuration, or in a
  Debian package that SHADOW ships;
* how you would like to be credited, if at all.

If you are unsure whether something is a vulnerability, report it this way
anyway.

### What to expect

This is a small project without a dedicated security team and without a
guaranteed response time. The maintainers will read the report, confirm or
question the finding in the advisory, and work on a fix in private. Once a
fix is in `main`, the advisory is published with credit to the reporter
unless you ask otherwise. Please give the maintainers a reasonable time to
fix the problem before you publish details.

## Scope

In scope — code and configuration in this repository:

* the installer engine and graphical installer (`system/installer`);
* boot-loader management (`system/boot`) and the live-media boot menu;
* the security baseline (`security/`): sysctl and kernel command-line
  settings, the nftables ruleset and `shadow-firewall`, audit rules,
  `shadow-security-status`, polkit policies;
* privileged helpers and polkit actions of SHADOW applications
  (for example `applications/network-center/network-helper`), and anything
  started through `pkexec`;
* the shell (`desktop/horizon-shell`), in particular the polkit
  authentication agent, the notification service and the system tray, which
  process input from other programs on the session bus;
* the login-screen theme and the lock screen (`desktop/themes`), including
  the PAM files shipped for the lock screen;
* SHADOW applications (`applications/`, `arsenal/manager`);
* the build system (`build/`, `scripts/`, `debian/`, `kernel/`) as far as a
  flaw affects what is shipped — for example the image's APT configuration,
  repository signing, file permissions in the image, or secrets ending up
  in an image or a log;
* defaults that are weaker than documented in
  [docs/security/hardening.md](docs/security/hardening.md).

Out of scope here — report to the upstream project instead:

* vulnerabilities in **Debian packages** that Shadow OS ships unmodified:
  the Linux kernel, GRUB, shim, systemd, KWin, Qt, KDE Frameworks, SDDM,
  PackageKit, NetworkManager, Firefox and so on. Report them to Debian
  (<https://www.debian.org/security/>) or to the upstream project. If the
  problem is in how SHADOW **configures or uses** such a component, it is in
  scope;
* vulnerabilities in the **security tools** that SHADOW Arsenal can install.
  They are unmodified Debian packages; the tools listed as "external" are
  not distributed by SHADOW at all;
* the known limitations listed in
  [docs/limitations.md](docs/limitations.md) and the documented exceptions
  in [docs/security/hardening.md](docs/security/hardening.md) — for example
  that the development archive key has no passphrase, that the GRUB menu
  has no password, that SHADOW ships no AppArmor profiles, or that the live
  session grants passwordless `sudo`. Proposals to change them are welcome
  as ordinary issues or pull requests;
* attacks that require an administrator (`sudo`) on the machine already,
  unless they cross a boundary that is supposed to hold (Secure Boot,
  kernel lockdown, disk encryption at rest);
* findings from using the security tools against systems you do not own.

## Using Shadow OS responsibly

The tools SHADOW Arsenal installs are for authorised testing, research,
defence and education only. Use them only on systems you own or have
explicit written permission to test:
[arsenal/docs/legal-use.md](arsenal/docs/legal-use.md).
