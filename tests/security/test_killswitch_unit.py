#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests of the kill switch and panic tools (security/firewall/).

    python3 tests/security/test_killswitch_unit.py [-v]

The tools are loaded with every path constant pointed at a temporary
directory and their run() replaced by FakeSystem, which keeps nftables
tables, sockets, NetworkManager and conntrack in memory. These tests prove
the logic: what is rendered per mode, what a configuration may contain, that
the status never claims a table the (fake) kernel does not have, when
connections are cut, what NetworkManager data is used, and what a panic
records and undoes. Real packets and the real nft are in
tests/network/test_killswitch_netns.py.

Also checked here: the polkit rule (with node, when installed), the polkit
action file, the units, the dispatcher script and the packaging lists.

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
KILLSWITCH = os.path.join(FW, "shadow-killswitch")
PANIC = os.path.join(FW, "shadow-panic")
TOR_UID, VPN_UID = 4243, 4244


def load(path, name):
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


def read_json(path):
    with open(path) as fh:
        return json.load(fh)


class FakeSystem:
    """nft, conntrack, ss, ip, nmcli, wg and loginctl, in memory."""

    def __init__(self):
        self.tables = {}                  # inet: name → {"comment", "hooks": {hook: (prio, policy)}, "text"}
        self.bridge_tables = {}           # the same for family bridge
        self.calls = []
        self.check_fail = False           # nft --check rejects the batch
        self.no_bridge = False            # a kernel without the nftables bridge family
        self.load_lies = False            # nft --file says 0 but changes nothing
        self.list_error = None            # nft list fails with this message
        self.sockets = []                 # (proto, laddr, lport, raddr, rport, uid)
        self.destroy_works = True
        self.addresses = []               # ip -j address show
        self.nm = None                    # {"active": [...], "all": [...], "vpn": {uuid: (type, data)}}
        self.wg = {}                      # device → "pubkey\tendpoint" lines
        self.networking = "enabled"       # nmcli networking
        self.nmcli_fail = False
        self.counted = 7                  # packets every drop counter shows

    def run(self, argv, check=True, timeout=60):
        self.calls.append(list(argv))
        name = argv[0]
        rc, out, err = 127, "", f"{name} is not installed"
        if name == "nft":
            rc, out, err = self.nft(argv[1:])
        elif name == "conntrack":
            rc, out, err = 0, "", "connection tracking table has been emptied.\n"
        elif name == "ss":
            rc, out, err = self.ss(argv[1:])
        elif name == "ip":
            rc, out, err = 0, json.dumps(self.addresses), ""
        elif name == "nmcli":
            rc, out, err = self.nmcli(argv[1:])
        elif name == "wg":
            dev = argv[2]
            rc, out, err = (0, self.wg[dev], "") if dev in self.wg else (1, "", "No such device")
        elif name == "loginctl":
            rc, out, err = 0, "", ""
        if check and rc != 0:
            raise AssertionError(f"unexpected check=True failure of {argv}")
        return rc, out, err

    # ---- nft
    def family(self, name):
        return self.bridge_tables if name == "bridge" else self.tables

    def nft(self, args):
        if args[:2] == ["--check", "--file"]:
            if self.check_fail:
                return 1, "", "/run/x.nft:3:1-5: Error: syntax error\n\tip daddr 192.0.2.66\n"
            if self.no_bridge and self.bridge_chain(args[2]):
                return 1, "", "/run/x.nft:2:1-9: Error: Could not process rule: Operation not supported\n"
            return 0, "", ""
        if args[:1] == ["--file"]:
            if self.no_bridge and self.bridge_chain(args[1]):
                return 1, "", "Error: Could not process rule: Operation not supported\n"
            if not self.load_lies:
                with open(args[1]) as fh:
                    self.apply_batch(fh.read())
            return 0, "", ""
        if args[:3] == ["--json", "list", "table"]:
            if self.list_error:
                return 1, "", self.list_error
            family, name = args[3], args[4]
            tables = self.family(family)
            if name not in tables:
                return 1, "", "Error: No such file or directory; did you mean table 'x' in family inet?"
            t = tables[name]
            items = [{"metainfo": {"version": "fake"}},
                     {"table": dict({"family": family, "name": name}, **({"comment": t["comment"]}
                                                                         if t["comment"] else {}))}]
            handle = 1
            for hook, (prio, policy) in t["hooks"].items():
                items.append({"chain": {"family": family, "table": name, "name": hook, "type": "filter",
                                        "hook": hook, "prio": prio, "policy": policy, "handle": handle}})
                for expr in t.get("extra", {}).get(hook, []):        # a rule someone added by hand
                    handle += 1
                    items.append({"rule": {"family": family, "table": name, "chain": hook, "handle": handle,
                                           "expr": expr}})
                handle += 1
                items.append({"rule": {"family": family, "table": name, "chain": hook, "handle": handle,
                                       "expr": [{"counter": {"packets": self.counted, "bytes": 100 * self.counted}},
                                                {"drop": None}]}})
                handle += 1
            return 0, json.dumps({"nftables": items}), ""
        return 1, "", f"Error: fake nft does not know {args}"

    @staticmethod
    def bridge_chain(path):
        """Does the batch create a base chain in a bridge table (what a kernel without the family refuses)?"""
        with open(path) as fh:
            text = fh.read()
        return any("type filter hook" in block for block in re.findall(r"^table bridge \S+ \{\n(.*?)^\}$",
                                                                         text, re.S | re.M))

    def apply_batch(self, text):
        lines = text.splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            m = re.match(r"^table (inet|bridge) (\S+) \{\}$", line)
            if m:
                self.family(m.group(1)).setdefault(m.group(2), {"comment": None, "hooks": {}, "text": ""})
                i += 1
                continue
            m = re.match(r"^delete table (inet|bridge) (\S+)$", line)
            if m:
                del self.family(m.group(1))[m.group(2)]
                i += 1
                continue
            m = re.match(r"^table (inet|bridge) (\S+) \{$", line)
            if m:
                body = []
                i += 1
                while lines[i] != "}":
                    body.append(lines[i])
                    i += 1
                text = "\n".join(body)
                c = re.search(r'^\tcomment "([^"]*)"$', text, re.M)
                hooks = {h: (int(p), pol) for h, p, pol in
                         re.findall(r"type filter hook (\w+) priority (-?\d+); policy (\w+);", text)}
                self.family(m.group(1))[m.group(2)] = {"comment": c.group(1) if c else None, "hooks": hooks,
                                                       "text": text}
                i += 1
                continue
            if line.strip():
                raise AssertionError(f"fake nft cannot parse: {line!r}")
            i += 1

    # ---- ss
    def ss(self, args):
        proto = "tcp" if "-t" in args else "udp"
        if "-K" in args:
            if self.destroy_works:
                for src, sport, dst, dport in re.findall(
                        r"\( src (\S+) and sport = :(\d+) and dst (\S+) and dport = :(\d+) \)", args[-1]):
                    self.sockets = [s for s in self.sockets if not (
                        s[0] == proto and s[1] == src.strip("[]") and s[2] == int(sport)
                        and s[3] == dst.strip("[]") and s[4] == int(dport))]
            return 0, "", ""
        lines = []
        for p, la, lp, ra, rp, uid in self.sockets:
            if p != proto:
                continue
            fmt = (lambda a, port: f"[{a}]:{port}" if ":" in a else f"{a}:{port}")
            lines.append(f"0      0      {fmt(la, lp)} {fmt(ra, rp)}" + (f" uid:{uid}" if uid else "")
                         + " ino:1 sk:1 <->")
        return 0, "\n".join(lines) + ("\n" if lines else ""), ""

    # ---- nmcli
    def nmcli(self, args):
        if self.nmcli_fail:
            return 8, "", "Error: NetworkManager is not running."
        if args == ["-t", "networking"]:
            return 0, self.networking + "\n", ""
        if args[:1] == ["networking"]:
            self.networking = "enabled" if args[1] == "on" else "disabled"
            return 0, "", ""
        if self.nm is None:
            return 8, "", "Error: NetworkManager is not running."
        if args[:4] == ["-t", "-f", "UUID,TYPE,DEVICE", "connection"] and args[-1] == "--active":
            return 0, "".join(line + "\n" for line in self.nm["active"]), ""
        if args[:4] == ["-t", "-f", "UUID,TYPE", "connection"]:
            return 0, "".join(line + "\n" for line in self.nm["all"]), ""
        if args[:3] == ["-t", "-g", "vpn.service-type,vpn.data"]:
            uuid = args[-1]
            if uuid not in self.nm["vpn"]:
                return 10, "", "Error: no such connection profile."
            stype, data = self.nm["vpn"][uuid]
            return 0, f"{stype}\n{data}\n", ""
        return 2, "", f"Error: fake nmcli does not know {args}"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shadow-ks-test.")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.sys = FakeSystem()
        self.euid = 0
        patcher = mock.patch("os.geteuid", side_effect=lambda: self.euid)
        patcher.start()
        self.addCleanup(patcher.stop)

    def call(self, tool, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = tool.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def nft_files(self):
        return [c for c in self.sys.calls if c[:2] == ["nft", "--file"]]


class KillSwitchBase(Base):
    def setUp(self):
        super().setUp()
        self.ks = load(KILLSWITCH, "shadow_killswitch")
        self.ks.CONFIG = os.path.join(self.tmp, "etc", "killswitch.json")
        self.ks.RUN_DIR = os.path.join(self.tmp, "run")
        self.ks.CMDLINE = os.path.join(self.tmp, "cmdline")
        self.ks.TOR_USER = str(TOR_UID)
        self.ks.OPENVPN_USER = str(VPN_UID)
        self.ks.run = self.sys.run
        write(self.ks.CMDLINE, "BOOT_IMAGE=/vmlinuz root=UUID=x ro quiet\n")

    def status(self):
        return read_json(os.path.join(self.ks.RUN_DIR, "killswitch.json"))

    def config(self):
        return read_json(self.ks.CONFIG)

    def table(self):
        return self.sys.tables.get("shadow_egress")


# --------------------------------------------------------------------------- rendering
class Rendering(KillSwitchBase):
    EPS = [{"address": "192.0.2.10", "port": 51820, "protocol": "udp"},
           {"address": "2001:db8::7", "port": 443, "protocol": "tcp"}]

    def render(self, mode, lan=False, ifaces=("wg*", "tun*"), eps=None, tor=TOR_UID, vpn=VPN_UID):
        return self.ks.render(mode, lan, list(ifaces), self.EPS if eps is None else eps, tor, vpn)[0]

    def chain(self, text, name):
        return re.search(r"\tchain %s \{\n(.*?)\n\t\}" % name, text, re.S).group(1)

    def test_tor(self):
        t = self.render("tor")
        out, fwd = self.chain(t, "output"), self.chain(t, "forward")
        self.assertIn("type filter hook output priority -50; policy drop;", out)
        self.assertIn("type filter hook forward priority -50; policy drop;", fwd)
        self.assertIn('oif "lo" counter accept', out)
        self.assertIn(f"meta skuid {TOR_UID} counter accept", out)
        self.assertIn("udp sport 68 udp dport 67", out)
        self.assertIn("udp sport 546 udp dport 547", out)
        self.assertTrue(out.rstrip().endswith('counter drop comment "kill switch"'))
        for absent in ("tunnel_ifaces", "vpn_endpoints", f"skuid {VPN_UID}", "lan4", "lan6"):
            self.assertNotIn(absent, t)
        self.assertEqual([ln.strip() for ln in fwd.splitlines()][1:], ['counter drop comment "kill switch"'])

    def test_loopback_destinations_may_not_leave_by_a_real_device(self):
        # Review r1-02: output accepts 127/8 and ::1 (Hide Me's redirect to TransPort/DNSPort
        # keeps the original device in the output hook), so a socket bound to the network
        # card (SO_BINDTODEVICE) could send to 127.x out of the card. Only postrouting sees
        # the real device; every mode drops a loopback destination there unless oif is lo.
        for mode in ("tor", "vpn", "tor-or-vpn"):
            for lan in (False, True):
                t = self.render(mode, lan=lan)
                post = self.chain(t, "postrouting")
                self.assertEqual([ln.strip() for ln in post.splitlines()], [
                    "type filter hook postrouting priority -50; policy accept;",
                    'oif != "lo" ip daddr 127.0.0.0/8 counter drop comment "loopback destination on a real device"',
                    'oif != "lo" ip6 daddr ::1 counter drop comment "loopback destination on a real device"'],
                    (mode, lan))
                self.assertIn('ip daddr 127.0.0.0/8 counter accept comment "redirected to local Tor"',
                              self.chain(t, "output"))

    def test_bridged_frames_are_dropped_unless_they_go_to_a_local_guest(self):
        # Review r1-07: frames a bridge forwards never reach the inet hooks without br_netfilter
        for mode in ("tor", "vpn", "tor-or-vpn"):
            text, marker = self.ks.render(mode, False, ["wg*"], [], TOR_UID, VPN_UID)
            bridge = re.search(r"^table bridge shadow_egress \{\n(.*?)^\}$", text, re.S | re.M)
            self.assertIsNotNone(bridge, mode)
            self.assertEqual([ln.strip() for ln in bridge.group(1).splitlines()], [
                f'comment "{marker}"', "chain forward {", "type filter hook forward priority -50; policy drop;",
                'meta oifkind { "tun", "veth" } counter accept comment "to a local virtual machine or container"',
                'counter drop comment "kill switch"', "}"], mode)
            self.assertEqual(text.count(f'comment "{marker}"'), 2)
        # without the bridge family: the inet table alone, and a different marker
        text, marker = self.ks.render("tor", False, [], [], TOR_UID, VPN_UID, bridge=False)
        self.assertNotIn("table bridge", text)
        self.assertNotEqual(marker, self.ks.render("tor", False, [], [], TOR_UID, VPN_UID)[1])

    def test_tor_without_tor_user_blocks_tor_too(self):
        self.assertNotIn("skuid", self.render("tor", tor=None))

    def test_vpn(self):
        t = self.render("vpn")
        out, fwd = self.chain(t, "output"), self.chain(t, "forward")
        self.assertIn('elements = { "tun*", "wg*" }', t)
        self.assertIn("elements = { 192.0.2.10 . udp . 51820 }", t)
        self.assertIn("elements = { 2001:db8::7 . tcp . 443 }", t)
        self.assertIn("ip daddr . meta l4proto . th dport @vpn_endpoints4 counter accept", out)
        self.assertIn("ip6 daddr . meta l4proto . th dport @vpn_endpoints6 counter accept", out)
        self.assertIn(f"meta skuid {VPN_UID} counter accept", out)
        self.assertNotIn(f"skuid {TOR_UID}", t)
        self.assertIn("iifname @tunnel_ifaces ct state established,related counter accept", fwd)
        # a tunnel is a name AND a layer-3 tunnel device (type none) or a TAP (kind tun), never
        # the name alone: a macvlan or VLAN "tun7" on the network card is Ethernet (review r1-08)
        for chain in (out, fwd):
            tunnel = [ln.strip() for ln in chain.splitlines() if "@tunnel_ifaces" in ln and "iifname" not in ln]
            self.assertEqual([ln.split(" counter")[0] for ln in tunnel],
                             ["oifname @tunnel_ifaces meta oiftype 65534", 'oifname @tunnel_ifaces meta oifkind "tun"'])

    def test_vpn_without_endpoints_has_empty_sets(self):
        t = self.render("vpn", eps=[])
        self.assertIn("set vpn_endpoints4 {\n\t\ttype ipv4_addr . inet_proto . inet_service\n\t}", t)

    def test_tor_or_vpn_is_the_union(self):
        t = self.render("tor-or-vpn")
        self.assertIn(f"meta skuid {TOR_UID}", t)
        self.assertIn(f"meta skuid {VPN_UID}", t)
        self.assertIn("oifname @tunnel_ifaces", t)

    def test_lan_never_includes_the_onion_range(self):
        out = self.chain(self.render("tor", lan=True), "output")
        onion = out.index("ip daddr 10.192.0.0/10 counter drop")
        self.assertLess(onion, out.index("ip daddr @lan4 counter accept"))
        self.assertLess(onion, out.index("ip6 daddr @lan6 counter accept"))
        self.assertIn("10.0.0.0/8", self.render("tor", lan=True))

    def test_overlapping_interfaces_are_normalised(self):
        t = self.render("vpn", ifaces=["wg*", "wg0", "tun*", "tun-x*", "office0", "wg*"])
        self.assertIn('elements = { "office0", "tun*", "wg*" }', t)

    def test_marker_follows_the_content(self):
        a = self.ks.render("vpn", False, ["wg*"], [], TOR_UID, VPN_UID)[1]
        b = self.ks.render("vpn", False, ["wg*"], self.EPS[:1], TOR_UID, VPN_UID)[1]
        self.assertTrue(a.startswith("shadow-killswitch mode=vpn lan=no sha="))
        self.assertNotEqual(a, b)
        self.assertEqual(a, self.ks.render("vpn", False, ["wg*"], [], TOR_UID, VPN_UID)[1])

    def test_off_is_not_rendered(self):
        with self.assertRaises(ValueError):
            self.ks.render("off", False, [], [], None, None)
        self.assertEqual(self.ks.removal_text(), "table inet shadow_egress {}\ndelete table inet shadow_egress\n"
                                                 "table bridge shadow_egress {}\ndelete table bridge shadow_egress\n")
        self.assertEqual(self.ks.removal_text(bridge=False),
                         "table inet shadow_egress {}\ndelete table inet shadow_egress\n")


# --------------------------------------------------------------------------- configuration
class Configuration(KillSwitchBase):
    def test_missing_file_is_off(self):
        self.assertEqual(self.ks.load_config(), self.ks.default_config())

    def test_endpoints(self):
        p = self.ks.parse_endpoint
        self.assertEqual(p("192.0.2.10:51820/udp"), {"address": "192.0.2.10", "port": 51820, "protocol": "udp"})
        self.assertEqual(p("[2001:DB8:0::1]:443/tcp"), {"address": "2001:db8::1", "port": 443, "protocol": "tcp"})
        for bad in ("vpn.example.net:1194/udp", "192.0.2.10:0/udp", "192.0.2.10:70000/udp", "192.0.2.10/udp",
                    "192.0.2.10:1194", "192.0.2.10:1194/icmp", "2001:db8::1:443/tcp", "[192.0.2.1]:1/udp",
                    "127.0.0.1:1/udp", "224.0.0.1:1/udp", "0.0.0.0:1/udp", "[::1]:1/udp", "999.0.0.1:1/udp"):
            with self.assertRaises(self.ks.Failure, msg=bad):
                p(bad)

    def test_interfaces(self):
        for good in ("wg0", "wg*", "tun*", "office-vpn0", "a.b_c"):
            self.assertEqual(self.ks.check_iface(good), good)
        for bad in ("*", "w*", "lo", "lo*", "l*", "eth 0", 'x"y', "a" * 16, "abcdefghijklmn*x", ""):
            with self.assertRaises(self.ks.Failure, msg=bad):
                self.ks.check_iface(bad)

    def test_valid_file_and_endpoint_forms(self):
        write(self.ks.CONFIG, json.dumps({"version": 1, "mode": "vpn", "lan": True, "tunnel_ifaces": ["wg*", "wg9"],
                                          "vpn_endpoints": ["192.0.2.1:1/udp",
                                                            {"address": "2001:db8::2", "port": 2, "protocol": "tcp"},
                                                            {"address": "192.0.2.1", "port": 1, "protocol": "udp"}]}))
        cfg = self.ks.load_config(strict=True)
        self.assertEqual(cfg["mode"], "vpn")
        self.assertEqual(cfg["tunnel_ifaces"], ["wg*"])
        self.assertEqual(len(cfg["vpn_endpoints"]), 2)

    def test_broken_file_fails_closed_or_raises(self):
        for content in ("{not json", json.dumps({"mode": "everything"}), json.dumps({"lan": "yes"}),
                        json.dumps({"version": 2}), json.dumps({"vpn_endpoints": ["vpn.example.net:1/udp"]}),
                        json.dumps({"tunnel_ifaces": ["*"]}), json.dumps([1, 2])):
            write(self.ks.CONFIG, content)
            with self.assertRaises(self.ks.Failure, msg=content):
                self.ks.load_config(strict=True)
            cfg = self.ks.load_config()
            self.assertEqual(cfg["mode"], "tor-or-vpn", content)
            self.assertIn("fails closed", cfg["config_error"])

    def test_saved_file_is_world_readable_and_round_trips(self):
        cfg = self.ks.default_config()
        cfg["vpn_endpoints"] = [self.ks.parse_endpoint("192.0.2.3:3/udp")]
        self.ks.save_config(cfg)
        self.assertEqual(os.stat(self.ks.CONFIG).st_mode & 0o777, 0o644)
        self.assertEqual(self.ks.load_config(strict=True), cfg)


# --------------------------------------------------------------------------- set / status honesty
class SetAndStatus(KillSwitchBase):
    def test_set_tor_persists_and_is_loaded(self):
        rc, out, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.config()["mode"], "tor")
        st = self.status()
        self.assertEqual((st["mode"], st["active"], st["loaded"], st["error"]), ("tor", True, True, None))
        self.assertFalse(st["runtime_override"])
        self.assertEqual(self.table()["hooks"], {"output": (-50, "drop"), "forward": (-50, "drop"),
                                                 "postrouting": (-50, "accept")})
        self.assertEqual(self.sys.bridge_tables["shadow_egress"]["hooks"], {"forward": (-50, "drop")})
        self.assertEqual(self.sys.bridge_tables["shadow_egress"]["comment"], self.table()["comment"])
        self.assertEqual(os.stat(os.path.join(self.ks.RUN_DIR, "killswitch.json")).st_mode & 0o777, 0o644)
        self.assertEqual(os.stat(os.path.join(self.ks.RUN_DIR, "killswitch-state.json")).st_mode & 0o777, 0o600)
        # the same file is validated before it is loaded
        nft = [c[1:] for c in self.sys.calls if c[0] == "nft"]
        check = next(i for i, c in enumerate(nft) if c[:2] == ["--check", "--file"])
        self.assertEqual(nft[check + 1], ["--file", nft[check][2]])

    def test_runtime_does_not_touch_the_configuration(self):
        rc, _, err = self.call(self.ks, "set", "tor", "--runtime")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.ks.CONFIG))
        st = self.status()
        self.assertEqual((st["mode"], st["persistent_mode"], st["runtime_override"]), ("tor", "off", True))

    def test_off_removes_the_table(self):
        self.call(self.ks, "set", "vpn")
        rc, _, err = self.call(self.ks, "set", "off")
        self.assertEqual(rc, 0, err)
        self.assertIsNone(self.table())
        st = self.status()
        self.assertEqual((st["active"], st["loaded"], st["since"], st["error"]), (False, False, None, None))

    def test_rejected_ruleset_changes_nothing(self):
        self.call(self.ks, "set", "tor")
        before = dict(self.table())
        self.sys.check_fail = True
        rc, _, err = self.call(self.ks, "set", "vpn")
        self.assertEqual(rc, 1)
        self.assertIn("could not change to mode vpn", err)
        self.assertIn("nothing was changed; the kill switch stays mode tor", err)
        self.assertNotIn("192.0.2.66", err)          # nft's quoted context is not repeated (privacy)
        self.assertEqual(self.table(), before)
        self.assertEqual(self.config()["mode"], "tor")
        st = self.status()
        self.assertEqual((st["mode"], st["loaded"]), ("tor", True))   # what is really in force
        # Review r1-14: the failed request is not the health of what is in force. Tor only is
        # loaded and verified, so there is no error (the desktop shows "protected"); the
        # failure is change_error, and a warning of the last change.
        self.assertIsNone(st["error"])
        self.assertIn("could not change to mode vpn", st["change_error"])
        self.assertIn(st["change_error"], st["warnings"])

    def test_failed_change_while_the_old_table_is_gone_is_still_an_error(self):
        self.call(self.ks, "set", "tor")
        del self.sys.tables["shadow_egress"]
        self.sys.check_fail = True
        rc, _, err = self.call(self.ks, "set", "vpn")
        self.assertEqual(rc, 1)
        st = self.status()
        self.assertEqual((st["mode"], st["loaded"]), ("tor", False))
        self.assertEqual(st["error"], "the kill switch table is not loaded")
        self.assertIn("could not change to mode vpn", st["change_error"])

    def test_failed_change_is_kept_by_status_and_ended_by_the_next_change(self):
        self.call(self.ks, "set", "tor")
        self.sys.check_fail = True
        self.call(self.ks, "set", "vpn")
        self.sys.check_fail = False
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual(rc, 0)                                   # consistent: Tor only is in force
        st = json.loads(out)
        self.assertIsNone(st["error"])
        self.assertIn("could not change to mode vpn", st["change_error"])
        rc, out, _ = self.call(self.ks, "status")
        self.assertIn("Last change:", out)
        self.assertEqual(self.call(self.ks, "set", "vpn")[0], 0)
        self.assertNotIn("change_error", self.status())

    def test_stale_error_is_replaced_once_the_kernel_matches(self):
        # A status written while something was wrong (here: an older version's failed
        # change, recorded as "error") must not survive reassert and the watchdog.
        self.call(self.ks, "set", "tor")
        st = self.status()
        st["error"] = "could not change to mode vpn: loading the kill switch ruleset failed: simulated"
        write(os.path.join(self.ks.RUN_DIR, "killswitch.json"), json.dumps(st))
        self.sys.calls.clear()
        self.assertEqual(self.call(self.ks, "reassert", "--watch"), (0, "", ""))
        self.assertIsNone(self.status()["error"])
        self.assertEqual(self.nft_files(), [])                   # corrected without a reload
        before = snapshot(self.ks.RUN_DIR)
        self.assertEqual(self.call(self.ks, "reassert"), (0, "", ""))
        self.assertEqual(snapshot(self.ks.RUN_DIR), before)       # and then quiet, without writes again

    def test_broken_configuration_error_is_kept_by_the_watchdog(self):
        write(self.ks.CONFIG, "{broken")
        self.assertEqual(self.call(self.ks, "boot")[0], 1)
        before = snapshot(self.ks.RUN_DIR)
        self.assertEqual(self.call(self.ks, "reassert", "--watch"), (0, "", ""))
        self.assertEqual(snapshot(self.ks.RUN_DIR), before)
        self.assertIn("fails closed", self.status()["error"])

    def test_nft_that_lies_is_not_believed(self):
        self.sys.load_lies = True
        rc, _, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(self.ks.CONFIG))        # not saved: it is not in force
        st = self.status()
        self.assertEqual((st["active"], st["loaded"]), (True, False))
        self.assertEqual(st["error"], "the kill switch table is not loaded")

    def test_status_rechecks_the_kernel(self):
        self.call(self.ks, "set", "tor")
        del self.sys.tables["shadow_egress"]                     # e.g. nft flush ruleset
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual(rc, 3)
        st = json.loads(out)
        self.assertEqual((st["active"], st["loaded"], st["verified"]), (True, False, True))
        self.assertFalse(self.status()["loaded"])                # the /run file is corrected too

    def test_foreign_table_with_the_same_name_is_not_ours(self):
        self.call(self.ks, "set", "tor")
        self.sys.tables["shadow_egress"]["comment"] = "something else"
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual(rc, 3)
        self.assertIn("not the one this configuration produces", json.loads(out)["error"])

    def test_unreadable_ruleset(self):
        self.call(self.ks, "set", "tor")
        self.sys.list_error = "Error: Operation not permitted"
        rc, out, _ = self.call(self.ks, "status", "--json")
        st = json.loads(out)
        self.assertEqual(rc, 3)
        self.assertFalse(st["loaded"])
        self.assertIn("cannot read the live ruleset", st["error"])

    def test_status_as_a_user_reads_the_record_only(self):
        self.call(self.ks, "set", "tor")
        self.euid = 1000
        calls = len(self.sys.calls)
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual(rc, 4)
        st = json.loads(out)
        self.assertEqual((st["mode"], st["verified"]), ("tor", False))
        self.assertEqual(len(self.sys.calls), calls)            # no nft as a user
        rc, _, err = self.call(self.ks, "set", "off")
        self.assertEqual(rc, 1)
        self.assertIn("must run as root", err)

    def test_bridge_table_is_part_of_the_kill_switch(self):
        # Review r1-07: the bridge table is loaded with the inet one, read back, and its
        # removal alone makes the kill switch "not loaded" (and the watchdog restores it)
        self.call(self.ks, "set", "tor")
        del self.sys.bridge_tables["shadow_egress"]
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual(rc, 3)
        st = json.loads(out)
        self.assertEqual((st["loaded"], st["error"]),
                         (False, "the loaded kill switch table is not the one this configuration produces"))
        rc, out, _ = self.call(self.ks, "reassert", "--watch")
        self.assertIn("had been changed", out)
        self.assertIn("shadow_egress", self.sys.bridge_tables)
        self.assertTrue(self.status()["loaded"])
        self.call(self.ks, "set", "off")
        self.assertEqual((self.sys.tables, self.sys.bridge_tables), ({}, {}))
        self.sys.bridge_tables["shadow_egress"] = {"comment": "x", "hooks": {}, "text": ""}   # a stray one
        rc, out, _ = self.call(self.ks, "status", "--json")
        self.assertEqual((rc, json.loads(out)["error"]), (3, "the kill switch is off but its table is still loaded"))

    def test_kernel_without_the_bridge_family_gets_the_inet_table_and_a_warning(self):
        self.sys.no_bridge = True
        rc, _, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 0, err)
        self.assertIn("no nftables bridge family", err)
        self.assertEqual(self.sys.bridge_tables, {})
        st = self.status()
        self.assertEqual((st["loaded"], st["error"]), (True, None))
        self.assertIn(self.ks.NO_BRIDGE_WARNING, st["warnings"])
        self.sys.calls.clear()
        self.assertEqual(self.call(self.ks, "reassert", "--watch"), (0, "", ""))   # quiet: what was loaded
        self.assertEqual(self.nft_files(), [])
        rc, _, err = self.call(self.ks, "set", "off")                 # turning it off still works
        self.assertEqual(rc, 0, err)
        self.assertIsNone(self.table())
        # with the bridge family, a load that fails is not retried without the bridge table
        self.sys.no_bridge = False
        self.ks._BRIDGE_FAMILY = True
        self.sys.calls.clear()
        with mock.patch.object(self.sys, "apply_batch", side_effect=AssertionError("must not load")):
            self.sys.check_fail = True
            rc, _, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 1)
        self.assertIn("rejected by nft --check", err)
        self.assertEqual([c[1] for c in self.sys.calls if c[0] == "nft"].count("--check"), 1)

    def test_dropped_counter_is_reported(self):
        self.call(self.ks, "set", "tor")
        _, out, _ = self.call(self.ks, "status", "--json")
        # 7 per chain in the fake: output, forward, postrouting and the bridge table's forward
        self.assertEqual(json.loads(out)["dropped_packets"], 28)

    def test_lock_is_root_only_and_bounded(self):
        import fcntl
        self.call(self.ks, "set", "tor")
        lock = os.path.join(self.ks.RUN_DIR, "killswitch.lock")
        self.assertEqual(os.stat(lock).st_mode & 0o777, 0o600)
        fd = os.open(lock, os.O_RDWR)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX)                 # someone else holds it
        self.ks.Lock.WAIT = 0.3
        rc, _, err = self.call(self.ks, "set", "off")
        self.assertEqual(rc, 1)
        self.assertIn("another shadow-killswitch", err)
        self.assertIsNotNone(self.table())             # nothing changed

    def test_world_readable_status_holds_no_addresses(self):
        write(self.ks.CONFIG, json.dumps({"vpn_endpoints": ["192.0.2.77:51820/udp", "[2001:db8::77]:1/tcp"]}))
        self.call(self.ks, "set", "vpn")
        with open(os.path.join(self.ks.RUN_DIR, "killswitch.json")) as fh:
            text = fh.read()
        self.assertEqual(json.loads(text)["endpoints"], 2)
        self.assertNotIn("192.0.2.77", text)
        self.assertNotIn("2001:db8::77", text)

    def test_status_has_the_contract_fields(self):
        self.call(self.ks, "set", "tor-or-vpn", "--lan")
        st = self.status()
        for key, typ in (("mode", str), ("active", bool), ("loaded", bool), ("since", str), ("lan", bool),
                         ("endpoints", int), ("escape_hatch", bool)):
            self.assertIsInstance(st[key], typ, key)
        self.assertIn("error", st)


# --------------------------------------------------------------------------- flushing and closing connections
class Flush(KillSwitchBase):
    def test_needs_flush(self):
        f = self.ks.needs_flush
        self.assertTrue(f("off", False, False, "tor", False))
        self.assertTrue(f("tor", False, False, "tor", False))         # previous not loaded
        self.assertFalse(f("tor", False, True, "tor-or-vpn", False))  # loosening
        self.assertFalse(f("tor", False, True, "tor", True))          # adds the LAN
        self.assertTrue(f("tor-or-vpn", False, True, "tor", False))   # stricter
        self.assertTrue(f("tor", True, True, "tor", False))           # LAN removed
        self.assertTrue(f("tor", False, True, "vpn", False))          # different
        self.assertFalse(f("tor", False, True, "off", False))

    def test_engaging_closes_only_what_the_mode_blocks(self):
        self.sys.sockets = [
            ("tcp", "198.51.100.1", 40000, "198.51.100.2", 443, 1000),   # user, direct: closed
            ("udp", "198.51.100.1", 40001, "198.51.100.2", 53, 1000),    # user, UDP: closed
            ("tcp", "198.51.100.1", 40002, "198.51.100.3", 9001, TOR_UID),  # Tor: kept
            ("tcp", "127.0.0.1", 40003, "127.0.0.1", 9050, 1000),        # loopback: kept
            ("udp", "198.51.100.1", 68, "198.51.100.254", 67, 0),         # DHCP: kept
        ]
        rc, _, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 0, err)
        self.assertIn(["conntrack", "-F"], self.sys.calls)
        self.assertEqual(sorted(s[2] for s in self.sys.sockets), [68, 40002, 40003])
        st = self.status()
        self.assertEqual((st["conntrack_flushed"], st["sockets_closed"], st["warnings"]), (True, 2, []))

    def test_vpn_keeps_tunnel_endpoint_and_openvpn_sockets(self):
        self.sys.addresses = [{"ifname": "wg0", "link_type": "none", "linkinfo": {"info_kind": "wireguard"},
                               "addr_info": [{"local": "203.0.113.5"}]},
                              {"ifname": "tun3", "link_type": "ether", "linkinfo": {"info_kind": "tun"},
                               "addr_info": [{"local": "203.0.113.6"}]},
                              {"ifname": "tun7", "link_type": "ether", "linkinfo": {"info_kind": "macvlan"},
                               "addr_info": [{"local": "198.51.100.77"}]},
                              {"ifname": "eth0", "link_type": "ether", "addr_info": [{"local": "198.51.100.1"}]}]
        write(self.ks.CONFIG, json.dumps({"vpn_endpoints": ["198.51.100.9:51820/udp"]}))
        self.sys.sockets = [
            ("tcp", "203.0.113.5", 1, "198.51.100.2", 443, 1000),      # through the tunnel: kept
            ("tcp", "203.0.113.6", 6, "198.51.100.2", 443, 1000),      # through a TAP tunnel: kept
            ("tcp", "198.51.100.77", 7, "198.51.100.2", 443, 1000),    # a macvlan named tun7: closed
            ("udp", "198.51.100.1", 2, "198.51.100.9", 51820, 0),     # to the endpoint: kept
            ("tcp", "198.51.100.1", 3, "198.51.100.7", 1194, VPN_UID),  # openvpn: kept
            ("tcp", "198.51.100.1", 4, "198.51.100.9", 443, 1000),    # endpoint address, other port: closed
            ("tcp", "198.51.100.1", 5, "198.51.100.2", 443, TOR_UID),  # Tor uid in vpn mode: closed
        ]
        self.call(self.ks, "set", "vpn")
        self.assertEqual(sorted(s[2] for s in self.sys.sockets), [1, 2, 3, 6])
        self.assertIn(["ip", "-d", "-j", "address", "show"], self.sys.calls)

    def test_lan_keeps_lan_but_not_the_onion_range(self):
        self.sys.sockets = [("tcp", "192.168.7.2", 1, "192.168.7.3", 445, 1000),
                            ("tcp", "10.1.2.3", 2, "10.192.0.9", 80, 1000)]
        self.call(self.ks, "set", "tor", "--lan")
        self.assertEqual([s[2] for s in self.sys.sockets], [1])

    def test_loosening_does_not_flush(self):
        self.call(self.ks, "set", "tor")
        self.sys.calls.clear()
        self.call(self.ks, "set", "tor-or-vpn")
        self.assertNotIn(["conntrack", "-F"], self.sys.calls)
        self.sys.calls.clear()
        self.call(self.ks, "set", "tor")
        self.assertIn(["conntrack", "-F"], self.sys.calls)

    def test_kernel_without_sock_destroy_is_reported(self):
        self.sys.destroy_works = False
        self.sys.sockets = [("tcp", "198.51.100.1", 1, "198.51.100.2", 443, 1000)]
        rc, _, err = self.call(self.ks, "set", "tor")
        self.assertEqual(rc, 0)                                   # the table is what protects
        st = self.status()
        self.assertEqual(st["sockets_closed"], 0)
        self.assertIn("could not be closed", " ".join(st["warnings"]))
        self.assertNotIn("198.51.100.2", json.dumps(st) + err)  # counts only, never addresses


# --------------------------------------------------------------------------- boot and the escape hatch
class Boot(KillSwitchBase):
    def test_boot_loads_the_saved_mode_without_flushing(self):
        write(self.ks.CONFIG, json.dumps({"mode": "tor"}))
        rc, _, err = self.call(self.ks, "boot")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.status()["mode"], "tor")
        self.assertTrue(self.status()["loaded"])
        self.assertNotIn(["conntrack", "-F"], self.sys.calls)

    def test_escape_hatch(self):
        write(self.ks.CONFIG, json.dumps({"mode": "tor"}))
        write(self.ks.CMDLINE, "root=UUID=x ro quiet shadow.killswitch=off\n")
        rc, _, err = self.call(self.ks, "boot")
        self.assertEqual(rc, 0, err)
        self.assertIn("shadow.killswitch=off", err)
        self.assertIsNone(self.table())
        st = self.status()
        self.assertEqual((st["mode"], st["active"], st["loaded"], st["escape_hatch"], st["persistent_mode"]),
                         ("off", False, False, True, "tor"))
        self.assertEqual(self.config()["mode"], "tor")             # the configuration is kept
        # the automatic paths keep it off for this boot ...
        self.call(self.ks, "reassert")
        self.assertIsNone(self.table())
        # ... an administrator's explicit choice wins
        self.call(self.ks, "set", "tor", "--runtime")
        self.assertIsNotNone(self.table())
        self.assertFalse(self.status()["escape_hatch"])

    def test_escape_hatch_spellings(self):
        for cmdline, hatch in (("shadow.killswitch=off", True), ("shadow.killswitch=0", True),
                               ("shadow.killswitch=on", False), ("shadow.killswitch=off shadow.killswitch=on", False),
                               ("x -- shadow.killswitch=off", False), ("shadow.killswitchy=off", False), ("", False)):
            write(self.ks.CMDLINE, cmdline + "\n")
            self.assertEqual(self.ks.escape_hatch(), hatch, cmdline)

    def test_second_boot_run_keeps_a_runtime_mode(self):
        self.call(self.ks, "boot")
        self.call(self.ks, "set", "tor", "--runtime")
        self.call(self.ks, "boot")                                  # unit restarted, package upgraded
        self.assertEqual(self.status()["mode"], "tor")
        self.assertIsNotNone(self.table())

    def test_broken_configuration_fails_closed_and_says_so(self):
        write(self.ks.CONFIG, "{broken")
        rc, _, err = self.call(self.ks, "boot")
        self.assertEqual(rc, 1)
        st = self.status()
        self.assertEqual((st["mode"], st["loaded"]), ("tor-or-vpn", True))
        self.assertIn("fails closed", st["error"])
        rc, _, err = self.call(self.ks, "set", "off")              # the documented way out
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.config()["mode"], "off")
        self.assertIsNone(self.table())
        self.assertIsNone(self.status()["error"])

    def test_boot_failure_is_recorded(self):
        write(self.ks.CONFIG, json.dumps({"mode": "vpn"}))
        self.sys.check_fail = True
        rc, _, err = self.call(self.ks, "boot")
        self.assertEqual(rc, 1)
        st = self.status()
        self.assertEqual((st["mode"], st["active"], st["loaded"]), ("vpn", True, False))
        self.assertIn("rejected by nft --check", st["error"])


# --------------------------------------------------------------------------- NetworkManager
class NetworkManager(KillSwitchBase):
    def nm(self):
        self.sys.nm = {
            "active": ["u1:802-11-wireless:wlan0", "u2:wireguard:office0", "u3:tun:tun5", "u4:vpn:wlan0",
                       "u5:wireguard:lo", "u6:wireguard:bad name"],
            "all": ["u1:802-11-wireless", "u2:wireguard", "u4:vpn", "u7:vpn", "u8:vpn"],
            "vpn": {
                # escaped as nmcli -t prints it: ':' → '\:', ',' inside a value → '\,' (and that '\' → '\\')
                "u4": ("org.freedesktop.NetworkManager.openvpn",
                       r"remote = 192.0.2.10\:1194\:udp\\, vpn.example.net\:443\:tcp\\, [2001\:db8\:\:5]\:1195, "
                       r"port = 1194, connection-type = tls"),
                "u7": ("org.freedesktop.NetworkManager.openvpn",
                       r"remote = 198.51.100.20, proto-tcp = yes, port = 443"),
                "u8": ("org.freedesktop.NetworkManager.vpnc", r"IPSec gateway = 192.0.2.99"),
            }}
        self.sys.wg = {"office0": "PUBKEYa=\t203.0.113.9:51820\nPUBKEYb=\t(none)\n"}

    def test_parsers(self):
        self.assertEqual(self.ks.nm_split(r"a\:b:c\\d:"), ["a:b", r"c\d", ""])
        self.assertEqual(self.ks.parse_vpn_data(r"remote = a\, b, port = 1"), {"remote": "a, b", "port": "1"})
        eps, skipped = self.ks.openvpn_endpoints({"remote": "192.0.2.1:1194:tcp-client, host.example:1, 2001:db8::9",
                                                  "port": "1200"})
        self.assertEqual(skipped, 1)
        self.assertEqual([self.ks.endpoint_text(e) for e in eps],
                         ["192.0.2.1:1194/tcp", "[2001:db8::9]:1200/udp"])

    def test_collect(self):
        self.nm()
        ifaces, eps, skipped = self.ks.nm_collect()
        self.assertEqual(ifaces, ["office0", "tun5"])
        self.assertEqual(sorted(self.ks.endpoint_text(e) for e in eps),
                         ["192.0.2.10:1194/udp", "198.51.100.20:443/tcp", "203.0.113.9:51820/udp",
                          "[2001:db8::5]:1195/udp"])
        self.assertEqual(skipped, 1)
        # names are never resolved: no resolver program, and the fake would refuse any
        self.assertFalse(any(c[0] not in ("nmcli", "wg") for c in self.sys.calls))

    def test_sync_does_nothing_unless_a_vpn_mode_is_on(self):
        self.nm()
        self.call(self.ks, "set", "tor")
        self.sys.calls.clear()
        self.assertEqual(self.call(self.ks, "sync")[0], 0)
        self.assertEqual(self.sys.calls, [])

    def test_sync_reloads_without_flushing_and_only_on_change(self):
        self.call(self.ks, "set", "vpn")                    # NetworkManager not running yet
        self.assertEqual(self.status()["endpoints"], 0)
        self.nm()
        self.sys.calls.clear()
        rc, _, err = self.call(self.ks, "sync")
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(self.nft_files()), 1)
        self.assertNotIn(["conntrack", "-F"], self.sys.calls)
        st = self.status()
        self.assertEqual((st["endpoints"], st["skipped_hostnames"], st["loaded"]), (4, 1, True))
        self.assertIn('"office0"', self.table()["text"])
        self.sys.calls.clear()
        self.call(self.ks, "sync")
        self.assertEqual(self.nft_files(), [])               # unchanged: nothing reloaded

    def test_nmcli_failure_keeps_what_was_learnt(self):
        self.nm()
        self.call(self.ks, "set", "vpn")
        self.assertEqual(self.status()["endpoints"], 4)
        self.sys.nmcli_fail = True
        rc, _, err = self.call(self.ks, "sync")
        self.assertEqual(rc, 1)
        self.assertIn("could not be asked", err)
        self.assertEqual(self.status()["endpoints"], 4)
        self.assertTrue(self.status()["loaded"])

    def test_vpn_endpoint_command(self):
        rc, _, err = self.call(self.ks, "vpn-endpoint", "add", "192.0.2.44:51820/udp")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.config()["vpn_endpoints"], [{"address": "192.0.2.44", "port": 51820, "protocol": "udp"}])
        self.assertEqual(self.nft_files(), [])               # kill switch off: nothing to load
        self.call(self.ks, "set", "vpn")
        self.assertIn("192.0.2.44 . udp . 51820", self.table()["text"])
        rc, _, _ = self.call(self.ks, "vpn-endpoint", "remove", "192.0.2.44:51820/udp")
        self.assertEqual(rc, 0)
        self.assertNotIn("192.0.2.44", self.table()["text"])
        rc, _, err = self.call(self.ks, "vpn-endpoint", "remove", "192.0.2.44:51820/udp")
        self.assertEqual(rc, 1)
        rc, _, err = self.call(self.ks, "vpn-endpoint", "add", "vpn.example.net:1/udp")
        self.assertEqual(rc, 1)
        self.assertIn("host names are not accepted", err)


# --------------------------------------------------------------------------- panic
class Panic(Base):
    def setUp(self):
        super().setUp()
        self.p = load(PANIC, "shadow_panic")
        self.p.CONFIG = os.path.join(self.tmp, "etc", "panic.json")
        self.p.RUN_DIR = os.path.join(self.tmp, "run")
        self.p.PENDING = os.path.join(self.tmp, "var", "shadow-os", "panic-pending.json")
        self.p.NM_STATE_FILE = os.path.join(self.tmp, "var", "nm", "NetworkManager.state")
        self.p.RFKILL_SYSFS = os.path.join(self.tmp, "sys", "rfkill")
        self.p.RFKILL_SAVED = os.path.join(self.tmp, "var", "rfkill")
        self.p.run = self.sys.run
        for idx, typ, name, soft in (("rfkill0", "wlan", "phy0", "0"), ("rfkill1", "bluetooth", "hci0", "1"),
                                     ("rfkill2", "wwan", "wwan0", "0")):
            base = os.path.join(self.p.RFKILL_SYSFS, idx)
            write(os.path.join(base, "type"), typ + "\n")
            write(os.path.join(base, "name"), name + "\n")
            write(os.path.join(base, "soft"), soft + "\n")
            write(os.path.join(base, "hard"), "0\n")
        write(os.path.join(self.p.RFKILL_SAVED, "pci-0000:00:14.3:wlan"), "0\n")
        write(os.path.join(self.p.RFKILL_SAVED, "platform-x:bluetooth"), "1\n")
        self.sys.sockets = [("tcp", "198.51.100.1", 1, "198.51.100.2", 443, 1000),
                            ("tcp", "127.0.0.1", 2, "127.0.0.1", 9050, 1000),
                            ("udp", "198.51.100.1", 3, "198.51.100.2", 53, 0)]

    def soft(self, idx):
        with open(os.path.join(self.p.RFKILL_SYSFS, idx, "soft")) as fh:
            return fh.read().strip()

    def status(self):
        return read_json(os.path.join(self.p.RUN_DIR, "panic.json"))

    def test_engage_and_release_exactly(self):
        rc, out, err = self.call(self.p, "engage")
        self.assertEqual(rc, 0, err)
        t = self.sys.tables["shadow_panic"]
        self.assertEqual(t["hooks"], {"input": (-300, "drop"), "output": (-300, "drop"), "forward": (-300, "drop")})
        self.assertIn('iif "lo" accept', t["text"])
        self.assertIn(["conntrack", "-F"], self.sys.calls)
        self.assertEqual([s[2] for s in self.sys.sockets], [2])            # only loopback left
        self.assertEqual(self.sys.networking, "disabled")
        self.assertEqual((self.soft("rfkill0"), self.soft("rfkill1"), self.soft("rfkill2")), ("1", "1", "1"))
        self.assertIn(["loginctl", "lock-sessions"], self.sys.calls)
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"], st["error"]), (True, True, None))
        self.assertEqual(st["steps"]["rfkill_blocked"], 2)
        self.assertNotIn("rfkill_saved", st["undo"])                       # stays in the root-only file
        pending = read_json(self.p.PENDING)
        self.assertEqual(os.stat(self.p.PENDING).st_mode & 0o777, 0o600)
        self.assertTrue(pending["undo"]["networking_was_enabled"])
        self.assertEqual(pending["undo"]["rfkill_saved"], {"pci-0000:00:14.3:wlan": "0", "platform-x:bluetooth": "1"})
        # the cut comes first
        names = [c[0] for c in self.sys.calls]
        self.assertLess(names.index("nft"), names.index("nmcli"))

        rc, out, err = self.call(self.p, "release")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("shadow_panic", self.sys.tables)
        self.assertEqual(self.sys.networking, "enabled")
        # Bluetooth was blocked before the panic and stays blocked
        self.assertEqual((self.soft("rfkill0"), self.soft("rfkill1"), self.soft("rfkill2")), ("0", "1", "0"))
        self.assertFalse(os.path.exists(self.p.PENDING))
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"], st["error"]), (False, False, None))

    def test_bridged_frames_are_cut_too(self):
        # Review r1-07: without br_netfilter the inet forward hook never sees bridged frames
        self.assertEqual(self.call(self.p, "engage")[0], 0)
        b = self.sys.bridge_tables["shadow_panic"]
        self.assertEqual((b["hooks"], b["comment"]), ({"forward": (-300, "drop")}, "shadow-panic"))
        self.assertEqual(b["text"].splitlines()[-2:], ["\t\tcounter drop", "\t}"])
        self.assertTrue(self.status()["bridge"])
        del self.sys.bridge_tables["shadow_panic"]
        rc, out, _ = self.call(self.p, "status", "--json")
        self.assertEqual(rc, 3)
        self.assertIn("NOT cut off", json.loads(out)["error"])
        rc, out, _ = self.call(self.p, "reassert")
        self.assertIn("had been changed", out)
        self.assertIn("shadow_panic", self.sys.bridge_tables)
        self.assertEqual(self.call(self.p, "release")[0], 0)
        self.assertEqual((self.sys.tables, self.sys.bridge_tables), ({}, {}))

    def test_kernel_without_the_bridge_family_still_panics_and_releases(self):
        self.sys.no_bridge = True
        rc, _, err = self.call(self.p, "engage")
        self.assertEqual(rc, 0, err)
        self.assertIn("no nftables bridge family", err)
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"], st["error"], st["bridge"]), (True, True, None, False))
        self.sys.calls.clear()
        self.assertEqual(self.call(self.p, "reassert"), (0, "", ""))
        self.assertEqual(self.nft_files(), [])
        rc, _, err = self.call(self.p, "release")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("shadow_panic", self.sys.tables)

    def test_lock_is_root_only(self):
        self.call(self.p, "engage")
        self.assertEqual(os.stat(os.path.join(self.p.RUN_DIR, "panic.lock")).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(os.path.join(self.p.RUN_DIR, "panic.json")).st_mode & 0o777, 0o644)

    def test_engaging_twice_keeps_the_first_snapshot(self):
        self.call(self.p, "engage")
        self.call(self.p, "engage")                    # networking is "disabled" by now
        self.assertTrue(self.status()["undo"]["networking_was_enabled"])
        self.call(self.p, "release")
        self.assertEqual(self.sys.networking, "enabled")

    def test_networking_off_before_stays_off(self):
        self.sys.networking = "disabled"
        self.call(self.p, "engage")
        self.call(self.p, "release")
        self.assertEqual(self.sys.networking, "disabled")
        self.assertNotIn(["nmcli", "networking", "on"], self.sys.calls)

    def test_config_switches_steps_off(self):
        write(self.p.CONFIG, json.dumps({"rfkill": False, "lock_session": False, "clear_clipboard": False}))
        self.call(self.p, "engage")
        self.assertEqual(self.soft("rfkill0"), "0")
        self.assertNotIn(["loginctl", "lock-sessions"], self.sys.calls)
        st = self.status()
        self.assertEqual((st["clear_clipboard"], st["lock_session"], st["steps"]["rfkill_blocked"]),
                         (False, False, None))

    def test_broken_config_still_panics(self):
        write(self.p.CONFIG, "{nope")
        rc, _, err = self.call(self.p, "engage")
        self.assertEqual(rc, 0)
        self.assertIn("using the defaults", err)
        self.assertEqual(self.soft("rfkill0"), "1")

    def test_table_failure_is_an_error_but_the_rest_still_happens(self):
        self.sys.check_fail = True
        rc, _, err = self.call(self.p, "engage")
        self.assertEqual(rc, 1)
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"]), (True, False))
        self.assertIn("rejected", st["error"])
        self.assertEqual(self.sys.networking, "disabled")
        self.assertEqual(self.soft("rfkill0"), "1")

    def test_failed_release_keeps_the_record(self):
        self.call(self.p, "engage")
        self.sys.nmcli_fail = True
        rc, _, err = self.call(self.p, "release")
        self.assertEqual(rc, 1)
        self.assertTrue(os.path.exists(self.p.PENDING))
        st = self.status()
        self.assertTrue(st["engaged"])
        self.assertIn("not turned back on", st["error"])
        self.sys.nmcli_fail = False
        self.sys.networking = "disabled"
        rc, _, err = self.call(self.p, "release")      # a second release finishes the job
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.sys.networking, "enabled")
        self.assertFalse(os.path.exists(self.p.PENDING))

    def test_radio_that_cannot_be_unblocked_keeps_the_panic_recorded(self):
        self.call(self.p, "engage")
        shutil.rmtree(os.path.join(self.p.RFKILL_SYSFS, "rfkill2"))       # WWAN unplugged: nothing to undo
        rc, _, err = self.call(self.p, "release")
        self.assertEqual(rc, 0, err)
        self.assertIn("radio not unblocked", err)
        self.call(self.p, "engage")
        with mock.patch.object(self.p, "rfkill_set", return_value="wlan radio rfkill0: Permission denied"):
            rc, _, err = self.call(self.p, "release")
        self.assertEqual(rc, 1)
        self.assertTrue(os.path.exists(self.p.PENDING))
        self.assertTrue(self.status()["engaged"])

    def test_status_rechecks_the_kernel(self):
        self.call(self.p, "engage")
        del self.sys.tables["shadow_panic"]
        rc, out, _ = self.call(self.p, "status", "--json")
        self.assertEqual(rc, 3)
        st = json.loads(out)
        self.assertEqual((st["engaged"], st["loaded"]), (True, False))
        self.assertIn("NOT cut off", st["error"])
        self.euid = 1000
        rc, out, _ = self.call(self.p, "status", "--json")
        self.assertEqual(rc, 4)
        self.assertFalse(json.loads(out)["verified"])

    def test_boot_undoes_a_panic_left_from_the_previous_boot(self):
        write(self.p.NM_STATE_FILE, "[main]\nNetworkingEnabled=true\nWirelessEnabled=true\n")
        self.call(self.p, "engage")
        # what NetworkManager and systemd-rfkill saved during the panic
        write(self.p.NM_STATE_FILE, "[main]\nNetworkingEnabled=false\nWirelessEnabled=true\nWWANEnabled=true\n")
        write(os.path.join(self.p.RFKILL_SAVED, "pci-0000:00:14.3:wlan"), "1\n")
        write(os.path.join(self.p.RFKILL_SAVED, "usb-1:wwan"), "1\n")             # first saved during it
        rc, _, err = self.call(self.p, "boot")
        self.assertEqual(rc, 0, err)
        with open(self.p.NM_STATE_FILE) as fh:
            self.assertEqual(fh.read(), "[main]\nNetworkingEnabled=true\nWirelessEnabled=true\nWWANEnabled=true\n")
        self.assertEqual(sorted(os.listdir(self.p.RFKILL_SAVED)), ["pci-0000:00:14.3:wlan", "platform-x:bluetooth"])
        with open(os.path.join(self.p.RFKILL_SAVED, "pci-0000:00:14.3:wlan")) as fh:
            self.assertEqual(fh.read().strip(), "0")
        with open(os.path.join(self.p.RFKILL_SAVED, "platform-x:bluetooth")) as fh:
            self.assertEqual(fh.read().strip(), "1")                                # blocked before: kept
        self.assertFalse(os.path.exists(self.p.PENDING))
        self.assertEqual(self.call(self.p, "boot")[0], 0)                            # nothing pending: no-op

    def test_boot_keeps_networking_off_if_it_was_off(self):
        self.sys.networking = "disabled"
        self.call(self.p, "engage")
        write(self.p.NM_STATE_FILE, "[main]\nNetworkingEnabled=false\n")
        self.call(self.p, "boot")
        with open(self.p.NM_STATE_FILE) as fh:
            self.assertIn("NetworkingEnabled=false", fh.read())


# --------------------------------------------------------------------------- watchdog (reassert)
def snapshot(directory):
    """{name: (mtime_ns, bytes)} of every file in directory: proves that nothing was written."""
    out = {}
    if os.path.isdir(directory):
        for name in sorted(os.listdir(directory)):
            full = os.path.join(directory, name)
            with open(full, "rb") as fh:
                out[name] = (os.stat(full).st_mtime_ns, fh.read())
    return out


class HeldLock:
    """Holds a tool's lock file, as a running shadow-killswitch/shadow-panic would."""

    def __init__(self, lock_path):
        self.lock_path = lock_path

    def __enter__(self):
        import fcntl
        os.makedirs(os.path.dirname(self.lock_path), exist_ok=True)
        self.fd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(self.fd, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        os.close(self.fd)


class KillSwitchWatch(KillSwitchBase):
    """shadow-killswitch reassert [--watch], run every 20 s by shadow-killswitch-watch.timer."""

    def engage(self, *args):
        rc, _, err = self.call(self.ks, "set", *args)
        self.assertEqual(rc, 0, err)
        self.sys.calls.clear()

    def watch(self, *extra):
        return self.call(self.ks, "reassert", "--watch", *extra)

    def test_silent_and_cheap_while_the_kernel_matches(self):
        self.engage("tor")
        before = snapshot(self.ks.RUN_DIR)
        self.sys.counted = 99                                  # counters move; the table is the same
        for _ in range(3):
            self.assertEqual(self.watch(), (0, "", ""))
        self.assertEqual(self.sys.calls, [["nft", "--json", "list", "table", "inet", "shadow_egress"],
                                          ["nft", "--json", "list", "table", "bridge", "shadow_egress"]] * 3)
        self.assertEqual(snapshot(self.ks.RUN_DIR), before)    # not one byte written
        self.assertEqual(self.call(self.ks, "reassert"), (0, "", ""))   # the manual form too
        self.assertEqual(self.nft_files(), [])

    def test_a_flushed_table_is_put_back_with_one_line(self):
        self.engage("tor", "--lan")
        marker = self.table()["comment"]
        del self.sys.tables["shadow_egress"]                    # nft flush ruleset
        rc, out, err = self.watch()
        self.assertEqual((rc, err), (0, ""))
        self.assertEqual(len(out.splitlines()), 1)
        self.assertIn("removed", out)
        self.assertIn("loaded again (mode tor)", out)
        self.assertEqual(self.table()["comment"], marker)       # the same mode, LAN setting included
        st = self.status()
        self.assertEqual((st["mode"], st["lan"], st["loaded"], st["error"]), ("tor", True, True, None))
        self.assertNotIn(["conntrack", "-F"], self.sys.calls)   # restoring never cuts connections
        self.assertEqual(self.watch(), (0, "", ""))             # and the next round is quiet again

    def test_a_changed_table_is_put_back(self):
        self.engage("vpn")
        self.table()["extra"] = {"output": [[{"accept": None}]]}   # nft insert rule ... accept
        rc, out, _ = self.watch()
        self.assertEqual(rc, 0)
        self.assertIn("had been changed", out)
        self.assertNotIn("extra", self.table())
        self.sys.calls.clear()
        self.table()["hooks"]["output"] = (-50, "accept")          # a policy changed by hand
        self.assertIn("had been changed", self.watch()[1])
        self.assertEqual(self.table()["hooks"]["output"], (-50, "drop"))

    def test_nothing_before_the_boot_unit_ran(self):
        write(self.ks.CONFIG, json.dumps({"mode": "tor"}))
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertEqual(self.sys.calls, [])
        self.assertFalse(os.path.exists(self.ks.RUN_DIR))       # not even the lock file
        self.assertIsNone(self.table())                         # the package never engages it
        # an administrator's manual reassert loads the mode in force, as before
        rc, out, err = self.call(self.ks, "reassert")
        self.assertEqual((rc, out), (0, ""), err)
        self.assertIsNotNone(self.table())

    def test_escape_hatch_and_runtime_off_stay_off(self):
        write(self.ks.CONFIG, json.dumps({"mode": "tor"}))
        write(self.ks.CMDLINE, "root=UUID=x ro shadow.killswitch=off\n")
        self.call(self.ks, "boot")
        self.sys.calls.clear()
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertIsNone(self.table())
        self.assertEqual(self.nft_files(), [])
        write(self.ks.CMDLINE, "root=UUID=x ro\n")
        self.engage("tor")
        self.engage("off", "--runtime")
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertIsNone(self.table())
        self.assertEqual(self.config()["mode"], "tor")

    def test_a_stray_table_while_off_is_removed(self):
        self.engage("tor")
        saved = dict(self.table())
        self.engage("off")
        self.sys.tables["shadow_egress"] = saved                 # e.g. an old "nft list ruleset" restored
        rc, out, _ = self.watch()
        self.assertEqual(rc, 0)
        self.assertIn("although the kill switch is off", out)
        self.assertIsNone(self.table())

    def test_a_failed_load_is_not_retried_every_round(self):
        write(self.ks.CONFIG, json.dumps({"mode": "vpn"}))
        self.sys.check_fail = True
        self.assertEqual(self.call(self.ks, "boot")[0], 1)     # reported here, once
        self.sys.calls.clear()
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertEqual([c for c in self.sys.calls if c[0] == "nft"], [])
        rc, _, err = self.call(self.ks, "reassert")             # the manual form does retry
        self.assertEqual(rc, 1)
        self.assertIn("rejected by nft --check", err)
        self.sys.check_fail = False
        self.assertEqual(self.call(self.ks, "reassert")[0], 0)
        self.assertTrue(self.status()["loaded"])
        del self.sys.tables["shadow_egress"]
        self.assertIn("loaded again", self.watch()[1])          # watched again once it worked

    def test_a_failed_restore_is_reported_once(self):
        self.engage("tor")
        del self.sys.tables["shadow_egress"]
        self.sys.check_fail = True
        rc, out, err = self.watch()
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("rejected by nft --check", err)
        self.assertFalse(self.status()["loaded"])
        self.assertEqual(self.watch(), (0, "", ""))             # not every 20 s

    def test_a_running_shadow_killswitch_is_not_waited_for(self):
        self.engage("tor")
        del self.sys.tables["shadow_egress"]
        with HeldLock(os.path.join(self.ks.RUN_DIR, "killswitch.lock")):
            self.assertEqual(self.watch(), (0, "", ""))          # skips this round at once
            self.assertIsNone(self.table())
        self.assertIn("loaded again", self.watch()[1])

    def test_state_of_an_older_version_gets_its_digest_once(self):
        self.engage("tor")
        state_file = os.path.join(self.ks.RUN_DIR, "killswitch-state.json")
        st = read_json(state_file)
        for key in ("table_digest", "applied"):
            st.pop(key)
        write(state_file, json.dumps(st))
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertEqual(self.nft_files(), [])                  # trusted (marker and chains match) ...
        self.assertIsNotNone(read_json(state_file)["table_digest"])   # ... and from now on exactly
        self.table()["extra"] = {"forward": [[{"accept": None}]]}
        self.assertIn("had been changed", self.watch()[1])

    def test_a_stale_status_is_corrected_without_a_reload(self):
        self.engage("tor")
        del self.sys.tables["shadow_egress"]
        self.call(self.ks, "status")                            # records "not loaded"
        self.assertFalse(self.status()["loaded"])
        self.call(self.ks, "reassert")
        self.sys.calls.clear()
        st = self.status()
        st["loaded"] = False                                     # as if written during the gap
        write(os.path.join(self.ks.RUN_DIR, "killswitch.json"), json.dumps(st))
        self.assertEqual(self.watch(), (0, "", ""))
        self.assertTrue(self.status()["loaded"])
        self.assertEqual(self.nft_files(), [])

    def test_digest_ignores_handles_and_counters_only(self):
        expr = [{"counter": {"packets": 5, "bytes": 9}}, {"drop": None}]
        items = [{"metainfo": {"version": "1"}}, {"table": {"name": "t", "handle": 3}},
                 {"rule": {"chain": "output", "handle": 9, "expr": expr}}]
        same = json.loads(json.dumps(items))
        same[0]["metainfo"]["version"] = "2"
        same[1]["table"]["handle"] = 4
        same[2]["rule"]["handle"] = 1
        same[2]["rule"]["expr"][0]["counter"] = {"packets": 0, "bytes": 0}
        self.assertEqual(self.ks.table_digest(items), self.ks.table_digest(same))
        other = json.loads(json.dumps(items))
        other[2]["rule"]["expr"][1] = {"accept": None}
        self.assertNotEqual(self.ks.table_digest(items), self.ks.table_digest(other))


class PanicWatch(Base):
    """shadow-panic reassert."""

    def setUp(self):
        super().setUp()
        self.p = load(PANIC, "shadow_panic")
        self.p.CONFIG = os.path.join(self.tmp, "etc", "panic.json")
        self.p.RUN_DIR = os.path.join(self.tmp, "run")
        self.p.PENDING = os.path.join(self.tmp, "var", "shadow-os", "panic-pending.json")
        self.p.NM_STATE_FILE = os.path.join(self.tmp, "var", "nm", "NetworkManager.state")
        self.p.RFKILL_SYSFS = os.path.join(self.tmp, "sys", "rfkill")
        self.p.RFKILL_SAVED = os.path.join(self.tmp, "var", "rfkill")
        self.p.run = self.sys.run
        write(self.p.CONFIG, json.dumps({"rfkill": False, "lock_session": False}))

    def status(self):
        return read_json(os.path.join(self.p.RUN_DIR, "panic.json"))

    def reassert(self):
        return self.call(self.p, "reassert")

    def engage(self):
        rc, _, err = self.call(self.p, "engage")
        self.assertEqual(rc, 0, err)
        self.sys.calls.clear()

    def test_nothing_without_a_panic(self):
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertEqual(self.sys.calls, [])
        self.assertFalse(os.path.exists(self.p.RUN_DIR))
        self.engage()
        self.call(self.p, "release")
        self.sys.calls.clear()
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertEqual(self.sys.calls, [])
        self.assertNotIn("shadow_panic", self.sys.tables)

    def test_silent_while_the_table_is_intact(self):
        self.engage()
        before = snapshot(self.p.RUN_DIR)
        self.sys.counted = 1234
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertEqual(self.sys.calls, [["nft", "--json", "list", "table", "inet", "shadow_panic"],
                                          ["nft", "--json", "list", "table", "bridge", "shadow_panic"]])
        self.assertEqual(snapshot(self.p.RUN_DIR), before)

    def test_a_flushed_or_changed_table_is_put_back(self):
        self.engage()
        del self.sys.tables["shadow_panic"]
        rc, out, err = self.reassert()
        self.assertEqual((rc, err), (0, ""))
        self.assertEqual(len(out.splitlines()), 1)
        self.assertIn("removed", out)
        t = self.sys.tables["shadow_panic"]
        self.assertEqual(t["hooks"], {"input": (-300, "drop"), "output": (-300, "drop"), "forward": (-300, "drop")})
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"], st["error"]), (True, True, None))
        self.assertEqual(st["steps"]["table"], True)
        # only the table: no second round of nmcli, rfkill or session locking
        self.assertEqual({c[0] for c in self.sys.calls}, {"nft"})
        t["extra"] = {"output": [[{"accept": None}]]}
        self.assertIn("had been changed", self.reassert()[1])
        self.assertNotIn("extra", self.sys.tables["shadow_panic"])
        self.assertEqual(self.reassert(), (0, "", ""))

    def test_a_release_in_progress_is_not_undone(self):
        self.engage()
        with mock.patch.object(self.p, "nm_set_networking", return_value="NetworkManager is not running"):
            self.call(self.p, "release")                          # the table goes, networking stays off
        st = self.status()
        self.assertEqual((st["engaged"], st["release_incomplete"]), (True, True))
        self.assertNotIn("shadow_panic", self.sys.tables)
        self.sys.calls.clear()
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertNotIn("shadow_panic", self.sys.tables)
        self.assertEqual(self.sys.calls, [])

    def test_a_table_engage_could_not_load_is_not_retried(self):
        self.sys.check_fail = True
        self.assertEqual(self.call(self.p, "engage")[0], 1)
        self.sys.check_fail = False
        self.sys.calls.clear()
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertEqual(self.sys.calls, [])

    def test_a_failed_reload_is_reported_once(self):
        self.engage()
        del self.sys.tables["shadow_panic"]
        self.sys.check_fail = True
        rc, out, err = self.reassert()
        self.assertEqual(rc, 1)
        self.assertIn("loading it again failed", err)
        st = self.status()
        self.assertEqual((st["engaged"], st["loaded"], st["steps"]["table"]), (True, False, False))
        self.assertIn("NOT cut off", st["error"])
        self.assertEqual(self.reassert(), (0, "", ""))

    def test_a_running_shadow_panic_is_not_waited_for(self):
        self.engage()
        del self.sys.tables["shadow_panic"]
        with HeldLock(os.path.join(self.p.RUN_DIR, "panic.lock")):
            self.assertEqual(self.reassert(), (0, "", ""))
        self.assertIn("loaded again", self.reassert()[1])

    def test_engaged_by_an_older_version(self):
        self.engage()
        st = self.status()
        st.pop("table_digest")
        write(os.path.join(self.p.RUN_DIR, "panic.json"), json.dumps(st))
        self.assertEqual(self.reassert(), (0, "", ""))
        self.assertEqual(self.nft_files(), [])
        self.assertIsNotNone(self.status()["table_digest"])


# --------------------------------------------------------------------------- polkit, units, packaging
class Policy(unittest.TestCase):
    POLICY = os.path.join(ROOT, "security", "permissions", "org.shadow.network.policy")
    RULES = os.path.join(ROOT, "security", "permissions", "49-shadow-panic.rules")

    def test_actions(self):
        root = ET.parse(self.POLICY).getroot()
        actions = {a.get("id"): a for a in root.findall("action")}
        # org.shadow.netprivacy.manage (shadow-netprivacy) shares the file;
        # tests/security/test_netprivacy.py checks it.
        self.assertEqual(set(actions) - {"org.shadow.netprivacy.manage"},
                         {"org.shadow.killswitch.manage", "org.shadow.panic.release"})
        expect = {"org.shadow.killswitch.manage": ("auth_admin_keep", "/usr/sbin/shadow-killswitch"),
                  "org.shadow.panic.release": ("auth_admin", "/usr/sbin/shadow-panic")}
        for aid, (active, exe) in expect.items():
            a = actions[aid]
            self.assertEqual(a.find("defaults/allow_active").text, active)
            self.assertEqual(a.find("defaults/allow_any").text, "auth_admin")
            self.assertEqual(a.find("defaults/allow_inactive").text, "auth_admin")
            self.assertEqual(a.find("annotate[@key='org.freedesktop.policykit.exec.path']").text, exe)

    @unittest.skipUnless(shutil.which("xmllint"), "xmllint is not installed")
    def test_policy_against_the_dtd(self):
        dtd = "/usr/share/polkit-1/policyconfig-1.dtd"
        if not os.path.exists(dtd):
            self.skipTest("the polkit DTD is not installed")
        p = subprocess.run(["xmllint", "--noout", "--nonet", "--dtdvalid", dtd, self.POLICY],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 0, p.stderr.decode())

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_rule_grants_only_starting_the_panic_unit(self):
        harness = r"""
const fs = require("fs");
const rules = [];
const polkit = {Result: {YES: "yes", NO: "no", AUTH_ADMIN: "auth_admin", NOT_HANDLED: null},
                addRule: f => rules.push(f), log: () => {}, spawn: () => { throw new Error("no spawn"); }};
new Function("polkit", fs.readFileSync(process.argv[1], "utf8"))(polkit);
const cases = JSON.parse(process.argv[2]);
const out = cases.map(c => {
  const action = {id: c.id, lookup: k => c.details[k]};
  const subject = {local: c.local, active: c.active, isInGroup: g => c.groups.includes(g), user: "u"};
  return rules.map(r => r(action, subject)).find(r => r !== null && r !== undefined) ?? null;
});
console.log(JSON.stringify({count: rules.length, out}));
"""
        base = {"id": "org.freedesktop.systemd1.manage-units",
                "details": {"unit": "shadow-panic.service", "verb": "start"},
                "local": True, "active": True, "groups": ["sudo"]}
        variants = [({}, "yes"),
                    ({"details": {"unit": "shadow-panic.service", "verb": "stop"}}, None),
                    ({"details": {"unit": "shadow-panic.service", "verb": "restart"}}, None),
                    ({"details": {"unit": "shadow-killswitch.service", "verb": "start"}}, None),
                    ({"details": {"unit": "shadow-panic.service"}}, None),
                    ({"id": "org.freedesktop.systemd1.manage-unit-files"}, None),
                    ({"id": "org.shadow.panic.release"}, None),
                    ({"local": False}, None), ({"active": False}, None), ({"groups": ["netdev"]}, None)]
        cases = [dict(base, **v) for v, _ in variants]
        p = subprocess.run(["node", "-e", harness, self.RULES, json.dumps(cases)], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr.decode())
        result = json.loads(p.stdout)
        self.assertEqual(result["count"], 1)
        self.assertEqual(result["out"], [want for _, want in variants])


class UnitsAndPackaging(unittest.TestCase):
    SYSTEMD = os.path.join(FW, "systemd")

    def unit(self, name):
        with open(os.path.join(self.SYSTEMD, name)) as fh:
            return [ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")]

    def test_killswitch_unit_runs_before_the_network(self):
        u = self.unit("shadow-killswitch.service")
        for line in ("DefaultDependencies=no", "After=local-fs.target", "Wants=network-pre.target",
                     "Type=oneshot", "RemainAfterExit=yes", "ExecStart=/usr/sbin/shadow-killswitch boot",
                     "WantedBy=sysinit.target"):
            self.assertIn(line, u)
        self.assertTrue(any(ln.startswith("Before=") and "network-pre.target" in ln for ln in u))
        self.assertFalse(any(ln.startswith("ExecStop") for ln in u))       # stopping never opens the network

    def test_panic_units(self):
        u = self.unit("shadow-panic.service")
        self.assertIn("ExecStart=/usr/sbin/shadow-panic engage", u)
        self.assertNotIn("RemainAfterExit=yes", u)          # a start after a release must engage again
        self.assertFalse(any(ln.startswith("WantedBy") for ln in u))
        r = self.unit("shadow-panic-reset.service")
        self.assertIn("ExecStart=/usr/sbin/shadow-panic boot", r)
        self.assertIn("ConditionPathExists=/var/lib/shadow-os/panic-pending.json", r)
        before = [ln for ln in r if ln.startswith("Before=")][0]
        for unit in ("NetworkManager.service", "systemd-rfkill.service"):
            self.assertIn(unit, before)

    def test_nftables_dropin(self):
        u = self.unit("nftables.service.d/shadow.conf")
        stops = [ln for ln in u if ln.startswith("ExecStop=")]
        self.assertEqual(stops, ["ExecStop=", "ExecStop=-/usr/sbin/nft delete table inet shadow_filter"])
        self.assertNotIn("flush ruleset", " ".join(u))
        self.assertEqual([ln for ln in u if ln.startswith("ExecStartPost=")],
                         ["ExecStartPost=-/usr/sbin/shadow-panic reassert",
                          "ExecStartPost=-/usr/sbin/shadow-killswitch reassert"])

    def test_watchdog_units(self):
        t = self.unit("shadow-killswitch-watch.timer")
        for line in ("OnBootSec=30s", "OnUnitActiveSec=20s", "AccuracySec=2s", "WantedBy=timers.target"):
            self.assertIn(line, t)
        self.assertFalse(any(ln.startswith("Unit=") for ln in t))         # the service of the same name
        s = self.unit("shadow-killswitch-watch.service")
        self.assertIn("Type=oneshot", s)
        self.assertNotIn("RemainAfterExit=yes", s)                          # every tick starts it again
        self.assertFalse(any(ln.startswith("WantedBy") for ln in s))       # only the timer starts it
        self.assertEqual([ln for ln in s if ln.startswith("ExecStart")],
                         ["ExecStart=-/usr/sbin/shadow-panic reassert",
                          # the per-application firewall's rule, while a blocked application runs
                          "ExecStart=-/usr/sbin/shadow-appfw reassert",
                          "ExecStart=/usr/sbin/shadow-killswitch reassert --watch"])
        # quiet when nothing happens, but what the tools print is kept
        self.assertIn("LogLevelMax=notice", s)
        self.assertIn("SyslogLevel=warning", s)
        self.assertIn("CapabilityBoundingSet=CAP_NET_ADMIN", s)
        with open(os.path.join(ROOT, "debian", "rules")) as fh:
            rules = [ln.strip() for ln in fh if ln.startswith("\tdh_installsystemd")]
        self.assertIn("dh_installsystemd -pshadow-hardening shadow-killswitch-watch.timer", rules)
        self.assertIn("dh_installsystemd -pshadow-hardening --no-enable --no-start shadow-killswitch-watch.service",
                      rules)

    def test_dispatcher(self):
        script = os.path.join(FW, "NetworkManager", "50-shadow-killswitch")
        self.assertTrue(os.access(script, os.X_OK))
        self.assertEqual(subprocess.run(["sh", "-n", script]).returncode, 0)
        with open(script) as fh:
            text = fh.read()
        self.assertIn("exec /usr/sbin/shadow-killswitch sync", text)
        for event in ("up", "down", "pre-up", "vpn-up", "vpn-down", "vpn-pre-up"):
            self.assertRegex(text, r"\b%s\b" % re.escape(event))
        # other events end the script before anything runs
        self.assertEqual(subprocess.run(["sh", script, "wlan0", "dhcp4-change"]).returncode, 0)

    def test_packaging_lists(self):
        with open(os.path.join(ROOT, "debian", "shadow-hardening.install")) as fh:
            entries = [ln.split() for ln in fh if ln.strip()]
        sources = {src: dst for src, dst in entries}
        for src, dst in (("security/firewall/shadow-killswitch", "usr/sbin"),
                         ("security/firewall/shadow-panic", "usr/sbin"),
                         ("security/firewall/systemd/shadow-killswitch.service", "usr/lib/systemd/system"),
                         ("security/firewall/systemd/shadow-panic.service", "usr/lib/systemd/system"),
                         ("security/firewall/systemd/shadow-panic-reset.service", "usr/lib/systemd/system"),
                         ("security/firewall/systemd/shadow-killswitch-watch.service", "usr/lib/systemd/system"),
                         ("security/firewall/systemd/shadow-killswitch-watch.timer", "usr/lib/systemd/system"),
                         ("security/firewall/systemd/nftables.service.d", "usr/lib/systemd/system"),
                         ("security/firewall/NetworkManager/50-shadow-killswitch",
                          "usr/lib/NetworkManager/dispatcher.d"),
                         ("security/permissions/org.shadow.network.policy", "usr/share/polkit-1/actions"),
                         ("security/permissions/49-shadow-panic.rules", "usr/share/polkit-1/rules.d")):
            self.assertEqual(sources.get(src), dst, src)
            self.assertTrue(os.path.exists(os.path.join(ROOT, src)), src)
        with open(os.path.join(ROOT, "debian", "shadow-hardening.links")) as fh:
            self.assertIn("usr/lib/NetworkManager/dispatcher.d/50-shadow-killswitch "
                          "usr/lib/NetworkManager/dispatcher.d/pre-up.d/50-shadow-killswitch", fh.read())
        with open(os.path.join(ROOT, "debian", "control")) as fh:
            stanza = fh.read().split("Package: shadow-hardening\n", 1)[1].split("\n\n", 1)[0]
        depends = re.search(r"^Depends:(.*?)^\S", stanza + "\nX", re.S | re.M).group(1)
        self.assertIn("conntrack", depends)
        for tool in (KILLSWITCH, PANIC):
            self.assertTrue(os.access(tool, os.X_OK), tool)


class Documentation(unittest.TestCase):
    DOC = os.path.join(ROOT, "docs", "security", "kill-switch.md")

    def test_manual_recovery_is_not_undone_by_the_watchdog(self):
        # Review r1-17: tables deleted with nft while the recorded state says "on" or
        # "engaged" are loaded again by shadow-killswitch-watch.timer within ~20 s. The
        # recovery section must lead with the commands that record the change, and may
        # name a raw "nft delete table" only after stopping the watchdog.
        with open(self.DOC, encoding="utf-8") as fh:
            text = fh.read()
        section = text.split("## Turning it off when there is no network", 1)[1].split("\n## ", 1)[0]
        self.assertIn("sudo shadow-killswitch set off", section)
        self.assertIn("sudo shadow-panic release", section)
        stop = section.find("\nsudo systemctl stop shadow-killswitch-watch.timer")
        deletes = [m.start() for m in re.finditer(r"^(?:sudo )?nft delete table", section, re.M)]
        self.assertTrue(deletes)
        self.assertTrue(stop >= 0 and all(stop < d for d in deletes), section)


if __name__ == "__main__":
    unittest.main()
