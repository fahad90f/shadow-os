# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only inspection of the machine the installer is running on.

Nothing in this module writes to a disk. The optional operating-system scan
(os-prober) mounts foreign filesystems read-only for a moment and is therefore
only run when explicitly requested.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

from .util import read_text

SQUASHFS_CANDIDATES = (
    "/run/live/medium/live/filesystem.squashfs",
    "/lib/live/mount/medium/live/filesystem.squashfs",
)
MEDIUM_MOUNTS = ("/run/live/medium", "/lib/live/mount/medium")

LSBLK_COLUMNS = ("NAME,KNAME,PATH,PKNAME,TYPE,SIZE,RO,RM,HOTPLUG,MODEL,SERIAL,VENDOR,TRAN,FSTYPE,"
                 "LABEL,PARTLABEL,UUID,PARTUUID,PARTTYPE,PARTTYPENAME,PARTFLAGS,MOUNTPOINTS,PTTYPE,"
                 "LOG-SEC,PHY-SEC,ROTA,START")

ESP_GUID = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
BIOS_GRUB_GUID = "21686148-6449-6e6f-744e-656564454649"
MSFT_GUIDS = {
    "e3c9e316-0b5c-4db8-817d-f92df00215ae": "Microsoft reserved",
    "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7": "Microsoft basic data",
    "de94bba4-06d1-4d40-a16a-bfd50179d6ac": "Windows recovery environment",
}

MIN_DISK_BYTES = 20 * 1024**3
MIN_RAM_BYTES = 2 * 1024**3
RECOMMENDED_RAM_BYTES = 4 * 1024**3


def _run(argv: list[str], timeout: float = 60) -> tuple[int, str]:
    env = dict(os.environ, LC_ALL="C.UTF-8", LANGUAGE="C")
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return 127, ""


def architecture() -> str:
    rc, out = _run(["dpkg", "--print-architecture"])
    if rc == 0 and out.strip():
        return out.strip()
    return {"x86_64": "amd64", "aarch64": "arm64"}.get(os.uname().machine, os.uname().machine)


def firmware() -> dict:
    efi = Path("/sys/firmware/efi")
    if not efi.is_dir():
        kind = "bios" if architecture() == "amd64" else "none"
        return {"type": kind, "bits": None, "secure_boot": None, "efivars_writable": False}
    bits = read_text(efi / "fw_platform_size") or None
    secure = None
    var = efi / "efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"
    try:
        data = var.read_bytes()
        if len(data) >= 5:
            secure = data[4] == 1
    except OSError:
        pass
    writable = os.access(efi / "efivars", os.W_OK) and _mount_is_rw("/sys/firmware/efi/efivars")
    return {"type": "uefi", "bits": int(bits) if bits and bits.isdigit() else None,
            "secure_boot": secure, "efivars_writable": writable}


def _mount_is_rw(mountpoint: str) -> bool:
    for line in read_text("/proc/self/mounts").splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == mountpoint:
            return "rw" in parts[3].split(",")
    return False


def memory_bytes() -> int:
    m = re.search(r"^MemTotal:\s+(\d+)\s*kB", read_text("/proc/meminfo"), re.M)
    return int(m.group(1)) * 1024 if m else 0


def live_medium() -> dict:
    """Where the running live system comes from (so it is never a target)."""
    info = {"squashfs": None, "mount": None, "device": None, "disk": None, "installed_size": None}
    for candidate in SQUASHFS_CANDIDATES:
        if os.path.isfile(candidate):
            info["squashfs"] = candidate
            size_file = Path(candidate).with_name("filesystem.size")
            size = read_text(size_file)
            if size.isdigit():
                info["installed_size"] = int(size)
            break
    for mount in MEDIUM_MOUNTS:
        rc, out = _run(["findmnt", "-n", "-o", "SOURCE", "--target", mount])
        if rc == 0 and out.strip().startswith("/dev/") and os.path.ismount(mount):
            info["mount"] = mount
            info["device"] = out.strip().splitlines()[0]
            rc, parent = _run(["lsblk", "-n", "-d", "-o", "PKNAME", info["device"]])
            pk = parent.strip().splitlines()[0] if rc == 0 and parent.strip() else ""
            info["disk"] = f"/dev/{pk}" if pk else info["device"]
            break
    return info


def _flatten(node: dict, disk: dict | None, out: list):
    node = dict(node)
    children = node.pop("children", [])
    out.append((node, disk))
    for child in children:
        _flatten(child, disk if disk is not None else node, out)


def _free_regions(disk_path: str) -> list[dict]:
    """Unallocated regions of a disk, from parted's machine-readable output."""
    rc, out = _run(["parted", "-m", "-s", disk_path, "unit", "B", "print", "free"])
    regions = []
    if rc != 0:
        return regions
    for line in out.splitlines():
        fields = line.rstrip(";").split(":")
        if len(fields) >= 5 and fields[4] == "free":
            try:
                start, end, size = (int(fields[i].rstrip("B")) for i in (1, 2, 3))
            except ValueError:
                continue
            if size >= 1024**2:
                regions.append({"start": start, "end": end, "size": size})
    return regions


def disks(include_free: bool = True) -> list[dict]:
    rc, out = _run(["lsblk", "-J", "-b", "-o", LSBLK_COLUMNS])
    if rc != 0 or not out.strip():
        return []
    medium = live_medium()
    result = []
    for top in json.loads(out).get("blockdevices", []):
        if top.get("type") != "disk":
            continue
        size = int(top.get("size") or 0)
        if size == 0:
            continue                         # empty card readers etc.
        path = top.get("path") or f"/dev/{top['name']}"
        flat: list = []
        _flatten(top, None, flat)
        parts, mounted, has_luks, has_lvm, has_raid = [], [], False, False, False
        active = []
        for node, _ in flat[1:]:
            mps = [m for m in (node.get("mountpoints") or []) if m]
            if mps:
                mounted += mps
            ntype = node.get("type")
            if ntype in ("crypt", "lvm", "dm", "mpath") or (ntype or "").startswith("raid"):
                active.append(node.get("name"))
            has_luks |= node.get("fstype") == "crypto_LUKS" or ntype == "crypt"
            has_lvm |= node.get("fstype") == "LVM2_member" or ntype == "lvm"
            has_raid |= (node.get("fstype") or "").startswith("linux_raid") or (ntype or "").startswith("raid")
            if ntype != "part":
                continue
            ptype = (node.get("parttype") or "").lower()
            parts.append({
                "path": node.get("path"),
                "name": node.get("name"),
                "size": int(node.get("size") or 0),
                "start": int(node["start"]) * int(top.get("log-sec") or 512) if node.get("start") is not None else None,
                "fstype": node.get("fstype"),
                "label": node.get("label"),
                "partlabel": node.get("partlabel"),
                "uuid": node.get("uuid"),
                "partuuid": node.get("partuuid"),
                "parttype": ptype,
                "parttype_name": node.get("parttypename") or MSFT_GUIDS.get(ptype),
                "is_esp": ptype == ESP_GUID or (ptype == "0xef"),
                "is_bios_grub": ptype == BIOS_GRUB_GUID,
                "mountpoints": mps,
            })
        top_mps = [m for m in (top.get("mountpoints") or []) if m]
        is_medium = bool(medium["disk"]) and os.path.realpath(medium["disk"]) == os.path.realpath(path)
        entry = {
            "path": path,
            "name": top.get("name"),
            "size": size,
            "model": (top.get("model") or "").strip() or None,
            "vendor": (top.get("vendor") or "").strip() or None,
            "serial": (top.get("serial") or "").strip() or None,
            "transport": top.get("tran"),
            "removable": bool(top.get("rm")) or bool(top.get("hotplug")),
            "read_only": bool(top.get("ro")),
            "rotational": bool(top.get("rota")),
            "sector_size": int(top.get("log-sec") or 512),
            "partition_table": top.get("pttype"),
            "partitions": parts,
            "mountpoints": sorted(set(mounted + top_mps)),
            "has_luks": has_luks,
            "has_lvm": has_lvm,
            "has_raid": has_raid,
            "active_mappings": sorted(set(a for a in active if a)),
            "is_live_medium": is_medium,
            "whole_disk_fstype": top.get("fstype"),
        }
        entry["free_regions"] = _free_regions(path) if include_free and not entry["read_only"] else []
        entry["largest_free"] = max((r["size"] for r in entry["free_regions"]), default=0)
        entry["usable"], entry["unusable_reason"] = _usable(entry)
        result.append(entry)
    return result


def _usable(disk: dict) -> tuple[bool, str | None]:
    if disk["is_live_medium"]:
        return False, "This is the installation medium."
    if disk["read_only"]:
        return False, "The device is read-only."
    if disk["size"] < MIN_DISK_BYTES:
        return False, f"Smaller than the {MIN_DISK_BYTES // 1024**3} GiB minimum."
    return True, None


def other_systems() -> list[dict]:
    """Operating systems found by os-prober (slow; mounts filesystems read-only)."""
    rc, out = _run(["os-prober"], timeout=180)
    systems = []
    if rc != 0:
        return systems
    for line in out.splitlines():
        fields = line.split(":")
        if len(fields) >= 4:
            systems.append({"device": fields[0].split("@")[0], "name": fields[1], "label": fields[2], "type": fields[3]})
    return systems


def volume_groups() -> list[str]:
    rc, out = _run(["vgs", "--noheadings", "-o", "vg_name"])
    return sorted({line.strip() for line in out.splitlines() if line.strip()}) if rc == 0 else []


def mapper_names() -> list[str]:
    try:
        return sorted(n for n in os.listdir("/dev/mapper") if n != "control")
    except OSError:
        return []


def power() -> dict:
    base = Path("/sys/class/power_supply")
    on_ac, battery = None, None
    if base.is_dir():
        for supply in base.iterdir():
            kind = read_text(supply / "type")
            if kind == "Mains":
                on_ac = (on_ac or False) or read_text(supply / "online") == "1"
            elif kind == "Battery" and read_text(supply / "scope") != "Device":
                cap = read_text(supply / "capacity")
                if cap.isdigit():
                    battery = int(cap)
    return {"on_ac": on_ac, "battery_percent": battery}


def network_online() -> bool:
    rc, out = _run(["ip", "-j", "route", "show", "default"])
    try:
        return rc == 0 and len(json.loads(out or "[]")) > 0
    except json.JSONDecodeError:
        return False


def checks(report: dict) -> list[dict]:
    """Hardware compatibility check shown before the user commits to anything."""
    out = []

    def add(cid, status, title, detail=""):
        out.append({"id": cid, "status": status, "title": title, "detail": detail})

    arch, fw = report["architecture"], report["firmware"]
    if arch not in ("amd64", "arm64"):
        add("arch", "fail", f"Unsupported architecture: {arch}", "NEXUS OS is built for amd64 and arm64.")
    else:
        add("arch", "pass", f"Architecture: {arch}")

    if fw["type"] == "uefi":
        if fw["bits"] == 32:
            add("firmware", "fail", "32-bit UEFI firmware",
                "The boot loader shipped on this medium requires 64-bit UEFI.")
        else:
            add("firmware", "pass", "UEFI firmware")
        if fw["secure_boot"] is True:
            add("secureboot", "pass", "Secure Boot is enabled",
                "The system will be installed with the Microsoft-signed shim and Debian-signed GRUB and kernel.")
        elif fw["secure_boot"] is False:
            add("secureboot", "warn", "Secure Boot is disabled",
                "Installation works either way. Enabling Secure Boot in the firmware setup protects the boot chain.")
        if not fw["efivars_writable"]:
            add("efivars", "warn", "UEFI variables are not writable",
                "A boot entry cannot be registered; the removable-media boot path will be used instead.")
    elif fw["type"] == "bios":
        add("firmware", "warn", "Legacy BIOS boot",
            "Supported. If this computer offers UEFI mode, booting the installer in UEFI mode is preferable.")
    else:
        add("firmware", "fail", "No supported boot firmware detected",
            "On arm64, NEXUS OS installs only on machines with UEFI firmware.")

    ram = report["memory_bytes"]
    if ram < MIN_RAM_BYTES:
        add("memory", "fail", f"Only {ram / 1024**3:.1f} GiB of memory", "At least 2 GiB is required.")
    elif ram < RECOMMENDED_RAM_BYTES:
        add("memory", "warn", f"{ram / 1024**3:.1f} GiB of memory", "4 GiB or more is recommended for the desktop.")
    else:
        add("memory", "pass", f"{ram / 1024**3:.1f} GiB of memory")

    usable = [d for d in report["disks"] if d["usable"]]
    if not usable:
        add("disk", "fail", "No suitable disk found",
            f"A disk of at least {MIN_DISK_BYTES // 1024**3} GiB that is not the installation medium is required.")
    else:
        biggest = max(d["size"] for d in usable)
        add("disk", "pass", f"{len(usable)} suitable disk(s)", f"Largest: {biggest / 1024**3:.0f} GiB")

    if not report["live_medium"]["squashfs"]:
        add("medium", "fail", "System image not found on the installation medium",
            "Expected live/filesystem.squashfs. Verify the medium's checksum and write it again.")
    else:
        add("medium", "pass", "System image found")

    pw = report["power"]
    if pw["on_ac"] is False:
        level = pw["battery_percent"]
        add("power", "warn" if (level is None or level >= 30) else "fail",
            "Running on battery" + (f" ({level}%)" if level is not None else ""),
            "Connect the charger: losing power during installation leaves the disk in an unusable state.")
    elif pw["on_ac"]:
        add("power", "pass", "Connected to mains power")

    add("network", "pass" if report["network_online"] else "warn",
        "Network connection available" if report["network_online"] else "No network connection",
        "" if report["network_online"] else
        "Installation works offline; security updates and optional tool profiles need a connection later.")
    return out


def probe(with_other_systems: bool = False) -> dict:
    report = {
        "architecture": architecture(),
        "firmware": firmware(),
        "memory_bytes": memory_bytes(),
        "live_medium": live_medium(),
        "disks": disks(),
        "volume_groups": volume_groups(),
        "mapper_names": mapper_names(),
        "power": power(),
        "network_online": network_online(),
        "other_systems": other_systems() if with_other_systems else None,
        "min_disk_bytes": MIN_DISK_BYTES,
    }
    report["checks"] = checks(report)
    report["can_install"] = not any(c["status"] == "fail" for c in report["checks"])
    return report
