# Updates

Where updates come from, how to install them, what happens to the kernel
and the boot loader, and what to do when an update fails.

## Where updates come from

| What | Source | State |
|---|---|---|
| Debian packages — almost the whole system, including the kernel, GRUB, shim, KWin, Qt, firmware and browsers | `trixie-security` (security fixes) and `trixie-updates` / `trixie` point releases, from `deb.debian.org` over HTTPS, authenticated with Debian's archive keyring | enabled |
| SHADOW packages (`shadow-*`: shell, applications, installer, hardening, themes) | the SHADOW repository | **no hosted repository exists.** SHADOW packages on an installed system receive **no updates** until one is published and enabled ([../packaging/repositories.md](../packaging/repositories.md)) |
| Device firmware (UEFI, SSD, docks) | LVFS through `fwupd` | `fwupd` is installed; there is no SHADOW front-end. Use `fwupdmgr refresh && fwupdmgr get-updates` yourself; untested |

Security support for the base system is therefore Debian's: the security
team's advisories for Debian 13 apply as they are. A vulnerability in a
SHADOW component has no update channel today — see [../../SECURITY.md](../../SECURITY.md).

**Updates are not installed automatically, and nothing checks for them in
the background.** `unattended-upgrades` is not in the image, and the
Updater's periodic check is not scheduled by anything yet. Check regularly
yourself.

## Installing updates

### SHADOW Updater

Start **Updater** from the launcher (`shadow-updater`).

| Page | Use |
|---|---|
| Updates | "Check" refreshes the package lists. Updates are grouped: **security** (what PackageKit reports as security updates — derived from the security archive, never guessed from names), kernel, SHADOW packages, others, and held-back updates. Install all, security only, or a selection. Every installation is simulated first and removals are confirmed |
| History | what was installed, upgraded and removed, from APT's history log |
| Last transaction | the log of the last job run from the Updater; "Copy details for a bug report" |
| Recovery | checks whether the package system is consistent, and offers Repair |
| Software sources | the configured APT sources and whether their indexes are signed |
| Settings | automatic check and interval (stored; see the note above) |

The Updater has no privileges of its own. PackageKit asks for
authorisation through polkit; the shell shows the authentication dialog.

Verified in the build environment against a real PackageKit daemon: a
security update was detected and installed through the GUI
(`applications/updater/README.md`). Not verified: the polkit prompt for an
ordinary user, and the Updater in the booted image.

### Command line

```sh
sudo apt update
apt list --upgradable
sudo apt full-upgrade
```

A check without a window, for scripts:

```sh
shadow-updater --check ; echo $?
# 0 = no updates, 10 = updates, 11 = security updates, 1 = the check failed
```

The SHADOW Store also lists and installs updates.

## Security updates

* They come from `trixie-security`. In the Updater they form their own
  group and can be installed on their own ("security only").
* On the command line, `apt full-upgrade` installs everything available,
  security updates included.
* To see what is pending and why:
  `shadow-updater --dump updates`, or
  `sudo shadow-security-status --section updates --pretty`.

## Kernel updates

The image has Debian's metapackage `linux-image-amd64`, so a kernel update
arrives as a new `linux-image-6.12.…` package:

1. the new kernel is installed next to the running one;
2. Debian's kernel hooks build its initramfs and run `update-grub`, so the
   new kernel becomes the default boot entry (`GRUB_SAVEDEFAULT=false`: the
   default is always the newest);
3. the previous kernel stays installed and remains under "Advanced options"
   in the boot menu.

The new kernel is signed by Debian, so Secure Boot keeps working. It is
used after a restart. Out-of-tree (DKMS) modules, if you installed any, are
rebuilt by DKMS and under Secure Boot need your own signature.

Kernel updates through this path are standard Debian behaviour; a kernel
update on an installed Shadow OS system has not been observed yet.

## Boot-loader updates and the trigger

GRUB and shim are special: the copies the firmware actually boots are on
the EFI System Partition (or in the boot sector), not under `/usr`. A
package update alone would leave the old copies in use — including after a
security fix, and including copies that a later Secure Boot revocation
would refuse to start.

`shadow-boot` closes that gap:

* it has a dpkg trigger on `/usr/lib/grub` and `/usr/lib/shim`;
* when an update changes files there, `shadow-grub-install` runs
  automatically and re-installs the boot loader exactly the way the system
  was installed (`/etc/shadow/boot.conf`): signed binaries to the ESP, the
  "Shadow OS" firmware entry, the legacy boot sector where applicable.

If the refresh fails, the update itself is not failed. Instead a marker is
written to `/var/lib/shadow/boot-loader-refresh-failed` and a warning is
printed in the package log. `sudo shadow-security-status --section secureboot`
reports the marker. To fix it:

```sh
sudo shadow-grub-install --verbose
```

and read the output. One cause the script checks for explicitly is an EFI
System Partition that is not mounted at `/boot/efi`. A successful automatic refresh removes the
marker; after a successful manual run you can remove it yourself
(`sudo rm /var/lib/shadow/boot-loader-refresh-failed`).

Details: [../architecture/boot.md](../architecture/boot.md). The manual
re-installation is exercised by the installation tests; a triggered refresh
from a real GRUB or shim update has not been observed.

## Restart notices

The Updater shows a restart banner when any of these is true:

* `/run/reboot-required` exists (and lists packages in
  `/run/reboot-required.pkgs`);
* PackageKit reported that a transaction run from the Updater requires a
  restart;
* a newer kernel is installed in `/boot` than the one running.

"Restart now" asks first and then restarts through systemd-logind; it is
offered only when logind allows a restart. Services are not restarted
automatically after library updates: when in doubt, restart the computer.

## Recovery after a failed update

Work through these in order.

### 1. The package system was interrupted

Symptoms: `apt` reports "dpkg was interrupted", packages are half-installed
or unconfigured.

* Updater → **Recovery** shows the result of `dpkg --audit` and offers
  **Repair** (PackageKit's `RepairSystem`), simulated first.
* Or in a terminal:

  ```sh
  sudo dpkg --configure -a
  sudo apt -f install
  sudo apt full-upgrade
  ```

If the PackageKit daemon itself cannot start, only the terminal route
works.

### 2. The desktop does not start after an update

Switch to a text console with **Ctrl+Alt+F3**, log in, and run the commands
above. Logs: [../troubleshooting/README.md](../troubleshooting/README.md).

### 3. The new kernel does not boot

In the boot menu choose **Advanced options** and the previous kernel. Then
remove or hold the faulty kernel package until a fixed one is available:

```sh
sudo apt-mark hold linux-image-amd64
```

(`apt-mark unhold` releases it.)

### 4. The boot loader is broken

Boot the installation medium and repair from the live session:
[../troubleshooting/recovery.md](../troubleshooting/recovery.md).

### What does not exist

There are no snapshots and no automatic rollback
([../packaging/repositories.md](../packaging/repositories.md), "Rollback").
Going back to an earlier package version is manual and only possible while
that version is still available. Back up before large updates if the
machine matters.

## Release upgrades

There is one Shadow OS version (0.1.0) and one Debian release (13). No
upgrade path to a future Shadow OS version or Debian release is defined or
tested.
