#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hide Me (shadow-anon): the kill switch delegate, Ghost Mode's order, the
transparent proxy's rules, the MAC address and the timers.

    python3 tests/security/test_shadow_anon_killswitch.py [-v]

The fakes are programs on PATH, so shadow-anon's real sh()/run() code paths
and exit-status checks run:

  shadow-killswitch  the CLI and status JSON contract of the real tool
                     (status --json; set MODE [--lan|--no-lan] [--runtime]),
                     with its state in a file; it can refuse a change, or
                     claim success without loading (tests that the status is
                     checked, not only the exit status)
  iptables/ip6tables a recorder that keeps chains (filter and nat)
  nmcli              one connected Ethernet device, profile properties
  systemctl          unit activity; records restarts
  tor, rfkill, …     harmless stand-ins for every other program Ghost Mode runs

PATH contains ONLY the fake directory: a program without a fake is "not
found", never the computer's own. Every absolute path constant of the tool
points into a temporary directory (shadow_anon_testlib checks this).
Any user; standard library only.
"""
import io
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_anon_testlib as lib  # noqa: E402

UUID = "0f3c2a4e-5b6d-4e7f-8a9b-0c1d2e3f4a5b"

HEAD = r'''#!{python}
import json, os, sys
STATE = {state!r}
PROG = os.path.basename(sys.argv[0])
def load(name, default):
    try:
        with open(os.path.join(STATE, name)) as f:
            return json.load(f)
    except FileNotFoundError:
        return default
def save(name, obj):
    with open(os.path.join(STATE, name), "w") as f:
        json.dump(obj, f)
with open(os.path.join(STATE, "calls.jsonl"), "a") as f:
    f.write(json.dumps([PROG] + sys.argv[1:]) + "\n")
for pattern in load("fail.json", {{}}).get(PROG, []):
    if pattern in " ".join(sys.argv[1:]):
        print(PROG + ": injected failure", file=sys.stderr)
        sys.exit(1)
'''

FAKE_KILLSWITCH = r'''
DEFAULT = {"mode": "off", "lan": False, "persistent_mode": "off", "runtime_override": False,
           "loaded": False, "error": None, "escape_hatch": False}
ks = load("ks.json", DEFAULT)
behaviour = load("ks-behaviour.json", {})
a = sys.argv[1:]
if a[:2] == ["status", "--json"]:
    st = dict(ks, active=ks["mode"] != "off", version=1)
    print(json.dumps(st))
    sys.exit(3 if st["error"] else 0)
if a[:1] == ["set"] and a[1] in ("off", "tor", "vpn", "tor-or-vpn"):
    mode = a[1]
    if behaviour.get("refuse"):
        # like the real tool: the previous mode stays, the status names the error
        ks["error"] = "could not change to mode %s: the ruleset was rejected by nft --check" % mode
        save("ks.json", ks)
        print("shadow-killswitch: " + ks["error"], file=sys.stderr)
        sys.exit(1)
    lan = True if "--lan" in a else False if "--no-lan" in a else ks["lan"]
    ks.update(mode=mode, lan=lan, runtime_override="--runtime" in a, escape_hatch=False, error=None)
    if "--runtime" not in a:
        ks["persistent_mode"] = mode
    ks["loaded"] = mode != "off" and not behaviour.get("not_loaded")
    save("ks.json", ks)
    sys.exit(0)
print("fake shadow-killswitch: unhandled " + " ".join(a), file=sys.stderr)
sys.exit(2)
'''

FAKE_IPTABLES = r'''
fam = "v6" if PROG.startswith("ip6") else "v4"
db = load("ipt.json", {})
t = db.setdefault(fam, {})
a = sys.argv[1:]
table = "filter"
if a[:1] == ["-t"]:
    table, a = a[1], a[2:]
T = t.setdefault(table, {"chains": {}, "OUTPUT": []})
chains, hooks = T["chains"], T["OUTPUT"]
op, rc, out, err = a[0], 0, "", ""
if op == "-N":
    if a[1] in chains:
        rc, err = 1, "iptables: Chain already exists."
    else:
        chains[a[1]] = []
elif op == "-F":
    if a[1] in chains:
        chains[a[1]] = []
    else:
        rc, err = 1, "iptables: No chain/target/match by that name."
elif op == "-X":
    if a[1] in chains and a[1] not in hooks:
        del chains[a[1]]
    else:
        rc, err = 1, "iptables: Too many links."
elif op == "-A" and a[1] == "OUTPUT":
    hooks.append(a[-1])
elif op == "-A":
    if a[1] in chains:
        chains[a[1]].append(" ".join(a[2:]))
    else:
        rc, err = 1, "iptables: No chain/target/match by that name."
elif op == "-I" and a[1] == "OUTPUT":
    hooks.insert(0, a[-1])
elif op == "-C" and a[1] == "OUTPUT":
    rc = 0 if a[-1] in hooks else 1
elif op == "-D" and a[1] == "OUTPUT":
    if a[-1] in hooks:
        hooks.remove(a[-1])
    else:
        rc, err = 1, "iptables: Bad rule (does a matching rule exist in that chain?)."
elif op == "-S" and a[1] == "OUTPUT":
    out = "\n".join(["-P OUTPUT ACCEPT"] + ["-A OUTPUT -j " + h for h in hooks])
elif op == "-S":
    if a[1] in chains:
        out = "\n".join(["-N " + a[1]] + ["-A %s %s" % (a[1], r) for r in chains[a[1]]])
    else:
        rc, err = 1, "iptables: No chain/target/match by that name."
else:
    rc, err = 2, "fake iptables: unhandled " + " ".join(a)
save("ipt.json", db)
if out:
    print(out)
if err:
    print(err, file=sys.stderr)
sys.exit(rc)
'''

FAKE_NMCLI = r'''
nm = load("nm.json", {})
a = sys.argv[1:]
props = nm.setdefault("props", {})
if a[:2] == ["-g", "GENERAL.STATE"]:
    print(nm.get("devices", {}).get(a[-1], {}).get("state", "10 (unmanaged)"))
elif a[:5] == ["-t", "-f", "UUID,DEVICE", "connection", "show"]:
    for dev, d in nm.get("devices", {}).items():
        if d.get("uuid"):
            print(d["uuid"] + ":" + dev)
elif a[0] == "-g" and a[2:4] == ["connection", "show"]:
    print(props.get(a[4], {}).get(a[1], ""))
elif a[:3] == ["connection", "modify", "--temporary"]:
    p = props.setdefault(a[3], {})
    rest = a[4:]
    for k, v in zip(rest[0::2], rest[1::2]):
        p[k] = v
    save("nm.json", nm)
sys.exit(0)
'''

FAKE_SYSTEMCTL = r'''
cfg = load("systemctl.json", {"active": ["tor@default"], "enabled": []})
a = [x for x in sys.argv[1:] if x not in ("--quiet", "--no-block")]
if a[0] == "is-active":
    if a[-1] in cfg["active"]:
        print("active")
        sys.exit(0)
    print("inactive")
    sys.exit(3)
if a[0] == "is-enabled":
    sys.exit(0 if a[-1] in cfg["enabled"] else 1)
if a[0] == "is-system-running":                 # a running system unless a test says "starting"
    print(cfg.get("system", "running"))
    sys.exit(0 if cfg.get("system", "running") == "running" else 1)
sys.exit(0)
'''

FAKE_RFKILL = r'''
print("0: hci0: Bluetooth\n\tSoft blocked: no\n\tHard blocked: no")
'''

# "resolvectl dns": the global servers resolved builds from the shipped
# 80-shadow-privacy.conf and the drop-ins under the test root (its rules: a
# DNS= value adds to the list, an empty one clears it).
FAKE_RESOLVECTL = r'''
cfg = load("resolved.json", None)
if sys.argv[1:] == ["dns"] and cfg:
    sys.dont_write_bytecode = True
    sys.path.insert(0, cfg["lib"])
    import shadow_anon_testlib
    print("Global: " + " ".join(shadow_anon_testlib.resolved_global_dns(cfg["root"])))
    print("Link 2 (eth0): 192.0.2.1")
sys.exit(0)
'''

FAKE_TRUE = "\nsys.exit(0)\n"

FAKES = {"shadow-killswitch": FAKE_KILLSWITCH, "iptables": FAKE_IPTABLES, "ip6tables": FAKE_IPTABLES,
         "nmcli": FAKE_NMCLI, "systemctl": FAKE_SYSTEMCTL, "rfkill": FAKE_RFKILL, "resolvectl": FAKE_RESOLVECTL}
HARMLESS = ("tor", "hostnamectl", "timedatectl", "sysctl", "journalctl", "find", "udevadm", "modprobe",
            "chattr", "hostname", "ip", "curl", "torsocks")


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


class HideMeTest(unittest.TestCase):
    resolved = True

    def setUp(self):
        self.tool = t = lib.load_tool()
        self.dir = tempfile.mkdtemp(prefix="shadow-anon-ks-")
        self.root = os.path.join(self.dir, "root")
        self.state_dir = os.path.join(self.dir, "fake-state")
        self.bin = os.path.join(self.dir, "bin")
        for d in (self.root, self.state_dir, self.bin):
            os.makedirs(d)
        lib.redirect_paths(t, self.root)
        for name in list(FAKES) + list(HARMLESS):
            body = FAKES.get(name, FAKE_TRUE)
            path = os.path.join(self.bin, name)
            with open(path, "w") as f:
                f.write(HEAD.format(python=sys.executable, state=self.state_dir) + body)
            os.chmod(path, 0o755)
        # one connected Ethernet interface managed by NetworkManager
        os.makedirs(os.path.join(t.SYS_NET, "eth0"))
        with open(os.path.join(t.SYS_NET, "eth0", "address"), "w") as f:
            f.write("02:00:00:00:00:01\n")
        self.put("nm.json", {"devices": {"eth0": {"uuid": UUID, "state": "100 (connected)"}}, "props": {}})
        if self.resolved:
            with open(t.RESOLVED_DAEMON, "w") as f:
                f.write("")
            os.symlink(t.STUB_TARGET, t.RESOLV)
            self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
            self.put("resolved.json", {"root": self.root, "lib": lib.HERE})
        self.clock = Clock()
        self.patches = [
            mock.patch.dict(os.environ, {"PATH": self.bin}),
            mock.patch.object(t, "need_root", lambda: None),
            mock.patch.object(t, "user_exists", lambda name: True),
            mock.patch.object(t, "_boottime", self.clock),
            mock.patch.object(t.time, "sleep", lambda s: None),
        ]
        for p in self.patches:
            p.start()
        self.out = io.StringIO()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    # ---- helpers
    def put(self, name, obj):
        with open(os.path.join(self.state_dir, name), "w") as f:
            json.dump(obj, f)

    def get(self, name, default=None):
        try:
            with open(os.path.join(self.state_dir, name)) as f:
                return json.load(f)
        except FileNotFoundError:
            return default

    def calls(self):
        try:
            with open(os.path.join(self.state_dir, "calls.jsonl")) as f:
                return [json.loads(line) for line in f]
        except FileNotFoundError:
            return []

    def index(self, pred, calls=None):
        for i, c in enumerate(self.calls() if calls is None else calls):
            if pred(c):
                return i
        return None

    def main(self, *argv):
        lib.check_redirected(self.tool, self.root)
        err = io.StringIO()
        with redirect_stdout(self.out), mock.patch("sys.stderr", err):
            rc = self.tool.main(list(argv))
        self.stderr = err.getvalue()
        return rc

    def state(self):
        with open(self.tool.STATE_FILE) as f:
            return json.load(f)

    def ks(self):
        return self.get("ks.json", {"mode": "off", "persistent_mode": "off", "runtime_override": False})

    def nat(self, chain):
        return self.get("ipt.json", {}).get("v4", {}).get("nat", {}).get("chains", {}).get(chain)

    def hooks(self, fam="v4", table="filter"):
        return self.get("ipt.json", {}).get(fam, {}).get(table, {}).get("OUTPUT", [])

    def nmcli_changes(self):
        return [c for c in self.calls() if c[0] == "nmcli" and c[1:2] in (["connection"], ["device"], ["--wait"])]


class GhostMode(HideMeTest):
    def test_the_kill_switch_comes_first(self):
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        calls = self.calls()
        ks = self.index(lambda c: c[:3] == ["shadow-killswitch", "set", "tor"], calls)
        self.assertIsNotNone(ks)
        self.assertEqual(calls[ks], ["shadow-killswitch", "set", "tor", "--no-lan", "--runtime"])
        later = {
            "MAC": lambda c: c[0] == "nmcli" and ("modify" in c or "down" in c or "up" in c),
            "Tor": lambda c: c[:2] == ["systemctl", "restart"] and c[-1].startswith("tor"),
            "proxy": lambda c: c[0] == "iptables" and "SHADOWTOR" in c,
            "DNS": lambda c: c[0] == "iptables" and "SHADOWDNS" in c,
        }
        for name, pred in later.items():
            i = self.index(pred, calls)
            self.assertIsNotNone(i, f"Ghost Mode did no {name} step")
            self.assertLess(ks, i, f"the {name} step ran before the kill switch")
        st = self.state()
        self.assertTrue(st["ghost_mode"] and st["kill_switch"] and st["transparent_proxy"] and st["dns_protected"])
        self.assertEqual(st["threat_model"], "maximum")
        self.assertIsNone(st["error"])
        self.assertEqual(self.ks()["mode"], "tor")
        self.assertTrue(self.ks()["runtime_override"])          # not saved for start-up

    def test_persist_saves_the_kill_switch_for_start_up(self):
        self.assertEqual(self.main("ghost-on", "--persist"), 0, self.stderr)
        self.assertIn(["shadow-killswitch", "set", "tor", "--no-lan"], self.calls())
        self.assertEqual(self.ks()["persistent_mode"], "tor")

    def test_a_refused_kill_switch_stops_ghost_mode_and_is_reported(self):
        self.put("ks-behaviour.json", {"refuse": True})
        self.assertEqual(self.main("ghost-on"), 3)
        self.assertIn("kill switch", self.stderr)
        st = self.state()
        self.assertFalse(st["ghost_mode"])
        self.assertFalse(st["kill_switch"])
        self.assertIn("kill switch could not be engaged", st["error"])
        self.assertIn("nft --check", st["error"])
        calls = self.calls()
        self.assertIsNone(self.index(lambda c: c[0] == "iptables" and "SHADOWTOR" in c and "-A" in c, calls))
        self.assertFalse(self.nmcli_changes())                    # nothing after the failed first step
        self.assertIsNone(self.index(lambda c: c[:2] == ["systemctl", "restart"], calls))

    def test_a_kill_switch_that_is_not_loaded_is_not_reported_as_on(self):
        # The tool exits 0 but its status says the table is not loaded.
        self.put("ks-behaviour.json", {"not_loaded": True})
        self.assertEqual(self.main("ghost-on"), 3)
        st = self.state()
        self.assertFalse(st["kill_switch"])
        self.assertFalse(st["ghost_mode"])
        self.assertIn("could not be engaged", st["error"])
        self.assertEqual(self.main("status", "--json"), 0)

    def test_a_failure_after_the_kill_switch_keeps_it_on(self):
        self.put("fail.json", {"iptables": ["REDIRECT --to-ports 40"]})
        self.assertEqual(self.main("ghost-on"), 3)
        st = self.state()
        self.assertFalse(st["ghost_mode"])
        self.assertTrue(st["kill_switch"])                        # fail closed
        self.assertIn("Ghost Mode is NOT on", st["error"])
        self.assertEqual(self.ks()["mode"], "tor")
        self.assertIsNone(self.nat("SHADOWTOR"))                  # the half-built chain was removed
        self.assertNotIn("SHADOWTOR", self.hooks(table="nat"))

    def test_off_restores_everything_and_the_previous_kill_switch_mode(self):
        self.put("ks.json", {"mode": "vpn", "lan": True, "persistent_mode": "vpn", "runtime_override": False,
                             "loaded": True, "error": None, "escape_hatch": False})
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.assertEqual(self.ks()["mode"], "tor")
        self.assertEqual(self.main("ghost-off"), 0, self.stderr)
        ks = self.ks()
        self.assertEqual((ks["mode"], ks["lan"], ks["persistent_mode"], ks["runtime_override"]),
                         ("vpn", True, "vpn", False))
        self.assertIn(["shadow-killswitch", "set", "vpn", "--lan"], self.calls())
        self.assertIsNone(self.nat("SHADOWTOR"))
        self.assertIsNone(self.nat("SHADOWDNS"))
        self.assertFalse(os.path.exists(self.tool.RESOLVED_DROPIN))
        st = self.state()
        self.assertFalse(st["ghost_mode"] or st["kill_switch"] or st["transparent_proxy"] or st["dns_protected"])
        self.assertNotIn("killswitch_before", st)
        # the proxy and DNS are undone before the kill switch
        calls = self.calls()
        restore = self.index(lambda c: c[:3] == ["shadow-killswitch", "set", "vpn"], calls)
        self.assertLess(self.index(lambda c: c[:5] == ["iptables", "-t", "nat", "-D", "OUTPUT"], calls), restore)

    def test_threat_maximum_never_touches_the_mac_address(self):
        self.assertEqual(self.main("threat", "maximum"), 0, self.stderr)
        self.assertTrue(self.state()["ghost_mode"])
        self.assertFalse(self.nmcli_changes())
        self.assertFalse(os.path.exists(self.tool.MAC_SALT))


class KillSwitchDelegate(HideMeTest):
    resolved = False

    def ks_state(self, mode, persistent, runtime=False, hatch=False, lan=False):
        self.put("ks.json", {"mode": mode, "lan": lan, "persistent_mode": persistent, "runtime_override": runtime,
                             "loaded": mode != "off", "error": None, "escape_hatch": hatch})

    def test_on_then_off_from_off(self):
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.assertEqual(self.ks()["mode"], "tor")
        self.assertTrue(self.state()["kill_switch"])
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        ks = self.ks()
        self.assertEqual((ks["mode"], ks["persistent_mode"], ks["runtime_override"]), ("off", "off", False))
        self.assertFalse(self.state()["kill_switch"])

    def test_persisted_on_then_off(self):
        self.assertEqual(self.main("kill-switch-on", "--persist"), 0, self.stderr)
        self.assertEqual(self.ks()["persistent_mode"], "tor")
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        self.assertEqual(self.ks()["persistent_mode"], "off")

    def test_escape_hatch_boot_keeps_the_saved_mode(self):
        # shadow.killswitch=off at boot: off in force, "tor" saved for start-up.
        self.ks_state("off", "tor", hatch=True)
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        self.assertIn(["shadow-killswitch", "set", "off", "--runtime", "--no-lan"], self.calls())
        self.assertEqual((self.ks()["mode"], self.ks()["persistent_mode"]), ("off", "tor"))

    def test_a_saved_lan_setting_is_not_kept_in_tor_mode(self):
        # Regression: "set tor" without a flag keeps the LAN setting saved
        # earlier ("vpn --lan" for a printer), so Hide Me's "Tor only" let the
        # private ranges and multicast out directly.
        for persist in ((), ("--persist",)):
            with self.subTest(persist=persist):
                self.ks_state("vpn", "vpn", lan=True)
                if os.path.exists(os.path.join(self.state_dir, "calls.jsonl")):
                    os.unlink(os.path.join(self.state_dir, "calls.jsonl"))
                self.assertEqual(self.main("kill-switch-on", *persist), 0, self.stderr)
                self.assertIn(["shadow-killswitch", "set", "tor", "--no-lan"] + ([] if persist else ["--runtime"]),
                              self.calls())
                self.assertEqual((self.ks()["mode"], self.ks()["lan"]), ("tor", False))
                self.assertTrue(self.state()["kill_switch"])
                self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
                self.assertEqual((self.ks()["mode"], self.ks()["lan"], self.ks()["persistent_mode"]),
                                 ("vpn", True, "vpn"))
                self.assertFalse(self.state()["kill_switch"])

    def test_tor_with_the_lan_is_not_tor_only(self):
        # Tor mode with the local network allowed, set in the Control Center:
        # not taken as Hide Me's "Tor only" (state, status), and engaging sets
        # it strict; turning it off puts the LAN setting back.
        self.ks_state("tor", "tor", lan=True)
        self.assertFalse(self.tool.killswitch_tor_active(self.tool.killswitch_status()))
        self.assertFalse(self.tool.real_state()["kill_switch"])
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.assertIn(["shadow-killswitch", "set", "tor", "--no-lan", "--runtime"], self.calls())
        self.assertEqual((self.ks()["mode"], self.ks()["lan"]), ("tor", False))
        self.assertTrue(self.state()["kill_switch"])
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        self.assertEqual((self.ks()["mode"], self.ks()["lan"], self.ks()["runtime_override"]), ("tor", True, False))

    def test_a_mode_changed_elsewhere_is_left_alone(self):
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.ks_state("vpn", "vpn")                     # changed in the Control Center meanwhile
        before = len(self.calls())
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        self.assertFalse([c for c in self.calls()[before:] if c[:2] == ["shadow-killswitch", "set"]])
        self.assertEqual(self.ks()["mode"], "vpn")

    def test_off_failure_is_reported(self):
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.put("ks-behaviour.json", {"refuse": True})
        self.assertEqual(self.main("kill-switch-off"), 3)
        st = self.state()
        self.assertTrue(st["kill_switch"])              # still on, and reported as on
        self.assertIn("shadow-killswitch set off", st["error"])

    def test_a_missing_shadow_killswitch_is_reported(self):
        os.unlink(os.path.join(self.bin, "shadow-killswitch"))
        self.assertEqual(self.main("kill-switch-on"), 3)
        self.assertIn("not installed", self.state()["error"])
        self.assertFalse(self.state()["kill_switch"])

    def test_restore_plan(self):
        plan = self.tool.killswitch_restore_plan
        ks = "shadow-killswitch"

        def before(mode, saved, runtime=False, hatch=False, lan=False):
            return {"mode": mode, "persistent_mode": saved, "runtime_override": runtime, "escape_hatch": hatch,
                    "lan": lan}
        now = {"mode": "tor", "persistent_mode": "off", "runtime_override": True}
        self.assertEqual(plan(before("off", "off"), now, False), [[ks, "set", "off", "--no-lan"]])
        self.assertEqual(plan(before("vpn", "vpn", lan=True), now, False), [[ks, "set", "vpn", "--lan"]])
        self.assertEqual(plan(before("tor-or-vpn", "off", runtime=True), now, False),
                         [[ks, "set", "tor-or-vpn", "--runtime", "--no-lan"]])
        self.assertEqual(plan(before("tor-or-vpn", "vpn", runtime=True), now, True),
                         [[ks, "set", "vpn", "--no-lan"], [ks, "set", "tor-or-vpn", "--runtime", "--no-lan"]])
        self.assertEqual(plan(before("off", "tor", hatch=True), now, False),
                         [[ks, "set", "off", "--runtime", "--no-lan"]])
        # nothing recorded: back to the saved mode, or off
        self.assertEqual(plan(None, {"mode": "tor", "persistent_mode": "vpn", "runtime_override": True}, False),
                         [[ks, "set", "vpn"]])
        self.assertEqual(plan(None, {"mode": "tor", "persistent_mode": "tor", "runtime_override": False}, False),
                         [[ks, "set", "off"]])

    def test_old_iptables_kill_switch_is_replaced_at_boot_without_a_gap(self):
        ipt = {"v4": {"filter": {"chains": {"SHADOWKILL": ["-j DROP"]}, "OUTPUT": ["SHADOWKILL"]}},
               "v6": {"filter": {"chains": {"SHADOWKILL": ["-j DROP"]}, "OUTPUT": ["SHADOWKILL"]}}}
        self.put("ipt.json", ipt)
        self.assertEqual(self.main("boot"), 0, self.stderr)
        calls = self.calls()
        engage = self.index(lambda c: c[:3] == ["shadow-killswitch", "set", "tor"], calls)
        unhook = self.index(lambda c: c[1:] == ["-D", "OUTPUT", "-j", "SHADOWKILL"], calls)
        self.assertIsNotNone(engage)
        self.assertLess(engage, unhook)
        for fam in ("v4", "v6"):
            self.assertEqual(self.hooks(fam), [])
            self.assertNotIn("SHADOWKILL", self.get("ipt.json")[fam]["filter"]["chains"])
        self.assertTrue(self.state()["kill_switch"])

    def test_old_kill_switch_stays_when_the_new_one_cannot_engage(self):
        ipt = {"v4": {"filter": {"chains": {"SHADOWKILL": ["-j DROP"]}, "OUTPUT": ["SHADOWKILL"]}}}
        self.put("ipt.json", ipt)
        self.put("ks-behaviour.json", {"refuse": True})
        self.assertEqual(self.main("boot"), 3)
        self.assertEqual(self.hooks("v4"), ["SHADOWKILL"])        # fail closed
        self.assertIn("could not be engaged", self.state()["error"])

    def test_unhooked_leftover_chain_is_removed_without_engaging(self):
        self.put("ipt.json", {"v4": {"filter": {"chains": {"SHADOWKILL": []}, "OUTPUT": []}}})
        self.assertEqual(self.main("boot"), 0, self.stderr)
        self.assertNotIn("SHADOWKILL", self.get("ipt.json")["v4"]["filter"]["chains"])
        self.assertFalse([c for c in self.calls() if c[:2] == ["shadow-killswitch", "set"]])


class TransparentProxy(HideMeTest):
    def rules(self):
        return self.nat("SHADOWTOR")

    def test_onion_range_is_redirected_before_the_private_ranges_return(self):
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        rules = self.rules()
        onion = [i for i, r in enumerate(rules) if r.startswith("-d 10.192.0.0/10")]
        ten = [i for i, r in enumerate(rules) if r.startswith("-d 10.0.0.0/8")]
        self.assertEqual(len(onion), 1)
        self.assertEqual(rules[onion[0]], "-d 10.192.0.0/10 -p tcp --syn -j REDIRECT --to-ports 40")
        self.assertEqual(rules[ten[0]], "-d 10.0.0.0/8 -j RETURN")
        self.assertLess(onion[0], ten[0])
        self.assertEqual(rules[-1], "-p tcp --syn -j REDIRECT --to-ports 40")
        self.assertEqual(rules[0], "-m owner --uid-owner debian-tor -j RETURN")
        self.assertEqual(self.hooks(table="nat"), ["SHADOWTOR"])

    def test_dns_over_tcp_is_refused_not_sent_to_the_dnsport(self):
        """Regression: TCP port 53 went to TCP 127.0.0.1:5353, where Tor does
        not listen (its DNSPort is UDP only) and any local user could."""
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        rules = self.rules()
        self.assertIn("-p udp --dport 53 -j REDIRECT --to-ports 26", rules)
        self.assertIn("-p tcp --dport 53 -j REDIRECT --to-ports 1", rules)
        self.assertFalse([r for r in rules if "-p tcp" in r and r.endswith("--to-ports 26")])

    def test_tors_ports_are_privileged_and_kept_across_reloads(self):
        """Regression: the TransPort was TCP 127.0.0.1:9040 and the DNSPort
        UDP 127.0.0.1:5353, which any user may bind: next to Tor's DNSPort
        (both SO_REUSEADDR) while Tor runs, and either one first while Tor
        restarts (it then cannot start) - every redirected connection and
        query reached that user. Both are below 53 now; Tor binds them as
        root and keeps the capability for reloads."""
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        for port in ("40", "26"):
            self.assertLess(int(port), 53)
        self.assertFalse([r for r in self.rules() if "9040" in r or "5353" in r])
        with open(self.tool.TPROXY_CF) as f:
            tproxy = f.read()
        with open(self.tool.TORDNS_CF) as f:
            dns = f.read()
        self.assertIn("TransPort 127.0.0.1:40 ", tproxy)
        self.assertIn("DNSPort 127.0.0.1:26\n", dns)
        for text in (tproxy, dns):
            self.assertIn("KeepBindCapabilities 1\n", text)
        self.assertIn(["systemctl", "restart", "tor@default"], self.calls())
        self.assertFalse([c for c in self.calls() if c[0] == "systemctl" and "reload" in " ".join(c)])

    def test_no_proxy_when_tor_does_not_start(self):
        """Regression: a user who held the TransPort when Tor restarted kept
        it (Tor: "Could not bind ... Address already in use"); "systemctl
        restart tor@default" failed, and Hide Me then restarted Debian's
        tor.service, a oneshot running /bin/true, and took its success for
        Tor's: the redirect was hooked and the proxy reported on, with every
        connection going to that user."""
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
        self.put("fail.json", {"systemctl": ["restart tor@default"]})   # "restart tor" (the oneshot) succeeds
        self.assertEqual(self.main("transparent-proxy-on"), 3)
        self.assertIn("Tor did not start", self.stderr)
        self.assertIsNone(self.rules())
        self.assertNotIn("SHADOWTOR", self.hooks(table="nat"))
        self.assertNotIn(["systemctl", "restart", "tor"], self.calls())
        self.assertFalse(self.state().get("transparent_proxy"))
        self.assertEqual(self.ks()["mode"], "off")                 # the proxy's kill switch was released

    def test_tor_failing_in_ghost_mode_is_not_reported_as_ghost_mode(self):
        """Regression: with Ghost Mode on, Tor failed to restart (the DNS
        toggle, a crash, an upgrade) and the state, "status --json" and the
        panel kept showing "all TCP and DNS go through Tor"."""
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.assertTrue(self.state()["ghost_mode"])
        # Hide Me's DNS switch while Ghost Mode is on restarts Tor, which fails.
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
        self.put("fail.json", {"systemctl": ["restart tor@default"]})
        self.assertEqual(self.main("dns-protect-on"), 3)
        st = self.state()
        self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])
        self.assertIn("Tor did not start", st["error"])
        self.assertTrue(st["kill_switch"])                         # fail closed
        self.assertIn("SHADOWTOR", self.hooks(table="nat"))        # the redirect stays: connections fail
        # Re-checked as root (what boot and "status --json" record).
        rep = self.tool.real_state()
        self.assertFalse(rep["ghost_mode"] or rep["transparent_proxy"])
        self.assertEqual(rep["threat_model"], "enhanced")
        self.assertEqual(rep["error"], self.tool.TOR_DOWN)
        self.assertEqual(self.main("boot"), 3)
        st = self.state()
        self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])
        self.assertIn(self.tool.TOR_DOWN, st["error"])
        # Tor back: in force again, Ghost Mode included (d1-03: restart_tor()
        # had overwritten what was asked for, so Ghost Mode stayed "off" and
        # the threat level "enhanced" for good, with the old TOR_DOWN error).
        os.unlink(os.path.join(self.state_dir, "fail.json"))
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
        rep = self.tool.real_state()
        self.assertTrue(rep["transparent_proxy"])
        self.assertTrue(rep["ghost_mode"])
        self.assertEqual(rep["threat_model"], "maximum")
        self.assertIsNone(rep["error"])
        self.assertEqual(self.main("boot"), 0, self.stderr)
        st = self.state()
        self.assertTrue(st["ghost_mode"] and st["transparent_proxy"] and st["kill_switch"])
        self.assertEqual(st["threat_model"], "maximum")
        self.assertIsNone(st["error"])

    def tor_down(self):
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
        self.put("fail.json", {"systemctl": ["restart tor@default"]})

    def tor_back(self):
        os.unlink(os.path.join(self.state_dir, "fail.json"))
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})

    def test_rotate_and_tor_new_report_a_tor_that_does_not_restart(self):
        """Regression (d1-03): in Ghost Mode with a Tor that does not start,
        "Rotate identity" and "tor-new" exited 0 with no error (the panel
        said "Identity rotated."), and the state said Ghost Mode was off with
        no error, while every connection still went to Tor's dead TransPort."""
        t = self.tool
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.tor_down()
        for cmd in ("tor-new", "rotate"):
            with self.subTest(cmd=cmd):
                self.assertEqual(self.main(cmd), 3, self.stderr)
                self.assertIn(t.TOR_DOWN, self.stderr)
                st = self.state()
                self.assertEqual(st["error"], t.TOR_DOWN)
                self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])
                self.assertTrue(st["kill_switch"])                     # fail closed
                self.assertIn("SHADOWTOR", self.hooks(table="nat"))
        self.tor_back()
        rep = t.real_state()
        self.assertTrue(rep["ghost_mode"] and rep["transparent_proxy"])
        self.assertEqual(rep["threat_model"], "maximum")
        self.assertIsNone(rep["error"])

    def test_an_action_after_tor_recovers_records_ghost_mode_again(self):
        """Regression (d2-02): after a failed Tor restart in Ghost Mode, only
        real_state() (status --json, status, leak-test, boot) turned
        ghost_wanted back into Ghost Mode. An action that succeeded once Tor
        ran again (Rotate identity, tor-new, the DNS switch, the timer) saved
        only error=None: the state FILE, which the Control Center reads, kept
        Ghost Mode and the proxy off next to threat level "maximum" while
        every connection went to Tor. It is re-checked now while a redirect
        is hooked; the file is checked here, not real_state()."""
        t = self.tool
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        for cmd in (["rotate"], ["tor-new"], ["dns-protect-on"], ["tor-newnym"]):
            with self.subTest(cmd=cmd):
                self.tor_down()
                self.assertEqual(self.main("tor-new"), 3, self.stderr)
                st = self.state()
                self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])
                self.assertTrue(st["ghost_wanted"])
                self.tor_back()
                self.clock.now += 400                    # past the address and circuit intervals
                self.assertEqual(self.main(*cmd), 0, self.stderr)
                st = self.state()
                self.assertTrue(st["ghost_mode"] and st["transparent_proxy"] and st["kill_switch"], st)
                self.assertEqual(st["threat_model"], "maximum")
                self.assertIsNone(st["error"])
                self.assertNotIn("ghost_wanted", st)
                self.assertEqual(self.hooks(table="nat")[:2], ["SHADOWDNS", "SHADOWTOR"])
        # A Tor that is still down stays recorded after an unrelated success
        # (it was cleared, with Ghost Mode still off in the record).
        self.tor_down()
        self.assertEqual(self.main("tor-new"), 3, self.stderr)
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        st = self.state()
        self.assertEqual(st["error"], t.TOR_DOWN)
        self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])
        self.assertTrue(st["ghost_wanted"])
        # No redirect hooked and nothing waiting: a success clears the error (as before).
        self.tor_back()
        self.assertEqual(self.main("ghost-off"), 0, self.stderr)
        with open(t.STATE_FILE) as f:
            st = json.load(f)
        st["error"] = "an earlier failure"
        with open(t.STATE_FILE, "w") as f:
            json.dump(st, f)
        calls = len(self.calls())
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        self.assertIsNone(self.state()["error"])
        self.assertFalse([c for c in self.calls()[calls:] if c[0] == "shadow-killswitch"])   # not re-checked

    def test_ghost_mode_survives_a_tor_that_stopped_on_its_own(self):
        """Regression (d3-01): Tor stopped without a restart of shadow-anon's
        (a crash, OOM, "systemctl stop", a package upgrade). The next success
        (the rotation timer's tor-newnym, which exits 0 with "Tor is not
        running", or anything else) recorded real_state() with Ghost Mode
        off, threat level "enhanced" and no ghost_wanted (only restart_tor()
        set it), so once Tor ran again the state file, which the Control
        Center reads, said Ghost Mode off for good while the proxy and the
        kill switch were in force."""
        t = self.tool
        for cmd in (["tor-newnym"], ["hostname"]):
            with self.subTest(cmd=cmd):
                self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
                self.assertEqual(self.main("ghost-on"), 0, self.stderr)
                self.assertTrue(self.state()["ghost_mode"])
                # Tor stops by itself: no restart is attempted, none fails.
                self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
                self.clock.now += 400
                calls = len(self.calls())
                self.assertEqual(self.main(*cmd), 0, self.stderr)
                self.assertNotIn(["systemctl", "restart", "tor@default"], self.calls()[calls:])
                st = self.state()
                self.assertFalse(st["ghost_mode"] or st["transparent_proxy"])     # not in force now
                self.assertEqual(st["error"], t.TOR_DOWN)
                self.assertTrue(st["ghost_wanted"])
                # Tor runs again (systemd, or the user, started it).
                self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
                self.clock.now += 400
                self.assertEqual(self.main(*cmd), 0, self.stderr)
                st = self.state()
                self.assertTrue(st["ghost_mode"] and st["transparent_proxy"] and st["kill_switch"], st)
                self.assertEqual(st["threat_model"], "maximum")
                self.assertIsNone(st["error"])
                self.assertNotIn("ghost_wanted", st)
                rep = t.real_state()                       # status --json as root
                self.assertTrue(rep["ghost_mode"])
                self.assertEqual(rep["threat_model"], "maximum")
                self.assertEqual(self.main("ghost-off"), 0, self.stderr)
        # Ghost Mode turned off while Tor is down is not brought back.
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        self.assertTrue(self.state()["ghost_wanted"])
        self.assertEqual(self.main("ghost-off"), 0, self.stderr)
        self.assertNotIn("ghost_wanted", self.state())
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        self.assertFalse(self.state()["ghost_mode"])
        self.assertFalse(t.real_state()["ghost_mode"])
        # Nor is a plain proxy promoted to Ghost Mode by such an outage.
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        self.assertNotIn("ghost_wanted", self.state())
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
        self.assertEqual(self.main("hostname"), 0, self.stderr)
        st = self.state()
        self.assertTrue(st["transparent_proxy"])
        self.assertFalse(st["ghost_mode"])
        self.assertNotEqual(st["threat_model"], "maximum")

    def test_a_plain_proxy_is_not_reported_as_ghost_mode_after_tor_recovers(self):
        t = self.tool
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        self.tor_down()
        self.assertEqual(self.main("tor-new"), 3, self.stderr)
        self.tor_back()
        rep = t.real_state()
        self.assertTrue(rep["transparent_proxy"])
        self.assertFalse(rep["ghost_mode"])
        self.assertNotEqual(rep["threat_model"], "maximum")
        self.assertIsNone(rep["error"])
        # Nor is a Ghost Mode that Tor interrupted brought back by a later plain proxy.
        self.assertEqual(self.main("transparent-proxy-off"), 0, self.stderr)
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.tor_down()
        self.assertEqual(self.main("tor-new"), 3, self.stderr)
        self.tor_back()
        self.assertEqual(self.main("transparent-proxy-off"), 0, self.stderr)
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        self.assertFalse(t.real_state()["ghost_mode"])

    def test_with_tor_down_the_leak_test_and_status_do_not_claim_the_proxy(self):
        """Regression (d1-05): with Tor down the root-run leak test (which
        the Control Center shows) printed "ACTIVE — all TCP through Tor" and
        "Ghost Mode: ON", and "status" "ghost mode: ON", "tproxy: ON" and
        "threat model: maximum", next to "Tor is not running"."""
        t = self.tool
        self.assertEqual(self.main("ghost-on"), 0, self.stderr)
        self.put("systemctl.json", {"active": [], "enabled": ["systemd-resolved.service"]})   # Tor stopped

        def report(cmd):
            self.out = io.StringIO()
            with mock.patch.object(t.os, "geteuid", lambda: 0):
                self.assertEqual(self.main(cmd), 0, self.stderr)
            return self.out.getvalue()
        leak, status = report("leak-test"), report("status")
        for text in ("ACTIVE — all TCP through Tor", "Ghost Mode    : ON", "Threat model  : maximum"):
            self.assertNotIn(text, leak)
        self.assertIn("NOT IN FORCE — its redirect is in place but Tor is not running", leak)
        self.assertIn("Ghost Mode    : OFF", leak)
        for text in ("ghost mode  : ON", "tproxy      : ON", "threat model: maximum"):
            self.assertNotIn(text, status)
        self.assertIn("tproxy      : NOT IN FORCE", status)
        self.assertIn("last error  : " + t.TOR_DOWN, status)
        # control: Tor running
        self.put("systemctl.json", {"active": ["tor@default"], "enabled": ["systemd-resolved.service"]})
        leak, status = report("leak-test"), report("status")
        self.assertIn("ACTIVE — all TCP through Tor", leak)
        self.assertIn("Ghost Mode    : ON", leak)
        self.assertIn("tproxy      : ON", status)
        self.assertIn(["curl", "-s", "--max-time", "15", "--socks5-hostname", "127.0.0.1:39",
                       "https://api.ipify.org"], self.calls())             # Tor's port, not torsocks' 9050
        self.assertFalse([c for c in self.calls() if c[0] == "torsocks"])

    OLD_PROXY = ["-m owner --uid-owner debian-tor -j RETURN", "-p udp --dport 53 -j REDIRECT --to-ports 5353",
                 "-p tcp --dport 53 -j REDIRECT --to-ports 1",
                 "-d 10.192.0.0/10 -p tcp --syn -j REDIRECT --to-ports 9040", "-d 10.0.0.0/8 -j RETURN",
                 "-p tcp --syn -j REDIRECT --to-ports 9040"]

    def old_ghost_mode(self):
        """Ghost Mode of the previous version (TransPort 9040, DNSPort 5353),
        as it is between an upgrade's unpack and its "boot"."""
        t = self.tool
        self.put("ipt.json", {"v4": {"nat": {"chains": {"SHADOWTOR": list(self.OLD_PROXY)}, "OUTPUT": ["SHADOWTOR"]}}})
        self.put("ks.json", {"mode": "tor", "lan": False, "persistent_mode": "off", "runtime_override": True,
                             "loaded": True, "error": None, "escape_hatch": False})
        os.makedirs(t.TORRC_D, exist_ok=True)
        with open(t.TPROXY_CF, "w") as f:
            f.write("TransPort 127.0.0.1:9040\nVirtualAddrNetworkIPv4 10.192.0.0/10\n")
        with open(t.TORDNS_CF, "w") as f:
            f.write("DNSPort 127.0.0.1:5353\n")
        with open(t.STATE_FILE, "w") as f:
            json.dump({"ghost_mode": True, "threat_model": "maximum", "transparent_proxy": True,
                       "kill_switch": True, "killswitch_by": ["ghost", "proxy"],
                       "killswitch_before": {"mode": "off", "lan": False, "persistent_mode": "off",
                                             "runtime_override": False, "escape_hatch": False}}, f)

    def test_an_older_versions_proxy_is_not_left_without_the_kill_switch(self):
        """Regression (d1-08): between an upgrade's unpack and its "boot" the
        new tool did not recognise the previous version's SHADOWTOR (9040,
        5353): "threat basic" and "kill-switch-off" released the kill switch
        and left the proxy hooked (UDP, QUIC and IPv6 then left directly),
        and "dns-protect-off" removed Tor's DNSPort while the chain still
        sent every UDP port-53 query to 127.0.0.1:5353, which any user could
        then bind."""
        t = self.tool
        self.old_ghost_mode()
        self.assertFalse(t.transparent_proxy_active())                # not this version's ports
        self.assertTrue(t._tproxy_hooked())
        self.assertEqual(self.main("dns-protect-off"), 0, self.stderr)
        self.assertTrue(os.path.exists(t.TORDNS_CF))                   # the old chain still needs it
        self.assertNotIn(["systemctl", "try-restart", "--no-block", "tor@default"], self.calls())
        for cmd in (["threat", "basic"], ["kill-switch-off"]):
            with self.subTest(cmd=cmd):
                self.old_ghost_mode()
                self.assertEqual(self.main(*cmd), 0, self.stderr)
                self.assertNotIn("SHADOWTOR", self.hooks(table="nat"))
                self.assertIsNone(self.rules())
                self.assertEqual(self.ks()["mode"], "off")
                self.assertFalse(self.state()["transparent_proxy"])
        # "boot" with such a chain but without Tor's files (removed by hand):
        # the redirect gets its TransPort and DNSPort back.
        self.old_ghost_mode()
        os.unlink(t.TPROXY_CF)
        os.unlink(t.TORDNS_CF)
        self.assertEqual(self.main("boot"), 0, self.stderr)
        with open(t.TPROXY_CF) as f:
            self.assertEqual(f.read(), t._tproxy_text())
        with open(t.TORDNS_CF) as f:
            self.assertEqual(f.read(), t._tor_dns_text())
        self.assertEqual(self.rules(), [" ".join(r) for r in t.tproxy_rules()])
        self.assertTrue(self.state()["transparent_proxy"])

    def test_boot_replaces_an_old_proxy_that_sends_tcp_to_the_dnsport(self):
        old = ["-m owner --uid-owner debian-tor -j RETURN", "-p udp --dport 53 -j REDIRECT --to-ports 5353",
               "-p tcp --dport 53 -j REDIRECT --to-ports 5353",
               "-d 10.192.0.0/10 -p tcp --syn -j REDIRECT --to-ports 9040", "-d 10.0.0.0/8 -j RETURN",
               "-p tcp --syn -j REDIRECT --to-ports 9040"]
        self.put("ipt.json", {"v4": {"nat": {"chains": {"SHADOWTOR": old}, "OUTPUT": ["SHADOWTOR"]}}})
        self.assertEqual(self.main("boot"), 0, self.stderr)
        self.assertEqual(self.rules(), [" ".join(r) for r in self.tool.tproxy_rules()])
        self.assertTrue(self.state()["transparent_proxy"])

    def test_upgrade_moves_a_running_proxy_to_the_privileged_ports(self):
        """Regression (upgrade while Ghost Mode is on): the previous version's
        chain (TransPort 9040, DNSPort 5353, which any user may bind) and
        Tor's files are rewritten for the new ports, the chain first (it
        then points at ports only root may bind), and Tor is restarted, not
        reloaded, and waited for: the state is the truth afterwards."""
        t = self.tool
        old = ["-m owner --uid-owner debian-tor -j RETURN", "-p udp --dport 53 -j REDIRECT --to-ports 5353",
               "-p tcp --dport 53 -j REDIRECT --to-ports 1",
               "-d 10.192.0.0/10 -p tcp --syn -j REDIRECT --to-ports 9040", "-d 10.0.0.0/8 -j RETURN",
               "-p tcp --syn -j REDIRECT --to-ports 9040"]
        self.assertFalse(t._sends_tcp_to_dns_port("SHADOWTOR"))
        self.put("ipt.json", {"v4": {"nat": {"chains": {"SHADOWTOR": old}, "OUTPUT": ["SHADOWTOR"]}}})
        self.put("ks.json", {"mode": "tor", "lan": False, "persistent_mode": "off", "runtime_override": True,
                             "loaded": True, "error": None, "escape_hatch": False})
        os.makedirs(t.TORRC_D, exist_ok=True)
        with open(t.TPROXY_CF, "w") as f:
            f.write("TransPort 127.0.0.1:9040 IsolateClientAddr\nVirtualAddrNetworkIPv4 10.192.0.0/10\n")
        with open(t.TORDNS_CF, "w") as f:
            f.write("DNSPort 127.0.0.1:5353\nAutomapHostsOnResolve 1\n")
        with open(t.STATE_FILE, "w") as f:
            json.dump({"ghost_mode": True, "threat_model": "maximum"}, f)
        self.assertFalse(t.transparent_proxy_active())            # not this version's TransPort
        self.assertEqual(self.main("boot"), 0, self.stderr)
        self.assertEqual(self.rules(), [" ".join(r) for r in t.tproxy_rules()])
        with open(t.TPROXY_CF) as f:
            self.assertEqual(f.read(), t._tproxy_text())
        with open(t.TORDNS_CF) as f:
            self.assertEqual(f.read(), t._tor_dns_text())
        calls = self.calls()
        chain = max(i for i, c in enumerate(calls) if c[0] == "iptables" and "SHADOWTOR" in c and "-A" in c)
        restart = self.index(lambda c: c == ["systemctl", "restart", "tor@default"], calls)
        self.assertIsNotNone(restart)
        self.assertLess(chain, restart)
        self.assertFalse([c for c in calls if c[0] == "systemctl" and "reload" in " ".join(c)])
        st = self.state()
        self.assertTrue(st["ghost_mode"] and st["transparent_proxy"] and st["kill_switch"])
        self.assertIsNone(st["error"])

    def test_the_proxy_engages_the_kill_switch_first_and_releases_it(self):
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        calls = self.calls()
        ks = self.index(lambda c: c[:3] == ["shadow-killswitch", "set", "tor"], calls)
        chain = self.index(lambda c: c[0] == "iptables" and "SHADOWTOR" in c, calls)
        self.assertLess(ks, chain)
        self.assertEqual(self.state()["killswitch_by"], ["proxy"])
        self.assertEqual(self.main("transparent-proxy-off"), 0, self.stderr)
        self.assertEqual(self.ks()["mode"], "off")
        self.assertIsNone(self.rules())

    def test_a_kill_switch_the_user_engaged_stays_after_the_proxy(self):
        self.assertEqual(self.main("kill-switch-on"), 0, self.stderr)
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        self.assertEqual(self.main("transparent-proxy-off"), 0, self.stderr)
        self.assertEqual(self.ks()["mode"], "tor")
        self.assertTrue(self.state()["kill_switch"])

    def test_no_proxy_without_the_kill_switch(self):
        self.put("ks-behaviour.json", {"refuse": True})
        self.assertEqual(self.main("transparent-proxy-on"), 3)
        self.assertIsNone(self.rules())
        self.assertFalse(self.state().get("transparent_proxy"))

    def test_a_refused_rule_is_reported_and_undone(self):
        self.put("fail.json", {"iptables": ["10.192.0.0/10"]})
        self.assertEqual(self.main("transparent-proxy-on"), 3)
        self.assertIn("iptables refused a rule of SHADOWTOR", self.state()["error"])
        self.assertIsNone(self.rules())
        self.assertEqual(self.hooks(table="nat"), [])
        self.assertEqual(self.ks()["mode"], "off")                 # the proxy's kill switch was released

    def test_kill_switch_off_also_ends_the_proxy(self):
        self.assertEqual(self.main("transparent-proxy-on"), 0, self.stderr)
        self.assertEqual(self.main("kill-switch-off"), 0, self.stderr)
        self.assertIsNone(self.rules())
        self.assertEqual(self.ks()["mode"], "off")
        self.assertFalse(self.state()["transparent_proxy"])

    def test_boot_adds_the_onion_rule_to_an_old_proxy(self):
        self.put("ipt.json", {"v4": {"nat": {"chains": {"SHADOWTOR": [
            "-m owner --uid-owner debian-tor -j RETURN", "-d 10.0.0.0/8 -j RETURN",
            "-p tcp --syn -j REDIRECT --to-ports 9040"]}, "OUTPUT": ["SHADOWTOR"]}}})
        self.assertEqual(self.main("boot"), 0, self.stderr)
        self.assertIn("-d 10.192.0.0/10 -p tcp --syn -j REDIRECT --to-ports 40", self.rules())
        self.assertEqual(self.ks()["mode"], "tor")                  # the proxy now needs it
        self.assertTrue(self.state()["transparent_proxy"])


class AddressAndTimers(HideMeTest):
    def test_rotate_uses_a_salted_stable_address_and_releases_the_lease_first(self):
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        with open(self.tool.MAC_SALT) as f:
            salt = json.load(f)["salt"]
        calls = self.nmcli_changes()
        self.assertEqual(calls[0], ["nmcli", "device", "modify", "eth0", "ipv4.dhcp-send-release", "yes"])
        self.assertEqual(calls[1], ["nmcli", "connection", "modify", "--temporary", UUID,
                                    "connection.stable-id", "${CONNECTION}/${BOOT}/" + salt,
                                    "802-3-ethernet.cloned-mac-address", "stable"])
        self.assertEqual(calls[2], ["nmcli", "--wait", "30", "connection", "down", UUID])
        self.assertEqual(calls[3], ["nmcli", "--wait", "30", "connection", "up", UUID])
        self.assertFalse([c for c in self.calls() if "random" in c])

    def test_rotate_waits_300_seconds_between_address_changes(self):
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        first = len(self.nmcli_changes())
        self.clock.now += 299
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        self.assertEqual(len(self.nmcli_changes()), first)          # nothing changed
        self.assertIn("minimum interval is 300 s", self.out.getvalue())
        with open(self.tool.MAC_SALT) as f:
            salt1 = json.load(f)["salt"]
        self.clock.now += 2
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        self.assertEqual(len(self.nmcli_changes()), 2 * first)
        with open(self.tool.MAC_SALT) as f:
            self.assertNotEqual(json.load(f)["salt"], salt1)

    def test_a_network_with_its_own_address_setting_is_left_alone(self):
        nm = self.get("nm.json")
        nm["props"][UUID] = {"802-3-ethernet.cloned-mac-address": "permanent"}
        self.put("nm.json", nm)
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        self.assertFalse(self.nmcli_changes())

    def test_a_network_pinned_by_shadow_netprivacy_is_left_alone(self):
        # "shadow-netprivacy mac-exception add" sets connection.stable-id=${CONNECTION} on the profile.
        nm = self.get("nm.json")
        nm["props"][UUID] = {"connection.stable-id": "${CONNECTION}"}
        self.put("nm.json", nm)
        self.assertEqual(self.main("rotate"), 0, self.stderr)
        self.assertFalse(self.nmcli_changes())
        self.assertIn("an address setting of its own", self.out.getvalue())

    def test_a_failed_reconnection_is_reported(self):
        self.put("fail.json", {"nmcli": ["connection up"]})
        self.assertEqual(self.main("rotate"), 3)
        self.assertIn("eth0", self.state()["error"])

    def test_threat_presets_never_change_the_address(self):
        for level in ("basic", "enhanced", "basic"):
            self.assertEqual(self.main("threat", level), 0, self.stderr)
        self.assertFalse(self.nmcli_changes())
        self.assertFalse(os.path.exists(self.tool.MAC_SALT))

    def test_the_timer_only_renews_tor_circuits(self):
        t = self.tool
        cookie = os.urandom(32)
        os.makedirs(os.path.dirname(t.TOR_COOKIE), exist_ok=True)
        with open(t.TOR_COOKIE, "wb") as f:
            f.write(cookie)
        got = []
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(t.TOR_CONTROL)
        srv.listen(4)

        def serve():
            while True:
                try:
                    conn, _ = srv.accept()
                except OSError:
                    return
                with conn, conn.makefile("rb") as rf:
                    for raw in rf:
                        line = raw.decode().strip()
                        got.append(line)
                        if line.startswith("AUTHENTICATE"):
                            conn.sendall(b"250 OK\r\n" if line.split()[1] == cookie.hex()
                                         else b"515 Authentication failed\r\n")
                        elif line == "QUIT":
                            conn.sendall(b"250 closing connection\r\n")
                            break
                        else:
                            conn.sendall(b"250 OK\r\n")
        th = threading.Thread(target=serve, daemon=True)
        th.start()
        try:
            self.assertEqual(self.main("tor-newnym"), 0, self.stderr)
            self.assertIn("SIGNAL NEWNYM", got)
            self.clock.now += 59
            self.assertEqual(self.main("tor-newnym"), 0, self.stderr)
            self.assertEqual(got.count("SIGNAL NEWNYM"), 1)            # 60 s minimum
            self.clock.now += 2
            self.assertEqual(self.main("tor-newnym"), 0, self.stderr)
            self.assertEqual(got.count("SIGNAL NEWNYM"), 2)
        finally:
            srv.close()
        self.assertFalse([c for c in self.calls() if c[0] == "nmcli"])  # never the address
        self.assertFalse(os.path.exists(t.MAC_SALT))

    def test_the_timer_falls_back_to_a_reload(self):
        self.assertEqual(self.main("tor-newnym"), 0, self.stderr)      # no control socket here
        self.assertIn(["systemctl", "reload", "tor@default"], self.calls())

    def test_the_timer_without_tor_does_nothing(self):
        self.put("systemctl.json", {"active": [], "enabled": []})
        self.assertEqual(self.main("tor-newnym"), 0, self.stderr)
        self.assertFalse([c for c in self.calls() if c[:2] == ["systemctl", "reload"]])

    def test_timer_units(self):
        with open(os.path.join(lib.UNITS, "shadow-anon-rotate.service")) as f:
            service = f.read()
        self.assertIn("ExecStart=/usr/bin/shadow-anon tor-newnym\n", service)
        with open(os.path.join(lib.UNITS, "shadow-anon-rotate.timer")) as f:
            timer = f.read()
        import re
        for key in ("OnBootSec", "OnUnitActiveSec"):
            values = [int(v) for v in re.findall(rf"^{key}=(\d+)s$", timer, re.M)]
            self.assertTrue(values and min(values) >= 60, f"{key} below 60 s: {values}")

    def test_start_raises_short_intervals_to_60_seconds(self):
        drop = os.path.join(self.tool.TIMER_DROPIN_DIR, "interval.conf")
        self.assertEqual(self.main("start", "--interval", "5"), 0, self.stderr)
        with open(drop) as f:
            text = f.read()
        self.assertIn("OnUnitActiveSec=60s", text)
        self.assertNotIn("OnUnitActiveSec=5s", text)
        self.assertEqual(self.main("start", "--interval", "300"), 0, self.stderr)
        with open(drop) as f:
            self.assertIn("OnUnitActiveSec=300s", f.read())
        self.assertIn(["systemctl", "restart", "shadow-anon-rotate.timer"], self.calls())

    def test_boot_raises_an_old_too_short_interval(self):
        drop = os.path.join(self.tool.TIMER_DROPIN_DIR, "interval.conf")
        os.makedirs(os.path.dirname(drop))
        with open(drop, "w") as f:
            f.write("[Timer]\nOnBootSec=\nOnUnitActiveSec=\nOnBootSec=30s\nOnUnitActiveSec=30s\n")
        self.assertEqual(self.main("boot"), 0, self.stderr)
        with open(drop) as f:
            self.assertIn("OnUnitActiveSec=60s", f.read())


if __name__ == "__main__":
    unittest.main()
