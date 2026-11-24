#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the Privacy page's Hide Me and OS integrity actions, and of its
Network identity values
(applications/control-center/backends/privacybackend.{h,cpp}).

A stand-in shadow-anon and shadow-harden (first on PATH, through the
back-end's development hooks) record their arguments and exit as the real
tools do: 0 when done, 3 (shadow-anon) or 1 (shadow-harden) with the reason
on stderr when something could not be changed. A stand-in pkexec records its
arguments and runs them, or exits 126 (authentication cancelled or refused)
or 127 (not authorised) like the real one. The Tor status tests add a
stand-in systemctl whose "is-active --quiet UNIT" answers 0 only for the
units they name, the way Debian's units behave (tor.service is a /bin/true
oneshot that stays active while tor@default fails).

  SHADOW_CC_HIDEME_HARNESS  tests/harness/cchideme: runs the page's actions
                            and prints the message the page would show, the
                            Hide Me state and the Network identity values
  SHADOW_CC_BIN             shadow-control-center ("--dump privacy")

Network identity reads NetworkManager files written into a temporary root
(SHADOW_NM_CONFIG_ROOT), with the texts the tree's shadow-netprivacy writes.

When the tests run as root, the programs are started as the user "nobody"
(setpriv) so that the pkexec path is the one tested; one class checks that
root runs the tools directly. Without the harness the tests are skipped. Run
with PYTHONDONTWRITEBYTECODE=1; nothing is written outside a temporary
directory.
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest

HARNESS = os.environ.get("SHADOW_CC_HIDEME_HARNESS", "")
CC_BIN = os.environ.get("SHADOW_CC_BIN", "")
NOBODY = 65534

FAKE_PKEXEC = """#!/bin/sh
printf 'pkexec' >> "$FAKE_LOG"; printf ' %s' "$@" >> "$FAKE_LOG"; printf '\\n' >> "$FAKE_LOG"
if [ -n "$FAKE_PKEXEC_EXIT" ]; then echo "Error executing command as another user: Not authorized" >&2; echo "This incident has been reported." >&2; exit "$FAKE_PKEXEC_EXIT"; fi
exec "$@"
"""

# Like the real tool: progress on stdout; on failure "shadow-anon: error: …"
# as the last stderr line, exit 3 and the reason in the state record's
# "error"; on success "error": null.
FAKE_ANON = """#!/usr/bin/python3
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("anon " + " ".join(sys.argv[1:]) + "\\n")
print("· doing " + " ".join(sys.argv[1:]), flush=True)
state_path = os.environ["SHADOW_ANON_STATE"]
try:
    state = json.load(open(state_path))
except (OSError, ValueError):
    state = {}
code = int(os.environ.get("FAKE_ANON_EXIT", "0"))
if code:
    sys.stderr.write(os.environ.get("FAKE_ANON_STDERR", ""))
    if code == 3:
        state["error"] = os.environ.get("FAKE_ANON_ERROR", "")
        json.dump(state, open(state_path, "w"))
    sys.exit(code)
state["error"] = None
if sys.argv[1:] == ["ghost-on"]:
    state.update(ghost_mode=True, threat_model="maximum", transparent_proxy=True, dns_protected=True)
json.dump(state, open(state_path, "w"))
"""

FAKE_HARDEN = """#!/bin/sh
printf 'harden %s\\n' "$*" >> "$FAKE_LOG"
if [ -n "$FAKE_HARDEN_EXIT" ]; then
  echo "  Applied no-exec /tmp on /tmp"
  printf '  NOT applied: hidepid on /proc (could not create the group proc: groupadd failed)\\nFilesystem hardening incomplete:\\n  - could not create the group proc: groupadd failed\\n' >&2
  exit "$FAKE_HARDEN_EXIT"
fi
echo "Filesystem hardening applied."
"""

# "is-active --quiet UNIT": 0 for the units in FAKE_ACTIVE_UNITS, no answer
# (a hang) for FAKE_HANG_UNIT, 3 (inactive or failed) for every other one.
FAKE_SYSTEMCTL = """#!/bin/sh
printf 'systemctl %s\\n' "$*" >> "$FAKE_SYSTEMCTL_LOG"
if [ "$1" = is-active ] && [ "$2" = --quiet ] && [ $# -eq 3 ]; then
  case " $FAKE_ACTIVE_UNITS " in *" $3 "*) exit 0 ;; esac
  [ "$3" = "$FAKE_HANG_UNIT" ] && exec sleep 8
  exit 3
fi
exit 1
"""


def have_setpriv():
    return shutil.which("setpriv") is not None


@unittest.skipUnless(HARNESS and os.access(HARNESS, os.X_OK), "SHADOW_CC_HIDEME_HARNESS (cchideme) not given")
class Base(unittest.TestCase):
    as_user = True

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cc-hideme-")
        os.chmod(self.tmp, 0o777)        # the program may run as "nobody"; the fakes write here
        self.fake = os.path.join(self.tmp, "fake")
        os.makedirs(self.fake)
        os.chmod(self.fake, 0o755)
        self.home = os.path.join(self.tmp, "home")
        os.makedirs(self.home)
        os.chmod(self.home, 0o777)
        self.log = os.path.join(self.tmp, "calls.log")
        self.state = os.path.join(self.tmp, "anon-state.json")
        self.dropin = os.path.join(self.tmp, "interval.conf")
        for path in (self.log, self.state):
            open(path, "w").close()
            os.chmod(path, 0o666)
        os.unlink(self.state)            # no record until a test writes one
        for name, text in (("pkexec", FAKE_PKEXEC), ("shadow-anon", FAKE_ANON), ("shadow-harden", FAKE_HARDEN)):
            path = os.path.join(self.fake, name)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            os.chmod(path, 0o755)
        self.anon = os.path.join(self.fake, "shadow-anon")
        self.harden = os.path.join(self.fake, "shadow-harden")
        self.env_extra = {}

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_state(self, obj):
        with open(self.state, "w", encoding="utf-8") as fh:
            json.dump(obj, fh)
        os.chmod(self.state, 0o666)

    def calls(self):
        with open(self.log, encoding="utf-8") as fh:
            return [line.strip() for line in fh if line.strip()]

    def env(self):
        env = {"PATH": self.fake + ":/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
               "HOME": self.home, "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
               "XDG_RUNTIME_DIR": self.home, "QT_QPA_PLATFORM": "offscreen",
               "SHADOW_ANON_TOOL": self.anon, "SHADOW_HARDEN_TOOL": self.harden,
               "SHADOW_ANON_STATE": self.state, "SHADOW_ANON_TIMER_DROPIN": self.dropin,
               "FAKE_LOG": self.log, "PYTHONDONTWRITEBYTECODE": "1"}
        if os.environ.get("LD_LIBRARY_PATH"):
            env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        env.update(self.env_extra)
        return env

    def command(self, argv):
        if os.geteuid() == 0 and self.as_user:
            if not have_setpriv():
                self.skipTest("running as root without setpriv: cannot test the pkexec path")
            return ["setpriv", f"--reuid={NOBODY}", f"--regid={NOBODY}", "--clear-groups", "--"] + argv
        return argv

    def harness(self, *ops):
        p = subprocess.run(self.command([HARNESS, *ops]), env=self.env(), capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = [json.loads(line) for line in p.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(lines), len(ops), p.stdout + p.stderr)
        return lines

    def pk(self, *args):
        return "pkexec " + " ".join(args)


class Commands(Base):
    """The exact command line each control runs."""

    def test_each_control(self):
        ops = {"ghost-on": "ghost-on", "ghost-off": "ghost-off", "rotate": "rotate", "nuke": "nuke",
               "wipe-logs": "wipe-logs", "harden-kernel": "harden-kernel", "stop": "stop",
               "threat:enhanced": "threat enhanced", "ks:1": "kill-switch-on", "ks:0": "kill-switch-off",
               "proxy:1": "transparent-proxy-on", "proxy:0": "transparent-proxy-off",
               "dns:1": "dns-protect-on", "dns:0": "dns-protect-off", "bt:1": "kill-bluetooth",
               "bt:0": "restore-bluetooth", "usb:1": "kill-usb", "usb:0": "restore-usb"}
        out = self.harness(*ops)
        for line in out:
            self.assertEqual(line["result"], "succeeded", line)
            self.assertFalse(line["hideMe"]["busy"], line)
        expected = []
        for args in ops.values():
            expected += [self.pk(self.anon, args), "anon " + args]
        self.assertEqual(self.calls(), expected)

    def test_kill_switch_is_never_persisted_from_the_page(self):
        self.harness("ks:1", "ghost-on", "nuke", "threat:maximum", "proxy:1")
        self.assertFalse([c for c in self.calls() if "--persist" in c])

    def test_auto_rotate_minimum_is_60_seconds(self):
        out = self.harness("start:5", "start:59", "start:60", "start:300")
        self.assertEqual([c for c in self.calls() if c.startswith("anon")],
                         ["anon start --interval 60", "anon start --interval 60",
                          "anon start --interval 60", "anon start --interval 300"])
        self.assertEqual(out[0]["hideMe"]["autoRotateMinimumSeconds"], 60)
        self.assertIn("60", out[0]["message"])

    def test_unknown_threat_level_runs_nothing(self):
        out = self.harness("threat:paranoid")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(self.calls(), [])

    def test_success_messages_say_what_happened(self):
        out = self.harness("ghost-on", "ks:1", "ks:0")
        self.assertIn("Ghost Mode is on", out[0]["message"])
        self.assertIn("Tor only", out[1]["message"])
        self.assertIn("mode in force before", out[2]["message"])

    def test_not_installed(self):
        os.unlink(self.anon)
        out = self.harness("ghost-on")
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("not installed", out[0]["message"])
        self.assertFalse(out[0]["hideMe"]["toolInstalled"])
        self.assertEqual(self.calls(), [])


class Failures(Base):
    """Exit status → what the page says."""

    def test_tool_failure_shows_its_last_stderr_line(self):
        reason = "Ghost Mode is NOT on: the kill switch could not be engaged: nft: table not loaded"
        self.env_extra = {"FAKE_ANON_EXIT": "3", "FAKE_ANON_ERROR": reason,
                          "FAKE_ANON_STDERR": "warning on stderr\nshadow-anon: error: " + reason + "\n"}
        out = self.harness("ghost-on")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], reason)
        # The state record's "error" is shown too (it also covers failures
        # of the boot unit and the timer).
        self.assertEqual(out[0]["hideMe"]["error"], reason)
        self.assertIn("doing ghost-on", out[0]["hideMe"]["output"])
        self.assertIn(reason, out[0]["hideMe"]["output"])

    def test_a_later_success_clears_the_error(self):
        self.write_state({"error": "the kill switch could not be engaged: x"})
        out = self.harness("show", "rotate")
        self.assertEqual(out[0]["hideMe"]["error"], "the kill switch could not be engaged: x")
        self.assertEqual(out[1]["hideMe"]["error"], "")

    def test_exit_1_is_a_failure_not_a_cancelled_dialog(self):
        self.env_extra = {"FAKE_ANON_EXIT": "1",
                          "FAKE_ANON_STDERR": "shadow-anon: run as root (the Hide Me panel does this via pkexec).\n"}
        out = self.harness("rotate")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], "run as root (the Hide Me panel does this via pkexec).")

    def test_failure_without_a_message(self):
        self.env_extra = {"FAKE_ANON_EXIT": "3", "FAKE_ANON_STDERR": ""}
        out = self.harness("rotate")
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("exit status 3", out[0]["message"])

    def test_authentication_cancelled(self):
        self.env_extra = {"FAKE_PKEXEC_EXIT": "126"}
        out = self.harness("ghost-on")
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("cancelled or refused", out[0]["message"])
        self.assertEqual(self.calls(), [self.pk(self.anon, "ghost-on")])     # the tool never ran

    def test_not_authorised(self):
        self.env_extra = {"FAKE_PKEXEC_EXIT": "127"}
        out = self.harness("ks:1")
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("not authorised", out[0]["message"])
        self.assertNotIn("incident", out[0]["message"])

    def test_harden_failure_is_reported(self):
        # shadow-harden exits 1 when something could not be applied; that used
        # to be taken for a cancelled password dialog and not shown at all.
        self.env_extra = {"FAKE_HARDEN_EXIT": "1"}
        out = self.harness("harden:fs")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], "could not create the group proc: groupadd failed")
        self.assertEqual(self.calls(), [self.pk(self.harden, "fs-harden"), "harden fs-harden"])

    def test_harden_success(self):
        out = self.harness("harden:fs")
        self.assertEqual(out[0]["result"], "succeeded")
        self.assertIn("Filesystem hardening applied.", out[0]["hardenOutput"])


class State(Base):
    """What is read from the state record and the timer drop-in."""

    def test_state_record(self):
        self.write_state({"ghost_mode": True, "threat_model": "maximum", "transparent_proxy": True,
                          "dns_protected": True, "bt_killed": True, "usb_killed": False,
                          "kernel_hardened": True, "kill_switch": True, "error": None})
        h = self.harness("show")[0]["hideMe"]
        self.assertTrue(h["ghostMode"])
        self.assertEqual(h["threatModel"], "maximum")
        self.assertTrue(h["transparentProxy"])
        self.assertTrue(h["dnsProtected"])
        self.assertTrue(h["bluetoothKilled"])
        self.assertEqual(h["error"], "")
        # The kill switch is not taken from Hide Me's record (KillSwitchBackend
        # reads shadow-killswitch's own file); nothing here claims it.
        self.assertNotIn("killSwitch", h)

    def test_unknown_threat_model_is_basic(self):
        self.write_state({"threat_model": "extreme"})
        self.assertEqual(self.harness("show")[0]["hideMe"]["threatModel"], "basic")

    def test_rotation_interval(self):
        self.assertEqual(self.harness("show")[0]["hideMe"]["autoRotateIntervalSeconds"], 300)   # the unit's default
        for text, seconds in (("[Timer]\nOnUnitActiveSec=\nOnUnitActiveSec=600s\n", 600),
                              ("[Timer]\nOnUnitActiveSec=\nOnUnitActiveSec=5s\n", 60),    # older versions; raised
                              ("[Timer]\nOnUnitActiveSec=90\n", 90)):
            with open(self.dropin, "w", encoding="utf-8") as fh:
                fh.write(text)
            os.chmod(self.dropin, 0o644)
            self.assertEqual(self.harness("show")[0]["hideMe"]["autoRotateIntervalSeconds"], seconds, text)

    @unittest.skipUnless(CC_BIN and os.access(CC_BIN, os.X_OK), "SHADOW_CC_BIN not given")
    def test_dump(self):
        self.write_state({"ghost_mode": False, "threat_model": "enhanced", "dns_protected": True,
                          "error": "DNS protection could not be put back: Tor did not start"})
        p = subprocess.run(self.command([CC_BIN, "--dump", "privacy"]), env=self.env(),
                           capture_output=True, text=True, timeout=150)
        self.assertEqual(p.returncode, 0, p.stderr)
        h = json.loads(p.stdout)["hideMe"]
        self.assertEqual(h["threatModel"], "enhanced")
        self.assertTrue(h["dnsProtected"])
        self.assertEqual(h["error"], "DNS protection could not be put back: Tor did not start")


class TorStatus(Base):
    """"Tor: running" / "Tor: stopped" on the Rotate identity row
    (torRunning) is whether tor@default runs, as shadow-anon's tor_running()
    checks it. Debian's tor.service is a oneshot running /bin/true
    (RemainAfterExit=yes): it stays active for the whole boot while
    tor@default fails (port taken, bad bridge line, crash), so it never
    counts."""

    def setUp(self):
        super().setUp()
        path = os.path.join(self.fake, "systemctl")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(FAKE_SYSTEMCTL)
        os.chmod(path, 0o755)
        self.systemctl_log = os.path.join(self.tmp, "systemctl.log")
        open(self.systemctl_log, "w").close()
        os.chmod(self.systemctl_log, 0o666)
        self.env_extra["FAKE_SYSTEMCTL_LOG"] = self.systemctl_log

    def systemctl_calls(self):
        with open(self.systemctl_log, encoding="utf-8") as fh:
            return [line.strip() for line in fh if line.strip()]

    def tor_running(self, active, hang=""):
        self.env_extra.update(FAKE_ACTIVE_UNITS=active, FAKE_HANG_UNIT=hang)
        open(self.systemctl_log, "w").close()
        return self.harness("show")[0]["hideMe"]["torRunning"]

    def test_tor_service_active_tor_at_default_failed_is_stopped(self):
        self.assertFalse(self.tor_running("tor tor.service"))
        calls = self.systemctl_calls()
        self.assertIn("systemctl is-active --quiet tor@default", calls)
        self.assertNotIn("systemctl is-active --quiet tor", calls)
        self.assertNotIn("systemctl is-active --quiet tor.service", calls)

    def test_tor_at_default_active_is_running(self):
        for active in ("tor tor@default", "tor@default"):
            self.assertTrue(self.tor_running(active), active)
        self.assertFalse(self.tor_running(""))

    def test_no_answer_is_not_running(self):
        # systemctl that does not answer within the back-end's 2 s is not
        # taken for "running" (QProcess's exit code is 0 until a process ends).
        self.assertFalse(self.tor_running("tor", hang="tor@default"))

    @unittest.skipUnless(CC_BIN and os.access(CC_BIN, os.X_OK), "SHADOW_CC_BIN not given")
    def test_dump(self):
        self.env_extra["FAKE_ACTIVE_UNITS"] = "tor"
        p = subprocess.run(self.command([CC_BIN, "--dump", "privacy"]), env=self.env(),
                           capture_output=True, text=True, timeout=150)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(json.loads(p.stdout)["hideMe"]["torRunning"])
        self.assertIn("systemctl is-active --quiet tor@default", self.systemctl_calls())


HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
NETPRIVACY = os.path.join(TREE, "security", "hardening", "shadow-netprivacy")
NM_SHIPPED = os.path.join(TREE, "security", "hardening", "NetworkManager", "80-shadow-privacy.conf")


def load_netprivacy():
    """The tree's shadow-netprivacy as a module: its file names and the texts it writes."""
    loader = importlib.machinery.SourceFileLoader("shadow_netprivacy_for_cc", NETPRIVACY)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class NetworkIdentity(Base):
    """Privacy → Network identity follows NetworkManager's section precedence:
    a named [connection-…] section without match-device applies to every
    device and comes before [connection]; shadow.netprivacy=off's file is one."""

    def setUp(self):
        super().setUp()
        self.np = load_netprivacy()
        self.root = os.path.join(self.tmp, "root")
        self.env_extra["SHADOW_NM_CONFIG_ROOT"] = self.root
        with open(NM_SHIPPED, encoding="utf-8") as fh:
            self.put(self.np.NM_DEFAULT, fh.read())

    def put(self, path, text):
        full = self.root + path
        os.makedirs(os.path.dirname(full), mode=0o755, exist_ok=True)
        with open(full, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.chmod(full, 0o644)

    def network(self):
        net = self.harness("show")[0]["network"]
        return net, {s["key"]: s for s in net["settings"]}

    def test_shipped_defaults(self):
        net, s = self.network()
        self.assertEqual(net["shadowPrivacyFile"], self.root + self.np.NM_DEFAULT)
        self.assertEqual(net["netPrivacyEscapeFile"], "")
        for key in ("wifi.cloned-mac-address", "ethernet.cloned-mac-address"):
            # [connection-shadow-mac-fixed-drivers] has match-device: some devices only.
            self.assertEqual((s[key]["value"], s[key]["group"], s[key]["source"]),
                             ("stable", "connection", self.root + self.np.NM_DEFAULT), key)
        self.assertEqual(s["connection.stable-id"]["value"], "${CONNECTION}/${BOOT}")
        self.assertEqual((s["wifi.scan-rand-mac-address"]["value"], s["wifi.scan-rand-mac-address"]["group"]),
                         ("yes", "device"))
        self.assertEqual(s["ipv6.ip6-privacy"]["value"], "2")

    def test_escape_hatch_real_addresses(self):
        # What shadow-netprivacy.service writes for a boot with shadow.netprivacy=off,
        # with a MAC policy chosen earlier in /etc (read after /run, in [connection]).
        self.put(self.np.NM_ESCAPE, self.np.render_escape_nm())
        self.put(self.np.NM_USER, self.np.render_mac("per-network"))
        net, s = self.network()
        escape = self.root + self.np.NM_ESCAPE
        self.assertEqual(net["netPrivacyEscapeFile"], escape)
        for key in ("wifi.cloned-mac-address", "ethernet.cloned-mac-address"):
            self.assertEqual((s[key]["set"], s[key]["value"], s[key]["group"], s[key]["source"]),
                             (True, "preserve", "connection-shadow-escape", escape), key)
        self.assertEqual((s["connection.stable-id"]["value"], s["connection.stable-id"]["source"]),
                         (self.np.STABLE_PER_NETWORK, self.root + self.np.NM_USER))
        self.assertEqual(s["wifi.scan-rand-mac-address"]["value"], "yes")    # scanning stays random

    def test_escape_file_shadowed_by_etc(self):
        # A file of the same name in /etc keeps NetworkManager from reading the one in /run.
        self.put(self.np.NM_ESCAPE, self.np.render_escape_nm())
        self.put("/etc/NetworkManager/conf.d/" + os.path.basename(self.np.NM_ESCAPE), "# empty\n")
        net, s = self.network()
        self.assertEqual(net["netPrivacyEscapeFile"], "")
        self.assertEqual(s["wifi.cloned-mac-address"]["value"], "stable")

    def test_section_precedence(self):
        self.put(self.np.NM_ESCAPE, self.np.render_escape_nm())
        # A later file's named sections come first, within a file top to bottom;
        # sections with match-device are skipped; [device-…] works like [connection-…].
        self.put("/etc/NetworkManager/conf.d/95-later.conf",
                 "[connection-one-device]\nmatch-device=interface-name:wlan9\nwifi.cloned-mac-address=permanent\n"
                 "[connection-first]\nwifi.cloned-mac-address=random\n"
                 "[connection-second]\nwifi.cloned-mac-address=permanent\nethernet.cloned-mac-address=stable-ssid\n"
                 "[device-all]\nwifi.scan-rand-mac-address=no\n")
        _net, s = self.network()
        self.assertEqual((s["wifi.cloned-mac-address"]["value"], s["wifi.cloned-mac-address"]["group"]),
                         ("random", "connection-first"))
        self.assertEqual((s["ethernet.cloned-mac-address"]["value"], s["ethernet.cloned-mac-address"]["group"]),
                         ("stable-ssid", "connection-second"))
        self.assertEqual((s["wifi.scan-rand-mac-address"]["value"], s["wifi.scan-rand-mac-address"]["group"]),
                         ("no", "device-all"))
        self.assertEqual((s["ipv6.ip6-privacy"]["value"], s["ipv6.ip6-privacy"]["group"]), ("2", "connection"))
        # stop-match=yes without match-device ends the search: NetworkManager's built-in defaults.
        self.put("/etc/NetworkManager/conf.d/99-stop.conf", "[connection-stop]\nstop-match=yes\n")
        _net, s = self.network()
        for key in ("wifi.cloned-mac-address", "ethernet.cloned-mac-address", "ipv6.ip6-privacy"):
            self.assertEqual((s[key]["set"], s[key]["group"]), (False, ""), key)
        self.assertEqual(s["wifi.scan-rand-mac-address"]["value"], "no")

    @unittest.skipUnless(CC_BIN and os.access(CC_BIN, os.X_OK), "SHADOW_CC_BIN not given")
    def test_dump(self):
        self.put(self.np.NM_ESCAPE, self.np.render_escape_nm())
        p = subprocess.run(self.command([CC_BIN, "--dump", "privacy"]), env=self.env(),
                           capture_output=True, text=True, timeout=150)
        self.assertEqual(p.returncode, 0, p.stderr)
        net = json.loads(p.stdout)["network"]
        self.assertEqual(net["netPrivacyEscapeFile"], self.root + self.np.NM_ESCAPE)
        wifi = [s for s in net["settings"] if s["key"] == "wifi.cloned-mac-address"][0]
        self.assertEqual((wifi["value"], wifi["group"]), ("preserve", "connection-shadow-escape"))


class AsRoot(Base):
    """Root runs the tools directly, without pkexec."""
    as_user = False

    def setUp(self):
        if os.geteuid() != 0:
            self.skipTest("needs root")
        super().setUp()

    def test_direct(self):
        out = self.harness("ghost-on", "harden:fs")
        self.assertEqual([line["result"] for line in out], ["succeeded", "succeeded"])
        self.assertEqual(self.calls(), ["anon ghost-on", "harden fs-harden"])


if __name__ == "__main__":
    unittest.main()
