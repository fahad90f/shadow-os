# Troubleshooting

Problems you may meet and what to do about them. Because NEXUS OS has only
run in virtual machines so far, most of this page is derived from how the
system is built, not from observed failures on real hardware; where a
procedure has not been tried, it says so. When you find something that is
wrong or missing here, please report it
([../../CONTRIBUTING.md](../../CONTRIBUTING.md)).

For a system that no longer boots, see [recovery.md](recovery.md).

## Getting a text console

Most fixes below need a shell while the desktop is not usable.

* **Ctrl+Alt+F3** switches to a text console (**Ctrl+Alt+F1** or **F2**
  returns to the graphical session, depending on where it runs). Log in
  with your user name and password; in the live session: `nexus` / `live`.
* On the live medium, the boot entry *Advanced options ▸ Live session -
  verbose boot (no splash)* shows boot messages instead of the splash.

## Where the logs are

| Log | How to read it |
|---|---|
| Session start, compositor (KWin), session leader, shell | `~/.local/share/sddm/wayland-session.log` |
| User services, portals, autostarted programs, lock-screen greeter | `journalctl --user -b` |
| Everything of this boot, system-wide | `sudo journalctl -b` (errors only: `sudo journalctl -b -p err`) |
| Kernel messages | `sudo dmesg` (restricted to root by the hardening settings) |
| Login screen | `sudo journalctl -b -u sddm` |
| Failed services | `systemctl --failed`, `systemctl --user --failed` |
| Installer (live session) | `sudo less /var/log/nexus-installer/install-*.log` |
| Installer (installed system) | `sudo less /var/log/installer/install.log`, `/var/log/installer/plan.json` |
| Package operations | `/var/log/apt/history.log`, `/var/log/apt/term.log`, `/var/log/dpkg.log` |
| Audit events | `sudo ausearch -k <key>` ([../security/hardening.md](../security/hardening.md)) |
| Previous boot | add `-b -1` to `journalctl` (the journal is persistent on installed systems) |

When you report a problem, attach the relevant log and the output of
`nexus-sysinfo --dump report`.

## Boot problems

### Black screen after the boot menu

1. Reboot and choose **Live session (safe graphics)**. It adds `nomodeset`,
   so the kernel does not load its graphics drivers and uses the firmware's
   display mode. Without a GPU render node the NEXUS session then renders
   on the CPU automatically: slower, no blur or transparency effects, but
   usable.
2. If that works, the problem is the graphics driver. Note the graphics
   card (`lspci -nnk | grep -A3 -E 'VGA|3D'`) and report it.
3. If the screen is black but the system is running — a cursor, or
   Ctrl+Alt+F3 gives a console — the compositor failed on the GPU. Force
   CPU rendering with `NEXUS_SOFTWARE_RENDERING=1`, see below.
4. Choose *verbose boot (no splash)* to see where the boot stops. If you
   see nothing at all after selecting an entry, try *serial console* on
   hardware that has one, or another USB port or stick.

On an installed system, add `nomodeset` once: in the GRUB menu press **e**
on the NEXUS OS entry, append `nomodeset` to the line that starts with
`linux`, and press **Ctrl+X**. To make it permanent, create
`/etc/default/grub.d/90-local.cfg` with

```
GRUB_CMDLINE_LINUX="$GRUB_CMDLINE_LINUX nomodeset"
```

and run `sudo update-grub`.

### Forcing CPU rendering: `NEXUS_SOFTWARE_RENDERING`

The session script chooses CPU rendering only when no `/dev/dri/renderD*`
node exists. If a GPU is present but the desktop crashes, shows garbage or
stays black, override the choice. The variable must be in the environment
before the session starts. From a text console:

```sh
echo 'NEXUS_SOFTWARE_RENDERING=1' | sudo tee -a /etc/environment
sudo systemctl restart sddm
```

`/etc/environment` is read by PAM at login. This route follows from
Debian's PAM configuration for SDDM and has not been tried on NEXUS OS. If
it has no effect, check that the variable arrived:
`tr '\0' '\n' < /proc/$(pgrep -u "$USER" -x kwin_wayland | head -n1)/environ | grep NEXUS`.

`NEXUS_SOFTWARE_RENDERING=0` forces GPU rendering; remove the line to
return to automatic detection. With CPU rendering, translucent surfaces are
opaque by design.

### Secure Boot errors

| Symptom | Likely cause | What to do |
|---|---|---|
| Firmware refuses the medium ("Security violation", "Invalid signature", or it silently skips the USB stick) | the firmware does not trust the **Microsoft third-party UEFI CA**, which signs shim. Some recent PCs ship with it disabled | enable "Allow Microsoft 3rd Party UEFI CA" (wording varies) in the firmware's Secure Boot settings |
| A shim message about SBAT, or "Verification failed" for GRUB | the firmware's revocation data is newer than the shim or GRUB on the medium | use an image built from current Debian packages (rebuild with `--fresh`) |
| Boot stops after you enrolled or removed keys | the firmware is in setup/custom mode without the standard keys | restore the factory keys in the firmware setup |
| The medium boots only with Secure Boot disabled | one of the above | you can install with Secure Boot disabled — the system is installed "Secure Boot ready" — and enable it later |
| A kernel module does not load on an installed system: "Lockdown" or "Key was rejected by service" in `dmesg` | kernel lockdown: under Secure Boot only signed modules load. Applies to DKMS drivers | sign the module with your own key and enrol it (`mokutil --import`), or do without the module |

Check the state from a running system: `mokutil --sb-state`, and
`cat /sys/kernel/security/lockdown`.

The Secure Boot chain was verified only with OVMF in QEMU. None of the
firmware behaviours in this table has been observed with NEXUS OS.

### The firmware does not offer the USB stick, or boots the old system

* Use the firmware's boot menu (often F12, F10, F9 or Esc) rather than
  changing the boot order.
* Check the stick: `sha256sum -c` on the ISO, write it again with `dd`
  (see the [README](../../README.md)), try another port.
* In the firmware setup, disable "fast boot" if USB devices are not
  initialised.
* After installation the firmware entry is called "NEXUS OS". If the
  machine still starts another system, select "NEXUS OS" in the firmware's
  boot menu and move it to the top of the boot order. Some firmware ignores
  boot entries; the installer writes the fallback path `EFI/BOOT` for whole
  disk installations for that case.

### The installed system asks for a passphrase and rejects it

The passphrase is typed with the keyboard layout that was active when you
set it up and is asked for at boot with the system's configured layout.
If they differ (you changed layouts between the two), type the passphrase
as the keys of the original layout. There is no recovery key.

## Hardware problems

### No Wi-Fi

1. Is there a device? `nmcli device` and `lspci -nnk | grep -A3 -i network`
   (`lsusb` for USB adapters). "Kernel driver in use" missing means no
   driver was loaded.
2. Is firmware missing?

   ```sh
   sudo dmesg | grep -i -E 'firmware|iwlwifi|rtw|ath|brcm|mt76'
   ```

   A line such as "failed to load …" names the missing file. The desktop
   image includes the firmware packages listed in
   [../installation/hardware.md](../installation/hardware.md); the minimal
   image includes none. With another connection (Ethernet, USB tethering
   from a phone) search for the file and install the package that ships it:

   ```sh
   apt-file search <file name>      # needs: sudo apt install apt-file && sudo apt-file update
   sudo apt install <firmware package>
   ```

3. Is the radio blocked? `rfkill list`; `sudo rfkill unblock wifi`; check
   the laptop's hardware switch or Fn key.
4. Some chipsets have no driver in the mainline kernel (certain Broadcom
   devices). Their DKMS drivers are in Debian's `non-free` component, which
   NEXUS OS does not enable.
5. A network that filters by hardware address does not accept the
   per-network random address NEXUS OS uses by default:
   `nmcli connection modify "<name>" wifi.cloned-mac-address permanent`.

### No sound

`wpctl status` shows the devices PipeWire sees. On recent laptops a missing
"sof" firmware message in `dmesg` points at the audio DSP firmware
(`firmware-sof-signed` is in the desktop image). Check that the output is
not muted in the quick settings.

### Something cannot connect *to* this computer

Expected: the firewall drops inbound connections. Open the port:
`sudo nexus-firewall allow <port>/tcp` ([../security/hardening.md](../security/hardening.md)).

## Installer problems

### The installer reports a failure

The result page shows the engine's message, a hint and the log location.
The complete log, with every command and its output, is in the live
session:

```sh
sudo less /var/log/nexus-installer/install-*.log
```

Search for the last `RUN` line and its `EXIT` status. After a failure the
engine unmounts the target and, for an installation into free space,
removes the partitions it created, so you can try again.

| Message | Meaning and remedy |
|---|---|
| "The installation plan differs from the one that was confirmed" (exit status 3) | the disks changed after the plan was shown — a device was plugged in or removed, or a partition was mounted. Nothing was written. Review the new plan and confirm again |
| "… is in use: mounted at …" | unmount the filesystems on the target disk (the live session may have mounted them automatically) and turn off swap on it: `sudo swapoff -a` |
| "… has active encrypted or logical volumes" | `sudo vgchange -an` and `sudo cryptsetup close <name>` |
| "/target is already a mount point" | a previous attempt is still mounted; restart the live session |
| "… does not use a GPT partition table" / "has no unallocated space" | "alongside" needs a GPT disk with free space; shrink a partition with the other system's tool first, or use manual mode |
| "The disk is too small" | see the sizes in [../installation/installing.md](../installation/installing.md) |
| "The system image is no longer available" | the medium was removed or is faulty; verify its checksum and write it again |
| "Unexpected installer error" (`internal`) | a bug: please report it with the log |
| The authentication for the engine fails | outside the live session the installer needs an administrator password; in the live session it should not ask. Check that `/run/nexus/live-session` exists |

The tool profiles step cannot fail the installation: a problem there is a
warning, and the profiles can be installed later.

### The installed system does not boot

See [recovery.md](recovery.md).

## Session problems

### The desktop does not start, or returns to the login screen

What normally happens: SDDM starts `nexus-horizon-session`, which starts
KWin, which starts the session leader, which runs `horizon-shell`. If the
shell crashes five times within a minute, the leader ends the session and
you are back at the login screen. NEXUS OS installs no second desktop to
fall back to; use a text console.

1. Read the session log:

   ```sh
   less ~/.local/share/sddm/wayland-session.log
   journalctl --user -b -p warning
   ```

   Lines from `horizon-session-leader` say how often the shell exited and
   with which status. Lines from `kwin_wayland` about DRM, EGL or OpenGL
   point at graphics: try `NEXUS_SOFTWARE_RENDERING=1` (above).
2. Is it your configuration? Move the shell's and the compositor's settings
   aside and log in again:

   ```sh
   mv ~/.config/nexus ~/.config/nexus.bak
   mv ~/.config/kwinrc ~/.config/kwinrc.bak
   ```

   If a new user account works and yours does not, it is configuration.
3. Is the installation complete?

   ```sh
   dpkg -s nexus-horizon nexus-ui nexus-themes kwin-wayland | grep -E '^(Package|Status)'
   sudo dpkg --configure -a
   ```

4. Is the login screen itself missing? `systemctl status sddm` and
   `sudo journalctl -b -u sddm`. The greeter runs on Wayland, composited by
   KWin, and needs a working display device just like the session.
5. Start the shell's diagnostics without a display; they must print JSON:

   ```sh
   horizon-shell --dump modules
   horizon-shell --dump settings
   ```

### Only the wallpaper appears, or panel and dock are missing

The compositor runs but the shell does not. From a text console check
`pgrep -a horizon-shell` and the session log. After five crashes the
session should end by itself; if it hangs, end it with
`loginctl terminate-user "$USER"` (this closes all your applications).

### The Meta key does not open the launcher

The shell repairs a lost Meta binding at start. Log out and in again. If
it stays broken, check `horizon-shell --dump shortcuts`: `available` must
be true and the `launcher` entry must list `Meta`. Alt+F1 opens the
launcher as well.

### Lock screen problems

Locking is done by the compositor's screen locker
([../desktop/architecture.md](../desktop/architecture.md)). The lock screen
has not been verified in the booted image.

**The screen is locked and you cannot unlock it** (the lock screen is
black, shows an error, or does not accept the correct password):

1. Switch to a text console (Ctrl+Alt+F3) and log in.
2. Find your graphical session and unlock it:

   ```sh
   loginctl list-sessions
   loginctl unlock-session <ID>
   ```

   The session to unlock is the one of your user whose `TTY` or `SEAT`
   column shows the graphical seat (class `user`, not the console session
   you just opened). `loginctl unlock-sessions` (as root, with `sudo`)
   unlocks all of them.
3. Switch back to the graphical session.

If the password is rejected only on the lock screen:

* check Caps Lock and the keyboard layout (the lock screen uses the
  session's layout; the login screen uses the system layout);
* check that the PAM files are in place:
  `ls /etc/pam.d/kde /etc/pam.d/kde-fingerprint /etc/pam.d/kde-smartcard`
  (package `nexus-themes`);
* look for the greeter's messages:
  `journalctl --user -b | grep -i -E 'kscreenlocker|pam'`.

If the greeter does not start at all, check that `layer-shell-qt` is
installed (`dpkg -s layer-shell-qt`); the greeter cannot run on Wayland
without it.

**The screen locks too early or not at all:** the defaults are in
`/etc/xdg/kscreenlockerrc` (10 minutes, lock on resume). Your own values go
into `~/.config/kscreenlockerrc`:

```ini
[Daemon]
Autolock=true
Timeout=20
LockOnResume=true
LockGrace=0
```

### Applications look wrong or ignore the dark/light setting

Run `nexus-apply-appearance` and restart the application. GTK and
libadwaita theming has not been tested. Applications started with `sudo`
or as another user do not use your settings.

### An X11 application's tray icon is missing

Legacy XEmbed tray icons are not supported. Only applications that provide
a StatusNotifierItem appear in the tray.

### A keyboard shortcut reports "… is not installed"

`nexus-terminal` and `nexus-screenshot` are not part of the image yet ([../limitations.md](../limitations.md)). Install a terminal
emulator from Debian in the meantime; it appears in the launcher.

## Package problems

### "dpkg was interrupted" or half-configured packages

```sh
sudo dpkg --configure -a
sudo apt -f install
```

or Updater → Recovery → Repair. Then `sudo apt update && sudo apt full-upgrade`.

### `apt update` reports errors

| Message | Meaning |
|---|---|
| Could not resolve / connect to `deb.debian.org` | no network or DNS: `nmcli general status`, `ping -c1 deb.debian.org` |
| Release file "is not valid yet" | the clock is wrong: `timedatectl`; enable time synchronisation with `sudo timedatectl set-ntp true` |
| A problem with `file:/usr/share/nexus/repository` | the on-disk NEXUS repository is damaged or missing. It only provides the NEXUS packages of the release; disable it by adding `Enabled: no` to `/etc/apt/sources.list.d/nexus-local.sources` |
| Signature errors (`NO_PUBKEY`, `BADSIG`) for Debian | `sudo apt install --reinstall debian-archive-keyring`; check the clock; do not disable signature checks |

`/etc/apt/sources.list.d/nexus.sources` is disabled on purpose and does not
cause errors ([../packaging/repositories.md](../packaging/repositories.md)).

### A package cannot be installed: "held broken packages" / unmet dependencies

`apt -s install <package>` shows what conflicts. NEXUS OS enables only
Debian 13 `main`, `contrib` and `non-free-firmware`; packages from other
suites, `non-free`, or other distributions are not available and mixing
them in is the usual cause of such conflicts. `nexus-boot` conflicts with
Debian's `grub-pc` and `grub-efi-*` front-end packages on purpose; do not
install those.

### A package operation removed the desktop

If `apt` proposes to remove `nexus-horizon`, `nexus-ui` or `kwin-wayland`,
answer no. If it already happened: from a text console,
`sudo apt install nexus-horizon nexus-themes nexus-control-center` (the
on-disk repository provides the NEXUS packages).

### The boot loader was not refreshed after an update

`sudo nexus-grub-install --verbose` — see
[../security/updates.md](../security/updates.md).

## Still stuck

Collect and attach to your report:

```sh
nexus-sysinfo --dump report > report.json
sudo journalctl -b > journal.txt
cp ~/.local/share/sddm/wayland-session.log .
sudo nexus-security-status --pretty > security.json      # desktop image / installed system
```

Review the files before you share them: they contain host names, user
names, hardware serial numbers and network addresses.
