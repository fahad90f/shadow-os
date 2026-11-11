#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the synthetic probe reports in testing/probes/.

These files describe machines that do not exist. They are test fixtures for
the graphical installer (`nexus-installer --probe-file …`, which then runs in
TEST MODE and can never install) and are not shipped in any package.

The disks are made up here; everything the engine derives from them — the
"usable" verdict of each disk and the list of checks — is computed by the
engine's own code (nexus_installer.probe), so the fixtures always have the
shape and the rules of the engine they are generated with. Re-run after the
engine's probe format changes:

    python3 system/installer/gui/testing/make-probes.py
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "engine"))
sys.dont_write_bytecode = True

from nexus_installer import probe as PR  # noqa: E402

MIB, GIB = 1024**2, 1024**3
ESP = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
BIOS_GRUB = "21686148-6449-6e6f-744e-656564454649"
MSR = "e3c9e316-0b5c-4db8-817d-f92df00215ae"
MS_DATA = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
MS_RECOVERY = "de94bba4-06d1-4d40-a16a-bfd50179d6ac"
LINUX = "0fc63daf-8483-4772-8e79-3d69d8477de4"
LINUX_SWAP = "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"
LINUX_HOME = "933ac7e1-2eb4-4f13-b844-0e14e2aef915"

TYPE_NAMES = {
    ESP: "EFI System", BIOS_GRUB: "BIOS boot", MSR: "Microsoft reserved", MS_DATA: "Microsoft basic data",
    MS_RECOVERY: "Windows recovery environment", LINUX: "Linux filesystem", LINUX_SWAP: "Linux swap",
    LINUX_HOME: "Linux home", "0x83": "Linux", "0x82": "Linux swap / Solaris", "0x0": "Empty",
}


def part(path, start, size, fstype=None, parttype=LINUX, label=None, partlabel=None, mounts=()):
    return {"path": path, "name": path.split("/")[-1], "size": size, "start": start, "fstype": fstype,
            "label": label, "partlabel": partlabel, "uuid": None, "partuuid": None, "parttype": parttype,
            "parttype_name": TYPE_NAMES.get(parttype), "is_esp": parttype in (ESP, "0xef"),
            "is_bios_grub": parttype == BIOS_GRUB, "mountpoints": list(mounts)}


def disk(path, size, *, model, vendor=None, serial=None, transport="sata", removable=False, ro=False,
         rotational=False, table="gpt", parts=(), live=False, active=(), luks=False, lvm=False, whole_fs=None):
    parts = list(parts)
    d = {"path": path, "name": path.split("/")[-1], "size": size, "model": model, "vendor": vendor,
         "serial": serial, "transport": transport, "removable": removable, "read_only": ro,
         "rotational": rotational, "sector_size": 512, "partition_table": table, "partitions": parts,
         "mountpoints": sorted({m for p in parts for m in p["mountpoints"]}),
         "has_luks": luks, "has_lvm": lvm, "has_raid": False, "active_mappings": list(active),
         "is_live_medium": live, "whole_disk_fstype": whole_fs}
    # Unallocated regions the way `parted print free` reports them: gaps of at
    # least 1 MiB between the first usable sector and the end of the disk.
    regions, pos = [], MIB
    end_of_disk = size - MIB if table == "gpt" else size
    for p in sorted(parts, key=lambda x: x["start"]):
        if p["start"] - pos >= MIB:
            regions.append({"start": pos, "end": p["start"] - 1, "size": p["start"] - pos})
        pos = max(pos, p["start"] + p["size"])
    if table and end_of_disk - pos >= MIB:
        regions.append({"start": pos, "end": end_of_disk - 1, "size": end_of_disk - pos})
    d["free_regions"] = [] if ro else regions
    d["largest_free"] = max((r["size"] for r in d["free_regions"]), default=0)
    d["usable"], d["unusable_reason"] = PR._usable(d)
    return d


def report(disks, *, fw="uefi", bits=64, arch="amd64", secure=True, efivars=True, ram=16 * GIB,
           installed=6_400_000_000, medium=("/dev/sdb1", "/dev/sdb"), on_ac=True, battery=None, online=True,
           other_systems=None, volume_groups=(), mapper_names=()):
    r = {"architecture": arch,
         "firmware": {"type": fw, "bits": bits if fw == "uefi" else None,
                      "secure_boot": secure if fw == "uefi" else None,
                      "efivars_writable": efivars and fw == "uefi"},
         "memory_bytes": ram,
         "live_medium": {"squashfs": "/run/live/medium/live/filesystem.squashfs", "mount": "/run/live/medium",
                         "device": medium[0], "disk": medium[1], "installed_size": installed},
         "disks": disks, "volume_groups": list(volume_groups), "mapper_names": list(mapper_names),
         "power": {"on_ac": on_ac, "battery_percent": battery}, "network_online": online,
         "other_systems": other_systems, "min_disk_bytes": PR.MIN_DISK_BYTES}
    r["checks"] = PR.checks(r)
    r["can_install"] = not any(c["status"] == "fail" for c in r["checks"])
    return r


def usb_medium(path="/dev/sdb"):
    return disk(path, 30_752_636_928, model="Ultra Fit", vendor="SanDisk", serial="4C530001230521", transport="usb",
                removable=True, table="dos", live=True, parts=[
                    part(f"{path}1", 0, 3_100 * MIB, "iso9660", "0x0", label="NEXUS_OS_0_1_0",
                         mounts=["/run/live/medium"]),
                    part(f"{path}2", 3_100 * MIB, 10 * MIB, "vfat", "0xef", label="NEXUS-EFI"),
                ])


def scenario_uefi_dualboot():
    """Laptop with Windows on an NVMe disk and unallocated space next to it,
    a second data disk, and the live medium on a USB stick."""
    size = 512_110_190_592
    win_start = 277 * MIB
    win_size = 300 * GIB
    rec_size = 1 * GIB
    rec_start = (size - MIB - rec_size) // MIB * MIB
    nvme = disk("/dev/nvme0n1", size, model="Samsung SSD 980 PRO 500GB", serial="S5GXNX0T123456A", transport="nvme",
                parts=[
                    part("/dev/nvme0n1p1", MIB, 260 * MIB, "vfat", ESP, label="SYSTEM",
                         partlabel="EFI system partition"),
                    part("/dev/nvme0n1p2", 261 * MIB, 16 * MIB, None, MSR, partlabel="Microsoft reserved partition"),
                    part("/dev/nvme0n1p3", win_start, win_size, "ntfs", MS_DATA, label="Windows",
                         partlabel="Basic data partition"),
                    part("/dev/nvme0n1p4", rec_start, rec_size, "ntfs", MS_RECOVERY, label="Recovery"),
                ])
    hdd_size = 2_000_398_934_016
    hdd = disk("/dev/sda", hdd_size, model="WDC WD20EZAZ-00GGJB0", vendor="ATA", serial="WD-WXK2A80L1234",
               rotational=True, parts=[
                   part("/dev/sda1", MIB, (hdd_size - 2 * MIB) // MIB * MIB, "ext4", LINUX, label="backup"),
               ])
    systems = [{"device": "/dev/nvme0n1p1", "name": "Windows Boot Manager", "label": "Windows", "type": "efi"}]
    return report([nvme, hdd, usb_medium("/dev/sdb")], battery=87, other_systems=systems)


def scenario_bios_legacy():
    """Older desktop: legacy BIOS, an MBR disk with a Linux installation,
    3 GiB of memory, no network; booted from a DVD (/dev/sr0)."""
    size = 120_034_123_776
    ssd = disk("/dev/sda", size, model="KINGSTON SA400S37120G", vendor="ATA", serial="50026B77824F1234", table="dos",
               parts=[
                   part("/dev/sda1", MIB, 100 * GIB, "ext4", "0x83", label="rootfs"),
                   part("/dev/sda2", MIB + 100 * GIB, 8 * GIB, "swap", "0x82"),
               ])
    systems = [{"device": "/dev/sda1", "name": "Debian GNU/Linux 12 (bookworm)", "label": "Debian", "type": "linux"}]
    return report([ssd], fw="bios", ram=3 * GIB, medium=("/dev/sr0", "/dev/sr0"), on_ac=None, online=False,
                  other_systems=systems)


def scenario_uefi_manual():
    """Virtual machine with an existing Linux layout (ESP, root, home, swap)
    for manual partition assignment, and a second disk that is in use."""
    start = MIB
    parts = []
    for name, size, fs, ptype, label in (("vda1", 512 * MIB, "vfat", ESP, None),
                                         ("vda2", 30 * GIB, "ext4", LINUX, "old-root"),
                                         ("vda3", 20 * GIB, "ext4", LINUX_HOME, "home"),
                                         ("vda4", 4 * GIB, "swap", LINUX_SWAP, None)):
        parts.append(part(f"/dev/{name}", start, size, fs, ptype, label=label))
        start += size
    vda = disk("/dev/vda", 64 * GIB, model=None, vendor="0x1af4", transport="virtio", parts=parts)
    vdb = disk("/dev/vdb", 40 * GIB, model=None, vendor="0x1af4", transport="virtio", parts=[
        part("/dev/vdb1", MIB, 39 * GIB, "ext4", LINUX, label="data", mounts=["/mnt/data"]),
    ])
    systems = [{"device": "/dev/vda2", "name": "Ubuntu 24.04.1 LTS (24.04)", "label": "Ubuntu", "type": "linux"}]
    return report([vda, vdb], secure=False, ram=8 * GIB, medium=("/dev/sr0", "/dev/sr0"), on_ac=None,
                  other_systems=systems)


def scenario_blocked():
    """A machine the engine refuses: too little memory, the only internal
    disk is too small, and the battery is nearly empty."""
    emmc = disk("/dev/mmcblk0", 15_634_268_160, model="DF4016", transport="mmc", parts=[
        part("/dev/mmcblk0p1", MIB, 100 * MIB, "vfat", ESP),
        part("/dev/mmcblk0p2", 101 * MIB, 14 * GIB, "ntfs", MS_DATA, label="Windows"),
    ])
    return report([emmc, usb_medium("/dev/sda")], ram=int(1.5 * GIB), medium=("/dev/sda1", "/dev/sda"),
                  on_ac=False, battery=12, secure=False, other_systems=[])


def scenario_arm64_blank():
    """arm64 board with UEFI firmware and one empty eMMC module."""
    emmc = disk("/dev/mmcblk0", 62_537_072_640, model="BJTD4R", transport="mmc", table=None)
    return report([emmc, usb_medium("/dev/sda")], arch="arm64", ram=4 * GIB, medium=("/dev/sda1", "/dev/sda"),
                  on_ac=None, efivars=False, secure=None, other_systems=[])


SCENARIOS = {
    "uefi-dualboot": scenario_uefi_dualboot,
    "bios-legacy": scenario_bios_legacy,
    "uefi-manual": scenario_uefi_manual,
    "blocked": scenario_blocked,
    "arm64-blank": scenario_arm64_blank,
}

if __name__ == "__main__":
    out = HERE / "probes"
    out.mkdir(exist_ok=True)
    for name, make in SCENARIOS.items():
        data = make()
        (out / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: can_install={data['can_install']}, disks={[d['path'] for d in data['disks']]}, "
              f"checks={[(c['id'], c['status']) for c in data['checks']]}")
