#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the installer's answer validation and planning.

These run anywhere (no root, no disks): the planner is a pure function of an
answer file and a probe report, and it is the component that decides what is
erased — so its rules are pinned down here.

    python3 tests/installation/test_plan.py
"""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "system/installer/engine"))

from nexus_installer import answers as A, plan as P, probe as PR  # noqa: E402
from nexus_installer.util import InstallError  # noqa: E402

MIB, GIB = 1024**2, 1024**3
ESP_GUID = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
BIOS_GUID = "21686148-6449-6e6f-744e-656564454649"


def part(path, start, size, fstype=None, parttype="0fc63daf-8483-4772-8e79-3d69d8477de4", label=None, mounts=()):
    return {"path": path, "name": path.split("/")[-1], "size": size, "start": start, "fstype": fstype,
            "label": label, "partlabel": None, "uuid": None, "partuuid": None, "parttype": parttype,
            "parttype_name": None, "is_esp": parttype == ESP_GUID, "is_bios_grub": parttype == BIOS_GUID,
            "mountpoints": list(mounts)}


def disk(path="/dev/vda", size=64 * GIB, table="gpt", parts=(), free=None, live=False, ro=False, mounts=(),
         active=()):
    d = {"path": path, "name": path.split("/")[-1], "size": size, "model": "TESTDISK", "vendor": "NX",
         "serial": "S1", "transport": "virtio", "removable": False, "read_only": ro, "rotational": False,
         "sector_size": 512, "partition_table": table, "partitions": list(parts),
         "mountpoints": sorted(set(mounts) | {m for p in parts for m in p["mountpoints"]}),
         "has_luks": False, "has_lvm": False, "has_raid": False, "active_mappings": list(active),
         "is_live_medium": live, "whole_disk_fstype": None,
         "free_regions": free if free is not None else [], }
    d["largest_free"] = max((r["size"] for r in d["free_regions"]), default=0)
    d["usable"], d["unusable_reason"] = PR._usable(d)
    return d


def report(disks, fw="uefi", arch="amd64", secure=True, ram=8 * GIB, installed=6 * GIB, efivars=True):
    r = {"architecture": arch,
         "firmware": {"type": fw, "bits": 64 if fw == "uefi" else None,
                      "secure_boot": secure if fw == "uefi" else None,
                      "efivars_writable": efivars and fw == "uefi"},
         "memory_bytes": ram,
         "live_medium": {"squashfs": "/run/live/medium/live/filesystem.squashfs", "mount": "/run/live/medium",
                         "device": "/dev/sr0", "disk": "/dev/sr0", "installed_size": installed},
         "disks": disks, "volume_groups": [], "mapper_names": [],
         "power": {"on_ac": True, "battery_percent": None}, "network_online": True,
         "other_systems": None, "min_disk_bytes": PR.MIN_DISK_BYTES}
    r["checks"] = PR.checks(r)
    r["can_install"] = not any(c["status"] == "fail" for c in r["checks"])
    return r


def answers(**storage):
    base = {"version": 1, "locale": "en_US.UTF-8", "keyboard": {"layout": "us"}, "timezone": "Etc/UTC",
            "hostname": "nexus", "user": {"fullname": "Test User", "username": "tester", "password": "secret"},
            "storage": {"mode": "erase-disk", "disk": "/dev/vda"}}
    base["storage"].update(storage)
    return base


def parse(data):
    # Do not depend on the test host's locale/zoneinfo databases.
    return A.parse(data, zoneinfo_dir="/nonexistent", supported_locales="/nonexistent")


def build(data, rep):
    return P.build(parse(data), rep)


class AnswerValidation(unittest.TestCase):
    def test_minimal_answers_parse(self):
        a = parse(answers())
        self.assertEqual(a.user.username, "tester")
        self.assertEqual(a.storage.swap, "file")
        self.assertTrue(a.install_bootloader)

    def test_unknown_keys_are_rejected(self):
        for mutate in (lambda d: d.update(colour="blue"),
                       lambda d: d["storage"].update(encrypted=True),      # typo of "encrypt"
                       lambda d: d["user"].update(shell="/bin/zsh")):
            data = answers()
            mutate(data)
            with self.assertRaises(InstallError):
                parse(data)

    def test_bad_identities(self):
        for field, value in (("username", "Root"), ("username", "root"), ("username", "1abc"),
                             ("username", "a b"), ("username", "x" * 40)):
            data = answers()
            data["user"][field] = value
            with self.assertRaises(InstallError, msg=value):
                parse(data)
        for host in ("Nexus", "-nexus", "nexus_", "a" * 64, "ne xus", "nexus;rm"):
            data = answers()
            data["hostname"] = host
            with self.assertRaises(InstallError, msg=host):
                parse(data)

    def test_password_rules(self):
        data = answers()
        data["user"]["password_hash"] = "$y$j9T$abc$def"
        with self.assertRaises(InstallError):       # both given
            parse(data)
        del data["user"]["password"]
        parse(data)                                 # hash only is fine
        data["user"]["password_hash"] = "plaintext"
        with self.assertRaises(InstallError):
            parse(data)

    def test_encryption_needs_passphrase(self):
        with self.assertRaises(InstallError):
            parse(answers(encrypt=True))
        with self.assertRaises(InstallError):
            parse(answers(encrypt=True, passphrase="short"))
        with self.assertRaises(InstallError):
            parse(answers(passphrase="long enough but encrypt is false"))
        parse(answers(encrypt=True, passphrase="correct horse battery"))

    def test_device_paths_and_cmdline_are_sanitised(self):
        for dev in ("vda", "/dev/../etc/passwd", "/dev/vda; rm -rf /", "/tmp/disk.img"):
            with self.assertRaises(InstallError, msg=dev):
                parse(answers(disk=dev))
        data = answers()
        data["kernel"] = {"extra_cmdline": "console=ttyS0,115200n8 quiet"}
        self.assertEqual(parse(data).extra_kernel_cmdline, "console=ttyS0,115200n8 quiet")
        data["kernel"] = {"extra_cmdline": "x\"; reboot"}
        with self.assertRaises(InstallError):
            parse(data)

    def test_manual_mode_rules(self):
        root = {"device": "/dev/vda3", "mountpoint": "/", "format": True}
        esp = {"device": "/dev/vda1", "mountpoint": "/boot/efi"}
        parse(answers(mode="manual", disk=None, mounts=[root, esp]) | {"storage": {"mode": "manual", "mounts": [root, esp]}})
        bad = [
            [esp],                                                          # no root
            [dict(root, format=False), esp],                                # root must be formatted
            [root, dict(esp, device="/dev/vda3")],                          # duplicate device
            [root, esp, {"device": "/dev/vda4", "mountpoint": "/"}],        # duplicate mountpoint
            [root, {"device": "/dev/vda1", "mountpoint": "/boot/efi", "format": True, "filesystem": "ext4"}],
            [root, {"device": "/dev/vda4", "mountpoint": "/etc"}],          # unsupported mountpoint
        ]
        for mounts in bad:
            data = answers()
            data["storage"] = {"mode": "manual", "mounts": mounts}
            with self.assertRaises(InstallError, msg=str(mounts)):
                parse(data)
        data = answers()
        data["storage"] = {"mode": "manual", "mounts": [root, esp], "encrypt": True, "passphrase": "x" * 12}
        with self.assertRaises(InstallError):
            parse(data)


class EraseDisk(unittest.TestCase):
    def test_uefi_plain_layout(self):
        plan = build(answers(), report([disk()]))
        roles = [p["role"] for p in plan["partitions"]]
        self.assertEqual(roles, ["bios_grub", "esp", "root"])
        self.assertEqual([p["device"] for p in plan["partitions"]], ["/dev/vda1", "/dev/vda2", "/dev/vda3"])
        self.assertEqual(plan["partitions"][1]["size"], 512 * MIB)
        self.assertEqual(plan["partitions"][1]["mountpoint"], "/boot/efi")
        self.assertEqual(plan["partitions"][2]["mountpoint"], "/")
        self.assertIsNone(plan["lvm"])
        self.assertGreater(plan["swapfile_bytes"], 0)
        self.assertTrue(plan["bootloader"]["removable"])
        self.assertTrue(plan["steps"][0]["destructive"])
        self.assertIn("ALL data", plan["steps"][0]["details"][0])

    def test_layout_is_aligned_contiguous_and_inside_the_disk(self):
        for size in (20 * GIB, 64 * GIB, 500107862016, 2000398934016):
            d = disk(size=size)
            plan = build(answers(), report([d]))
            pos = MIB
            for p in plan["partitions"]:
                self.assertEqual(p["start"], pos)
                self.assertEqual(p["start"] % MIB, 0)
                self.assertEqual(p["size"] % MIB, 0)
                pos += p["size"]
            self.assertLessEqual(pos, size - MIB, "must leave room for the backup GPT")

    def test_nvme_and_mmc_partition_names(self):
        for path, expect in (("/dev/nvme0n1", "/dev/nvme0n1p2"), ("/dev/mmcblk0", "/dev/mmcblk0p2"),
                             ("/dev/sdb", "/dev/sdb2")):
            plan = build(answers(disk=path), report([disk(path=path)]))
            self.assertEqual(plan["partitions"][1]["device"], expect)

    def test_arm64_has_no_bios_boot_partition(self):
        plan = build(answers(), report([disk()], arch="arm64"))
        self.assertEqual([p["role"] for p in plan["partitions"]], ["esp", "root"])
        self.assertEqual(plan["partitions"][1]["typecode"], "8305")

    def test_existing_partitions_are_listed_as_lost(self):
        d = disk(parts=[part("/dev/vda1", MIB, 100 * MIB, "vfat", ESP_GUID),
                        part("/dev/vda2", 101 * MIB, 40 * GIB, "ntfs", label="Windows")])
        text = "\n".join(build(answers(), report([d]))["steps"][0]["details"])
        self.assertIn("/dev/vda2", text)
        self.assertIn("Windows", text)

    def test_encrypted_layout_uses_boot_partition_and_lvm(self):
        plan = build(answers(encrypt=True, passphrase="correct horse battery", swap="partition"), report([disk()]))
        self.assertEqual([p["role"] for p in plan["partitions"]], ["bios_grub", "esp", "boot", "luks"])
        self.assertEqual(plan["partitions"][2]["mountpoint"], "/boot")
        self.assertEqual([lv["name"] for lv in plan["lvm"]["lvs"]], ["swap", "root"])
        self.assertIsNone(plan["lvm"]["lvs"][-1]["size"], "last volume takes the remaining space")
        self.assertEqual(plan["swapfile_bytes"], 0)
        self.assertTrue(plan["system"]["encrypted"])
        self.assertNotIn("correct horse", str(plan), "the passphrase must never be part of the plan")
        self.assertTrue(any(s["id"] == "encrypt" and s["destructive"] for s in plan["steps"]))

    def test_separate_home(self):
        plan = build(answers(separate_home=True), report([disk(size=256 * GIB)]))
        self.assertEqual([p["role"] for p in plan["partitions"]], ["bios_grub", "esp", "root", "home"])
        root, home = plan["partitions"][2], plan["partitions"][3]
        self.assertGreaterEqual(root["size"], 40 * GIB)
        self.assertLessEqual(root["size"], 120 * GIB)
        self.assertGreaterEqual(home["size"], 8 * GIB)
        with self.assertRaises(InstallError):
            build(answers(separate_home=True), report([disk(size=21 * GIB)]))

    def test_volume_names_avoid_collisions(self):
        rep = report([disk()])
        rep["volume_groups"] = ["nexus"]
        rep["mapper_names"] = ["nexus_crypt", "nexus-root"]
        plan = build(answers(encrypt=True, passphrase="correct horse battery"), rep)
        self.assertEqual(plan["lvm"]["vg"], "nexus1")
        self.assertEqual(plan["lvm"]["crypt_name"], "nexus_crypt1")

    def test_refuses_dangerous_targets(self):
        cases = {
            "live medium": disk(live=True),
            "mounted": disk(mounts=["/mnt/data"]),
            "swap in use": disk(parts=[part("/dev/vda1", MIB, GIB, "swap", mounts=["[SWAP]"])]),
            "active volumes": disk(active=["vg0-root"]),
            "read-only": disk(ro=True),
            "too small": disk(size=10 * GIB),
        }
        for name, d in cases.items():
            other = disk(path="/dev/vdb", size=64 * GIB)         # keeps the "a usable disk exists" check green
            with self.assertRaises(InstallError, msg=name):
                build(answers(), report([d, other]))

    def test_requirements_failures_block_planning(self):
        rep = report([disk()], ram=1 * GIB)
        with self.assertRaises(InstallError):
            build(answers(), rep)
        rep = report([disk()])
        rep["live_medium"]["squashfs"] = None
        rep["checks"] = PR.checks(rep)
        with self.assertRaises(InstallError):
            build(answers(), rep)

    def test_bios_firmware(self):
        plan = build(answers(), report([disk()], fw="bios"))
        self.assertEqual(plan["bootloader"]["bios_disk"], "/dev/vda")
        self.assertEqual([p["role"] for p in plan["partitions"]], ["bios_grub", "esp", "root"])

    def test_hash_is_stable_and_sensitive(self):
        rep = report([disk()])
        h1 = build(answers(), rep)["hash"]
        self.assertEqual(h1, build(answers(), copy.deepcopy(rep))["hash"])
        bigger = report([disk(size=128 * GIB)])
        self.assertNotEqual(h1, build(answers(), bigger)["hash"])
        self.assertNotEqual(h1, build(answers(separate_home=True), report([disk(size=128 * GIB)]))["hash"])
        data = answers()
        data["hostname"] = "other"
        self.assertNotEqual(h1, build(data, rep)["hash"])


class FreeSpace(unittest.TestCase):
    def windows_disk(self, free_size=80 * GIB, table="gpt"):
        esp = part("/dev/vda1", MIB, 260 * MIB, "vfat", ESP_GUID)
        msr = part("/dev/vda2", 261 * MIB, 16 * MIB, None, "e3c9e316-0b5c-4db8-817d-f92df00215ae")
        win = part("/dev/vda3", 277 * MIB, 100 * GIB, "ntfs", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Windows")
        start = 277 * MIB + 100 * GIB
        return disk(size=start + free_size + MIB, table=table, parts=[esp, msr, win],
                    free=[{"start": start, "end": start + free_size - 1, "size": free_size}])

    def test_existing_esp_is_reused_and_nothing_existing_is_touched(self):
        d = self.windows_disk()
        plan = build(answers(mode="free-space"), report([d]))
        esp = plan["partitions"][0]
        self.assertEqual((esp["role"], esp["device"], esp["create"], esp["format"]), ("esp", "/dev/vda1", False, None))
        created = [p for p in plan["partitions"] if p["create"]]
        self.assertEqual([p["role"] for p in created], ["root"])
        free = d["free_regions"][0]
        for p in created:
            self.assertGreaterEqual(p["start"], free["start"])
            self.assertLessEqual(p["start"] + p["size"], free["end"] + 1)
        existing = {(p["start"], p["size"]) for p in d["partitions"]}
        for p in created:                       # no overlap with any existing partition
            for (s, sz) in existing:
                self.assertFalse(p["start"] < s + sz and s < p["start"] + p["size"])
        self.assertFalse(plan["steps"][0]["destructive"])
        self.assertFalse(plan["bootloader"]["removable"], "EFI/BOOT on a shared ESP belongs to the other OS")
        self.assertTrue(plan["bootloader"]["os_prober"])

    def test_encrypted_into_free_space(self):
        plan = build(answers(mode="free-space", encrypt=True, passphrase="correct horse battery"),
                     report([self.windows_disk()]))
        self.assertEqual([p["role"] for p in plan["partitions"] if p["create"]], ["boot", "luks"])

    def test_requires_gpt_and_enough_space(self):
        with self.assertRaises(InstallError):
            build(answers(mode="free-space"), report([self.windows_disk(table="dos")]))
        with self.assertRaises(InstallError):
            build(answers(mode="free-space"), report([self.windows_disk(free_size=4 * GIB)]))
        d = self.windows_disk()
        d["free_regions"] = []
        with self.assertRaises(InstallError):
            build(answers(mode="free-space"), report([d]))

    def test_mounted_esp_blocks(self):
        d = self.windows_disk()
        d["partitions"][0]["mountpoints"] = ["/mnt/esp"]
        with self.assertRaises(InstallError):
            build(answers(mode="free-space"), report([d]))


class Manual(unittest.TestCase):
    def setUp(self):
        self.disk = disk(parts=[
            part("/dev/vda1", MIB, 512 * MIB, "vfat", ESP_GUID),
            part("/dev/vda2", 513 * MIB, 30 * GIB, "ext4", label="old-root"),
            part("/dev/vda3", 513 * MIB + 30 * GIB, 20 * GIB, "ext4", label="home"),
            part("/dev/vda4", 513 * MIB + 50 * GIB, 4 * GIB, "swap"),
        ])

    def manual(self, mounts, **extra):
        data = answers()
        data["storage"] = {"mode": "manual", "mounts": mounts, "swap": "none", **extra}
        return data

    def test_reuse_esp_keep_home_format_root(self):
        plan = build(self.manual([
            {"device": "/dev/vda2", "mountpoint": "/", "format": True},
            {"device": "/dev/vda1", "mountpoint": "/boot/efi"},
            {"device": "/dev/vda3", "mountpoint": "/home"},
            {"device": "/dev/vda4", "use": "swap"},
        ]), report([self.disk]))
        by = {p["device"]: p for p in plan["partitions"]}
        self.assertEqual(by["/dev/vda2"]["format"], "ext4")
        self.assertIsNone(by["/dev/vda1"]["format"], "an existing ESP is never formatted implicitly")
        self.assertIsNone(by["/dev/vda3"]["format"])
        self.assertEqual(by["/dev/vda4"]["format"], "swap")
        self.assertFalse(any(p["create"] for p in plan["partitions"]))
        self.assertNotIn("partition", [s["id"] for s in plan["steps"]])
        fmt = next(s for s in plan["steps"] if s["id"] == "format")
        self.assertTrue(fmt["destructive"])
        keep = next(s for s in plan["steps"] if s["id"] == "keep")
        self.assertIn("/dev/vda3", " ".join(keep["details"]))

    def test_uefi_requires_an_esp(self):
        with self.assertRaises(InstallError):
            build(self.manual([{"device": "/dev/vda2", "mountpoint": "/", "format": True}]), report([self.disk]))

    def test_bios_requires_bootloader_device_and_bios_boot_partition(self):
        mounts = [{"device": "/dev/vda2", "mountpoint": "/", "format": True}]
        with self.assertRaises(InstallError):
            build(self.manual(mounts), report([self.disk], fw="bios"))
        with self.assertRaises(InstallError):   # GPT disk without BIOS boot partition
            build(self.manual(mounts, bootloader_device="/dev/vda"), report([self.disk], fw="bios"))
        d = copy.deepcopy(self.disk)
        d["partitions"].append(part("/dev/vda5", 513 * MIB + 54 * GIB, MIB, None, BIOS_GUID))
        plan = build(self.manual(mounts, bootloader_device="/dev/vda"), report([d], fw="bios"))
        self.assertEqual(plan["bootloader"]["bios_disk"], "/dev/vda")

    def test_unknown_or_busy_partitions_are_refused(self):
        with self.assertRaises(InstallError):
            build(self.manual([{"device": "/dev/vdz9", "mountpoint": "/", "format": True},
                               {"device": "/dev/vda1", "mountpoint": "/boot/efi"}]), report([self.disk]))
        d = copy.deepcopy(self.disk)
        d["partitions"][2]["mountpoints"] = ["/mnt"]
        with self.assertRaises(InstallError):
            build(self.manual([{"device": "/dev/vda2", "mountpoint": "/", "format": True},
                               {"device": "/dev/vda1", "mountpoint": "/boot/efi"},
                               {"device": "/dev/vda3", "mountpoint": "/home"}]), report([d]))

    def test_keeping_a_filesystem_requires_one(self):
        d = copy.deepcopy(self.disk)
        d["partitions"][2]["fstype"] = None
        with self.assertRaises(InstallError):
            build(self.manual([{"device": "/dev/vda2", "mountpoint": "/", "format": True},
                               {"device": "/dev/vda1", "mountpoint": "/boot/efi"},
                               {"device": "/dev/vda3", "mountpoint": "/home"}]), report([d]))


if __name__ == "__main__":
    unittest.main(verbosity=1)
