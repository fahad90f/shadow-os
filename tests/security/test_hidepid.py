#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of shadow-harden fs-harden's /proc hidepid with a group for polkitd.

    python3 tests/security/test_hidepid.py [-v]

hidepid=2 hides other users' processes; polkitd runs as an unprivileged user
and needs /proc/<pid> of every process that asks it for an authorisation, so
without help pkexec breaks. fs-harden now mounts /proc with gid= of the group
"proc" and puts polkitd in it, before /proc is remounted, and the boot unit
does the same. Checked here:

  * the unit text (gid, escape hatch) and the parsing of /proc/self/mountinfo;
  * the order of operations, and that nothing is hidden when polkitd could not
    be given access (fake programs, temporary files);
  * migrate-hidepid, which the package's postinst runs: a unit written by an
    older version (hidepid=2 without the group or the escape hatch, pkexec
    broken at every start) is rewritten, the live /proc gets the group, the
    rest of fs-harden is not done, nothing is restarted or remounted with
    --offline, and without the group the unit goes away;
  * with root: the kernel really lets members of the gid= group see every
    process and hides them from everybody else (a private pid and mount
    namespace; nothing on the host changes).

Standard library only; the kernel test needs root, unshare and setpriv.
"""
import configparser
import grp
import importlib.machinery
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HARDEN = os.path.join(ROOT, "distro", "base", "rootfs", "usr", "bin", "shadow-harden")


def load():
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader("shadow_harden_test", HARDEN)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class Group:
    def __init__(self, gid, members):
        self.gr_gid, self.gr_mem = gid, members


class FakeAccounts:
    """grp/pwd and the programs fs-harden runs."""

    def __init__(self, group_exists=False, polkitd=True, fail=None):
        self.groups = {"proc": Group(995, [])} if group_exists else {}
        self.users = {"polkitd"} if polkitd else set()
        self.fail = fail or set()
        self.calls = []

    def getgrnam(self, name):
        if name not in self.groups:
            raise KeyError(name)
        return self.groups[name]

    def getpwnam(self, name):
        if name not in self.users:
            raise KeyError(name)
        return object()

    def run(self, argv):
        self.calls.append(list(argv))
        if argv[0] in self.fail:
            return 1, f"{argv[0]}: failed"
        if argv[:2] == ["groupadd", "--system"]:
            self.groups[argv[2]] = Group(995, [])
        elif argv[:2] == ["gpasswd", "-a"]:
            self.groups[argv[3]].gr_mem.append(argv[2])
        return 0, ""


class Rendering(unittest.TestCase):
    def setUp(self):
        self.h = load()

    def test_unit(self):
        text = self.h.render_hidepid_unit(995)
        unit = configparser.ConfigParser(interpolation=None)
        unit.optionxform = str
        unit.read_string(text)
        self.assertEqual(unit.get("Service", "ExecStart"), "/bin/mount -o remount,hidepid=2,gid=995 /proc")
        self.assertEqual(unit.get("Unit", "ConditionKernelCommandLine"), "!shadow.hidepid=off")
        self.assertEqual(unit.get("Install", "WantedBy"), "sysinit.target")
        self.assertEqual(unit.get("Unit", "DefaultDependencies"), "no")

    def test_mountinfo(self):
        sample = ("22 28 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12 - proc proc rw,hidepid=invisible,gid=995\n"
                  "35 22 0:31 / /proc/sys/fs/binfmt_misc rw,relatime shared:13 - autofs systemd-1 rw,fd=30\n")
        self.assertEqual(self.h.proc_mount_state(sample), {"mounted": True, "hidepid": "invisible", "gid": 995})
        plain = "22 28 0:21 / /proc rw,nosuid shared:12 - proc proc rw\n"
        self.assertEqual(self.h.proc_mount_state(plain), {"mounted": True, "hidepid": "off", "gid": None})
        old = "22 28 0:21 / /proc rw shared:12 - proc proc rw,hidepid=2\n"
        self.assertEqual(self.h.proc_mount_state(old)["hidepid"], "invisible")
        self.assertFalse(self.h.proc_mount_state("")["mounted"])


class FsHarden(unittest.TestCase):
    def setUp(self):
        self.h = load()
        self.tmp = tempfile.mkdtemp(prefix="hidepid-test.")
        self.h.HIDEPID_UNIT = Path(self.tmp, "proc-shadow-hidepid.service")
        self.h.STATE_FILE = Path(self.tmp, "state.json")
        self.h.FSTAB = Path(self.tmp, "fstab")
        self.h.FSTAB.write_text("tmpfs /tmp tmpfs defaults 0 0\ntmpfs /dev/shm tmpfs defaults 0 0\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def harden(self, fake):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(self.h, "_run", fake.run), \
                mock.patch.object(self.h.grp, "getgrnam", fake.getgrnam), \
                mock.patch.object(self.h.pwd, "getpwnam", fake.getpwnam), \
                mock.patch.object(self.h.Path, "is_mount", lambda path: str(path) == "/proc"), \
                redirect_stdout(out), redirect_stderr(err):
            try:
                self.h.cmd_fs_harden()
                code = 0
            except SystemExit as exc:
                code = exc.code
        return code, out.getvalue(), err.getvalue()

    def test_order(self):
        fake = FakeAccounts()
        code, out, err = self.harden(fake)
        self.assertEqual(code, 0, err)
        self.assertEqual(fake.calls, [
            ["groupadd", "--system", "proc"],
            ["gpasswd", "-a", "polkitd", "proc"],
            ["systemctl", "try-restart", "polkit.service"],
            ["mount", "-o", "remount,hidepid=2,gid=995", "/proc"],
            ["systemctl", "daemon-reload"],
            ["systemctl", "enable", "proc-shadow-hidepid.service"],
        ])
        self.assertIn("hidepid=2,gid=995", self.h.HIDEPID_UNIT.read_text())

    def test_second_run_changes_nothing(self):
        fake = FakeAccounts(group_exists=True)
        fake.groups["proc"].gr_mem.append("polkitd")
        self.h.HIDEPID_UNIT.write_text(self.h.render_hidepid_unit(995))
        code, _, err = self.harden(fake)
        self.assertEqual(code, 0, err)
        self.assertEqual(fake.calls, [["mount", "-o", "remount,hidepid=2,gid=995", "/proc"]])

    def test_old_unit_without_gid_is_replaced(self):
        fake = FakeAccounts(group_exists=True)
        self.h.HIDEPID_UNIT.write_text("[Service]\nExecStart=/bin/mount -o remount,hidepid=2 /proc\n")
        code, _, err = self.harden(fake)
        self.assertEqual(code, 0, err)
        self.assertIn("gid=995", self.h.HIDEPID_UNIT.read_text())

    def test_nothing_hidden_when_polkitd_cannot_join(self):
        fake = FakeAccounts(fail={"gpasswd"})
        code, _, err = self.harden(fake)
        self.assertEqual(code, 1)
        self.assertIn("NOT applied: hidepid", err)
        self.assertFalse(any(call[0] == "mount" for call in fake.calls))
        self.assertFalse(self.h.HIDEPID_UNIT.exists())

    def test_nothing_hidden_when_polkit_does_not_restart(self):
        fake = FakeAccounts(fail={"systemctl"})
        code, _, _ = self.harden(fake)
        self.assertEqual(code, 1)
        self.assertFalse(any(call[0] == "mount" for call in fake.calls))

    def test_without_polkit(self):
        fake = FakeAccounts(polkitd=False)
        code, out, err = self.harden(fake)
        self.assertEqual(code, 0, err)
        self.assertIn(["mount", "-o", "remount,hidepid=2,gid=995", "/proc"], fake.calls)

    def test_status(self):
        mountinfo = Path(self.tmp, "mountinfo")
        mountinfo.write_text("22 28 0:21 / /proc rw shared:12 - proc proc rw,hidepid=invisible,gid=995\n")
        fake = FakeAccounts(group_exists=True)
        with mock.patch.object(self.h, "MOUNTINFO", mountinfo), mock.patch.object(self.h.grp, "getgrnam", fake.getgrnam):
            state = self.h.hidepid_status()
            self.assertFalse(state["polkit_can_see_processes"], "polkitd is not in the group yet")
            fake.groups["proc"].gr_mem.append("polkitd")
            state = self.h.hidepid_status()
        self.assertEqual((state["hidepid"], state["gid"], state["group_gid"]), ("invisible", 995, 995))
        self.assertTrue(state["polkit_can_see_processes"])


OLD_UNIT = ("[Unit]\nDescription=Shadow OS: remount /proc with hidepid=2\nAfter=systemd-remount-fs.service\n"
            "DefaultDependencies=no\n\n[Service]\nType=oneshot\nExecStart=/bin/mount -o remount,hidepid=2 /proc\n"
            "RemainAfterExit=yes\n\n[Install]\nWantedBy=sysinit.target\n")     # shadow-harden of 0.2.0


class MigrateHidepid(unittest.TestCase):
    """shadow-harden migrate-hidepid (debian/shadow-hardening.postinst)."""

    def setUp(self):
        self.h = load()
        self.tmp = tempfile.mkdtemp(prefix="hidepid-migrate.")
        self.h.HIDEPID_UNIT = Path(self.tmp, "proc-shadow-hidepid.service")
        self.h.STATE_FILE = Path(self.tmp, "state.json")
        self.h.FSTAB = Path(self.tmp, "fstab")
        self.h.FSTAB.write_text("")
        self.h.MOUNTINFO = Path(self.tmp, "mountinfo")
        self.mounted("rw,hidepid=invisible")       # what the 0.2.0 unit mounted

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def mounted(self, options):
        self.h.MOUNTINFO.write_text(f"22 28 0:21 / /proc rw,nosuid shared:12 - proc proc {options}\n")

    def migrate(self, fake, *options):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(self.h, "_run", fake.run), \
                mock.patch.object(self.h.grp, "getgrnam", fake.getgrnam), \
                mock.patch.object(self.h.pwd, "getpwnam", fake.getpwnam), \
                mock.patch.object(self.h.sys, "argv", ["shadow-harden", "migrate-hidepid", *options]), \
                redirect_stdout(out), redirect_stderr(err):
            try:
                self.h.COMMANDS["migrate-hidepid"]()
                code = 0
            except SystemExit as exc:
                code = exc.code
        return code, out.getvalue(), err.getvalue()

    def test_old_unit_rewritten_and_proc_fixed_now(self):
        self.h.HIDEPID_UNIT.write_text(OLD_UNIT)
        fake = FakeAccounts()
        code, _, err = self.migrate(fake)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.h.HIDEPID_UNIT.read_text(), self.h.render_hidepid_unit(995))
        self.assertEqual(fake.calls, [
            ["groupadd", "--system", "proc"],
            ["gpasswd", "-a", "polkitd", "proc"],
            ["systemctl", "try-restart", "polkit.service"],
            ["systemctl", "daemon-reload"],
            ["mount", "-o", "remount,hidepid=2,gid=995", "/proc"],
        ])
        self.assertEqual(self.h.FSTAB.read_text(), "", "only the /proc part of fs-harden")
        self.assertFalse(any("enable" in call for call in fake.calls), "a disabled unit stays disabled")

    def test_offline_changes_files_only(self):
        self.h.HIDEPID_UNIT.write_text(OLD_UNIT)
        fake = FakeAccounts()
        code, _, err = self.migrate(fake, "--offline")
        self.assertEqual(code, 0, err)
        self.assertIn("ConditionKernelCommandLine=!shadow.hidepid=off", self.h.HIDEPID_UNIT.read_text())
        self.assertEqual(fake.calls, [["groupadd", "--system", "proc"], ["gpasswd", "-a", "polkitd", "proc"]])

    def test_proc_without_hidepid_is_not_remounted(self):
        self.h.HIDEPID_UNIT.write_text(OLD_UNIT)
        self.mounted("rw")
        fake = FakeAccounts(group_exists=True)
        fake.groups["proc"].gr_mem.append("polkitd")
        code, _, err = self.migrate(fake)
        self.assertEqual(code, 0, err)
        self.assertEqual(fake.calls, [["systemctl", "daemon-reload"]])

    def test_current_unit_and_no_unit_change_nothing(self):
        fake = FakeAccounts(group_exists=True)
        fake.groups["proc"].gr_mem.append("polkitd")
        code, _, err = self.migrate(fake)
        self.assertEqual((code, fake.calls), (0, []), err)
        self.assertFalse(self.h.HIDEPID_UNIT.exists())
        self.h.HIDEPID_UNIT.write_text(self.h.render_hidepid_unit(995))
        self.mounted("rw,hidepid=invisible,gid=995")
        code, _, err = self.migrate(fake)
        self.assertEqual((code, fake.calls), (0, []), err)

    def test_without_the_group_nothing_stays_hidden(self):
        self.h.HIDEPID_UNIT.write_text(OLD_UNIT)
        fake = FakeAccounts(fail={"gpasswd"})
        code, _, err = self.migrate(fake)
        self.assertEqual(code, 1)
        self.assertIn("removed so that pkexec keeps working", err)
        self.assertFalse(self.h.HIDEPID_UNIT.exists())
        self.assertIn(["systemctl", "disable", "proc-shadow-hidepid.service"], fake.calls)
        self.assertIn(["mount", "-o", "remount,hidepid=0", "/proc"], fake.calls)


@unittest.skipUnless(os.geteuid() == 0 and shutil.which("unshare") and shutil.which("setpriv"),
                     "needs root, unshare and setpriv")
class Kernel(unittest.TestCase):
    """hidepid=2,gid=G in a private pid and mount namespace."""

    def test_gid_sees_everything(self):
        gid = 4242
        while True:
            try:
                grp.getgrgid(gid)
                gid += 1
            except KeyError:
                break
        script = f"""
set -eu
d=$(mktemp -d)
mount -t proc -o hidepid=2,gid={gid} proc "$d"
sleep 30 & victim=$!
others=$(setpriv --reuid=65534 --regid=65534 --clear-groups ls "$d" | grep -cx "$victim" || true)
members=$(setpriv --reuid=65534 --regid=65534 --groups={gid} ls "$d" | grep -cx "$victim" || true)
own=$(setpriv --reuid=65534 --regid=65534 --clear-groups sh -c 'ls "$1" | grep -cx "$$"' sh "$d" || true)
kill "$victim"
umount "$d"; rmdir "$d"
echo "$others $members $own"
"""
        proc = subprocess.run(["unshare", "--pid", "--mount", "--propagation", "private", "--fork", "--mount-proc",
                               "sh", "-c", script], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        others, members, own = proc.stdout.split()
        self.assertEqual(others, "0", "a user outside the group must not see root's process")
        self.assertEqual(members, "1", "a member of the gid= group must see it")
        self.assertEqual(own, "1", "positive control: everybody sees their own process")


if __name__ == "__main__":
    unittest.main()
