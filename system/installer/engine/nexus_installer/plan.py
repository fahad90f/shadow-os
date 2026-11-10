# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn validated answers plus a probe report into an installation plan.

The plan is plain data: the exact partition layout (byte offsets and sizes),
which devices are formatted, where they are mounted, how the boot loader is
installed, and a human-readable list of steps. It contains no secrets, so it
can be shown, logged and hashed. Planning never touches the disk.

The same function is used to show the plan and, later, to re-derive it at
execution time; the executor refuses to run unless the hash matches, which
guarantees that what is executed is what was shown *and* that the machine has
not changed in between (a USB stick plugged in, device names shifted).
"""
from __future__ import annotations

import hashlib
import json
import math
import os

from .answers import Answers, KEEP_FILESYSTEMS
from .util import InstallError, human_size

MIB = 1024**2
GIB = 1024**3

BIOS_GRUB_SIZE = 1 * MIB
ESP_SIZE = 512 * MIB
BOOT_SIZE = 1024 * MIB
GPT_RESERVE = 1 * MIB                # space kept free before the backup GPT
LUKS_LVM_OVERHEAD = 48 * MIB         # LUKS2 header (16 MiB) + LVM metadata and extent rounding
MIN_ROOT = 16 * GIB
MIN_HOME = 8 * GIB

TYPECODES = {
    "bios_grub": "ef02", "esp": "ef00", "boot": "8300", "swap": "8200", "home": "8302",
    "luks": "8309", "root-amd64": "8304", "root-arm64": "8305",
}
LABELS = {"esp": "NEXUS-ESP", "boot": "nexus-boot", "root": "nexus-root", "home": "nexus-home", "swap": "nexus-swap"}
VG_NAME = "nexus"
CRYPT_NAME = "nexus_crypt"


def partition_path(disk: str, number: int) -> str:
    """Kernel naming: sda → sda1, nvme0n1 → nvme0n1p1, mmcblk0 → mmcblk0p1."""
    return f"{disk}p{number}" if disk[-1].isdigit() else f"{disk}{number}"


def _free_name(base: str, taken: set[str]) -> str:
    if base not in taken:
        return base
    for i in range(1, 100):
        if f"{base}{i}" not in taken:
            return f"{base}{i}"
    raise InstallError(f"No free name for a new volume ({base}).")


def _align_up(value: int, unit: int = MIB) -> int:
    return (value + unit - 1) // unit * unit


def _align_down(value: int, unit: int = MIB) -> int:
    return value // unit * unit


def _find_disk(report: dict, path: str) -> dict:
    real = os.path.realpath(path)
    for disk in report["disks"]:
        if os.path.realpath(disk["path"]) == real or disk["path"] == path:
            return disk
    raise InstallError(f"Disk {path} was not found.", "It may have been unplugged; restart the installer.")


def _find_partition(report: dict, path: str) -> tuple[dict, dict]:
    real = os.path.realpath(path)
    for disk in report["disks"]:
        for part in disk["partitions"]:
            if part["path"] == path or os.path.realpath(part["path"]) == real:
                return disk, part
    raise InstallError(f"Partition {path} was not found.")


def _describe_disk(disk: dict) -> str:
    name = " ".join(x for x in (disk.get("vendor"), disk.get("model")) if x) or "unknown model"
    return f"{disk['path']} ({name}, {human_size(disk['size'])})"


def _describe_partition(part: dict) -> str:
    bits = [part.get("fstype") or "no filesystem"]
    if part.get("label"):
        bits.append(f"label “{part['label']}”")
    elif part.get("parttype_name"):
        bits.append(part["parttype_name"])
    return f"{part['path']} ({', '.join(bits)}, {human_size(part['size'])})"


def _min_root(report: dict) -> int:
    installed = report["live_medium"].get("installed_size")
    if installed:
        return max(MIN_ROOT, _align_up(int(installed * 1.25) + 4 * GIB, GIB))
    return MIN_ROOT


def _swap_size(answers: Answers, report: dict) -> int:
    st = answers.storage
    if st.swap == "none":
        return 0
    if st.swap_size_gib:
        return st.swap_size_gib * GIB
    ram_gib = max(1, math.ceil(report["memory_bytes"] / GIB))
    if st.swap == "file":
        return min(max(math.ceil(ram_gib / 2), 2), 8) * GIB
    return min(max(ram_gib, 2), 32) * GIB


def _check_disk_free_for_use(disk: dict, what: str):
    if disk["is_live_medium"]:
        raise InstallError(f"{disk['path']} is the installation medium and cannot be {what}.")
    if disk["read_only"]:
        raise InstallError(f"{disk['path']} is read-only.")
    if disk["mountpoints"]:
        raise InstallError(
            f"{disk['path']} is in use: mounted at {', '.join(disk['mountpoints'])}.",
            "Unmount these filesystems (or turn off swap on this disk) and try again.")
    if disk.get("active_mappings"):
        raise InstallError(
            f"{disk['path']} has active encrypted or logical volumes: {', '.join(disk['active_mappings'])}.",
            "Lock/deactivate them first (cryptsetup close, vgchange -an) and try again.")


def _guided_layout(answers: Answers, report: dict, *, region_start: int, region_end: int,
                   need_esp: bool, need_bios_grub: bool, arch: str) -> tuple[list[dict], dict | None, int]:
    """Lay out new partitions inside [region_start, region_end). Returns
    (partitions, lvm, swapfile_size)."""
    st = answers.storage
    pos = _align_up(region_start)
    end = _align_down(region_end)
    parts: list[dict] = []

    def take(role: str, size: int, fmt: str | None, mountpoint: str | None):
        nonlocal pos
        if size is None:                    # rest of the region
            size = end - pos
        size = _align_down(size)
        if size <= 0 or pos + size > end:
            raise InstallError(
                "There is not enough space for the requested layout.",
                f"Needed at least {human_size(pos + max(size, 0) - _align_up(region_start))}, "
                f"available {human_size(end - _align_up(region_start))}.")
        code = TYPECODES.get(role) or TYPECODES[f"root-{arch}"]
        parts.append({"role": role, "create": True, "device": None, "start": pos, "size": size,
                      "typecode": code, "partlabel": f"nexus-{role.replace('_', '-')}",
                      "format": fmt, "label": LABELS.get(role), "mountpoint": mountpoint})
        pos += size

    if need_bios_grub:
        take("bios_grub", BIOS_GRUB_SIZE, None, None)
    if need_esp:
        take("esp", ESP_SIZE, "vfat", "/boot/efi")
    if st.encrypt:
        take("boot", BOOT_SIZE, "ext4", "/boot")

    swap_size = _swap_size(answers, report)
    swapfile = swap_size if st.swap == "file" else 0
    swap_part = swap_size if st.swap == "partition" else 0
    base_root = _min_root(report)
    if swapfile and not st.swap_size_gib:
        # An automatically sized swap file yields to the system on small disks
        # instead of making the installation impossible.
        room = (end - pos) - (LUKS_LVM_OVERHEAD if st.encrypt else 0) - base_root
        if not st.separate_home:
            swapfile = max(0, min(swapfile, _align_down(room, GIB)))
    min_root = base_root + swapfile
    lvm = None

    if st.encrypt:
        take("luks", None, "luks", None)
        inside = _align_down(parts[-1]["size"] - LUKS_LVM_OVERHEAD, 4 * MIB)
        avail = inside - swap_part
        if st.separate_home:
            root = st.root_size_gib * GIB if st.root_size_gib else min(max(_align_down(int(avail * 0.35), GIB), 40 * GIB), 120 * GIB)
            home = avail - root
            if root < min_root or home < MIN_HOME:
                raise InstallError(
                    "The disk is too small for a separate /home.",
                    f"Root needs at least {human_size(min_root)} and /home at least {human_size(MIN_HOME)}; "
                    f"{human_size(avail)} is available.")
        else:
            root, home = avail, 0
            if root < min_root:
                raise InstallError("The disk is too small.",
                                   f"The system needs at least {human_size(min_root)}; {human_size(root)} is available.")
        lvs = []
        if swap_part:
            lvs.append({"name": "swap", "size": swap_part, "format": "swap", "mountpoint": None, "label": LABELS["swap"]})
        if st.separate_home:
            lvs.append({"name": "root", "size": _align_down(root, 4 * MIB), "format": "ext4", "mountpoint": "/", "label": LABELS["root"]})
            lvs.append({"name": "home", "size": None, "format": "ext4", "mountpoint": "/home", "label": LABELS["home"]})
        else:
            lvs.append({"name": "root", "size": None, "format": "ext4", "mountpoint": "/", "label": LABELS["root"]})
        # Names must not collide with volumes that already exist on this machine
        # (for example another NEXUS OS installation on a second disk).
        vg = _free_name(VG_NAME, set(report.get("volume_groups") or []))
        crypt = _free_name(CRYPT_NAME, set(report.get("mapper_names") or []))
        lvm = {"vg": vg, "crypt_name": crypt, "lvs": lvs}
    else:
        if swap_part:
            take("swap", swap_part, "swap", None)
        avail = end - pos
        if st.separate_home:
            root = st.root_size_gib * GIB if st.root_size_gib else min(max(_align_down(int(avail * 0.35), GIB), 40 * GIB), 120 * GIB)
            if root < min_root or avail - root < MIN_HOME:
                raise InstallError(
                    "The disk is too small for a separate /home.",
                    f"Root needs at least {human_size(min_root)} and /home at least {human_size(MIN_HOME)}; "
                    f"{human_size(avail)} is available.")
            take("root", root, "ext4", "/")
            take("home", None, "ext4", "/home")
        else:
            if avail < min_root:
                raise InstallError("There is not enough space.",
                                   f"The system needs at least {human_size(min_root)}; {human_size(avail)} is available.")
            take("root", None, "ext4", "/")
    return parts, lvm, swapfile


def build(answers: Answers, report: dict) -> dict:
    failed = [c for c in report["checks"] if c["status"] == "fail"]
    if failed:
        raise InstallError("This computer does not meet the requirements: "
                           + "; ".join(c["title"] for c in failed) + ".",
                           " ".join(c["detail"] for c in failed if c["detail"]))

    arch = report["architecture"]
    fw = report["firmware"]["type"]
    st = answers.storage
    warnings: list[str] = []
    steps: list[dict] = []
    partitions: list[dict] = []
    lvm = None
    swapfile = 0
    target_disk = None
    bios_boot_disk = None

    def step(sid: str, title: str, details: list[str] | None = None, destructive: bool = False):
        steps.append({"id": sid, "title": title, "details": details or [], "destructive": destructive})

    # ------------------------------------------------------------------ storage
    if st.mode == "erase-disk":
        disk = _find_disk(report, st.disk)
        _check_disk_free_for_use(disk, "erased")
        if not disk["usable"]:
            raise InstallError(f"{disk['path']} cannot be used: {disk['unusable_reason']}")
        target_disk = disk["path"]
        partitions, lvm, swapfile = _guided_layout(
            answers, report, region_start=MIB, region_end=disk["size"] - GPT_RESERVE,
            need_esp=True, need_bios_grub=(arch == "amd64"), arch=arch)
        for i, part in enumerate(partitions, start=1):
            part["number"] = i
            part["device"] = partition_path(target_disk, i)
        lost = [_describe_partition(p) for p in disk["partitions"]] or \
               ([f"filesystem {disk['whole_disk_fstype']} on the whole disk"] if disk.get("whole_disk_fstype") else
                ["(the disk has no partitions)"])
        step("partition", f"Erase {_describe_disk(disk)}",
             ["ALL data on this disk will be permanently destroyed, including:"] + [f"  • {x}" for x in lost]
             + ["A new GPT partition table is created with:"]
             + [f"  • {p['device']}: {human_size(p['size'])} — {_role_text(p)}" for p in partitions],
             destructive=True)
        if fw == "bios":
            bios_boot_disk = target_disk

    elif st.mode == "free-space":
        disk = _find_disk(report, st.disk)
        if disk["is_live_medium"] or disk["read_only"]:
            raise InstallError(f"{disk['path']} cannot be used: {disk['unusable_reason'] or 'read-only'}")
        if disk["partition_table"] != "gpt":
            raise InstallError(
                f"{disk['path']} does not use a GPT partition table (found: {disk['partition_table'] or 'none'}).",
                "Installing into free space is supported on GPT disks. Use manual partitioning, or erase the disk.")
        if not disk["free_regions"]:
            raise InstallError(f"{disk['path']} has no unallocated space.",
                               "Shrink an existing partition first (for Windows: use its Disk Management tool), "
                               "then run the installer again.")
        region = max(disk["free_regions"], key=lambda r: r["size"])
        target_disk = disk["path"]
        existing_esp = next((p for p in disk["partitions"] if p["is_esp"] and p["fstype"] == "vfat"), None)
        existing_bios = next((p for p in disk["partitions"] if p["is_bios_grub"]), None)
        need_esp = fw == "uefi" and existing_esp is None
        need_bios = fw == "bios" and existing_bios is None
        partitions, lvm, swapfile = _guided_layout(
            answers, report, region_start=region["start"], region_end=region["end"] + 1,
            need_esp=need_esp, need_bios_grub=need_bios, arch=arch)
        if fw == "uefi" and existing_esp is not None:
            if existing_esp["mountpoints"]:
                raise InstallError(f"The EFI System Partition {existing_esp['path']} is mounted.",
                                   "Unmount it and try again.")
            partitions.insert(0, {"role": "esp", "create": False, "device": existing_esp["path"],
                                  "start": existing_esp["start"], "size": existing_esp["size"],
                                  "typecode": None, "partlabel": None, "format": None, "label": None,
                                  "mountpoint": "/boot/efi", "existing_fstype": "vfat"})
        step("partition", f"Create partitions in {human_size(region['size'])} of free space on {_describe_disk(disk)}",
             ["Existing partitions are NOT modified:"]
             + [f"  • {_describe_partition(p)} — kept" for p in disk["partitions"]]
             + ["New partitions:"]
             + [f"  • {human_size(p['size'])} — {_role_text(p)}" for p in partitions if p["create"]]
             + ([f"The existing EFI System Partition {existing_esp['path']} is reused without formatting."]
                if (fw == "uefi" and existing_esp) else []),
             destructive=False)
        if fw == "bios":
            bios_boot_disk = target_disk

    else:  # manual
        seen_disks = set()
        for m in st.mounts:
            disk, part = _find_partition(report, m.device)
            if disk["is_live_medium"]:
                raise InstallError(f"{m.device} is on the installation medium.")
            if part["mountpoints"]:
                raise InstallError(f"{m.device} is mounted at {', '.join(part['mountpoints'])}.",
                                   "Unmount it and try again.")
            seen_disks.add(disk["path"])
            entry = {"role": "swap" if m.use_as_swap else {"/": "root", "/boot": "boot", "/boot/efi": "esp",
                                                            "/home": "home"}.get(m.mountpoint, "data"),
                     "create": False, "device": part["path"], "start": part["start"], "size": part["size"],
                     "typecode": None, "partlabel": None, "format": m.filesystem if m.format else None,
                     "label": None, "mountpoint": m.mountpoint, "existing_fstype": part["fstype"]}
            if m.mountpoint == "/boot/efi":
                if fw != "uefi":
                    raise InstallError("An EFI System Partition was selected, but this computer booted in legacy "
                                       "BIOS mode.", "Remove the /boot/efi entry or boot the installer in UEFI mode.")
                if not m.format and part["fstype"] != "vfat":
                    raise InstallError(f"{m.device} is not a FAT filesystem and cannot be used as the EFI System "
                                       "Partition without formatting.")
                if not part["is_esp"]:
                    warnings.append(f"{m.device} is not flagged as an EFI System Partition; some firmware will "
                                    "not boot from it.")
                if part["size"] < 100 * MIB:
                    raise InstallError(f"{m.device} is too small for an EFI System Partition (minimum 100 MiB).")
            elif not m.format and not m.use_as_swap:
                if part["fstype"] not in KEEP_FILESYSTEMS:
                    raise InstallError(
                        f"{m.device} has no usable filesystem ({part['fstype'] or 'none'}) for {m.mountpoint}.",
                        "Choose to format it, or pick a different partition.")
            if m.mountpoint == "/" and part["size"] < _min_root(report):
                raise InstallError(f"{m.device} is too small for the system "
                                   f"(needs {human_size(_min_root(report))}).")
            partitions.append(entry)
        if fw == "uefi" and not any(p["mountpoint"] == "/boot/efi" for p in partitions):
            raise InstallError("No EFI System Partition was selected.",
                               "On a UEFI computer assign an existing EFI System Partition (usually the small "
                               "FAT32 partition at the start of the disk) to /boot/efi. It does not need formatting.")
        root_disk, _ = _find_partition(report, next(m.device for m in st.mounts if m.mountpoint == "/"))
        target_disk = root_disk["path"]
        if fw == "bios" and answers.install_bootloader:
            if not st.bootloader_device:
                raise InstallError("storage.bootloader_device is required for a manual BIOS installation.")
            bdisk = _find_disk(report, st.bootloader_device)
            if bdisk["is_live_medium"]:
                raise InstallError("The boot loader cannot be installed onto the installation medium.")
            if bdisk["partition_table"] == "gpt" and not any(p["is_bios_grub"] for p in bdisk["partitions"]):
                raise InstallError(
                    f"{bdisk['path']} uses GPT but has no BIOS boot partition.",
                    "Create a 1 MiB partition of type 'BIOS boot' on it, or boot the installer in UEFI mode.")
            bios_boot_disk = bdisk["path"]
        swapfile = _swap_size(answers, report) if st.swap == "file" else 0

    # ---------------------------------------------------------------- steps
    luks = next((p for p in partitions if p["role"] == "luks"), None)
    if luks:
        step("encrypt", "Encrypt the system partition",
             [f"{luks['device'] or 'the new system partition'} is encrypted with LUKS2 "
              "(AES-XTS 512-bit, Argon2id). The passphrase is required at every boot and cannot be recovered."],
             destructive=True)
    formatted = [p for p in partitions if p["format"] and p["format"] != "luks"]
    if formatted or lvm:
        details = [f"{p['device'] or 'new partition'}: {_fs_text(p['format'])} — {_role_text(p)}"
                   + (f" (existing {p.get('existing_fstype') or 'data'} is destroyed)" if not p["create"] else "")
                   for p in formatted]
        if lvm:
            details += [f"logical volume {lvm['vg']}/{lv['name']}: {_fs_text(lv['format'])}"
                        + (f" on {lv['mountpoint']}" if lv["mountpoint"] else "") for lv in lvm["lvs"]]
        step("format", "Create filesystems", details, destructive=any(not p["create"] for p in formatted))
    kept = [p for p in partitions if not p["format"] and not p["create"] and p["mountpoint"]]
    if kept:
        step("keep", "Use existing filesystems without formatting",
             [f"{p['device']} → {p['mountpoint']} (files are kept)" for p in kept])

    size = report["live_medium"].get("installed_size")
    step("copy", "Copy the system" + (f" ({human_size(size)})" if size else ""))
    step("configure", "Configure the system",
         [f"Language {answers.locale}, keyboard {answers.keyboard_layout}"
          + (f" ({answers.keyboard_variant})" if answers.keyboard_variant else "")
          + f", time zone {answers.timezone}, host name {answers.hostname}"]
         + ([f"Swap file of {human_size(swapfile)}"] if swapfile else []))
    step("user", f"Create the user account “{answers.user.username}”",
         ["Administrator rights through sudo; the root account stays locked."]
         + (["Automatic login is enabled."] if answers.user.autologin else []))
    step("cleanup", "Remove live-session components")

    removable = _decide_removable(answers, report, st.mode)
    boot_details: list[str] = []
    if not answers.install_bootloader:
        warnings.append("No boot loader will be installed: the new system will not start unless an existing "
                        "boot loader is configured for it by hand.")
    elif fw == "uefi":
        esp = next(p for p in partitions if p["mountpoint"] == "/boot/efi")
        boot_details.append(f"GRUB (UEFI, Secure Boot signed) on the EFI System Partition "
                            f"{esp['device'] or '(new)'}")
        boot_details.append("Registered in the firmware boot menu as “NEXUS OS”"
                            if not removable or report["firmware"]["efivars_writable"]
                            else "Installed to the removable-media boot path (EFI/BOOT)")
        if removable:
            boot_details.append("Also installed to the fallback path EFI/BOOT so the disk boots on firmware "
                                "that ignores boot entries")
        if st.mode != "erase-disk" and answers.os_prober:
            boot_details.append("Other operating systems found on this computer are added to the boot menu")
    else:
        boot_details.append(f"GRUB (legacy BIOS) in the boot sector of {bios_boot_disk}")
    if answers.install_bootloader:
        step("bootloader", "Install the boot loader", boot_details,
             destructive=(fw == "bios"))
    if answers.profiles and answers.install_profiles_now:
        step("profiles", "Install the selected security tool profiles",
             [", ".join(answers.profiles) + " (downloads packages; needs a network connection)"])
    step("finish", "Finish and unmount")

    if report["firmware"]["secure_boot"] is False and fw == "uefi":
        warnings.append("Secure Boot is disabled in the firmware. The system is installed Secure Boot ready; "
                        "you can enable it later in the firmware setup.")
    if st.mode != "erase-disk":
        warnings.append("Back up important data before continuing. Existing partitions are left untouched by "
                        "design, but a power failure or a hardware fault during partitioning can still damage "
                        "a partition table.")

    plan = {
        "plan_version": 1,
        "architecture": arch,
        "firmware": fw,
        "mode": st.mode,
        "target_disk": target_disk,
        "partitions": partitions,
        "lvm": lvm,
        "swapfile_bytes": swapfile,
        "bootloader": {
            "install": answers.install_bootloader,
            "bios_disk": bios_boot_disk if answers.install_bootloader else None,
            "removable": removable,
            "os_prober": answers.os_prober and st.mode != "erase-disk",
        },
        "system": {
            "locale": answers.locale, "keyboard_layout": answers.keyboard_layout,
            "keyboard_variant": answers.keyboard_variant, "keyboard_model": answers.keyboard_model,
            "timezone": answers.timezone, "hostname": answers.hostname,
            "username": answers.user.username, "fullname": answers.user.fullname,
            "autologin": answers.user.autologin, "extra_kernel_cmdline": answers.extra_kernel_cmdline,
            "encrypted": bool(luks),
        },
        "profiles": {"names": answers.profiles, "install_now": answers.install_profiles_now},
        "source": {"squashfs": report["live_medium"]["squashfs"], "installed_size": size},
        "steps": steps,
        "warnings": warnings,
    }
    plan["hash"] = plan_hash(plan)
    return plan


def _decide_removable(answers: Answers, report: dict, mode: str) -> bool:
    """Whether GRUB is also written to the removable-media path EFI/BOOT.

    'auto': yes when we created the ESP ourselves (nothing to overwrite; on a
    BIOS machine this keeps the disk bootable if the firmware is switched to
    UEFI later) or when the firmware cannot store a boot entry; no when an
    existing ESP is reused, because EFI/BOOT there usually belongs to another
    operating system.
    """
    if answers.bootloader_removable == "yes":
        return True
    if answers.bootloader_removable == "no":
        return False
    if mode == "erase-disk":
        return True         # also on BIOS machines: the new disk then starts in either firmware mode
    if report["firmware"]["type"] != "uefi":
        return False
    return not report["firmware"]["efivars_writable"]


def _role_text(part: dict) -> str:
    return {
        "bios_grub": "BIOS boot partition (lets the disk also start in legacy mode)",
        "esp": "EFI System Partition, mounted at /boot/efi",
        "boot": "boot partition (/boot, not encrypted — holds kernels)",
        "root": "system (/)",
        "home": "personal files (/home)",
        "swap": "swap",
        "luks": "encrypted volume for the system",
        "data": f"mounted at {part.get('mountpoint')}",
    }.get(part["role"], part["role"])


def _fs_text(fmt: str) -> str:
    return {"vfat": "FAT32", "ext4": "ext4", "swap": "swap"}.get(fmt, fmt)


def plan_hash(plan: dict) -> str:
    body = {k: v for k, v in plan.items() if k != "hash"}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def render_text(plan: dict) -> str:
    """Human-readable plan for terminals and logs."""
    lines = [f"NEXUS OS installation plan  [{plan['hash'][:16]}…]", ""]
    for i, s in enumerate(plan["steps"], start=1):
        mark = "  !! DESTRUCTIVE" if s["destructive"] else ""
        lines.append(f"{i:2}. {s['title']}{mark}")
        lines += [f"      {d}" for d in s["details"]]
    if plan["warnings"]:
        lines += ["", "Warnings:"] + [f"  - {w}" for w in plan["warnings"]]
    return "\n".join(lines) + "\n"
