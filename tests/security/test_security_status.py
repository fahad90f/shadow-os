#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit-style checks of security/audit/nexus-security-status.

    python3 tests/security/test_security_status.py [-v]

The probes that need a subsystem the test machine may lack (AppArmor,
dm-crypt, EFI, auditd logs, CPU vulnerability files) are run against small
fabricated /proc- and /sys-like trees built in a temporary directory, by
pointing the tool's PROC/SYS/BOOT constants at them. Everything in those
trees is test data. Tests that need a real tool (ausearch) skip themselves
when it is not installed.

Standard library only; runs as any user (in the build chroot:
scripts/dev.sh exec --dir <DIR> -- python3 /src/tests/security/test_security_status.py).
"""
import gzip
import importlib.machinery
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, "security", "audit", "nexus-security-status")
AUDIT_LOG = os.path.join(HERE, "fixtures", "audit.log")


def load_tool():
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader("nexus_security_status", TOOL)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def write(path, text="", mode="w"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as fh:
        fh.write(text)


class Base(unittest.TestCase):
    def setUp(self):
        self.t = load_tool()
        self.tmp = tempfile.mkdtemp(prefix="nexus-sec-test.")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.t.PROC = os.path.join(self.tmp, "proc")
        self.t.SYS = os.path.join(self.tmp, "sys")
        self.t.BOOT = os.path.join(self.tmp, "boot")
        os.makedirs(self.t.PROC)
        os.makedirs(self.t.SYS)
        os.makedirs(self.t.BOOT)
        self.calls = []

    def fake_run(self, table):
        """Replace the tool's run(): table maps argv[0] → (rc, out, err) or a callable(argv)."""
        def run(argv, timeout=20):
            self.calls.append(list(argv))
            entry = table.get(argv[0])
            if entry is None:
                return 127, "", f"{argv[0]}: not installed"
            return entry(argv) if callable(entry) else entry
        self.t.run = run


# --------------------------------------------------------------------------- mitigations
class Mitigations(Base):
    def test_vulnerable_anywhere_in_the_string(self):
        cases = {
            "Not affected": [],
            "Mitigation: PTI": [],
            "Vulnerable": ["Vulnerable"],
            "Vulnerable: Clear CPU buffers attempted, no microcode": ["Vulnerable: Clear CPU buffers attempted"],
            "Mitigation: Retpolines; IBPB: conditional; IBRS_FW; STIBP: conditional; RSB filling; "
            "PBRSB-eIBRS: Not affected; BHI: Vulnerable": ["BHI: Vulnerable"],
            "Mitigation: Clear CPU buffers; SMT vulnerable": ["SMT vulnerable"],
            "Mitigation: Enhanced / Automatic IBRS; IBPB: conditional; RSB filling; PBRSB-eIBRS: SW sequence; "
            "BHI: BHI_DIS_S": [],
        }
        for status, parts in cases.items():
            self.assertEqual(self.t._vulnerable_parts(status), parts, status)

    def test_section_reports_full_string_and_flag(self):
        vuln = os.path.join(self.t.SYS, "devices/system/cpu/vulnerabilities")
        write(os.path.join(vuln, "spectre_v2"), "Mitigation: Retpolines; IBPB: conditional; BHI: Vulnerable\n")
        write(os.path.join(vuln, "meltdown"), "Mitigation: PTI\n")
        write(os.path.join(vuln, "mds"), "Vulnerable: Clear CPU buffers attempted, no microcode; SMT vulnerable\n")
        info = self.t.section_hardening()
        got = {m["name"]: m for m in info["mitigations"]}
        self.assertTrue(got["spectre_v2"]["vulnerable"])
        self.assertEqual(got["spectre_v2"]["status"], "Mitigation: Retpolines; IBPB: conditional; BHI: Vulnerable")
        self.assertEqual(got["spectre_v2"]["vulnerable_parts"], ["BHI: Vulnerable"])
        self.assertFalse(got["meltdown"]["vulnerable"])
        self.assertTrue(got["mds"]["vulnerable"])
        recs = self.t.recommendations({"hardening": info})
        rec = [r for r in recs if r["id"] == "cpu-vulnerable"]
        self.assertEqual(len(rec), 1)
        self.assertIn("spectre_v2 (BHI: Vulnerable)", rec[0]["detail"])
        self.assertNotIn("meltdown", rec[0]["detail"])


# --------------------------------------------------------------------------- hardening.modules
class Modules(Base):
    def test_baseline_not_installed(self):
        self.t.BASELINE_MODPROBE = os.path.join(self.tmp, "missing.conf")
        info = self.t.section_hardening()
        self.assertIsNone(info["modules"])
        self.assertIn("not installed", info["modules_reason"])

    def test_shipped_baseline_against_fabricated_proc_modules(self):
        self.t.BASELINE_MODPROBE = os.path.join(ROOT, "security/hardening/modprobe.d/nexus-hardening.conf")
        write(os.path.join(self.t.PROC, "modules"),
              "cramfs 61440 0 - Live 0xffffffffc0a00000\n"
              "n_hdlc 20480 0 - Live 0xffffffffc0b00000\n"
              "ext4 1142784 2 - Live 0xffffffffc0c00000\n")
        info = self.t.section_hardening()
        rows = {r["module"]: r for r in info["modules"]}
        self.assertNotIn("modules_reason", info)
        self.assertEqual(len(rows), 28)                       # every "blacklist" line of the shipped file
        self.assertEqual(rows["cramfs"], {"module": "cramfs", "expected": "blacklisted", "actual": "loaded", "ok": False})
        self.assertEqual(rows["n-hdlc"]["actual"], "loaded")  # n-hdlc in modprobe.d is n_hdlc in the kernel
        self.assertEqual(rows["rds"], {"module": "rds", "expected": "blacklisted", "actual": "not loaded", "ok": True})
        self.assertNotIn("ext4", rows)

    def test_proc_modules_unreadable(self):
        self.t.BASELINE_MODPROBE = os.path.join(self.tmp, "b.conf")
        write(self.t.BASELINE_MODPROBE, "# c\nblacklist rds\n")
        info = self.t.section_hardening()
        self.assertEqual(info["modules"], [{"module": "rds", "expected": "blacklisted", "actual": None, "ok": None}])
        self.assertIn("not readable", info["modules_reason"])


# --------------------------------------------------------------------------- apparmor
class AppArmor(Base):
    def make_proc(self, pid, comm, label, flags=0x00400000, own_file=False):
        base = os.path.join(self.t.PROC, str(pid))
        write(os.path.join(base, "comm"), comm + "\n")
        write(os.path.join(base, "stat"), f"{pid} ({comm}) S 1 {pid} {pid} 0 -1 {flags} 0 0 0 0 0 0\n")
        if label is not None:
            write(os.path.join(base, "attr/apparmor/current" if own_file else "attr/current"), label + "\n")

    def test_profiles_of_every_mode_and_process_list(self):
        write(os.path.join(self.t.SYS, "module/apparmor/parameters/enabled"), "Y\n")
        write(os.path.join(self.t.SYS, "kernel/security/lsm"), "lockdown,capability,landlock,yama,apparmor\n")
        write(os.path.join(self.t.SYS, "kernel/security/apparmor/profiles"),
              "/usr/sbin/cupsd (enforce)\nnexus-terminal (enforce)\n/usr/sbin/dnsmasq (complain)\n"
              "firefox (unconfined)\nrunaway (kill)\nodd one (prompt)\n")
        self.make_proc(702, "cupsd", "/usr/sbin/cupsd (enforce)")
        self.make_proc(1290, "dnsmasq", "/usr/sbin/dnsmasq (complain)", own_file=True)
        self.make_proc(811, "sshd", "unconfined")
        self.make_proc(2, "kthreadd", "unconfined", flags=0x00208040)        # PF_KTHREAD → left out
        self.make_proc(900, "(sd-pam) x", "unconfined")                     # a comm with ") " in it
        self.make_proc(4242, "gone", None)                                  # label unreadable
        write(os.path.join(self.t.PROC, "self/x"), "")                      # non-numeric entries are ignored
        self.fake_run({})
        info = self.t.section_apparmor()
        self.assertEqual(info["profiles"], {"enforce": 2, "complain": 1, "kill": 1, "unconfined": 1, "other": 1})
        self.assertEqual(info["profile_names"], {
            "enforce": ["/usr/sbin/cupsd", "nexus-terminal"], "complain": ["/usr/sbin/dnsmasq"],
            "kill": ["runaway"], "unconfined": ["firefox"], "other": ["odd one"]})
        self.assertEqual(info["processes"], [
            {"process": "cupsd", "pid": 702, "profile": "/usr/sbin/cupsd", "mode": "enforce"},
            {"process": "sshd", "pid": 811, "profile": None, "mode": "unconfined"},
            {"process": "(sd-pam) x", "pid": 900, "profile": None, "mode": "unconfined"},
            {"process": "dnsmasq", "pid": 1290, "profile": "/usr/sbin/dnsmasq", "mode": "complain"}])
        self.assertIn("1 process", info["processes_reason"])
        # every count equals the number of names: the GUI relies on it
        for mode, n in info["profiles"].items():
            self.assertEqual(n, len(info["profile_names"][mode]))

    def test_module_not_loaded_keeps_old_shape(self):
        info = self.t.section_apparmor()
        self.assertEqual(info, {"available": True, "enabled": False,
                                "reason": "The AppArmor kernel module is not loaded."})

    def test_profile_list_unreadable(self):
        write(os.path.join(self.t.SYS, "module/apparmor/parameters/enabled"), "Y\n")
        self.make_proc(702, "cupsd", "/usr/sbin/cupsd (enforce)")
        info = self.t.section_apparmor()
        self.assertIsNone(info["profiles"])
        self.assertIsNone(info["profile_names"])
        self.assertIn("not readable", info["profile_names_reason"])
        self.assertEqual(len(info["processes"]), 1)


# --------------------------------------------------------------------------- encryption.filesystems
class Filesystems(Base):
    def blockdev(self, name, devno, parent=None, slaves=(), dm_uuid=None, partition=False, loop_backing=None):
        """Fabricate /sys/devices/virtual/block/<name> (or <parent>/<name>) and /sys/dev/block/<devno>."""
        base = os.path.join(self.t.SYS, "devices/virtual/block")
        path = os.path.join(base, parent, name) if parent else os.path.join(base, name)
        os.makedirs(path, exist_ok=True)
        write(os.path.join(path, "dev"), devno + "\n")
        if partition:
            write(os.path.join(path, "partition"), "1\n")
        if dm_uuid:
            write(os.path.join(path, "dm/uuid"), dm_uuid + "\n")
        if loop_backing:
            write(os.path.join(path, "loop/backing_file"), loop_backing + "\n")
        if slaves:
            os.makedirs(os.path.join(path, "slaves"), exist_ok=True)
            for s in slaves:
                os.symlink(self.sysdirs[s], os.path.join(path, "slaves", s))
        link_dir = os.path.join(self.t.SYS, "dev/block")
        os.makedirs(link_dir, exist_ok=True)
        os.symlink(path, os.path.join(link_dir, devno))
        self.sysdirs[name] = path

    def setUp(self):
        super().setUp()
        self.sysdirs = {}

    def test_device_stacks(self):
        # nvme0n1 ── p1 (ESP, plain) ── p3 → dm-0 (LUKS2) → dm-1 (LVM root), dm-2 (LVM home)
        # sdb ── sdb1 (plain data);  sdc → dm-3 (plain dm-crypt, no LUKS header) ; md0 (raid of sdd1+sde1) plain
        self.blockdev("nvme0n1", "259:0")
        self.blockdev("nvme0n1p1", "259:1", parent="nvme0n1", partition=True)
        self.blockdev("nvme0n1p3", "259:3", parent="nvme0n1", partition=True)
        self.blockdev("dm-0", "253:0", slaves=["nvme0n1p3"], dm_uuid="CRYPT-LUKS2-0123456789abcdef-nexus_crypt")
        self.blockdev("dm-1", "253:1", slaves=["dm-0"], dm_uuid="LVM-aaaa")
        self.blockdev("dm-2", "253:2", slaves=["dm-0"], dm_uuid="LVM-bbbb")
        self.blockdev("sdb", "8:16")
        self.blockdev("sdb1", "8:17", parent="sdb", partition=True)
        self.blockdev("sdc", "8:32")
        self.blockdev("dm-3", "253:3", slaves=["sdc"], dm_uuid="CRYPT-PLAIN-scratch")
        self.blockdev("sdd", "8:48")
        self.blockdev("sdd1", "8:49", parent="sdd", partition=True)
        self.blockdev("sde", "8:64")
        self.blockdev("sde1", "8:65", parent="sde", partition=True)
        self.blockdev("md0", "9:0", slaves=["sdd1", "sde1"])
        write(os.path.join(self.t.PROC, "self/mountinfo"), "\n".join([
            "22 1 0:21 / /proc rw,nosuid shared:12 - proc proc rw",
            "23 1 0:22 / /sys rw shared:2 - sysfs sysfs rw",
            "28 1 253:1 / / rw,relatime shared:1 - ext4 /dev/mapper/nexus-root rw",
            "30 28 0:25 / /run rw shared:5 - tmpfs tmpfs rw",
            "60 28 253:2 / /home rw shared:30 - ext4 /dev/mapper/nexus-home rw",
            "61 28 259:1 / /boot/efi rw shared:31 - vfat /dev/nvme0n1p1 rw",
            "70 28 8:17 / /media/ada/USB\\040stick rw shared:40 - ext4 /dev/sdb1 rw",
            "71 28 253:3 / /scratch rw shared:41 - xfs /dev/mapper/scratch rw",
            "72 28 9:0 / /srv rw shared:42 - ext4 /dev/md0 rw",
            "73 28 0:50 / /mnt/nas rw shared:43 - nfs4 nas:/export rw",
            "74 28 8:240 / /mnt/ghost rw shared:44 - ext4 /dev/sdp rw",
            "75 28 0:60 / /tank rw shared:45 - zfs tank rw",
        ]) + "\n")
        rows, reason = self.t._mounted_filesystems(), None
        if isinstance(rows, tuple):
            rows, reason = rows
        self.assertIsNone(reason)
        got = {r["mountpoint"]: r for r in rows}
        self.assertEqual(sorted(got), ["/", "/boot/efi", "/home", "/media/ada/USB stick", "/mnt/ghost", "/scratch",
                                       "/srv", "/tank"])
        self.assertEqual(got["/"], {"mountpoint": "/", "device": "/dev/mapper/nexus-root", "fstype": "ext4",
                                    "encrypted": True})
        self.assertIs(got["/home"]["encrypted"], True)          # LVM on LUKS
        self.assertIs(got["/boot/efi"]["encrypted"], False)     # partition straight on the disk
        self.assertIs(got["/media/ada/USB stick"]["encrypted"], False)
        self.assertIs(got["/scratch"]["encrypted"], True)       # plain dm-crypt
        self.assertIs(got["/srv"]["encrypted"], False)          # md raid over partitions
        self.assertIsNone(got["/mnt/ghost"]["encrypted"])       # device unknown to sysfs → not guessed
        self.assertIn("reason", got["/mnt/ghost"])
        self.assertIsNone(got["/tank"]["encrypted"])
        self.assertEqual(got["/tank"]["device"], "tank")

    def test_luks_on_top_of_raid_and_loop_on_unknown_storage(self):
        self.blockdev("sda", "8:0")
        self.blockdev("sdb", "8:16")
        self.blockdev("md1", "9:1", slaves=["sda", "sdb"])
        self.blockdev("dm-0", "253:0", slaves=["md1"], dm_uuid="CRYPT-LUKS1-ffff-data")
        self.blockdev("dm-5", "253:5", slaves=["dm-0"], dm_uuid="LVM-cccc")
        self.blockdev("loop0", "7:0", loop_backing="/nonexistent/image.squashfs")
        write(os.path.join(self.t.PROC, "self/mountinfo"),
              "28 1 253:5 / /data rw - ext4 /dev/mapper/vg-data rw\n"
              "29 1 7:0 / /usr/lib/live/mount/rootfs ro - squashfs /dev/loop0 ro\n")
        got = {r["mountpoint"]: r for r in self.t._mounted_filesystems()}
        self.assertIs(got["/data"]["encrypted"], True)
        self.assertIsNone(got["/usr/lib/live/mount/rootfs"]["encrypted"])
        self.assertIn("reason", got["/usr/lib/live/mount/rootfs"])

    def test_mountinfo_unreadable(self):
        info = {}
        self.t.probe(info, "filesystems", self.t._mounted_filesystems)
        self.assertIsNone(info["filesystems"])
        self.assertIn("mountinfo", info["filesystems_reason"])


# --------------------------------------------------------------------------- secure boot
def pe_image(signed=True, pe32plus=True, cert_type=0x0002, dirs=16):
    """A minimal PE/COFF file; with signed=True it has a certificate table like sbsign writes."""
    dos = bytearray(64)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)
    dirs_at = 112 if pe32plus else 96
    opt = bytearray(dirs_at + dirs * 8)
    struct.pack_into("<H", opt, 0, 0x20B if pe32plus else 0x10B)
    struct.pack_into("<I", opt, dirs_at - 4, dirs)
    coff = struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, len(opt), 0x0022)
    body = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + b"\0" * 200
    if signed:
        cert = struct.pack("<IHH", 8 + 32, 0x0200, cert_type) + b"\x30" * 32
        offset = len(body)
        body = bytearray(body + cert)
        struct.pack_into("<II", body, 64 + 24 + dirs_at + 4 * 8, offset, len(cert))
    return bytes(body)


class SecureBoot(Base):
    def kernel(self, data):
        path = os.path.join(self.t.BOOT, "vmlinuz-" + os.uname().release)
        write(path, data, "wb")
        return path

    def test_pe_parser(self):
        cases = [
            (pe_image(signed=True), True),
            (pe_image(signed=True, pe32plus=False), True),
            (pe_image(signed=False), False),
            (pe_image(signed=False, dirs=4), False),            # header too short to have a certificate table
            (pe_image(signed=True, cert_type=0x0EF1), None),    # table present but not a PKCS#7 signature
            (b"\x7fELF" + b"\0" * 100, False),                  # not an EFI binary at all
            (gzip.compress(b"Image"), None),                    # compressed: cannot look inside
            (pe_image(signed=True)[:-20], None),                # truncated file: table points past the end
        ]
        for n, (data, expected) in enumerate(cases):
            path = os.path.join(self.tmp, f"img{n}")
            write(path, data, "wb")
            signed, reason = self.t.pe_signature_present(path)
            self.assertIs(signed, expected, f"case {n}: {reason}")
            if expected is None:
                self.assertTrue(reason, f"case {n} must give a reason")
        self.assertEqual(self.t.pe_signature_present(os.path.join(self.tmp, "absent"))[0], None)

    def test_bios_system_reports_lockdown_and_kernel(self):
        path = self.kernel(pe_image(signed=True))
        write(os.path.join(self.t.SYS, "kernel/security/lockdown"), "none [integrity] confidentiality\n")
        self.fake_run({})                                       # no sbverify, no mokutil
        info = self.t.section_secureboot()
        self.assertEqual(info["firmware"], "bios")              # old keys, old meaning
        self.assertIs(info["supported"], False)
        self.assertIs(info["enabled"], False)
        self.assertIn("legacy BIOS", info["reason"])
        self.assertEqual(info["kernel_lockdown"], "integrity")
        self.assertEqual(info["kernel_image"], path)
        self.assertIs(info["kernel_signed"], True)
        self.assertEqual(info["kernel_signed_method"], "pe-header")

    def test_uefi_unsigned_kernel_and_no_lockdown_file(self):
        os.makedirs(os.path.join(self.t.SYS, "firmware/efi/efivars"))
        guid = "8be4df61-93ca-11d2-aa0d-00e098032b8c"
        write(os.path.join(self.t.SYS, f"firmware/efi/efivars/SecureBoot-{guid}"), b"\x06\0\0\0\x01", "wb")
        write(os.path.join(self.t.SYS, f"firmware/efi/efivars/SetupMode-{guid}"), b"\x06\0\0\0\x00", "wb")
        self.kernel(pe_image(signed=False))
        self.fake_run({})
        info = self.t.section_secureboot()
        self.assertEqual((info["firmware"], info["enabled"], info["setup_mode"]), ("uefi", True, False))
        self.assertIsNone(info["kernel_lockdown"])
        self.assertIn("lockdown", info["kernel_lockdown_reason"])
        self.assertIs(info["kernel_signed"], False)
        for key in ("supported", "shim_installed", "mok_keys", "boot_loader_refresh_failed"):
            self.assertIn(key, info)

    def test_no_kernel_image(self):
        self.fake_run({})
        info = self.t.section_secureboot()
        self.assertIsNone(info["kernel_image"])
        self.assertIn("No kernel image", info["kernel_image_reason"])
        self.assertIsNone(info["kernel_signed"])
        self.assertTrue(info["kernel_signed_reason"])

    def test_sbverify_output_is_used_when_installed(self):
        path = self.kernel(b"whatever")
        self.fake_run({"sbverify": (0, "signature 1\nimage signature issuers:\n - /CN=NEXUS OS Secure Boot\n", "")})
        info = self.t.section_secureboot()
        self.assertIs(info["kernel_signed"], True)
        self.assertEqual(info["kernel_signed_method"], "sbverify")
        self.assertIn(["sbverify", "--list", path], self.calls)
        self.fake_run({"sbverify": (0, "No signature table present\n", "")})
        self.assertIs(self.t.section_secureboot()["kernel_signed"], False)


# --------------------------------------------------------------------------- updates.last_upgrade
class LastUpgrade(Base):
    def setUp(self):
        super().setUp()
        self.t.APT_HISTORY_GLOB = os.path.join(self.tmp, "apt/history.log*")
        self.t.DPKG_LOG_GLOB = os.path.join(self.tmp, "dpkg.log*")

    def expect(self, local):
        return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.mktime(time.strptime(local, "%Y-%m-%d %H:%M:%S"))))

    def test_no_logs(self):
        value, reason = self.t._last_upgrade()
        self.assertIsNone(value)
        self.assertIn("Neither", reason)

    def test_install_only_history_is_not_an_upgrade(self):
        write(os.path.join(self.tmp, "apt/history.log"),
              "\nStart-Date: 2026-01-05  10:00:00\nCommandline: apt install vim\nInstall: vim:amd64 (2:9.1)\n"
              "End-Date: 2026-01-05  10:00:09\n")
        write(os.path.join(self.tmp, "dpkg.log"), "2026-01-05 10:00:03 install vim:amd64 <none> 2:9.1\n"
                                                 "2026-01-05 10:00:04 status installed vim:amd64 2:9.1\n")
        value, reason = self.t._last_upgrade()
        self.assertIsNone(value)
        self.assertIn("record no package upgrade", reason)

    def test_latest_upgrade_across_rotated_logs(self):
        write(os.path.join(self.tmp, "apt/history.log"),
              "\nStart-Date: 2026-02-09  19:43:10\nCommandline: apt-get dist-upgrade\n"
              "Upgrade: libssl3:amd64 (3.5.1-1, 3.5.1-1+deb13u1)\nEnd-Date: 2026-02-09  19:44:00\n"
              "\nStart-Date: 2026-02-20  08:00:00\nCommandline: apt install htop\nInstall: htop:amd64 (3.4)\n"
              "End-Date: 2026-02-20  08:00:05\n")
        with gzip.open(os.path.join(self.tmp, "apt/history.log.1.gz"), "wt") as fh:
            fh.write("\nStart-Date: 2025-12-01  07:00:00\nUpgrade: bash:amd64 (5.2-1, 5.2-2)\n"
                     "End-Date: 2025-12-01  07:01:00\n")
        self.assertEqual(self.t._last_upgrade(), self.expect("2026-02-09 19:44:00"))
        # dpkg -i of a newer version is only in dpkg's log
        write(os.path.join(self.tmp, "dpkg.log"), "2026-02-11 06:30:00 upgrade nexus-ui:amd64 0.9.0 0.9.1\n"
                                                 "2026-02-11 06:30:01 status installed nexus-ui:amd64 0.9.1\n")
        self.assertEqual(self.t._last_upgrade(), self.expect("2026-02-11 06:30:00"))


# --------------------------------------------------------------------------- audit.recent_events
class Audit(Base):
    def test_keys_come_from_the_shipped_rules(self):
        self.t.AUDIT_RULES = os.path.join(ROOT, "security/audit/50-nexus.rules")
        keys = self.t._audit_keys()
        self.assertEqual(sorted(keys), sorted(self.t.AUDIT_KEYS))
        self.t.AUDIT_RULES = os.path.join(self.tmp, "absent.rules")
        self.assertEqual(self.t._audit_keys(), self.t.AUDIT_KEYS)

    def test_parse_fixture_log(self):
        with open(AUDIT_LOG, encoding="utf-8") as fh:
            events = self.t.parse_audit_events(fh.read(), self.t.AUDIT_KEYS)
        self.assertEqual([(e["key"], e["type"]) for e in events],
                         [("identity", "SYSCALL"), ("modules", "KERN_MODULE"), ("privilege", "SYSCALL")])
        self.assertEqual(events[0]["time"], "2026-03-02T08:50:03Z")
        self.assertIn("/usr/bin/vim.basic", events[0]["summary"])
        self.assertIn("/etc/passwd", events[0]["summary"])
        self.assertNotIn("/etc/,", events[0]["summary"])                 # the PARENT directory record is not listed
        self.assertTrue(events[0]["summary"].endswith("succeeded"))
        self.assertIn("module v4l2loopback", events[1]["summary"])
        self.assertIn("/etc/sudoers.d/evil file", events[2]["summary"])  # hex-encoded name decoded
        self.assertIn("failed (exit -13)", events[2]["summary"])
        for e in events:
            self.assertEqual(sorted(e), ["key", "summary", "time", "type"])

    def test_bounded_to_last_50(self):
        lines = []
        for n in range(120):
            lines.append(f'type=SYSCALL msg=audit({1772000000 + n}.000:{n}): arch=c000003e syscall=257 success=yes '
                         f'exit=3 auid=4294967295 uid=0 comm="x" exe="/usr/bin/x" key="boot"')
        self.fake_run({"ausearch": (0, "\n".join(lines) + "\n", "")})
        info = {}
        self.t.probe(info, "recent_events", self.t._recent_audit_events, empty=[])
        self.assertEqual(len(info["recent_events"]), 50)
        self.assertEqual(info["recent_events"][-1]["time"], self.t.iso_utc(1772000119))
        self.assertNotIn("recent_events_reason", info)
        argv = self.calls[0]
        self.assertEqual(argv[0], "ausearch")
        self.assertIn("--input-logs", argv)

    def test_no_auditd(self):
        self.fake_run({})
        info = self.t.section_audit()
        self.assertEqual(info["recent_events"], [])
        self.assertIn("not installed", info["recent_events_reason"])
        self.assertIs(info["available"], False)                          # as before: auditctl missing

    def test_no_matches_and_errors(self):
        self.fake_run({"ausearch": (1, "", "<no matches>\n")})
        self.assertEqual(self.t._recent_audit_events(), [])
        self.fake_run({"ausearch": (1, "", "Error opening /var/log/audit/audit.log (No such file or directory)\n")})
        events, reason = self.t._recent_audit_events()
        self.assertEqual(events, [])
        self.assertIn("Error opening", reason)

    @unittest.skipUnless(shutil.which("ausearch"), "ausearch is not installed")
    def test_real_ausearch_on_the_fixture_log(self):
        # The fixture's events are moved to "an hour ago" so that the real
        # ausearch, with the arguments the tool really uses (-ts week-ago),
        # returns them; an event of 30 days ago must be left out.
        import re
        with open(AUDIT_LOG, encoding="utf-8") as fh:
            text = fh.read()
        shift = int(time.time()) - 3600 - 1772442300
        text = re.sub(r"audit\((\d+)\.", lambda m: f"audit({int(m.group(1)) + shift}.", text)
        old = int(time.time()) - 30 * 86400
        text = (f'type=SYSCALL msg=audit({old}.000:7): arch=c000003e syscall=257 success=yes exit=3 items=0 ppid=1 '
                f'pid=9 auid=1000 uid=0 gid=0 euid=0 suid=0 fsuid=0 egid=0 sgid=0 fsgid=0 tty=pts0 ses=1 comm="old" '
                f'exe="/usr/bin/old" subj=unconfined key="boot"\n') + text
        log = os.path.join(self.tmp, "audit.log")
        write(log, text)
        events = self.t._recent_audit_events(extra_args=["-if", log])
        self.assertNotIsInstance(events, tuple, events)
        self.assertEqual([e["key"] for e in events], ["identity", "modules", "privilege"])
        self.assertEqual(events[0]["time"], self.t.iso_utc(1772441403 + shift))


# --------------------------------------------------------------------------- firewall + whole program
class Firewall(Base):
    def test_ruleset_fetched_when_the_firewall_tool_lacks_it(self):
        status = {"installed": True, "enabled_at_boot": True, "active": True, "allowed": [], "rule_count": 30}
        ruleset = "table inet nexus_filter {\n}\n"
        self.fake_run({"nexus-firewall": (0, json.dumps(status), ""), "nft": (0, ruleset, "")})
        info = self.t.section_firewall()
        self.assertEqual(info["ruleset"], ruleset)
        self.assertIn(["nft", "list", "table", "inet", "nexus_filter"], self.calls)
        self.assertEqual(info["rule_count"], 30)

    def test_ruleset_of_the_tool_is_passed_through(self):
        status = {"active": False, "ruleset": None, "ruleset_reason": "The firewall table is not loaded.",
                  "allowed_port_count": None}
        self.fake_run({"nexus-firewall": (3, json.dumps(status), "")})
        info = self.t.section_firewall()
        self.assertIsNone(info["ruleset"])
        self.assertEqual(info["ruleset_reason"], "The firewall table is not loaded.")
        self.assertEqual([c[0] for c in self.calls], ["nexus-firewall"])


class WholeProgram(unittest.TestCase):
    def test_runs_and_keeps_every_documented_key(self):
        p = subprocess.run([sys.executable, TOOL], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr.decode())
        report = json.loads(p.stdout)
        for name in ["generated", "privileged", "firewall", "apparmor", "encryption", "secureboot", "updates",
                     "listening", "connections", "services", "logins", "audit", "hardening", "recommendations"]:
            self.assertIn(name, report)
        for section in report.values():
            if isinstance(section, dict):
                self.assertNotIn("internal error", str(section.get("reason", "")), section)
        self.assertIsInstance(report["audit"]["recent_events"], list)
        self.assertIn("modules", report["hardening"])
        self.assertIn("kernel_lockdown", report["secureboot"])
        self.assertIn("kernel_signed", report["secureboot"])

    def test_a_crashing_probe_does_not_take_the_report_down(self):
        t = load_tool()

        def boom():
            raise RuntimeError("probe exploded")
        info = {}
        t.probe(info, "x", boom)
        self.assertIsNone(info["x"])
        self.assertIn("probe exploded", info["x_reason"])


if __name__ == "__main__":
    unittest.main()
