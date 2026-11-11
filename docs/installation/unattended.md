# Unattended installation

The installer engine (`/usr/sbin/nexus-installer-engine`, package
`nexus-installer`, source `system/installer/engine/`) is a non-interactive
program. The graphical installer is one front-end to it; you can drive it
yourself from a shell or a script. This is also how the engine is tested in
QEMU (`tests/installation/test_install.py`).

Verified this way in QEMU, each followed by booting the installed disk:
erase disk (UEFI, UEFI with Secure Boot enforcing, legacy BIOS), erase disk
with LUKS2 (UEFI), alongside another system in free space (UEFI). Pending a
re-run: manual assignment (UEFI and BIOS), separate `/home`, and the refusal
cases. See [../testing.md](../testing.md).

## Command line

The engine must run as root, on the live medium.

```
nexus-installer-engine --version
nexus-installer-engine probe   [--other-systems]
nexus-installer-engine plan    --answers FILE [--text]
nexus-installer-engine install --answers FILE --confirm PLAN-HASH
```

| Verb | What it does | Touches disks |
|---|---|---|
| `probe` | Prints a JSON description of the machine: architecture, firmware (type, Secure Boot state, whether UEFI variables are writable), memory, the live medium, every disk with its partitions and free regions, existing volume groups, power state, network state, the list of `checks` and `can_install`. `--other-systems` additionally scans for installed operating systems (slow; mounts filesystems read-only) | no |
| `plan` | Validates the answer file and prints the plan as JSON: exact partition layout, what is formatted, mount points, boot-loader installation, the ordered `steps` (each with a `destructive` flag), `warnings` and a `hash`. `--text` prints a readable version and the exact `install` command | no |
| `install` | Rebuilds the plan from the same answers and the current state of the machine; runs it only if its hash equals `--confirm` | yes |

`--answers -` reads the answer file from standard input.

## Workflow: plan, review, confirm

```sh
# 1. What does the machine look like? Find the target disk.
sudo nexus-installer-engine probe | less
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS

# 2. Write answers.json (see below), then look at the plan.
sudo nexus-installer-engine plan --answers answers.json --text

# 3. Run exactly that plan. The command with the hash is printed by step 2.
sudo nexus-installer-engine install --answers answers.json --confirm <hash>
```

Why the hash: the plan is plain data and contains no secrets, so it can be
shown, logged and hashed (SHA-256 over its canonical JSON form). `install`
derives the plan again and refuses to run if the hash differs. That
guarantees that what is executed is what was shown, **and** that the machine
has not changed in between — for example a USB stick was plugged in and
device names shifted. In that case nothing is written, exit status is 3,
and you must review the new plan.

In a script:

```sh
hash=$(sudo nexus-installer-engine plan --answers answers.json \
       | python3 -c 'import json,sys; print(json.load(sys.stdin)["hash"])')
sudo nexus-installer-engine install --answers answers.json --confirm "$hash"
```

A script that computes the hash and passes it straight on has, of course,
not reviewed anything. Check the plan's `target_disk` and `steps` in the
script, or pin the disk by a stable name such as `/dev/disk/by-id/…`.

> **`install` erases data.** In `erase-disk` mode everything on
> `storage.disk` is destroyed. The engine refuses the installation medium, a
> mounted disk and a disk with active encrypted or logical volumes, but it
> cannot know that you named the wrong disk.

## Output of `install`

Standard output carries one JSON object per line:

| `event` | Fields | Meaning |
|---|---|---|
| `step` | `id`, `index`, `total`, `title` | a step starts. Ids in order: `partition`, `encrypt`, `format`, `mount`, `copy`, `configure`, `user`, `cleanup`, `initramfs`, `bootloader`, `profiles`, `finish` (only those that apply) |
| `progress` | `percent`, optional `message` | overall progress, 0–100 |
| `log` | `level` (`info`, `warning`), `message` | a message for the user |
| `done` | `message` | the installation is complete |
| `error` | `code`, `message`, `hint`, `log`; for `plan-changed` also `expected`, `actual` | failure. `code` is `plan-changed`, `failed`, `internal` or (from `plan` and `probe`) `invalid` |

A successful run ends with a `done` event **and** exit status 0. After a
failure the engine unmounts what it mounted, closes what it opened and, in
`free-space` mode, deletes the partitions it created.

The full log — every command with its output — is written to
`/var/log/nexus-installer/install-<date>-<time>.log` (mode 0600) and copied
to `/var/log/installer/` on the installed system. Secrets are passed to
commands on standard input and are not logged.

## Exit status

| Status | Meaning |
|---|---|
| 0 | success |
| 1 | the installation failed (`error` event with `code` `failed` or `internal`) |
| 2 | usage error, not root, or an invalid answer file or impossible request found by `probe`/`plan` (`error` event with `code` `invalid`, and a message on standard error) |
| 3 | the plan differs from the confirmed one (`code` `plan-changed`); nothing was written |

## Answer file

JSON, one object. Validation is strict: **an unknown key or an out-of-range
value is an error**, never ignored. Source of truth:
`system/installer/engine/nexus_installer/answers.py`.

### Top level

| Key | Type | Default | Rules |
|---|---|---|---|
| `version` | integer | — (required) | must be `1` |
| `locale` | string | `en_US.UTF-8` | a UTF-8 locale listed in `/usr/share/i18n/SUPPORTED`; `.UTF-8` is appended if missing |
| `keyboard` | object | — | see below |
| `timezone` | string | `Etc/UTC` | a zone under `/usr/share/zoneinfo` |
| `hostname` | string | `nexus` | lower-case letters, digits, hyphens; 1–63 characters; no leading or trailing hyphen |
| `user` | object | — (required) | see below |
| `storage` | object | — (required) | see below |
| `bootloader` | object | — | see below |
| `kernel` | object | — | see below |
| `packages` | object | — | see below |

### `keyboard`

| Key | Default | Rules |
|---|---|---|
| `layout` | `us` | XKB layout name: letters, digits, `_`, `-`; at most 32 characters |
| `variant` | empty | XKB variant name, same characters |
| `model` | `pc105` | XKB model name, same characters |

### `user`

| Key | Default | Rules |
|---|---|---|
| `username` | — (required) | starts with a lower-case letter or `_`; then lower-case letters, digits, `_`, `-`; at most 32 characters; not a system account name (`root`, `daemon`, `sddm`, `admin`, `sudo`, `users`, …) |
| `fullname` | empty | at most 120 characters; no `:`, `,`, `=` or line breaks |
| `password` | — | plain-text password, no line breaks |
| `password_hash` | — | a crypt(5) hash: yescrypt (`$y$`), SHA-512 (`$6$`), SHA-256 (`$5$`), bcrypt (`$2a$`/`$2b$`/`$2y$`), scrypt (`$7$`) or gost-yescrypt (`$gy$`) |
| `autologin` | `false` | log this user in automatically (written for SDDM) |

Give **exactly one** of `password` and `password_hash`. Prefer the hash: the
answer file then contains no password. In the live session:

```sh
mkpasswd --method=yescrypt
```

The user becomes a member of `sudo`; the root account is locked.

### `storage`

| Key | Default | Modes | Rules |
|---|---|---|---|
| `mode` | — (required) | — | `erase-disk`, `free-space` or `manual` |
| `disk` | — | erase-disk, free-space (required) | a block device path such as `/dev/nvme0n1` or `/dev/disk/by-id/…` |
| `encrypt` | `false` | erase-disk, free-space | LUKS2 + LVM. Not supported in manual mode |
| `passphrase` | — | with `encrypt` | at least 8 characters, no line breaks; must be absent when `encrypt` is false |
| `separate_home` | `false` | erase-disk, free-space | create `/home` as its own filesystem |
| `root_size_gib` | automatic | erase-disk, free-space | integer 16–4096, or `"auto"`; used with `separate_home` |
| `swap` | `file` | all | `file`, `partition` or `none`. In manual mode `partition` is not allowed: mark a partition with `"use": "swap"` instead |
| `swap_size_gib` | automatic | all | integer 1–256, or `"auto"` |
| `mounts` | — | manual (required) | non-empty list, see below |
| `bootloader_device` | — | manual | disk for the legacy BIOS boot sector; required for a manual installation in BIOS mode |

Entries of `storage.mounts`:

| Key | Default | Rules |
|---|---|---|
| `device` | — (required) | an existing partition; each device at most once |
| `use` | `filesystem` | `filesystem` or `swap` |
| `mountpoint` | — | one of `/`, `/boot`, `/boot/efi`, `/home`, `/var`, `/srv`, `/opt`, `/tmp`; each at most once; must be absent for swap |
| `format` | `false` | format the partition. `/` must have `true` |
| `filesystem` | `ext4`, or `vfat` for `/boot/efi` | only together with `"format": true`; `/boot/efi` can only be `vfat`, everything else only `ext4`. The filesystem of a kept partition is detected, not declared |

A manual plan needs a `/` entry and, on UEFI, a `/boot/efi` entry. A swap
partition is always re-initialised.

### `bootloader`

| Key | Default | Rules |
|---|---|---|
| `install` | `true` | `false` installs no boot loader; the system will not start unless you configure an existing loader yourself |
| `removable` | `auto` | `auto`, `yes` or `no`: also install GRUB to the removable-media path `EFI/BOOT`. `auto`: yes for `erase-disk`; otherwise only if the firmware cannot store a boot entry |
| `os_prober` | `true` | add other operating systems to the boot menu (never for `erase-disk`) |

### `kernel`

| Key | Default | Rules |
|---|---|---|
| `extra_cmdline` | empty | extra kernel parameters, at most 512 characters from `A–Z a–z 0–9 _ . , = : / @ + -` and spaces. If it contains `console=ttyS`, GRUB is also configured for the serial console |

### `packages`

| Key | Default | Rules |
|---|---|---|
| `profiles` | `[]` | Arsenal profile names (`core`, `web`, `network`, `redteam`, `blueteam`, `forensics`, `reversing`, `developer`, `complete`); the engine checks only the form of the name |
| `install_now` | `false` | install the `nexus-arsenal-<name>` packages during installation (needs a network connection; a failure is reported as a warning and does not fail the installation). See the note in [installing.md](installing.md): as implemented, the tools themselves are not installed by this step |

## Examples

Each example below was validated with the engine's own `answers.parse()`.

### Complete example: encrypted whole-disk installation

Every top-level section and every key that applies to this mode is present.

```json
{
  "version": 1,
  "locale": "en_GB.UTF-8",
  "keyboard": { "layout": "gb", "variant": "", "model": "pc105" },
  "timezone": "Europe/London",
  "hostname": "nexus-lab",
  "user": {
    "fullname": "Ada Lovelace",
    "username": "ada",
    "password_hash": "$y$j9T$REPLACE.WITH.YOUR.OWN$HASH.FROM.mkpasswd",
    "autologin": false
  },
  "storage": {
    "mode": "erase-disk",
    "disk": "/dev/nvme0n1",
    "encrypt": true,
    "passphrase": "correct horse battery staple",
    "separate_home": true,
    "root_size_gib": 60,
    "swap": "partition",
    "swap_size_gib": "auto"
  },
  "bootloader": { "install": true, "removable": "auto", "os_prober": true },
  "kernel": { "extra_cmdline": "" },
  "packages": { "profiles": ["core", "web"], "install_now": false }
}
```

Replace the `password_hash` value with a real hash and the passphrase with
your own. The file contains the disk passphrase in clear text: keep it on
the live system's memory-backed filesystem (for example `/tmp`), feed it on
standard input (`--answers -`), and delete it afterwards.

### Smallest valid file: alongside another system

```json
{
  "version": 1,
  "user": { "username": "ada", "password": "change-me-now" },
  "storage": { "mode": "free-space", "disk": "/dev/sda" }
}
```

Defaults apply: `en_US.UTF-8`, `us` keyboard, UTC, host name `nexus`, a swap
file, no encryption.

### Manual assignment keeping `/home`

```json
{
  "version": 1,
  "locale": "fr_FR.UTF-8",
  "keyboard": { "layout": "fr", "variant": "oss" },
  "timezone": "Europe/Paris",
  "hostname": "atelier",
  "user": { "fullname": "Sophie Germain", "username": "sophie", "password": "change-me-now" },
  "storage": {
    "mode": "manual",
    "swap": "none",
    "mounts": [
      { "device": "/dev/sda2", "mountpoint": "/", "format": true },
      { "device": "/dev/sda1", "mountpoint": "/boot/efi" },
      { "device": "/dev/sda3", "mountpoint": "/home" },
      { "device": "/dev/sda4", "use": "swap" }
    ]
  }
}
```

`/dev/sda1` (the existing ESP) and `/dev/sda3` (`/home`) are kept;
`/dev/sda2` is formatted; `/dev/sda4` becomes swap.

## What the plan can refuse

`plan` (and therefore `install`) stops with an error, before anything is
written, when for example:

* a hardware check failed (memory, firmware, no suitable disk, medium
  without system image, battery below 30 %);
* the target is the installation medium, read-only, mounted, or has active
  encrypted or logical volumes;
* the disk is too small for the layout;
* `free-space` is requested on a disk without GPT or without unallocated
  space, or the existing ESP is mounted;
* in manual mode a partition is mounted, has no usable filesystem and is not
  formatted, the root partition is too small, a UEFI system has no
  `/boot/efi` entry, or a BIOS system has no `bootloader_device`.

The planner's rules are pinned by unit tests that run without root and
without disks:

```sh
python3 tests/installation/test_plan.py
```
