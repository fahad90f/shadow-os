#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests of the per-application firewall (security/firewall/shadow-appfw*).

    python3 tests/security/test_appfw_unit.py [-v]

shadow-appfw is loaded with every path constant pointed at a temporary
directory and its run() replaced by FakeSystem, which keeps the nftables
table, the control groups and systemd's transient scopes in memory. These
tests prove the logic: what is written for a blocked application and what is
never touched, when the rule is loaded, that "confined" is only reported when
the (fake) kernel has both the process in the slice and the slice in the
rule, who may confine which process, and that every failure keeps a blocked
application from starting. Real packets and the real nft are in
tests/network/test_appfw_netns.py.

Also checked here: shadow-appfw-run with a stand-in pkexec, the polkit
actions, the unit, the watchdog and the packaging lists.

Standard library only; runs as any user, changes nothing outside the
temporary directory.
"""
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, "security", "firewall")
TOOL = os.path.join(FW, "shadow-appfw")
RUN = os.path.join(FW, "shadow-appfw-run")
ENTER = os.path.join(FW, "shadow-appfw-enter")
UID = 1000

BROWSER = """[Desktop Entry]
Type=Application
Name=Browser
Name[de]=Netzbetrachter
Icon=browser
TryExec=browser
Exec=browser --profile "My Profile" %U
DBusActivatable=true
MimeType=text/html;
Actions=private;

[Desktop Action private]
Name=Private window
Exec=browser --private %u
"""


def load():
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader("shadow_appfw", TOOL)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


class FakeSystem:
    """nft, busctl and update-desktop-database, in memory and in the temporary directories."""

    def __init__(self, tool):
        self.tool = tool
        self.calls = []
        self.loaded = False        # the table is in the kernel
        self.element = None        # generation of the slice the set holds, or None
        self.generation = 0        # bumped whenever the slice is created anew
        self.packets = (0, 0)
        self.load_lies = False     # nft says 0 but the table is not there afterwards
        self.check_fails = False   # nft --check rejects the table
        self.element_lies = False  # the same for the set
        self.busctl_rc = 0
        self.no_move = False       # systemd accepts the scope but never moves the process
        self.scopes = []           # [(name, {property: value})]

    def slice_dir(self):
        return os.path.join(self.tool.CGROUP_ROOT, self.tool.SLICE)

    def make_slice(self):
        if not os.path.isdir(self.slice_dir()):
            os.makedirs(self.slice_dir())
            self.generation += 1

    def drop_slice(self):
        shutil.rmtree(self.slice_dir(), True)

    def __call__(self, argv, check=True, timeout=60, stdin=None):
        self.calls.append((list(argv), stdin))
        rc, out, err = self.answer(list(argv), stdin)
        if check and rc != 0:
            raise self.tool.Failure(f"{argv[0]} failed: {(err or out).strip()}")
        return rc, out, err

    def answer(self, argv, stdin):
        if argv[0] == "update-desktop-database":
            return 0, "", ""
        if argv[0] == "busctl":
            return self.busctl(argv)
        assert argv[0] == "nft", argv
        if argv[1:3] == ["list", "table"]:
            return (0, self.tool.render(), "") if self.loaded else (1, "", "Error: No such file or directory")
        if argv[1:3] == ["list", "set"]:
            if not self.loaded:
                return 1, "", "Error: No such file or directory"
            if self.element is None:
                return 0, "set blocked {\n\ttype cgroupsv2\n}\n", ""
            shown = f'"{self.tool.SLICE}"' if (self.element == self.generation
                                              and os.path.isdir(self.slice_dir())) else "4242"
            return 0, f"set blocked {{\n\ttype cgroupsv2\n\telements = {{ {shown} }}\n}}\n", ""
        if argv[1:3] == ["list", "chain"]:
            if not self.loaded:
                return 1, "", "Error: No such file or directory"
            return 0, (f"counter packets {self.packets[0]} bytes 0 reject with tcp reset\n"
                       f"counter packets {self.packets[1]} bytes 0 reject\n"), ""
        if argv[1:] == ["--check", "-f", "-"]:
            return (1, "", "Error: syntax error") if self.check_fails else (0, "", "")
        if argv[1:] == ["-f", "-"]:
            if "delete table" in stdin:
                self.loaded, self.element = False, None
            if "chain output" in stdin and not self.load_lies:
                self.loaded = True
            if "flush set" in stdin:
                if not self.loaded:
                    return 1, "", "Error: No such file or directory"
                self.element = None
            if "add element" in stdin and not self.element_lies:
                if not os.path.isdir(self.slice_dir()):
                    return 1, "", "Error: cgroupv2 path fails: No such file or directory"
                self.element = self.generation
            return 0, "", ""
        raise AssertionError(f"unexpected nft call {argv}")

    def busctl(self, argv):
        if self.busctl_rc:
            return self.busctl_rc, "", "Call failed: Access denied"
        assert argv[1:7] == ["call", "--system", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                             "org.freedesktop.systemd1.Manager", "StartTransientUnit"], argv
        assert argv[7] == "ssa(sv)a(sa(sv))" and argv[9] == "fail", argv
        name, count, rest = argv[8], int(argv[10]), argv[11:]
        properties = {}
        for _ in range(count):
            key, signature = rest[0], rest[1]
            if signature in ("au", "as"):
                length = int(rest[2])
                properties[key], rest = rest[3:3 + length], rest[3 + length:]
            else:
                properties[key], rest = rest[2], rest[3:]
        assert rest == ["0"], rest
        self.scopes.append((name, properties))
        if not self.no_move:
            self.make_slice()
            pid = properties["PIDs"][0]
            write(os.path.join(self.slice_dir(), name, "cgroup.procs"), pid + "\n")
            write(os.path.join(self.tool.PROC, pid, "cgroup"), f"0::/{self.tool.SLICE}/{name}\n")
        return 0, 'o "/org/freedesktop/systemd1/job/1"\n', ""

    def nft_loads(self):
        return [stdin for argv, stdin in self.calls if argv[:1] == ["nft"] and argv[1:] == ["-f", "-"]]


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="appfw-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.t = load()
        t = self.t
        t.CONFIG = os.path.join(self.dir, "etc/shadow-os/appfw.json")
        t.RUN_DIR = os.path.join(self.dir, "run/shadow")
        t.CMDLINE = os.path.join(self.dir, "proc/cmdline")
        t.PROC = os.path.join(self.dir, "proc")
        t.CGROUP_ROOT = os.path.join(self.dir, "cgroup")
        t.SYSTEMD_RUNNING = os.path.join(self.dir, "run/systemd/system")
        t.SOURCE_DIRS = [os.path.join(self.dir, "usr/share/applications"),
                         os.path.join(self.dir, "flatpak/applications")]
        t.OVERRIDE_DIR = os.path.join(self.dir, "usr/local/share/applications")
        t.ENTER_WAIT = 0.2
        os.makedirs(t.SYSTEMD_RUNNING)
        os.makedirs(t.CGROUP_ROOT)
        write(t.CMDLINE, "BOOT_IMAGE=/vmlinuz quiet\n")
        self.system = FakeSystem(t)
        t.run = self.system
        self.owners = {}
        t.owner_of = lambda pid: self.owners.get(pid)
        self.euid = 0
        patcher = mock.patch.object(t.os, "geteuid", lambda: self.euid)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.app("browser", BROWSER)
        self.app("org.shadow.trading", "[Desktop Entry]\nType=Application\nName=Shadow Trading\nExec=shadow-trading\n")

    def app(self, name, text, directory=0):
        write(os.path.join(self.t.SOURCE_DIRS[directory], name + ".desktop"), text)

    def override(self, name):
        return os.path.join(self.t.OVERRIDE_DIR, name + ".desktop")

    def process(self, pid, parent=1, owner=UID, cgroup="/user.slice/user-1000.slice/session-2.scope"):
        write(os.path.join(self.t.PROC, str(pid), "stat"), f"{pid} (some (odd) name) S {parent} {pid} {pid} 0 -1\n")
        write(os.path.join(self.t.PROC, str(pid), "cgroup"), f"0::{cgroup}\n")
        self.owners[pid] = owner

    def call(self, *argv, env=None):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), mock.patch.dict(os.environ, env or {}, clear=False):
            if env is None:
                os.environ.pop("PKEXEC_UID", None)
            try:
                rc = self.t.main(list(argv))
            except SystemExit as stop:       # the argument parser's own refusal
                rc = stop.code
        return rc, out.getvalue(), err.getvalue()

    def config(self):
        return json.loads(read(self.t.CONFIG))


class Configuration(Base):
    def test_nothing_is_blocked_at_first(self):
        rc, out, _ = self.call("status", "--json")
        status = json.loads(out)
        self.assertEqual((rc, status["blocked"], status["running"], status["loaded"]), (0, [], [], False))
        self.assertEqual(self.system.nft_loads(), [])

    def test_deny_and_allow(self):
        rc, out, err = self.call("set", "browser", "deny")
        self.assertEqual(rc, 0, err)
        self.assertIn("from the next time it is started", out)
        self.assertEqual(self.config(), {"version": 1, "apps": {"browser": {"mode": "deny"}}})
        self.assertEqual(stat.S_IMODE(os.stat(self.t.CONFIG).st_mode), 0o644)  # the settings read it as the user
        self.assertTrue(os.path.exists(self.override("browser")))
        self.assertTrue(self.system.loaded)
        rc, _, err = self.call("set", "browser", "allow")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.config()["apps"], {})
        self.assertFalse(os.path.exists(self.override("browser")))

    def test_only_installed_applications_with_a_sane_name(self):
        for name in ("nosuchapp", "../etc/passwd", "a b", ""):
            rc, _, err = self.call("set", name, "deny")
            self.assertEqual(rc, 1, name)
            self.assertIn("shadow-appfw:", err)
        self.assertEqual(self.call("set", "-o", "deny")[0], 2)   # an option, not a name
        self.assertFalse(os.path.exists(self.t.CONFIG))
        self.assertEqual(os.listdir(self.dir + "/usr/local") if os.path.isdir(self.dir + "/usr/local") else [], [])

    def test_nothing_is_recorded_where_the_rule_cannot_be_loaded(self):
        self.system.check_fails = True
        rc, _, err = self.call("set", "browser", "deny")
        self.assertEqual(rc, 1)
        self.assertIn("nft failed", err)
        self.assertFalse(os.path.exists(self.t.CONFIG))
        self.assertFalse(os.path.exists(self.override("browser")))
        # Allowing again never depends on the rule.
        self.system.check_fails = False
        self.call("set", "browser", "deny")
        self.system.check_fails = True
        self.system.loaded = False
        self.assertEqual(self.call("set", "browser", "allow")[0], 0)
        self.assertFalse(os.path.exists(self.override("browser")))

    def test_changing_needs_root(self):
        self.euid = 1000
        for argv in (("set", "browser", "deny"), ("sync",), ("boot",), ("reassert",), ("enter", "5", "browser")):
            rc, _, err = self.call(*argv)
            self.assertEqual(rc, 1, argv)
            self.assertIn("needs root", err)
        self.assertEqual(self.system.calls, [])

    def test_an_unreadable_configuration_is_said_and_never_guessed(self):
        for text in ("{not json", '{"apps": []}', '{"apps": {"browser": {"mode": "maybe"}}}',
                     '{"apps": {"../x": {"mode": "deny"}}}'):
            write(self.t.CONFIG, text)
            rc, out, _ = self.call("status", "--json")
            status = json.loads(out)
            self.assertEqual(status["blocked"], [])
            self.assertIn("cannot be read", status["error"])
            rc, _, err = self.call("set", "browser", "deny")
            self.assertEqual(rc, 1)
            self.assertIn("cannot be read", err)
            self.assertEqual(read(self.t.CONFIG), text)  # not overwritten

    def test_the_escape_hatch_loads_nothing(self):
        write(self.t.CMDLINE, "quiet shadow.appfw=off\n")
        rc, _, err = self.call("set", "browser", "deny")
        self.assertEqual(rc, 0, err)
        self.assertFalse(self.system.loaded)
        status = json.loads(self.call("status", "--json")[1])
        self.assertEqual((status["enabled"], status["blocked"]), (False, ["browser"]))
        # "shadow.appfw=offline" or a word inside another is not the hatch.
        write(self.t.CMDLINE, "quiet xshadow.appfw=off shadow.appfw=offline\n")
        self.assertTrue(json.loads(self.call("status", "--json")[1])["enabled"])


class LauncherEntries(Base):
    def test_the_entry_of_a_blocked_application(self):
        self.call("set", "browser", "deny")
        text = read(self.override("browser"))
        wrapper = self.t.RUN_WRAPPER
        self.assertIn(f'Exec={wrapper} browser -- browser --profile "My Profile" %U\n', text)
        self.assertIn(f"Exec={wrapper} browser -- browser --private %u\n", text)  # the action too
        self.assertEqual(text.count("Exec="), 3)   # two Exec lines and TryExec, which stays
        self.assertIn("TryExec=browser\n", text)
        self.assertIn("DBusActivatable=false\n", text)
        self.assertNotIn("DBusActivatable=true", text)
        self.assertIn("X-Shadow-AppFw=deny\n", text)
        self.assertIn("X-Shadow-AppFw-Source=" + os.path.join(self.t.SOURCE_DIRS[0], "browser.desktop") + "\n", text)
        for kept in ("Name=Browser\n", "Name[de]=Netzbetrachter\n", "MimeType=text/html;\n", "Actions=private;\n",
                     "[Desktop Action private]\n", "Name=Private window\n"):
            self.assertIn(kept, text)
        # The mark is inside [Desktop Entry], where launchers and this tool look.
        self.assertLess(text.index("[Desktop Entry]"), text.index("X-Shadow-AppFw=deny"))
        self.assertLess(text.index("X-Shadow-AppFw=deny"), text.index("[Desktop Action private]"))
        self.assertEqual(stat.S_IMODE(os.stat(self.override("browser")).st_mode), 0o644)

    def test_writing_again_changes_nothing(self):
        self.call("set", "browser", "deny")
        before = os.stat(self.override("browser")).st_mtime_ns
        updates = [c for c, _ in self.system.calls if c[0] == "update-desktop-database"]
        self.assertEqual(len(updates), 1)
        self.assertEqual(self.call("sync")[0], 0)
        self.assertEqual(os.stat(self.override("browser")).st_mtime_ns, before)
        self.assertEqual(len([c for c, _ in self.system.calls if c[0] == "update-desktop-database"]), 1)

    def test_an_updated_application_gets_its_entry_written_again(self):
        self.call("set", "browser", "deny")
        self.app("browser", BROWSER.replace("--profile", "--new-option --profile"))
        self.assertEqual(self.call("sync")[0], 0)
        self.assertIn("-- browser --new-option --profile", read(self.override("browser")))

    def test_a_removed_application_loses_its_entry_but_stays_blocked(self):
        self.call("set", "browser", "deny")
        os.unlink(os.path.join(self.t.SOURCE_DIRS[0], "browser.desktop"))
        rc, _, err = self.call("sync")
        self.assertEqual(rc, 0)
        self.assertIn("browser is blocked but not installed", err)
        self.assertFalse(os.path.exists(self.override("browser")))
        self.assertEqual(list(self.config()["apps"]), ["browser"])
        rows = {r["id"]: r for r in json.loads(self.call("list", "--json")[1])["applications"]}
        self.assertEqual((rows["browser"]["mode"], rows["browser"]["missing"]), ("deny", True))
        self.assertEqual(self.call("set", "browser", "allow")[0], 0)  # can be allowed again

    def test_somebody_elses_entry_is_never_touched(self):
        own = "[Desktop Entry]\nType=Application\nName=My browser\nExec=browser --mine\n"
        write(self.override("browser"), own)
        write(os.path.join(self.t.OVERRIDE_DIR, "notes.desktop"), own)
        rc, _, err = self.call("set", "browser", "deny")
        self.assertEqual(rc, 0)
        self.assertIn("NOT confined", err)
        self.assertEqual(read(self.override("browser")), own)
        status = json.loads(self.call("status", "--json")[1])
        self.assertEqual(status["not_confined"], ["browser"])
        self.call("set", "browser", "allow")
        self.assertEqual(read(self.override("browser")), own)
        self.assertEqual(read(os.path.join(self.t.OVERRIDE_DIR, "notes.desktop")), own)

    def test_which_entries_count_as_applications(self):
        self.app("hidden", "[Desktop Entry]\nType=Application\nName=H\nExec=h\nHidden=true\n")
        self.app("link", "[Desktop Entry]\nType=Link\nName=L\nURL=https://example.invalid\n")
        self.app("noexec", "[Desktop Entry]\nType=Application\nName=N\n")
        self.app("background", "[Desktop Entry]\nType=Application\nName=B\nExec=b\nNoDisplay=true\n")
        self.app("bad name", "[Desktop Entry]\nType=Application\nName=X\nExec=x\n")
        self.app("browser", "[Desktop Entry]\nType=Application\nName=Other browser\nExec=other\n", directory=1)
        self.app("flat", "[Desktop Entry]\nType=Application\nName=Flat\nExec=flatpak run flat\n", directory=1)
        rows = {r["id"]: r for r in json.loads(self.call("list", "--json")[1])["applications"]}
        self.assertEqual(sorted(rows), ["background", "browser", "flat", "org.shadow.trading"])
        self.assertEqual(rows["browser"]["name"], "Browser")       # the first directory wins
        self.assertTrue(rows["background"]["hidden"])
        self.assertEqual({r["mode"] for r in rows.values()}, {"allow"})

    def test_the_run_wrapper_is_the_installed_program(self):
        self.assertEqual(self.t.RUN_WRAPPER, "/usr/bin/shadow-appfw-run")
        self.assertEqual(self.t.OVERRIDE_DIR.replace(self.dir, ""), "/usr/local/share/applications")


class Rule(Base):
    def test_the_rule(self):
        text = self.t.render()
        self.assertIn("table inet shadow_appfw {", text)
        self.assertIn("type cgroupsv2", text)
        self.assertIn("type filter hook output priority -40; policy accept;", text)
        rules = [line.strip() for line in text.splitlines() if "@blocked" in line]
        self.assertEqual(rules, ["meta l4proto tcp socket cgroupv2 level 1 @blocked counter reject with tcp reset",
                                 "socket cgroupv2 level 1 @blocked counter reject with icmpx admin-prohibited"])
        # Loopback is not excepted: a local proxy or resolver would carry the traffic out.
        self.assertNotIn("lo", re.findall(r"[a-z]+", text.replace("level", "").replace("blocked", "")
                                          .replace("policy", "").replace("l4proto", "")))

    def test_the_slice_joins_the_set_once_it_exists(self):
        self.t.ensure_rule()
        self.assertTrue(self.system.loaded)
        self.assertIsNone(self.system.element)        # no slice yet: nothing to name
        self.system.make_slice()
        self.t.ensure_rule()
        self.assertEqual(self.system.element, self.system.generation)
        loads = len(self.system.nft_loads())
        self.t.ensure_rule()
        self.assertEqual(len(self.system.nft_loads()), loads)   # nothing to do twice

    def test_a_slice_that_came_back_replaces_the_stale_one(self):
        self.system.make_slice()
        self.t.ensure_rule()
        self.system.drop_slice()
        self.system.make_slice()                      # a new control group under the old name
        self.assertFalse(self.t.slice_in_set())
        self.t.ensure_rule()
        self.assertTrue(self.t.slice_in_set())
        self.assertIn("flush set inet shadow_appfw blocked", self.system.nft_loads()[-1])

    def test_a_kernel_that_does_not_take_it_is_an_error(self):
        self.system.check_fails = True              # checked before anything is loaded
        with self.assertRaises(self.t.Failure):
            self.t.ensure_rule()
        self.assertEqual(self.system.nft_loads(), [])
        self.system.check_fails = False
        self.system.load_lies = True
        with self.assertRaises(self.t.Failure):
            self.t.ensure_rule()
        self.system.load_lies = False
        self.system.element_lies = True
        self.system.make_slice()
        with self.assertRaises(self.t.Failure):
            self.t.ensure_rule()

    def test_the_watchdog_puts_a_missing_rule_back_only_when_something_runs(self):
        self.assertEqual(self.call("reassert")[0], 0)
        self.assertEqual(self.system.calls, [])       # nothing confined: nothing to guard
        self.system.make_slice()
        rc, _, err = self.call("reassert")
        self.assertEqual(rc, 0)
        self.assertIn("put back", err)
        self.assertTrue(self.system.loaded and self.t.slice_in_set())
        self.assertEqual(self.call("reassert")[2], "")  # in place: silent
        write(self.t.CMDLINE, "shadow.appfw=off\n")
        self.system.loaded = False
        self.call("reassert")
        self.assertFalse(self.system.loaded)

    def test_boot(self):
        self.assertEqual(self.call("boot")[0], 0)
        self.assertFalse(self.system.loaded)          # nothing blocked: no rule
        self.call("set", "browser", "deny")
        self.system.loaded = False
        os.unlink(self.override("browser"))
        self.assertEqual(self.call("boot")[0], 0)
        self.assertTrue(self.system.loaded)
        self.assertTrue(os.path.exists(self.override("browser")))
        # An unreadable file: an error, and whatever runs in the slice stays cut off.
        write(self.t.CONFIG, "{")
        self.system.loaded = False
        self.system.make_slice()
        rc, _, err = self.call("boot")
        self.assertEqual(rc, 1)
        self.assertIn("cannot be read", err)
        self.assertTrue(self.system.loaded and self.t.slice_in_set())


class Enter(Base):
    def setUp(self):
        super().setUp()
        self.call("set", "browser", "deny")
        self.system.calls.clear()
        self.process(500, parent=1)                    # the launcher
        self.process(600, parent=500)                  # shadow-appfw-run
        patcher = mock.patch.object(self.t.os, "getppid", lambda: 600)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.env = {"PKEXEC_UID": str(UID)}

    def test_a_blocked_application_is_confined(self):
        rc, _, err = self.call("enter", "600", "browser", env=self.env)
        self.assertEqual(rc, 0, err)
        (name, properties), = self.system.scopes
        self.assertEqual(name, "shadowblock-browser-600.scope")
        self.assertEqual(properties["PIDs"], ["600"])
        self.assertEqual(properties["Slice"], "shadowblock.slice")
        # Outside the user's session now: it must end with it.
        self.assertEqual(properties["BindsTo"], [f"user@{UID}.service"])
        self.assertEqual(properties["After"], [f"user@{UID}.service"])
        self.assertTrue(self.t.cgroup_of(600).startswith("/shadowblock.slice/"))
        self.assertTrue(self.system.loaded and self.t.slice_in_set())
        status = json.loads(read(os.path.join(self.t.RUN_DIR, "appfw.json")))
        self.assertEqual(status["running"], [{"scope": name, "app": "browser", "processes": 1}])

    def test_an_odd_name_becomes_a_valid_unit_name(self):
        self.app("org.x.App+beta-2", "[Desktop Entry]\nType=Application\nName=X\nExec=x\n")
        self.call("set", "org.x.App+beta-2", "deny")
        self.assertEqual(self.call("enter", "600", "org.x.App+beta-2", env=self.env)[0], 0)
        self.assertEqual(self.system.scopes[-1][0], "shadowblock-org.x.App_beta_2-600.scope")

    def test_an_application_that_is_not_blocked_starts_normally(self):
        rc, _, _ = self.call("enter", "600", "org.shadow.trading", env=self.env)
        self.assertEqual(rc, self.t.NOT_BLOCKED)
        self.assertEqual(self.system.scopes, [])
        write(self.t.CMDLINE, "shadow.appfw=off\n")
        self.assertEqual(self.call("enter", "600", "browser", env=self.env)[0], self.t.NOT_BLOCKED)
        self.assertEqual(self.system.scopes, [])

    def test_only_the_callers_own_starting_process(self):
        self.process(700, parent=1)                        # the caller's, but unrelated
        self.process(800, parent=600, owner=1001)          # somebody else's
        self.process(1, parent=0, owner=0)
        for pid, why in ((700, "not the one that asked"), (800, "does not belong"), (1, "does not belong"),
                         (999, "does not belong")):
            rc, _, err = self.call("enter", str(pid), "browser", env=self.env)
            self.assertEqual(rc, 1, pid)
            self.assertIn(why, err)
        self.assertEqual(self.system.scopes, [])
        # The process that started the asking one is accepted too (a shell in between).
        self.assertEqual(self.call("enter", "500", "browser", env=self.env)[0], 0)

    def test_root_itself_may_confine_any_process(self):
        self.process(700, parent=1)
        self.assertEqual(self.call("enter", "700", "browser")[0], 0)
        self.assertEqual(self.system.scopes[0][1]["BindsTo"], [f"user@{UID}.service"])
        rc, _, err = self.call("enter", "4242", "browser")
        self.assertEqual(rc, 1)
        self.assertIn("no process 4242", err)

    def test_every_failure_keeps_the_application_from_starting(self):
        def attempt(expect):
            rc, _, err = self.call("enter", "600", "browser", env=self.env)
            self.assertEqual(rc, 1, expect)
            self.assertIn(expect, err)

        shutil.rmtree(self.t.SYSTEMD_RUNNING)
        attempt("systemd is not running")
        os.makedirs(self.t.SYSTEMD_RUNNING)
        self.system.busctl_rc = 1
        attempt("busctl failed")
        self.system.busctl_rc = 0
        self.system.no_move = True
        attempt("was not moved")
        self.system.no_move = False
        self.system.loaded = False
        self.system.load_lies = True
        attempt("did not take the per-application rule")
        self.system.load_lies = False
        self.system.element_lies = True
        attempt("did not take shadowblock.slice")
        self.system.element_lies = False
        write(self.t.CONFIG, "{")
        attempt("cannot be read")                 # not "not blocked": that would start it with the network
        for bad in ("a b", "../x", ""):
            self.assertEqual(self.call("enter", "600", bad, env=self.env)[0], 1)
        self.assertEqual(self.call("enter", "600", "browser", env={"PKEXEC_UID": "abc"})[0], 1)

    def test_entering_twice_starts_one_scope(self):
        self.assertEqual(self.call("enter", "600", "browser", env=self.env)[0], 0)
        self.assertEqual(self.call("enter", "600", "browser", env=self.env)[0], 0)
        self.assertEqual(len(self.system.scopes), 1)

    def test_status_for_an_ordinary_user(self):
        self.call("enter", "600", "browser", env=self.env)
        self.system.packets = (3, 4)
        self.call("status")                           # root records it
        self.euid = 1000
        self.system.calls.clear()
        rc, out, _ = self.call("status", "--json")
        status = json.loads(out)
        self.assertEqual((status["loaded"], status["rejected_packets"], status["blocked"]), (True, 7, ["browser"]))
        self.assertEqual(status["running"][0]["app"], "browser")
        self.assertEqual(self.system.calls, [])       # asks the kernel nothing as a user
        text = self.call("status")[1]
        self.assertIn("Running without network: browser", text)


class RunWrapper(unittest.TestCase):
    """shadow-appfw-run with a stand-in pkexec."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="appfw-run-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.record = os.path.join(self.dir, "pkexec-called")

    def run_wrapper(self, status, *argv):
        fake = os.path.join(self.dir, "bin")
        os.makedirs(fake, exist_ok=True)
        for name, text in (("pkexec", f'#!/bin/sh\nprintf "%s\\n" "$@" > "{self.record}"\nexit {status}\n'),
                           ("notify-send", f'#!/bin/sh\nprintf "%s|" "$@" > "{self.dir}/notified"\n')):
            with open(os.path.join(fake, name), "w", encoding="utf-8") as fh:
                fh.write(text)
            os.chmod(os.path.join(fake, name), 0o755)
        env = {"PATH": fake + ":/usr/bin:/bin", "KEEP": "this stays"}
        return subprocess.run(["sh", RUN, *argv], env=env, capture_output=True, text=True, timeout=30)

    def test_confined_then_the_same_process_becomes_the_application(self):
        done = self.run_wrapper(0, "browser", "--", "sh", "-c", 'echo "$$ $KEEP $1"', "x", "an argument")
        self.assertEqual(done.returncode, 0, done.stderr)
        asked = read(self.record).split("\n")
        self.assertEqual(asked[0], "/usr/sbin/shadow-appfw-enter")
        self.assertEqual(asked[2], "browser")
        pid, rest = done.stdout.strip().split(" ", 1)
        self.assertEqual(asked[1], pid)               # the process that asked is the one that runs
        self.assertEqual(rest, "this stays an argument")

    def test_not_blocked_starts_normally(self):
        done = self.run_wrapper(3, "browser", "--", "echo", "started")
        self.assertEqual((done.returncode, done.stdout), (0, "started\n"))

    def test_any_failure_starts_nothing(self):
        for status in (1, 2, 126, 127, 130):
            done = self.run_wrapper(status, "browser", "--", "echo", "started")
            self.assertEqual(done.returncode, 126, status)
            self.assertEqual(done.stdout, "")
            self.assertIn("It was not started", done.stderr)
            self.assertIn("Application not started", read(os.path.join(self.dir, "notified")))

    def test_usage(self):
        for argv in ((), ("browser",), ("browser", "--")):
            done = self.run_wrapper(0, *argv)
            self.assertEqual(done.returncode, 2, argv)
            self.assertFalse(os.path.exists(self.record))
        # Without "--" the command follows the application directly.
        done = self.run_wrapper(0, "browser", "echo", "ok")
        self.assertEqual(done.stdout, "ok\n")


class Packaging(unittest.TestCase):
    def test_the_helper_only_reaches_enter(self):
        lines = [line for line in read(ENTER).splitlines() if line and not line.startswith("#")]
        self.assertEqual(lines, ['exec /usr/sbin/shadow-appfw enter "$@"'])

    def test_polkit_actions(self):
        tree = ET.parse(os.path.join(ROOT, "security", "permissions", "org.shadow.security.policy"))
        actions = {a.get("id"): a for a in tree.getroot().findall("action")}

        def defaults(action):
            node = actions[action].find("defaults")
            return tuple(node.findtext(key) for key in ("allow_any", "allow_inactive", "allow_active"))

        def program(action):
            return actions[action].find("annotate[@key='org.freedesktop.policykit.exec.path']").text

        # Changing what is blocked: an administrator, always.
        self.assertEqual(defaults("org.shadow.appfw.manage"), ("auth_admin", "auth_admin", "auth_admin_keep"))
        self.assertEqual(program("org.shadow.appfw.manage"), "/usr/sbin/shadow-appfw")
        # Being confined: the active local session, nobody else.
        self.assertEqual(defaults("org.shadow.appfw.enter"), ("no", "no", "yes"))
        self.assertEqual(program("org.shadow.appfw.enter"), "/usr/sbin/shadow-appfw-enter")

    def test_installed_files_and_units(self):
        install = read(os.path.join(ROOT, "debian", "shadow-hardening.install"))
        for line in ("security/firewall/shadow-appfw usr/sbin", "security/firewall/shadow-appfw-enter usr/sbin",
                     "security/firewall/shadow-appfw-run usr/bin",
                     "security/firewall/systemd/shadow-appfw.service usr/lib/systemd/system"):
            self.assertIn(line + "\n", install)
        for program in (TOOL, RUN, ENTER):
            self.assertTrue(os.access(program, os.X_OK), program)
        unit = read(os.path.join(FW, "systemd", "shadow-appfw.service"))
        self.assertIn("ExecStart=/usr/sbin/shadow-appfw boot\n", unit)
        self.assertIn("ConditionKernelCommandLine=!shadow.appfw=off\n", unit)
        self.assertIn("WantedBy=multi-user.target\n", unit)
        watch = read(os.path.join(FW, "systemd", "shadow-killswitch-watch.service"))
        self.assertIn("ExecStart=-/usr/sbin/shadow-appfw reassert\n", watch)
        self.assertIn("dh_installsystemd -pshadow-hardening shadow-appfw.service\n",
                      read(os.path.join(ROOT, "debian", "rules")))

    def test_removal_and_application_changes(self):
        tool = load()
        prerm = read(os.path.join(ROOT, "debian", "shadow-hardening.prerm"))
        # Without the package the entries would name a program that is gone.
        self.assertIn("/usr/local/share/applications/*.desktop", prerm)
        self.assertIn(f"grep -q '^{tool.MARK}=' \"$entry\"", prerm)
        postrm = read(os.path.join(ROOT, "debian", "shadow-hardening.postrm"))
        self.assertIn('"inet shadow_appfw"', postrm)
        self.assertIn("/etc/shadow-os/appfw.json", postrm)
        self.assertEqual("/".join(tool.TABLE), "inet/shadow_appfw")
        triggers = [line for line in read(os.path.join(ROOT, "debian", "shadow-hardening.triggers")).splitlines()
                    if line and not line.startswith("#")]
        self.assertEqual(triggers, ["interest-noawait /usr/share/applications"])
        postinst = read(os.path.join(ROOT, "debian", "shadow-hardening.postinst"))
        self.assertIn("triggered)", postinst)
        self.assertIn("/usr/sbin/shadow-appfw sync", postinst)


if __name__ == "__main__":
    unittest.main()
