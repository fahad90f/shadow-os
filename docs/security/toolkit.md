# Security toolkit: SHADOW Arsenal

SHADOW Arsenal is how security tools get onto a Shadow OS system: curated
**profiles** of Debian packages, installed on demand through the package
manager. Source: `arsenal/`.

> **Use these tools only on systems you own or have explicit, written
> permission to test.** Read
> [arsenal/docs/legal-use.md](../../arsenal/docs/legal-use.md) before you
> install a profile, and practise in an isolated lab:
> [arsenal/docs/lab-setup.md](../../arsenal/docs/lab-setup.md).

## What is and is not installed by default

**No security tool profile is installed by default.** The desktop image
contains:

| In the image | Package |
|---|---|
| the Arsenal manager application | `shadow-arsenal` |
| the profile manifests | `shadow-arsenal-data` |
| the profile metapackages, as files in the on-disk repository `/usr/share/shadow/repository` (not installed) | `shadow-arsenal-<profile>` |
| general network utilities that are part of the base system | for example `tcpdump`, `traceroute`, `dig`, `whois`, `curl`, `openssh-client` |

The tools of a profile are downloaded from Debian when you install the
profile; you need a network connection.

(The variant file `distro/base/variants/desktop.conf` describes the image as
containing "the Core Security toolkit". The package lists do not install
`shadow-arsenal-core`; what is stated here is what the build does.)

## Profiles

| Profile | Metapackage | Tools | External entries |
|---|---|---:|---:|
| Core Security | `shadow-arsenal-core` | 15 | 1 |
| Web Application | `shadow-arsenal-web` | 19 | 4 |
| Network Security | `shadow-arsenal-network` | 26 | 0 |
| Red Team | `shadow-arsenal-redteam` | 25 | 6 |
| Blue Team | `shadow-arsenal-blueteam` | 20 | 4 |
| Forensics | `shadow-arsenal-forensics` | 22 | 1 |
| Reverse Engineering | `shadow-arsenal-reversing` | 23 (one amd64-only) | 3 |
| Developer | `shadow-arsenal-developer` | 25 | 0 |
| Complete Arsenal | `shadow-arsenal-complete` | all of the above: 159 distinct | 18 distinct |

The contents of every profile, with download and installed sizes, are in
[arsenal/docs/profiles.md](../../arsenal/docs/profiles.md) (generated from
the manifests). The Complete Arsenal is large — about 1.1 GB to download and
4.9 GB installed according to that file; most people are better served by
the specific profiles they need.

Rules the profiles follow (`arsenal/README.md`):

* Every package is in Debian 13 "trixie" **main** — no contrib, no
  non-free, no third-party repository. `arsenal/tools/check-availability.py`
  verifies this for amd64 and arm64.
* The repository contains manifests only: no exploit code, payloads,
  malware samples, credentials or word lists.
* A metapackage **recommends** its tools (it does not depend on them), so a
  single tool can be removed again without removing the profile.
* `complete` depends on the other profile metapackages.

## Installing and removing

### With the Arsenal manager

Start **SHADOW Arsenal** from the launcher (`shadow-arsenal`). On first start
you must acknowledge the conditions of responsible use; the acknowledgement
is stored in `~/.config/shadow/arsenal.conf` and records only that you were
told — it authorises nothing.

* **Install profile** installs the metapackage. The manager resolves the
  packages, simulates the transaction, shows every package that would be
  installed and the download size, and runs the transaction after you
  confirm. Authorisation is PackageKit's, through polkit.
* **Remove profile** is offered when the metapackage is installed. It
  removes the metapackage with autoremove: tools that came with it and that
  nothing else needs are removed, tools you installed by hand stay.
* **Install / remove a single tool** from the tool list. Removal is not
  offered for essential packages, and a removal that would take a SHADOW
  system package with it is refused.
* Each tool has a details view with its start command; command-line tools
  can be opened in a terminal showing their help.

If PackageKit is not available, the buttons open a dialog with the exact
`apt` command instead.

> The manager's PackageKit transactions have **not been tested** (there is
> no system bus in the build environment). If something fails, use `apt` as
> below.

### With apt

```sh
sudo apt update
sudo apt install shadow-arsenal-web          # a profile
sudo apt install nmap                       # a single tool
sudo apt remove nmap
sudo apt remove --autoremove shadow-arsenal-web   # a profile and the tools nothing else needs
```

`apt install` follows Recommends by default, so installing a metapackage
installs its tools. Do **not** add `--no-install-recommends`: you would get
the empty metapackage only.

The metapackages come from the on-disk repository
(`/etc/apt/sources.list.d/shadow-local.sources`); this has not been verified
on an installed system. If `apt` cannot find `shadow-arsenal-<profile>`,
install the tools directly — the package names are in
[arsenal/docs/profiles.md](../../arsenal/docs/profiles.md).

### During installation

The installer's Software step lets you select profiles. As implemented, the
engine installs the metapackages without their recommended tools; install
profiles after the first start instead
([../installation/installing.md](../installation/installing.md)).

## External tools policy

Some well-known tools are not in Debian `main` — for example Metasploit
Framework, Burp Suite, OWASP ZAP, Nikto, Nuclei, Ghidra, radare2/Cutter,
Frida, BloodHound, Responder, NetExec, Sliver, Volatility 3, Zeek, Wazuh.

* They are **listed**, per profile, as external entries with their licence,
  the reason they are not included, and a link to the vendor's official
  installation instructions.
* Shadow OS **does not install, download or repackage them**, and enables no
  third-party repository for them. The manager shows them as "not
  installable from SHADOW repositories" and opens the vendor's page.
* If you install one yourself, you are trusting its vendor's packaging and
  update channel, not Debian's or SHADOW's. Verify signatures and checksums
  as the vendor documents, and prefer installing into a lab machine.
* The session sets `_JAVA_AWT_WM_NONREPARENTING=1` so that Java tools such
  as Burp Suite and Ghidra lay out correctly through Xwayland. That is the
  only accommodation made for them; none of them has been tested.

The repository component `security` is reserved for the profile
metapackages and for tools SHADOW might package itself (`shadow-tool-*`);
no such tool package exists.

## Legal and responsible use

The short form; the full text is
[arsenal/docs/legal-use.md](../../arsenal/docs/legal-use.md).

* Test only what you own or have explicit written permission to test, given
  by someone entitled to give it. Shared and third-party infrastructure has
  its own owner.
* Agree scope, methods, time window, contacts, data handling and reporting
  in writing before you start, and stop when you reach something not
  covered.
* Unauthorised access, interference with systems, interception of
  communications and misuse of credentials are criminal offences almost
  everywhere. Some jurisdictions also regulate tools, exports and radio
  transmission. You are responsible for knowing the law that applies to you.
* The tools come with no warranty. Several can interrupt services or
  destroy data when used carelessly.
* This is not legal advice.

To practise, build an isolated lab of virtual machines:
[arsenal/docs/lab-setup.md](../../arsenal/docs/lab-setup.md). It uses
libvirt/QEMU from the Developer profile.

## Things to know when using the tools on Shadow OS

* **Firewall.** Inbound connections are dropped by default. A listener for
  a reverse connection, a local web server for a test, or a capture
  interface that expects unsolicited traffic needs an opened port:
  `sudo shadow-firewall allow 4444/tcp --name "lab listener"`, and
  `sudo shadow-firewall remove 4444/tcp` afterwards
  ([hardening.md](hardening.md)).
* **Packet capture as a user.** Debian's `wireshark-common` decides
  whether members of the group `wireshark` may capture without root
  (`sudo dpkg-reconfigure wireshark-common`; the default is no). The
  installer adds your user to that group only if the group already exists
  at installation time; otherwise add yourself with
  `sudo adduser "$USER" wireshark` and log in again. Without it, the
  Network Center captures through its privileged helper.
* **MAC address randomisation** is on for Wi-Fi by default; tools that
  expect the hardware address need the connection set to `permanent`.
* **Kernel hardening.** `ptrace` is limited to descendants, unprivileged
  eBPF and `perf` are off, some protocol modules are not auto-loaded. Tools
  that need these (debuggers attaching to running processes, eBPF tracers,
  protocol fuzzers) need `sudo` or a module loaded by hand
  ([hardening.md](hardening.md)).
* **Wireless tools** (monitor mode, injection) depend entirely on the Wi-Fi
  chipset and driver. Nothing has been tested on hardware.

## Maintaining the profiles

For contributors: [arsenal/docs/adding-tools.md](../../arsenal/docs/adding-tools.md)
(what may be listed, the manifest format, the verification commands) and
`arsenal/packaging/README.md` (how the manifests become packages).

```sh
python3 arsenal/tools/validate.py                 # manifests against the schema; needs no chroot
python3 arsenal/tools/gen-control.py --check      # arsenal/packaging/control.in up to date
python3 arsenal/tools/gen-profiles-doc.py --check # arsenal/docs/profiles.md up to date
```

State recorded in `arsenal/README.md`: 9 profiles and 159 packages validate;
all 159 packages were found in trixie main for amd64 and arm64; all profiles
resolve with `apt-get --simulate` in the build chroot. Nothing was run on
arm64.
