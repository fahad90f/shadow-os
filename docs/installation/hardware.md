# Hardware

> **Nothing on this page has been tested on physical hardware.** Shadow OS
> has only been booted in QEMU virtual machines with emulated devices. The
> first sections say what is *in the image*; the later sections say what is
> *expected* to work because Debian 13 supports it with the same kernel and
> firmware. Treat every expectation as unverified, and try the live session
> on your machine before installing.

## What determines hardware support

* The kernel is Debian's `linux-image-amd64` (6.12 series) with Debian's
  configuration. Driver support is therefore that of Debian 13.
* Firmware comes from Debian's `non-free-firmware` component — the same
  packages Debian's own installation images include.
* Graphics use Mesa. There are no proprietary drivers.

## Firmware and microcode in the desktop image

From `distro/base/package-lists/15-firmware.list`. The minimal image does
not include this list.

| Package | For |
|---|---|
| `firmware-linux-free`, `firmware-linux-nonfree`, `firmware-misc-nonfree` | assorted devices (Debian's general firmware collections) |
| `firmware-iwlwifi` | Intel wireless adapters |
| `firmware-realtek` | Realtek network, wireless and Bluetooth devices |
| `firmware-atheros` | Qualcomm Atheros wireless and Bluetooth devices |
| `firmware-brcm80211` | Broadcom/Cypress 802.11 wireless (brcmfmac/brcmsmac) |
| `firmware-mediatek` | MediaTek wireless and Bluetooth devices |
| `firmware-libertas` | Marvell Libertas wireless |
| `firmware-amd-graphics` | AMD/ATI graphics |
| `firmware-intel-graphics` | Intel graphics (GuC/HuC/DMC) |
| `firmware-intel-misc` | other Intel devices |
| `firmware-nvidia-graphics` | firmware used by the free `nouveau` driver for NVIDIA cards |
| `firmware-sof-signed` | Intel Sound Open Firmware (audio DSP of recent laptops) |
| `firmware-bnx2`, `firmware-bnx2x` | Broadcom NetXtreme II Ethernet |
| `intel-microcode`, `amd64-microcode` | CPU microcode updates (amd64) |

These packages are non-free but redistributable; their licences are in each
package's copyright file in the image.

## Drivers and services in the desktop image

From `10-core.list` and `40-desktop.list`:

| Area | Packages |
|---|---|
| Graphics | `libgl1-mesa-dri`, `libegl-mesa0`, `libgbm1`, `libglx-mesa0`, `mesa-vulkan-drivers`, `mesa-va-drivers` (Intel, AMD, nouveau, virtual GPUs, software rendering) |
| Display | `kwin-wayland`, `xwayland`, `sddm`, `libkscreen-bin` |
| Audio | `pipewire`, `pipewire-pulse`, `pipewire-alsa`, `wireplumber`, `alsa-utils` |
| Networking | `network-manager`, `wpasupplicant`, `wireless-regdb`, `iw` |
| Bluetooth | `bluez` |
| Power | `upower`, `power-profiles-daemon` |
| Storage | `udisks2`, `cryptsetup`, `lvm2`, `e2fsprogs`, `dosfstools`, `btrfs-progs`, `xfsprogs`, `ntfs-3g`, `exfatprogs`, `nvme-cli`, `smartmontools` |
| Firmware updates | `fwupd` (no SHADOW front-end; use `fwupdmgr`) |
| Inspection | `pciutils`, `usbutils`, `lshw`, `dmidecode`, `lm-sensors` |

## Verified (virtual hardware only)

| Environment | Result |
|---|---|
| QEMU q35 machine, OVMF UEFI, emulated CPU (TCG, no KVM) | minimal image boots from CD and USB; installed systems boot |
| The same with Secure Boot enforcing (Microsoft keys enrolled) | minimal image boots; whole-disk installation boots |
| QEMU with SeaBIOS (legacy BIOS) | minimal image boots from CD and USB; whole-disk installation boots |
| QEMU emulated display without a GPU render node | desktop image reaches the Shadow Horizon session with CPU rendering |
| virtio and emulated disks | used by the installation tests |

## Expected to work on common x86_64 hardware — untested

These expectations rest on Debian 13's hardware support. None has been
observed with Shadow OS.

| Hardware | Expectation | Notes |
|---|---|---|
| 64-bit Intel and AMD PCs with UEFI | boot and install | Secure Boot expected to work with firmware that trusts the Microsoft UEFI CA |
| PCs with legacy BIOS | boot and install | amd64 only |
| Intel integrated graphics | accelerated desktop | Mesa; firmware included |
| AMD Radeon graphics | accelerated desktop | Mesa; firmware included |
| NVIDIA graphics | basic desktop through `nouveau` | performance and power management depend strongly on the card generation; see below |
| SATA, NVMe, USB storage | work | |
| Wired Ethernet (Intel, Realtek, Broadcom) | works | |
| Wi-Fi with Intel, Realtek, Atheros, MediaTek, Broadcom (brcmfmac) chipsets | works where a mainline driver exists | firmware included |
| Bluetooth | works where a mainline driver exists | |
| Audio (HDA, USB audio, SOF laptops) | works | PipeWire |
| Touchpads, keyboards, mice | work | libinput through KWin |
| Suspend to RAM | depends on the machine | untested in any form, including in QEMU |
| Laptop backlight keys, battery status | work where the firmware exposes them | the status module's brightness and battery code was tested against fixtures only |
| External monitors, HiDPI, fractional scaling | handled by KWin | the shell has not been tested with several screens or HiDPI |

If the graphics driver fails, the session falls back to CPU rendering when
no GPU render node exists; you can force it
([../troubleshooting/README.md](../troubleshooting/README.md)). With CPU
rendering there are no blur or glass effects and animations cost noticeable
CPU time.

## Not included or not supported

| Item | State |
|---|---|
| Proprietary NVIDIA driver | not included and not packaged. `nvidia-driver` exists in Debian's `non-free` component, which Shadow OS does not enable. Installing it is your own responsibility; under Secure Boot its DKMS module must be signed with a key you enrol |
| Broadcom `wl` (broadcom-sta) and other DKMS-only drivers | not included; `non-free` is not enabled |
| Out-of-tree kernel modules | none shipped (`kernel/modules/README.md`) |
| Hibernation | the engine sets a resume device when you choose a swap partition; hibernation itself is untested. With a swap file no resume device is configured. Kernel lockdown under Secure Boot disables hibernation |
| TPM-based disk unlocking | not implemented |
| Fingerprint readers | PAM files for the lock screen have a fingerprint path (`pam_fprintd`), but `libpam-fprintd` is only suggested, not installed, and nothing is tested |
| Printers and scanners | no printing system (CUPS) or scanner support is in the package lists |
| Touchscreens, tablets, convertibles | no on-screen keyboard; untested |
| 32-bit PCs, 32-bit UEFI | not supported |
| arm64 (including Apple Silicon, Raspberry Pi, ARM laptops) | not supported: no arm64 image has been built or booted |
| Virtual machine guest tools | none of `qemu-guest-agent`, `spice-vdagent`, `open-vm-tools` or VirtualBox guest additions is in the package lists |

## Checking your hardware in the live session

```sh
lspci -nnk            # PCI devices and the kernel driver in use
lsusb                 # USB devices
sudo dmesg | grep -i -E 'firmware|failed|error'
nmcli device          # network devices NetworkManager sees
ls /dev/dri           # renderD* present = GPU acceleration available
shadow-sysinfo --dump system
```

A line such as "firmware: failed to load …" in the kernel log names a
firmware file that is missing from the image.
