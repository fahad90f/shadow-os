#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the Privacy page's kill switch and panic back-end
(applications/control-center/backends/privacykillswitchbackend.{h,cpp}).

Two programs are driven, with fake tools first on PATH and the state files in
a temporary directory (the back-end's development hooks):

  SHADOW_CC_BIN       shadow-control-center; "--dump killswitch" shows what
                      the page would show (parsing, states, live check)
  SHADOW_CC_HARNESS   tests/harness/cckillswitch; runs the page's actions
                      (set, lan, keep, reassert, release) and prints the state

The fake pkexec records its arguments and runs them, so the tests see the
exact command line a user's click produces. When the tests run as root, the
programs are started as the user "nobody" (setpriv) so that the pkexec path is
the one tested; one test also checks that root runs the tools directly.

Without SHADOW_CC_BIN / SHADOW_CC_HARNESS the tests are skipped. Run with
PYTHONDONTWRITEBYTECODE=1; nothing is written outside a temporary directory.
"""
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest

CC_BIN = os.environ.get("SHADOW_CC_BIN", "")
HARNESS = os.environ.get("SHADOW_CC_HARNESS", "")
NOBODY = 65534

FAKE_PKEXEC = """#!/bin/sh
printf 'pkexec' >> "$FAKE_LOG"; printf ' %s' "$@" >> "$FAKE_LOG"; printf '\\n' >> "$FAKE_LOG"
if [ -n "$FAKE_PKEXEC_EXIT" ]; then echo "Not authorized" >&2; exit "$FAKE_PKEXEC_EXIT"; fi
exec "$@"
"""

# A stand-in for shadow-killswitch: records its arguments and, like the real
# tool, publishes the new state to $SHADOW_NETGUARD_RUN_DIR/killswitch.json.
FAKE_KILLSWITCH = """#!/usr/bin/python3
import json, os, sys
run = os.environ["SHADOW_NETGUARD_RUN_DIR"]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("killswitch " + " ".join(sys.argv[1:]) + "\\n")
if os.environ.get("FAKE_KS_EXIT"):
    sys.stderr.write(os.environ.get("FAKE_KS_STDERR", ""))
    sys.exit(int(os.environ["FAKE_KS_EXIT"]))
path = os.path.join(run, "killswitch.json")
try:
    old = json.load(open(path))
except (OSError, ValueError):
    old = {"mode": "off", "lan": False, "persistent_mode": "off"}
args = sys.argv[1:]
if args[0] == "set":
    mode = args[1]
    lan = "--lan" in args if ("--lan" in args or "--no-lan" in args) else old.get("lan", False)
    runtime = "--runtime" in args
    new = {"mode": mode, "active": mode != "off", "loaded": mode != "off", "since": None if mode == "off" else "2026-11-23T18:00:00Z",
           "lan": lan, "endpoints": 0, "error": None, "escape_hatch": False, "version": 1,
           "persistent_mode": old.get("persistent_mode", "off") if runtime else mode, "runtime_override": runtime,
           "updated": "2026-11-23T18:30:00Z", "warnings": []}
elif args[0] == "reassert" and os.environ.get("FAKE_KS_REASSERT_KEEPS"):
    new = old                     # the kernel already had the table: exit 0, the document stays
elif args[0] == "reassert":
    new = dict(old, loaded=old.get("mode") != "off", error=None)
else:
    sys.exit(2)
json.dump(new, open(path, "w"))
print("ok")
"""

FAKE_PANIC = """#!/usr/bin/python3
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("panic " + " ".join(sys.argv[1:]) + "\\n")
if os.environ.get("FAKE_PANIC_EXIT"):
    sys.stderr.write("shadow-panic: NetworkManager networking was not turned back on: nmcli failed\\n")
    sys.exit(int(os.environ["FAKE_PANIC_EXIT"]))
path = os.path.join(os.environ["SHADOW_NETGUARD_RUN_DIR"], "panic.json")
json.dump({"engaged": False, "loaded": False, "error": None, "since": None, "released": "2026-11-23T18:40:00Z",
           "warnings": [], "version": 1}, open(path, "w"))
print("Panic released: networking is as it was before.")
"""

FAKE_STATUS = """#!/bin/sh
printf 'security-status %s\\n' "$*" >> "$FAKE_LOG"
[ -n "$FAKE_LIVE_EXIT" ] && { echo "shadow-security-status: broken" >&2; exit "$FAKE_LIVE_EXIT"; }
cat "$FAKE_LIVE"
"""

ON_TOR = {"mode": "tor", "active": True, "loaded": True, "since": "2026-11-23T18:00:00Z", "lan": False,
          "endpoints": 0, "error": None, "escape_hatch": False, "version": 1, "persistent_mode": "tor",
          "runtime_override": False, "tunnel_ifaces": 0, "skipped_hostnames": 0, "dropped_packets": 12,
          "updated": "2026-11-23T18:05:00Z", "warnings": []}
OFF = dict(ON_TOR, mode="off", active=False, loaded=False, since=None, persistent_mode="off", dropped_packets=None)


def have_setpriv():
    return shutil.which("setpriv") is not None


@unittest.skipUnless(CC_BIN and os.access(CC_BIN, os.X_OK), "SHADOW_CC_BIN (shadow-control-center) not given")
class Base(unittest.TestCase):
    as_user = True

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cc-ks-")
        os.chmod(self.tmp, 0o755)
        self.run_dir = self.mkdir("run")
        self.etc = self.mkdir("etc")
        self.fake = self.mkdir("fake")
        self.home = self.mkdir("home", 0o777)
        self.log = os.path.join(self.tmp, "calls.log")
        open(self.log, "w").close()
        os.chmod(self.log, 0o666)
        self.live = os.path.join(self.tmp, "live.json")
        self.env_extra = {}
        for name, text in (("pkexec", FAKE_PKEXEC), ("shadow-killswitch", FAKE_KILLSWITCH),
                           ("shadow-panic", FAKE_PANIC), ("shadow-security-status", FAKE_STATUS)):
            self.write(os.path.join("fake", name), text, 0o755)
        self.tools = {"SHADOW_KILLSWITCH_TOOL": os.path.join(self.fake, "shadow-killswitch"),
                      "SHADOW_PANIC_TOOL": os.path.join(self.fake, "shadow-panic"),
                      "SHADOW_SECURITY_STATUS_TOOL": os.path.join(self.fake, "shadow-security-status")}
        self.set_live(None, None)         # no verdict unless a test sets one

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- helpers ------------------------------------------------------------
    def mkdir(self, name, mode=0o777):
        path = os.path.join(self.tmp, name)
        os.makedirs(path)
        os.chmod(path, mode)          # the program may run as "nobody" and the fakes write here
        return path

    def write(self, rel, text, mode=0o666):
        path = os.path.join(self.tmp, rel)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.chmod(path, mode)
        return path

    def status(self, obj):
        return self.write("run/killswitch.json", obj if isinstance(obj, str) else json.dumps(obj))

    def panic(self, obj):
        return self.write("run/panic.json", obj if isinstance(obj, str) else json.dumps(obj))

    def set_live(self, ks_table, panic_table):
        self.write("live.json", json.dumps({"killswitch": {"available": True, "table_present": ks_table},
                                            "panic": {"available": True, "table_present": panic_table}}))

    def calls(self):
        with open(self.log, encoding="utf-8") as fh:
            return [line.strip() for line in fh if line.strip()]

    def env(self, tools=True):
        env = {"PATH": self.fake + ":/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
               "HOME": self.home, "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
               "XDG_RUNTIME_DIR": self.home, "QT_QPA_PLATFORM": "offscreen",
               "SHADOW_NETGUARD_RUN_DIR": self.run_dir, "SHADOW_NETGUARD_CONFIG_DIR": self.etc,
               "SHADOW_ANON_STATE": os.path.join(self.tmp, "anon.json"),
               "FAKE_LOG": self.log, "FAKE_LIVE": self.live,
               "PYTHONDONTWRITEBYTECODE": "1"}
        if os.environ.get("LD_LIBRARY_PATH"):
            env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        if tools:
            env.update(self.tools)
        env.update(self.env_extra)
        return env

    def command(self, argv):
        if os.geteuid() == 0 and self.as_user:
            if not have_setpriv():
                self.skipTest("running as root without setpriv: cannot test the pkexec path")
            return ["setpriv", f"--reuid={NOBODY}", f"--regid={NOBODY}", "--clear-groups", "--"] + argv
        return argv

    def dump(self, tools=True):
        p = subprocess.run(self.command([CC_BIN, "--dump", "killswitch"]), env=self.env(tools),
                           capture_output=True, text=True, timeout=90)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def harness(self, *ops, tools=True):
        if not (HARNESS and os.access(HARNESS, os.X_OK)):
            self.skipTest("SHADOW_CC_HARNESS (cckillswitch) not given")
        p = subprocess.run(self.command([HARNESS, *ops]), env=self.env(tools),
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = [json.loads(line) for line in p.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(lines), len(ops), p.stdout + p.stderr)
        return lines


class Parsing(Base):
    """What --dump killswitch reports for every kind of document."""

    def test_nothing_installed(self):
        d = self.dump(tools=False)
        self.assertFalse(d["toolInstalled"])
        self.assertEqual(d["killSwitch"]["state"], "unknown")
        self.assertFalse(d["killSwitch"]["protected"])
        self.assertEqual(d["panic"]["state"], "none")
        self.assertEqual(d["liveCheck"]["state"], "failed")
        self.assertIn("is not installed", d["liveCheck"]["reason"])
        # Without a session bus the shell's default key is shown, and said to be the default.
        self.assertEqual(d["panicShortcut"]["source"], "default")
        self.assertEqual(d["panicShortcut"]["keys"], "Ctrl+Alt+Shift+Esc")

    def test_on_and_loaded_is_protected(self):
        self.status(ON_TOR)
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["protected"], ks["mode"], ks["persistentMode"]), ("on", True, "tor", "tor"))
        self.assertEqual(ks["droppedPackets"], 12)
        self.assertEqual(ks["errorText"], "")

    def test_on_but_not_loaded_is_an_error(self):
        self.status(dict(ON_TOR, loaded=False))
        self.set_live(False, False)
        ks = self.dump()["killSwitch"]
        self.assertEqual(ks["state"], "error")
        self.assertFalse(ks["protected"])
        self.assertIn("NOT protected", ks["errorText"])

    def test_off_but_loaded_is_an_error(self):
        self.status(dict(OFF, loaded=True))
        ks = self.dump()["killSwitch"]
        self.assertEqual(ks["state"], "error")
        self.assertIn("still loaded", ks["errorText"])
        # Recorded off and not loaded, but the kernel has the table.
        self.status(OFF)
        self.set_live(True, False)
        ks = self.dump()["killSwitch"]
        self.assertEqual(ks["state"], "error")
        self.assertIn("its table is in the kernel", ks["errorText"])

    def test_tool_error_is_shown_even_when_loaded(self):
        self.status(dict(ON_TOR, error="could not change to mode vpn: nft failed"))
        ks = self.dump()["killSwitch"]
        self.assertEqual(ks["state"], "error")
        self.assertFalse(ks["protected"])
        self.assertEqual(ks["errorText"], "could not change to mode vpn: nft failed")

    def test_failed_change_with_the_old_mode_in_force_is_protected(self):
        # shadow-killswitch reports a change that failed while the previous mode stays
        # loaded and verified as change_error (and a warning), not as error (review r1-14)
        failed = "could not change to mode vpn: loading the kill switch ruleset failed"
        self.status(dict(ON_TOR, change_error=failed, warnings=[failed]))
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["protected"], ks["mode"]), ("on", True, "tor"))
        self.assertFalse(ks["trafficMayLeave"])

    def test_traffic_may_leave_only_without_the_table(self):
        broken = ("/etc/shadow-os/killswitch.json cannot be used: Expecting value; the kill switch fails "
                  "closed in mode tor-or-vpn")
        self.status(dict(ON_TOR, mode="tor-or-vpn", error=broken))
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["protected"], ks["trafficMayLeave"]), ("error", False, False))
        self.status(dict(ON_TOR, loaded=False))
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["trafficMayLeave"]), ("error", True))
        self.status(dict(ON_TOR))
        self.set_live(False, False)                   # the file says loaded, the kernel has no table
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["trafficMayLeave"]), ("error", True))

    def test_non_string_error_is_not_dropped(self):
        self.status(dict(ON_TOR, error={"code": 3}))
        ks = self.dump()["killSwitch"]
        self.assertEqual(ks["state"], "error")
        self.assertIn('"code"', ks["errorText"])

    def test_documents_that_cannot_be_trusted(self):
        cases = {
            "contradiction": (json.dumps(dict(ON_TOR, active=False)), "contradicts itself"),
            "invalid json": ("{\"mode\": \"tor\",", "is not valid JSON"),
            "empty": ("", "is empty"),
            "array": ("[1, 2]", "is not a JSON object"),
            "wrong type": (json.dumps(dict(ON_TOR, loaded="yes")), 'field "loaded" that is not true or false'),
            "unknown mode": (json.dumps(dict(ON_TOR, mode="everything")), "unknown mode"),
            "no mode": (json.dumps({k: v for k, v in ON_TOR.items() if k != "mode"}), "names no mode"),
        }
        for name, (text, expected) in cases.items():
            with self.subTest(name):
                self.status(text)
                ks = self.dump()["killSwitch"]
                self.assertEqual(ks["state"], "error")
                self.assertFalse(ks["protected"])
                self.assertIn(expected, ks["errorText"])
                self.assertTrue(ks["errorText"].startswith("The kill switch status cannot be trusted"))

    def test_unreadable_files(self):
        with self.subTest("too large"):
            self.status(json.dumps(ON_TOR) + " " * (300 * 1024))
            ks = self.dump()["killSwitch"]
            self.assertEqual(ks["state"], "error")
            self.assertIn("larger than", ks["errorText"])
        with self.subTest("dangling link"):
            os.unlink(os.path.join(self.run_dir, "killswitch.json"))
            os.symlink(os.path.join(self.tmp, "nowhere"), os.path.join(self.run_dir, "killswitch.json"))
            ks = self.dump()["killSwitch"]
            self.assertEqual(ks["state"], "error")
            self.assertIn("dangling", ks["errorText"])
        with self.subTest("directory"):
            os.unlink(os.path.join(self.run_dir, "killswitch.json"))
            os.mkdir(os.path.join(self.run_dir, "killswitch.json"))
            ks = self.dump()["killSwitch"]
            self.assertEqual(ks["state"], "error")
            self.assertIn("not a regular file", ks["errorText"])

    def test_escape_hatch(self):
        self.status(dict(OFF, escape_hatch=True, persistent_mode="tor"))
        self.set_live(False, False)
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["state"], ks["persistentMode"], ks["protected"]), ("bypassed", "tor", False))
        self.status(dict(OFF, escape_hatch=True, persistent_mode="off"))
        self.assertEqual(self.dump()["killSwitch"]["state"], "off")

    def test_mode_after_restart_from_the_configuration(self):
        status = {k: v for k, v in ON_TOR.items() if k not in ("persistent_mode", "runtime_override")}
        self.status(status)
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["persistentMode"], ks["persistentSource"]), ("off", "default"))
        self.write("etc/killswitch.json", json.dumps({"version": 1, "mode": "vpn", "lan": True, "vpn_endpoints": [{}]}))
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["persistentMode"], ks["persistentSource"]), ("vpn", "config"))
        self.assertEqual(ks["config"]["endpoints"], 1)
        # A broken file: shadow-killswitch fails closed in tor-or-vpn at start-up.
        self.write("etc/killswitch.json", "{broken")
        ks = self.dump()["killSwitch"]
        self.assertEqual((ks["persistentMode"], ks["persistentSource"]), ("tor-or-vpn", "fail-closed"))
        self.assertIn("is not valid JSON", ks["config"]["problem"])

    def test_live_check_contradicting_the_file(self):
        self.status(ON_TOR)
        self.panic({"engaged": True, "loaded": True, "error": None, "since": "2026-11-23T18:10:00Z"})
        self.set_live(False, False)
        d = self.dump()
        self.assertEqual(d["liveCheck"]["state"], "done")
        self.assertEqual(d["killSwitch"]["state"], "error")
        self.assertFalse(d["killSwitch"]["protected"])
        self.assertIn("kernel has no such table", d["killSwitch"]["errorText"])
        self.assertEqual(d["panic"]["state"], "error")
        self.assertIn("NOT cut off", d["panic"]["errorText"])
        # The kernel was asked through pkexec, read-only sections only.
        self.assertIn("pkexec " + self.tools["SHADOW_SECURITY_STATUS_TOOL"] + " --section killswitch,panic", self.calls())

    def test_live_check_agreeing_with_the_file(self):
        self.status(ON_TOR)
        self.set_live(True, False)
        d = self.dump()
        self.assertEqual((d["liveCheck"]["killSwitchTable"], d["killSwitch"]["state"]), (True, "on"))
        self.assertTrue(d["killSwitch"]["protected"])

    def test_live_check_failures_do_not_change_the_recorded_state(self):
        self.status(ON_TOR)
        self.env_extra = {"FAKE_PKEXEC_EXIT": "126"}
        d = self.dump()
        self.assertEqual(d["liveCheck"]["state"], "failed")
        self.assertIn("not authorised", d["liveCheck"]["reason"])
        self.assertEqual(d["killSwitch"]["state"], "on")
        self.env_extra = {"FAKE_LIVE_EXIT": "1"}
        d = self.dump()
        self.assertEqual(d["liveCheck"]["state"], "failed")
        self.assertIn("broken", d["liveCheck"]["reason"])
        # A section the tool could not fill: no verdict either way.
        self.write("live.json", json.dumps({"killswitch": {"available": True, "table_present": None,
                                                           "table_present_reason": "nft: permission denied"},
                                            "panic": {"available": False, "reason": "not installed"}}))
        self.env_extra = {}
        d = self.dump()
        self.assertEqual(d["liveCheck"]["state"], "done")
        self.assertIsNone(d["liveCheck"]["killSwitchTable"])
        self.assertEqual(d["liveCheck"]["reason"], "nft: permission denied")
        self.assertEqual(d["killSwitch"]["state"], "on")

    def test_panic_states(self):
        cases = [
            ({"engaged": True, "loaded": True, "error": None, "since": "2026-11-23T18:10:00Z"}, (True, True), "engaged"),
            ({"engaged": True, "loaded": True, "error": None}, (True, False), "error"),
            ({"engaged": True, "loaded": False, "error": None}, (True, None), "error"),
            ({"engaged": False, "loaded": None, "error": None}, (True, None), "none"),
            ({"engaged": False, "loaded": False, "released": "2026-11-23T18:20:00Z"}, (True, False), "released"),
            ({"engaged": False, "loaded": True, "error": None}, (True, None), "error"),
            ({"engaged": "yes"}, (True, None), "error"),
        ]
        for doc, (ks_live, panic_live), state in cases:
            with self.subTest(doc=doc):
                self.panic(doc)
                self.write("live.json", json.dumps({"killswitch": {"available": True, "table_present": ks_live},
                                                    "panic": {"available": True, "table_present": panic_live}}))
                self.assertEqual(self.dump()["panic"]["state"], state)

    def test_panic_options(self):
        cfg = self.dump()["panic"]["config"]
        self.assertEqual((cfg["rfkill"], cfg["lockSession"], cfg["clearClipboard"], cfg["source"]), (True, True, True, "default"))
        self.write("etc/panic.json", json.dumps({"rfkill": False, "clear_clipboard": False}))
        cfg = self.dump()["panic"]["config"]
        self.assertEqual((cfg["rfkill"], cfg["lockSession"], cfg["clearClipboard"], cfg["source"]), (False, True, False, "file"))
        # Like shadow-panic: a wrong value means the default, and says so.
        self.write("etc/panic.json", json.dumps({"rfkill": "no"}))
        cfg = self.dump()["panic"]["config"]
        self.assertTrue(cfg["rfkill"])
        self.assertIn('"rfkill"', cfg["problem"])
        self.write("etc/panic.json", "not json")
        cfg = self.dump()["panic"]["config"]
        self.assertTrue(cfg["rfkill"] and cfg["lockSession"] and cfg["clearClipboard"])
        self.assertIn("not valid JSON", cfg["problem"])

    def test_hide_me_holders(self):
        self.status(ON_TOR)
        self.write("anon.json", json.dumps({"killswitch_by": ["ghost", "proxy", "bogus", 3]}))
        self.assertEqual(self.dump()["killSwitch"]["hideMe"], ["ghost", "proxy"])
        self.write("anon.json", "garbage")
        self.assertEqual(self.dump()["killSwitch"]["hideMe"], [])


class Actions(Base):
    """What the page's controls run, through cckillswitch."""

    def ks_calls(self):
        return [c for c in self.calls() if c.startswith("pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"])
                or c.startswith("pkexec " + self.tools["SHADOW_PANIC_TOOL"])]

    def test_turning_on_is_saved_for_start_up_by_default(self):
        self.status(OFF)
        out = self.harness("set:tor")
        self.assertEqual(out[0]["result"], "succeeded")
        self.assertIn("also after a restart", out[0]["message"])
        self.assertEqual(self.ks_calls(), ["pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"] + " set tor --no-lan"])
        self.assertIn("killswitch set tor --no-lan", self.calls())
        ks = out[0]["backend"]["killSwitch"]
        self.assertEqual((ks["state"], ks["mode"], ks["persistentMode"]), ("on", "tor", "tor"))

    def test_keep_off_makes_changes_runtime(self):
        self.status(OFF)
        out = self.harness("keep:0", "set:vpn")
        self.assertEqual(out[0]["result"], "")                    # no command for switching it off
        self.assertFalse(out[0]["backend"]["keepAfterRestart"])
        self.assertEqual(self.ks_calls(), ["pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"] + " set vpn --no-lan --runtime"])
        ks = out[1]["backend"]["killSwitch"]
        self.assertEqual((ks["mode"], ks["persistentMode"], ks["runtimeOverride"]), ("vpn", "off", True))
        # The state now has a runtime mode: the switch follows it.
        self.assertFalse(out[1]["backend"]["keepAfterRestart"])

    def test_keep_on_saves_the_runtime_mode_in_force(self):
        self.status(dict(ON_TOR, runtime_override=True, persistent_mode="off", lan=True))
        out = self.harness("show", "keep:1")
        self.assertFalse(out[0]["backend"]["keepAfterRestart"])
        self.assertEqual(self.ks_calls(), ["pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"] + " set tor --lan"])
        self.assertEqual(out[1]["backend"]["killSwitch"]["persistentMode"], "tor")
        self.assertTrue(out[1]["backend"]["keepAfterRestart"])

    def test_lan(self):
        self.status(OFF)
        out = self.harness("lan:1")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(self.ks_calls(), [])
        self.status(ON_TOR)
        out = self.harness("lan:1")
        self.assertEqual(out[0]["result"], "succeeded")
        self.assertEqual(self.ks_calls(), ["pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"] + " set tor --lan"])
        self.assertTrue(out[0]["backend"]["killSwitch"]["lan"])

    def test_tool_failure_shows_the_tool_message_without_warnings(self):
        self.status(ON_TOR)
        self.env_extra = {"FAKE_KS_EXIT": "1",
                          "FAKE_KS_STDERR": "shadow-killswitch: could not change to mode vpn: nft --check failed\n"
                                            "shadow-killswitch: warning: NetworkManager could not be asked\n"}
        out = self.harness("set:vpn")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], "could not change to mode vpn: nft --check failed")
        self.assertEqual(out[0]["backend"]["killSwitch"]["mode"], "tor")

    def test_authorisation_refused(self):
        self.status(ON_TOR)
        self.env_extra = {"FAKE_PKEXEC_EXIT": "126"}
        out = self.harness("set:off")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], "The change was not authorised.")
        self.assertNotIn("killswitch set off --no-lan", self.calls())
        self.assertEqual(out[0]["backend"]["killSwitch"]["state"], "on")
        self.env_extra = {"FAKE_PKEXEC_EXIT": "127"}
        out = self.harness("set:off")
        self.assertIn("no authentication agent", out[0]["message"])

    def test_reassert_and_release(self):
        self.status(dict(ON_TOR, loaded=False))
        self.panic({"engaged": True, "loaded": True, "error": None, "since": "2026-11-23T18:10:00Z"})
        out = self.harness("reassert", "release")
        self.assertEqual([o["result"] for o in out], ["succeeded", "succeeded"])
        self.assertEqual(self.ks_calls(), ["pkexec " + self.tools["SHADOW_KILLSWITCH_TOOL"] + " reassert",
                                           "pkexec " + self.tools["SHADOW_PANIC_TOOL"] + " release"])
        self.assertEqual(out[0]["backend"]["killSwitch"]["state"], "on")
        self.assertEqual(out[1]["backend"]["panic"]["state"], "released")
        self.env_extra = {"FAKE_PANIC_EXIT": "1"}
        out = self.harness("release")
        self.assertEqual(out[0]["result"], "failed")
        self.assertEqual(out[0]["message"], "NetworkManager networking was not turned back on: nmcli failed")

    def test_load_again_is_not_reported_as_done_while_the_problem_stays(self):
        # "Load again" runs reassert, which exits 0 when the kernel already has the table;
        # the page must not say it worked while the status still has its error (review r1-14)
        problem = "/etc/shadow-os/killswitch.json cannot be used: Expecting value"
        self.status(dict(ON_TOR, error=problem))
        self.env_extra = {"FAKE_KS_REASSERT_KEEPS": "1"}
        out = self.harness("reassert")
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("still reports a problem", out[0]["message"])
        self.assertIn(problem, out[0]["message"])
        self.assertEqual(out[0]["backend"]["killSwitch"]["state"], "error")

    def test_refused_before_anything_runs(self):
        self.status(OFF)
        out = self.harness("set:everything", "set:tor", tools=False)
        self.assertEqual(out[0]["result"], "failed")
        self.assertIn("Unknown kill switch mode", out[0]["message"])
        self.assertEqual(out[1]["result"], "failed")
        self.assertIn("is not installed", out[1]["message"])
        self.assertEqual(self.ks_calls(), [])

    def test_follows_changes_made_by_other_programs(self):
        # The tools replace the file (rename); the directory watcher must see it.
        self.status(OFF)
        if not (HARNESS and os.access(HARNESS, os.X_OK)):
            self.skipTest("SHADOW_CC_HARNESS (cckillswitch) not given")
        proc = subprocess.Popen(self.command([HARNESS, "show", "wait:2500", "show"]), env=self.env(),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(1.0)
        tmp = os.path.join(self.run_dir, ".killswitch.json.new")
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(ON_TOR, fh)
        os.chmod(tmp, 0o666)
        os.replace(tmp, os.path.join(self.run_dir, "killswitch.json"))
        out, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, out + err)
        lines = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
        self.assertEqual([l["backend"]["killSwitch"]["state"] for l in lines], ["off", "on", "on"])

    def test_which_changes_close_connections(self):
        # shadow-killswitch flushes when it engages or a mode allows less.
        self.status(OFF)
        out = self.harness("closes:tor:0", "closes:off:0")
        self.assertEqual([o["closes"] for o in out], [True, False])
        self.status(ON_TOR)
        out = self.harness("closes:tor-or-vpn:0", "closes:tor:1", "closes:vpn:0", "closes:tor:0")
        self.assertEqual([o["closes"] for o in out], [False, False, True, False])
        self.status(dict(ON_TOR, mode="tor-or-vpn", lan=True, persistent_mode="tor-or-vpn"))
        out = self.harness("closes:tor:1", "closes:tor-or-vpn:0")
        self.assertEqual([o["closes"] for o in out], [True, True])
        self.status(dict(ON_TOR, loaded=False))
        self.assertEqual(self.harness("closes:tor:0")[0]["closes"], True)


CONTRACT = os.environ.get("SHADOW_CC_CONTRACT_DIR", "")


@unittest.skipUnless(CONTRACT and os.path.isdir(CONTRACT), "SHADOW_CC_CONTRACT_DIR (documents of the real tools) not given")
class RealToolDocuments(Base):
    """The documents the REAL shadow-killswitch, shadow-panic and shadow-security-status
    published in a throw-away network namespace (see the README of this directory):
    on/ (kill switch tor, panic engaged), off/ (both off), stale/ (mode vpn recorded as
    loaded, table then deleted behind the tool's back)."""

    def use(self, name):
        for doc in ("killswitch.json", "panic.json"):
            with open(os.path.join(CONTRACT, name, doc), encoding="utf-8") as fh:
                self.write(os.path.join("run", doc), fh.read())
        with open(os.path.join(CONTRACT, name, "live.json"), encoding="utf-8") as fh:
            self.write("live.json", fh.read())
        return self.dump()

    def test_on(self):
        d = self.use("on")
        ks, pn, live = d["killSwitch"], d["panic"], d["liveCheck"]
        self.assertEqual(ks["problem"], "")
        self.assertEqual((ks["state"], ks["mode"], ks["protected"]), ("on", "tor", True))
        self.assertEqual((ks["runtimeOverride"], ks["persistentMode"]), (True, "off"))
        self.assertEqual(pn["problem"], "")
        self.assertEqual((pn["state"], pn["cut"]), ("engaged", True))
        self.assertEqual((live["state"], live["killSwitchTable"], live["panicTable"]), ("done", True, True))

    def test_off(self):
        d = self.use("off")
        self.assertEqual((d["killSwitch"]["state"], d["killSwitch"]["problem"]), ("off", ""))
        self.assertEqual((d["panic"]["state"], d["panic"]["problem"]), ("released", ""))
        self.assertEqual((d["liveCheck"]["killSwitchTable"], d["liveCheck"]["panicTable"]), (False, False))

    def test_table_removed_behind_the_tools_back(self):
        d = self.use("stale")
        ks = d["killSwitch"]
        self.assertTrue(ks["loaded"])                       # what the file still says
        self.assertEqual(ks["state"], "error")
        self.assertFalse(ks["protected"])
        self.assertIn("kernel has no such table", ks["errorText"])


class AsRoot(Base):
    """Root runs the tools directly, without pkexec."""
    as_user = False

    def setUp(self):
        if os.geteuid() != 0:
            self.skipTest("needs root")
        super().setUp()

    def test_direct(self):
        self.status(OFF)
        out = self.harness("set:tor")
        self.assertEqual(out[0]["result"], "succeeded")
        self.assertIn("killswitch set tor --no-lan", self.calls())
        self.assertFalse([c for c in self.calls() if c.startswith("pkexec")])


if __name__ == "__main__":
    unittest.main()
