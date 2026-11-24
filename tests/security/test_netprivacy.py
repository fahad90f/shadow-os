#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests of shadow-netprivacy and the shipped encrypted-DNS / MAC files.

    python3 tests/security/test_netprivacy.py [-v]

shadow-netprivacy is loaded with ROOT pointed at a temporary directory and its
run() replaced by FakeSystem (systemctl, busctl, nmcli, dnscrypt-proxy). The
tests prove: what the shipped resolved and NetworkManager files configure
(and that every NetworkManager [connection] default has a value NetworkManager
1.52 accepts), the provider strings and DNS-over-HTTPS stamps, input
validation, what each command writes and rolls back, that status reports what
resolved says rather than what was asked for, the escape hatch, the root-only
lock file, and the packaging (libnss-resolve never pulled in, polkit action, unit).

The DNS probe runs against a real UDP socket on 127.0.0.1. Set DNSCRYPT_PROXY
to a dnscrypt-proxy binary to also have it check the generated configuration.

Standard library only; runs as any user, changes nothing outside the
temporary directory.
"""
import configparser
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, "security", "hardening", "shadow-netprivacy")
RESOLVED_FILE = os.path.join(ROOT, "security", "hardening", "resolved.conf.d", "80-shadow-privacy.conf")
NM_FILE = os.path.join(ROOT, "security", "hardening", "NetworkManager", "80-shadow-privacy.conf")
UNIT = os.path.join(ROOT, "security", "hardening", "systemd", "shadow-netprivacy.service")
POLICY = os.path.join(ROOT, "security", "permissions", "org.shadow.network.policy")
CONTROL = os.path.join(ROOT, "debian", "control")
WIFI_UUID = "2b0c7e4a-1d5f-4c3e-9a77-0f1e2d3c4b5a"
ETH_UUID = "7f9e8d7c-6b5a-4c3d-8e2f-1a0b9c8d7e6f"
VPN_UUID = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"


def shadow_anon():
    sys.path.insert(0, HERE)
    import shadow_anon_testlib
    return shadow_anon_testlib.load_tool()


def hide_me_dropin():
    """The drop-in Hide Me (shadow-anon) really writes, not a copy of it."""
    return shadow_anon().RESOLVED_DROPIN_TEXT


# Tor's DNSPort as shadow-anon sets it (a port below 1024), as text and as
# resolved's CurrentDNSServerEx reports it.
TOR = shadow_anon().TOR_DNS_SERVER
TOR_PORT = int(TOR.rsplit(":", 1)[1])


def load(path, name):
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def control_stanza(package):
    text = read(CONTROL)
    match = re.search(r"(?ms)^Package: " + re.escape(package) + r"\n.*?(?=\n\n|\Z)", text)
    assert match, package
    fields, key = {}, None
    for line in match.group(0).splitlines():
        if line.startswith((" ", "\t")) and key:
            fields[key] += " " + line.strip()
        else:
            key, _, value = line.partition(":")
            fields[key] = value.strip()
    return fields


def relation_names(value):
    return {re.split(r"[\s(\[]", item.strip())[0] for item in value.split(",") if item.strip()}


# NetworkManager 1.52 reads these [connection] defaults as integers in a range
# (src/core/devices/nm-device.c, nm_config_data_get_connection_default_int64);
# anything else, e.g. "no", is ignored silently.
NM_INT_KEYS = {
    "ipv4.dhcp-send-release": (0, 1), "ipv6.dhcp-send-release": (0, 1),
    "ipv4.dhcp-send-hostname": (0, 1), "ipv6.dhcp-send-hostname": (0, 1),
    "connection.llmnr": (0, 2), "connection.mdns": (0, 2), "connection.dns-over-tls": (0, 2),
    "ipv6.ip6-privacy": (-1, 2),
}
NM_STRING_KEYS = {"connection.stable-id", "wifi.cloned-mac-address", "ethernet.cloned-mac-address", "match-device",
                  "ipv6.dhcp-duid"}
NM_CLONED_VALUES = {"preserve", "permanent", "random", "stable", "stable-ssid"}
# ipv6.dhcp-duid (nm-device.c, _prop_get_ipv6_dhcp_duid). Unset means "lease":
# a DUID hashed from /etc/machine-id only, the same on every network and boot.
NM_DUID_VALUES = {"lease", "llt", "ll", "stable-llt", "stable-ll", "stable-uuid"}


class FakeSystem:
    """systemctl, busctl, nmcli and dnscrypt-proxy, in memory."""

    def __init__(self, np):
        self.np = np
        self.calls = []
        self.units = {"systemd-resolved.service": "active", "NetworkManager.service": "active"}
        self.live = None              # None: follow the files; dict: what resolved reports
        self.resolved_restart_fails = False
        self.busctl_fails = False
        self.nm_reload_fails = False
        self.dnscrypt_check_ok = True
        self.current_server = [0, 2, [192, 0, 2, 53], 0, "dns.example.net"]   # CurrentDNSServerEx
        self.connections = {WIFI_UUID: {"type": "802-11-wireless", "stable-id": ""},
                            ETH_UUID: {"type": "802-3-ethernet", "stable-id": ""},
                            VPN_UUID: {"type": "vpn", "stable-id": ""}}

    def resolved_reports(self):
        if self.live is not None:
            return self.live
        conf, _ = self.np.resolved_config()
        dns = self.np.configured_dns(conf)
        return {"DNSOverTLS": dns["dns_over_tls"], "DNSSEC": dns["dnssec"], "LLMNR": dns["llmnr"],
                "MulticastDNS": dns["mdns"]}

    def run(self, argv, check=True, timeout=None):
        self.calls.append(list(argv))
        rc, out, err = self._dispatch(list(argv))
        if check and rc != 0:
            raise self.np.Failure(f"{argv[0]} failed: {err or out}")
        return rc, out, err

    def _dispatch(self, a):
        prog = a[0]
        if prog == "systemctl":
            verb, units = a[1], [u for u in a[2:] if not u.startswith("-")]
            if verb == "is-active":
                state = self.units.get(units[0], "inactive")
                return (0 if state == "active" else 3), state + "\n", ""
            if verb == "restart" and units == ["systemd-resolved.service"]:
                if self.resolved_restart_fails:
                    return 1, "", "Job for systemd-resolved.service failed."
                self.units["systemd-resolved.service"] = "active"
                return 0, "", ""
            if verb in ("restart", "start", "enable", "disable", "daemon-reload", "try-restart"):
                for unit in units:
                    if verb in ("restart", "start"):
                        self.units[unit] = "active"
                    elif verb == "disable" and "--now" in a:
                        self.units[unit] = "inactive"
                return 0, "", ""
            return 1, "", f"unhandled systemctl {a}"
        if prog == "busctl":
            if self.busctl_fails or self.units.get("systemd-resolved.service") != "active":
                return 1, "", "Failed to get property"
            reports = self.resolved_reports()
            lines = []
            for name in a[6:]:
                if name == "CurrentDNSServerEx":
                    lines.append(json.dumps({"type": "(iiayqs)", "data": self.current_server}))
                else:
                    lines.append(json.dumps({"type": "s", "data": reports[name]}))
            return 0, "\n".join(lines) + "\n", ""
        if prog == "nmcli":
            if a[1:4] == ["general", "reload", "conf"]:
                return (1, "", "Error: reload failed") if self.nm_reload_fails else (0, "", "")
            if a[1:5] == ["-t", "-f", "UUID,TYPE", "connection"]:
                return 0, "".join(f"{u}:{c['type']}\n" for u, c in self.connections.items()), ""
            if a[1] == "-g":
                field, uuid = a[2], a[5]
                if uuid not in self.connections:
                    return 10, "", f"Error: {uuid} - no such connection profile."
                key = {"connection.type": "type", "connection.stable-id": "stable-id"}[field]
                return 0, self.connections[uuid][key] + "\n", ""
            if a[1:3] == ["connection", "modify"]:
                uuid, prop, value = a[3], a[4], a[5]
                self.connections[uuid][prop.split(".", 1)[1]] = value
                return 0, "", ""
            return 1, "", f"unhandled nmcli {a}"
        if prog.endswith("dnscrypt-proxy"):
            return (0, "Configuration successfully checked", "") if self.dnscrypt_check_ok else (255, "", "[FATAL] bad stamp")
        return 127, "", f"{prog} is not installed"


class Base(unittest.TestCase):
    def setUp(self):
        self.np = load(TOOL, "shadow_netprivacy_test")
        self.tmp = tempfile.mkdtemp(prefix="netprivacy-test.")
        self.np.ROOT = self.tmp
        self.fake = FakeSystem(self.np)
        self.np.run = self.fake.run
        self.np.time.sleep = lambda s: None
        self.np.wait_for_doh = lambda attempts=5, pause=2.0: self.doh_answers
        self.doh_answers = True
        # The installed system: resolved, the shipped defaults, its stub link.
        write(self.p("/usr/lib/systemd/systemd-resolved"), "")
        write(self.p("/etc/systemd/resolved.conf"), "[Resolve]\n#DNS=\n")
        write(self.p("/usr/lib/systemd/resolved.conf.d/00-disable-mdns.conf"), "[Resolve]\nMulticastDNS=no\n")
        shutil.copy(RESOLVED_FILE, self.p("/usr/lib/systemd/resolved.conf.d/80-shadow-privacy.conf"))
        write(self.p("/usr/lib/NetworkManager/conf.d/80-shadow-privacy.conf"), read(NM_FILE))
        write(self.p("/etc/NetworkManager/NetworkManager.conf"), "[main]\nplugins=ifupdown,keyfile\n")
        write(self.p("/etc/nsswitch.conf"), "passwd: files systemd\nhosts:          files dns\n")
        write(self.p("/run/systemd/resolve/stub-resolv.conf"), "nameserver 127.0.0.53\n")
        os.makedirs(self.p("/etc"), exist_ok=True)
        os.symlink("../run/systemd/resolve/stub-resolv.conf", self.p("/etc/resolv.conf"))
        write(self.p("/proc/cmdline"), "BOOT_IMAGE=/vmlinuz root=UUID=x ro quiet\n")
        self.root_patch = mock.patch.object(self.np.os, "geteuid", return_value=0)
        self.root_patch.start()

    def tearDown(self):
        self.root_patch.stop()
        shutil.rmtree(self.tmp)

    def p(self, path):
        return self.tmp + path

    def cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = self.np.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def effective(self):
        conf, _ = self.np.resolved_config()
        return self.np.configured_dns(conf)


# --------------------------------------------------------------------------- shipped files
class ShippedFiles(Base):
    def test_resolved_defaults(self):
        dns = self.effective()
        self.assertEqual(dns["mode"], "dot")
        self.assertEqual(dns["provider"], "quad9")
        self.assertEqual(dns["servers"], self.np.PROVIDERS["quad9"]["dot"])
        self.assertEqual(dns["dns_over_tls"], "opportunistic")
        self.assertEqual(dns["dnssec"], "allow-downgrade")
        self.assertTrue(dns["all_names_to_provider"], "Domains=~. keeps the network's server from seeing every name")
        self.assertEqual(dns["fallback_servers"], [])
        self.assertEqual((dns["llmnr"], dns["mdns"]), ("no", "no"))

    def test_resolved_defaults_win_over_main_file(self):
        write(self.p("/etc/systemd/resolved.conf"), "[Resolve]\nDNS=198.51.100.7\nDomains=example\n")
        dns = self.effective()
        self.assertEqual(dns["servers"], self.np.PROVIDERS["quad9"]["dot"], "the drop-in resets the list")
        self.assertTrue(dns["all_names_to_provider"])

    def test_every_sni_server_parses(self):
        for ident, entry in self.np.PROVIDERS.items():
            for server in entry["dot"]:
                self.assertEqual(self.np.parse_server(server), server, ident)

    def test_nm_file_values_are_valid_for_nm_1_52(self):
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.optionxform = str
        parser.read_string(read(NM_FILE))
        self.assertEqual(parser.get("main", "dns"), "systemd-resolved")
        self.assertEqual(parser.get("device", "wifi.scan-rand-mac-address"), "yes")
        for section in parser.sections():
            if not section.startswith("connection"):
                continue
            for key, value in parser.items(section):
                if key in NM_INT_KEYS:
                    low, high = NM_INT_KEYS[key]
                    self.assertRegex(value, r"^-?\d+$", f"{key}={value}: NetworkManager reads an integer")
                    self.assertTrue(low <= int(value) <= high, f"{key}={value}")
                else:
                    self.assertIn(key, NM_STRING_KEYS, f"unexpected key {key} in [{section}]")
                if key.endswith("cloned-mac-address"):
                    self.assertIn(value, NM_CLONED_VALUES)
                    self.assertNotEqual(value, "random", "random breaks DHCP pools and MAC filters")
                if key == "ipv6.dhcp-duid":
                    self.assertIn(value, NM_DUID_VALUES)
        conn = dict(parser.items("connection"))
        self.assertEqual(conn["connection.stable-id"], "${CONNECTION}/${BOOT}")
        self.assertEqual(conn["wifi.cloned-mac-address"], "stable")
        self.assertEqual(conn["ethernet.cloned-mac-address"], "stable")
        # The DHCPv6 DUID follows the stable-id like the MAC address; without it
        # DHCPv6 sends one machine-id DUID on every network (links the addresses).
        self.assertEqual(conn.get("ipv6.dhcp-duid"), "stable-uuid")
        self.assertEqual(conn["ipv4.dhcp-send-hostname"], "0")
        self.assertEqual(conn["connection.llmnr"], "0")
        self.assertEqual(conn["connection.mdns"], "0")
        self.assertEqual(conn["connection.dns-over-tls"], "1")
        self.assertEqual(conn["ipv4.dhcp-send-release"], "1")

    def test_nm_leaves_fixed_mac_drivers_alone(self):
        parser = configparser.ConfigParser(interpolation=None)
        parser.optionxform = str
        parser.read_string(read(NM_FILE))
        quirk = dict(parser.items("connection-shadow-mac-fixed-drivers"))
        self.assertEqual(set(re.findall(r"driver:(\w+)", quirk["match-device"])), {"eagle_sdio", "wl"})
        self.assertEqual(quirk["wifi.cloned-mac-address"], "preserve")
        self.assertEqual(quirk["ethernet.cloned-mac-address"], "preserve")

    def test_shipped_mac_policy_is_per_boot(self):
        merged, named, _ = self.np.nm_config()
        mac = self.np.configured_mac(merged)
        self.assertEqual(mac["mode"], "per-boot")
        self.assertTrue(mac["scan_randomization"])


# --------------------------------------------------------------------------- validation
class Validation(Base):
    def test_providers(self):
        for ident in ("quad9", "mullvad", "cloudflare"):
            self.assertEqual(self.np.parse_provider(ident)["id"], ident)
        self.assertEqual(self.np.parse_provider(None)["id"], "quad9")
        custom = self.np.parse_provider("custom:192.0.2.53#dns.example.net, 2001:db8::53#DNS.Example.net")
        self.assertEqual(custom["dot"], ["192.0.2.53#dns.example.net", "2001:db8::53#dns.example.net"])
        self.assertEqual(custom["doh"], [])

    def test_bad_providers(self):
        bad = ["custom:", "custom:dns.example.net#dns.example.net", "custom:192.0.2.53",
               "custom:192.0.2.53#nodot", "custom:192.0.2.53#-bad-.example", "custom:127.0.0.53#dns.example.net",
               "custom:0.0.0.0#dns.example.net", "custom:224.0.0.1#dns.example.net",
               "custom:192.0.2.53#a.example#b", "opendns", "custom:" + ",".join(
                   f"192.0.2.{i}#dns.example.net" for i in range(1, 11)),
               "custom:192.0.2.53#dns.example.net;rm"]
        for spec in bad:
            with self.assertRaises(self.np.Failure, msg=spec):
                self.np.parse_provider(spec)

    def test_mode_combinations(self):
        with self.assertRaises(self.np.Failure):
            self.np.validate_dns_request("doh", "custom:192.0.2.53#dns.example.net", None)
        with self.assertRaises(self.np.Failure):
            self.np.validate_dns_request("off", "quad9", None)
        with self.assertRaises(self.np.Failure):
            self.np.validate_dns_request("dot", None, "strict")
        with self.assertRaises(self.np.Failure):
            self.np.validate_dns_request("tls", None, None)
        provider, dnssec = self.np.validate_dns_request("dot-strict", "mullvad", "yes")
        self.assertEqual((provider["id"], dnssec), ("mullvad", "yes"))

    def test_cli_rejects_before_touching_anything(self):
        rc, _, err = self.cli("dns", "doh", "--provider", "custom:192.0.2.53#dns.example.net")
        self.assertEqual(rc, 1)
        self.assertIn("custom server needs dot", err)
        rc, _, err = self.cli("mac-exception", "add", "not-a-uuid")
        self.assertEqual(rc, 1)
        self.assertIn("is not a connection UUID", err)
        self.assertEqual(self.fake.calls, [])
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_USER)))
        with self.assertRaises(SystemExit):
            with redirect_stderr(io.StringIO()):
                self.np.main(["dns", "dns-over-pigeon"])

    def test_root_needed(self):
        with mock.patch.object(self.np.os, "geteuid", return_value=1000):
            rc, _, err = self.cli("dns", "dot")
        self.assertEqual(rc, 1)
        self.assertIn("must run as root", err)
        self.assertEqual(self.fake.calls, [])


# --------------------------------------------------------------------------- rendering
class Rendering(Base):
    def merged_with(self, text):
        write(self.p(self.np.RESOLVED_USER), text)
        return self.effective()

    def test_modes(self):
        quad9 = self.np.parse_provider("quad9")
        dns = self.merged_with(self.np.render_resolved("dot-strict", quad9, "allow-downgrade"))
        self.assertEqual((dns["mode"], dns["dns_over_tls"]), ("dot-strict", "yes"))
        dns = self.merged_with(self.np.render_resolved("doh", quad9, "yes"))
        self.assertEqual((dns["mode"], dns["servers"], dns["dns_over_tls"], dns["dnssec"]),
                         ("doh", ["127.0.2.1"], "no", "yes"))
        self.assertTrue(dns["all_names_to_provider"])
        dns = self.merged_with(self.np.render_resolved("off", quad9, "allow-downgrade"))
        self.assertEqual((dns["mode"], dns["servers"], dns["all_names_to_provider"]), ("off", [], False))
        custom = self.np.parse_provider("custom:192.0.2.53#dns.example.net")
        dns = self.merged_with(self.np.render_resolved("dot", custom, "no"))
        self.assertEqual((dns["provider"], dns["servers"]), ("custom", ["192.0.2.53#dns.example.net"]))

    def test_default_render_equals_shipped_file(self):
        user = self.merged_with(self.np.render_resolved("dot", self.np.parse_provider("quad9"), "allow-downgrade"))
        os.unlink(self.p(self.np.RESOLVED_USER))
        self.assertEqual(user, self.effective())

    def test_hide_me_override_is_seen(self):
        # Hide Me's real file, after the shipped one (and after a user's own
        # provider): Tor's DNSPort must be the ONLY global server. DNS= adds
        # up across files; without the reset Quad9 or the provider stays first
        # and gets every name in plain text (DNSOverTLS=no).
        write(self.p(self.np.RESOLVED_HIDEME), hide_me_dropin())
        dns = self.effective()
        self.assertEqual((dns["mode"], dns["servers"], dns["dns_over_tls"]), ("plain", [TOR], "no"))
        self.assertEqual(dns["fallback_servers"], [])
        self.assertTrue(dns["all_names_to_provider"])
        custom = self.np.parse_provider("custom:192.0.2.53#dns.example.net")
        dns = self.merged_with(self.np.render_resolved("dot-strict", custom, "yes"))
        self.assertEqual((dns["servers"], dns["dns_over_tls"], dns["dnssec"]), ([TOR], "no", "no"))

    def test_hide_me_server_is_shadow_anons(self):
        # Regression: shadow-anon moved Tor's DNSPort off 5353 (any user could
        # bind it next to Tor's socket and answer for the system); the server
        # this tool recognises as Hide Me's must follow it.
        self.assertEqual(self.np.HIDEME_TOR_DNS, TOR)
        self.assertLess(TOR_PORT, 1024)

    def test_doh_stamps(self):
        for ident, entry in self.np.PROVIDERS.items():
            for addr, host, path in entry["doh"]:
                stamp = self.np.doh_stamp(addr, host, path, entry["props"])
                self.assertEqual(self.np.parse_stamp(stamp),
                                 {"addr": addr, "hostname": host, "path": path, "props": entry["props"]})
        # The header (protocol 2, DNSSEC + no-log, address 9.9.9.9) is the one
        # of Quad9's published stamp; certificate pins are left out on purpose.
        self.assertTrue(self.np.doh_stamp("9.9.9.9", "dns.quad9.net", "/dns-query", 3)
                        .startswith("sdns://AgMAAAAAAAAABzkuOS45Ljk"))

    def test_dnscrypt_config(self):
        text = self.np.render_dnscrypt(self.np.parse_provider("cloudflare"))
        self.assertIn("listen_addresses = []", text)
        self.assertNotIn("query_log", text.replace("No [query_log]", ""))
        self.assertNotIn("[nx_log]", text.replace("no [nx_log]", ""))
        self.assertNotIn("[sources", text)
        names = re.findall(r"\[static\.'([^']+)'\]", text)
        self.assertEqual(re.findall(r"'(shadow-[^']+)'", text.split("\n[static]")[0]), names)
        self.assertIn("bootstrap_resolvers = ['1.1.1.1:53']", text)

    @unittest.skipUnless(os.environ.get("DNSCRYPT_PROXY") or shutil.which("dnscrypt-proxy"),
                         "no dnscrypt-proxy binary (set DNSCRYPT_PROXY)")
    def test_dnscrypt_accepts_config(self):
        binary = os.environ.get("DNSCRYPT_PROXY") or shutil.which("dnscrypt-proxy")
        for ident in ("quad9", "mullvad", "cloudflare"):
            path = os.path.join(self.tmp, f"{ident}.toml")
            write(path, self.np.render_dnscrypt(self.np.parse_provider(ident)))
            proc = subprocess.run([binary, "-config", path, "-check"], capture_output=True, text=True, cwd=self.tmp)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_mac_files(self):
        nm_user = self.p(self.np.NM_USER)
        self.assertIsNone(self.np.render_mac("per-boot"))
        for mode in ("per-network", "hardware"):
            write(nm_user, self.np.render_mac(mode))
            merged, _, _ = self.np.nm_config()
            self.assertEqual(self.np.configured_mac(merged)["mode"], mode)
            self.assertNotRegex(self.np.render_mac(mode), r"(?m)^[^#]*=random\s*$")


# --------------------------------------------------------------------------- commands
class DnsCommand(Base):
    def test_strict(self):
        rc, out, err = self.cli("dns", "dot-strict", "--provider", "mullvad")
        self.assertEqual(rc, 0, err)
        dns = self.effective()
        self.assertEqual((dns["mode"], dns["provider"]), ("dot-strict", "mullvad"))
        self.assertIn(["systemctl", "restart", "systemd-resolved.service"], self.fake.calls)
        self.assertEqual(os.stat(self.p(self.np.RESOLVED_USER)).st_mode & 0o777, 0o644)

    def test_back_to_default_removes_the_file(self):
        self.assertEqual(self.cli("dns", "off")[0], 0)
        self.assertTrue(os.path.exists(self.p(self.np.RESOLVED_USER)))
        self.assertEqual(self.cli("dns", "dot")[0], 0)
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_USER)))

    def test_rollback_when_resolved_disagrees(self):
        self.cli("dns", "off")
        before = read(self.p(self.np.RESOLVED_USER))
        self.fake.live = {"DNSOverTLS": "no", "DNSSEC": "allow-downgrade", "LLMNR": "no", "MulticastDNS": "no"}
        rc, _, err = self.cli("dns", "dot-strict")
        self.assertEqual(rc, 1)
        self.assertIn("previous setting was put back", err)
        self.assertEqual(read(self.p(self.np.RESOLVED_USER)), before)

    def test_rollback_when_restart_fails(self):
        self.fake.resolved_restart_fails = True
        rc, _, err = self.cli("dns", "dot-strict")
        self.assertEqual(rc, 1)
        self.assertIn("did not restart", err)
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_USER)))

    def test_needs_resolved(self):
        os.unlink(self.p("/usr/lib/systemd/systemd-resolved"))
        rc, _, err = self.cli("dns", "dot")
        self.assertEqual(rc, 1)
        self.assertIn("systemd-resolved is not installed", err)

    def test_doh_needs_dnscrypt(self):
        rc, _, err = self.cli("dns", "doh")
        self.assertEqual(rc, 1)
        self.assertIn("apt install dnscrypt-proxy", err)
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_USER)))

    def test_doh_success_and_leaving_it(self):
        write(self.p(self.np.DNSCRYPT_BINARY), "")
        rc, _, err = self.cli("dns", "doh", "--provider", "quad9")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.effective()["mode"], "doh")
        self.assertIn("-config /etc/shadow-os/dnscrypt-proxy.toml", read(self.p(self.np.DNSCRYPT_DROPIN)))
        self.assertIn(["systemctl", "disable", "--now", "dnscrypt-proxy-resolvconf.service"], self.fake.calls)
        self.assertEqual(self.np.status()["dns"]["provider"], "quad9")
        rc, _, err = self.cli("dns", "dot")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.p(self.np.DNSCRYPT_DROPIN)))
        self.assertFalse(os.path.exists(self.p(self.np.DNSCRYPT_CONFIG)))
        self.assertIn(["systemctl", "disable", "--now", "dnscrypt-proxy.socket", "dnscrypt-proxy.service"],
                      self.fake.calls)

    def test_doh_not_switched_when_proxy_is_silent(self):
        write(self.p(self.np.DNSCRYPT_BINARY), "")
        self.doh_answers = False
        rc, _, err = self.cli("dns", "doh")
        self.assertEqual(rc, 1)
        self.assertIn("DNS settings were not changed", err)
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_USER)))
        self.assertFalse(os.path.exists(self.p(self.np.DNSCRYPT_DROPIN)))
        self.assertFalse(os.path.exists(self.p(self.np.DNSCRYPT_CONFIG)))
        self.assertNotIn(["systemctl", "restart", "systemd-resolved.service"], self.fake.calls)

    def test_doh_config_rejected(self):
        write(self.p(self.np.DNSCRYPT_BINARY), "")
        self.fake.dnscrypt_check_ok = False
        rc, _, err = self.cli("dns", "doh")
        self.assertEqual(rc, 1)
        self.assertIn("rejected the configuration", err)
        self.assertFalse(os.path.exists(self.p(self.np.DNSCRYPT_CONFIG)))


class MacCommands(Base):
    def test_modes_never_reconnect(self):
        for mode in ("hardware", "per-network", "per-boot"):
            rc, out, err = self.cli("mac", mode)
            self.assertEqual(rc, 0, err)
            self.assertIn("Nothing was reconnected", out)
        self.assertFalse(os.path.exists(self.p(self.np.NM_USER)))
        self.assertIn(["nmcli", "general", "reload", "conf"], self.fake.calls)
        self.assertFalse(any("up" in call or "down" in call for call in self.fake.calls if call[0] == "nmcli"))

    def test_reload_failure_is_reported(self):
        self.fake.nm_reload_fails = True
        rc, _, err = self.cli("mac", "hardware")
        self.assertEqual(rc, 1)
        self.assertIn("did not reload", err)
        self.assertTrue(os.path.exists(self.p(self.np.NM_USER)))

    def test_exception(self):
        rc, _, err = self.cli("mac-exception", "add", WIFI_UUID.upper())
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.fake.connections[WIFI_UUID]["stable-id"], "${CONNECTION}")
        self.assertEqual(self.np.status()["mac"]["exceptions"], [WIFI_UUID])
        rc, _, err = self.cli("mac-exception", "remove", WIFI_UUID)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.fake.connections[WIFI_UUID]["stable-id"], "")
        rc, _, err = self.cli("mac-exception", "remove", WIFI_UUID)
        self.assertEqual(rc, 1)

    def test_exception_refusals(self):
        rc, _, err = self.cli("mac-exception", "add", VPN_UUID)
        self.assertEqual(rc, 1)
        self.assertIn("Wi-Fi and Ethernet", err)
        self.fake.connections[ETH_UUID]["stable-id"] = "lab-bench"
        rc, _, err = self.cli("mac-exception", "add", ETH_UUID)
        self.assertEqual(rc, 1)
        self.assertIn("already has its own stable-id", err)
        self.assertEqual(self.fake.connections[ETH_UUID]["stable-id"], "lab-bench")
        rc, _, err = self.cli("mac-exception", "add", "00000000-0000-4000-8000-000000000000")
        self.assertEqual(rc, 1)


class Status(Base):
    def test_shape_and_values(self):
        info = self.np.status()
        self.assertEqual(set(info), {"version", "escape_hatch", "dns", "mac", "errors", "warnings"})
        dns = info["dns"]
        for key in ("mode", "provider", "servers", "dns_over_tls", "dnssec", "all_names_to_provider",
                    "fallback_servers", "llmnr", "mdns", "source", "override", "resolver_installed",
                    "resolver_active", "resolv_conf", "nss_resolve", "files", "live", "doh", "encrypted"):
            self.assertIn(key, dns)
        self.assertEqual((dns["mode"], dns["source"], dns["encrypted"]), ("dot", "default", "opportunistic"))
        self.assertEqual(dns["live"]["current_server"], "192.0.2.53#dns.example.net")
        self.assertEqual(dns["resolv_conf"], {"kind": "stub", "target": "../run/systemd/resolve/stub-resolv.conf",
                                              "dangling": False})
        self.assertEqual(info["mac"]["mode"], "per-boot")
        self.assertEqual(info["mac"]["fixed_drivers"], ["eagle_sdio", "wl"])
        self.assertEqual((info["errors"], info["warnings"]), ([], []))
        json.dumps(info)

    def test_status_is_honest(self):
        self.fake.units["systemd-resolved.service"] = "inactive"
        info = self.np.status()
        self.assertEqual(info["dns"]["encrypted"], "unknown")
        self.assertTrue(any("not running" in e for e in info["errors"]))
        self.fake.units["systemd-resolved.service"] = "active"
        self.fake.live = {"DNSOverTLS": "no", "DNSSEC": "no", "LLMNR": "no", "MulticastDNS": "no"}
        info = self.np.status()
        self.assertEqual(info["dns"]["encrypted"], "no", "what resolved reports wins over the files")
        self.assertTrue(info["warnings"])

    def test_bypasses_are_reported(self):
        os.unlink(self.p("/etc/resolv.conf"))
        write(self.p("/etc/resolv.conf"), "nameserver 198.51.100.7\n")
        write(self.p("/etc/nsswitch.conf"), "hosts: files resolve [!UNAVAIL=return] dns\n")
        info = self.np.status()
        self.assertEqual(info["dns"]["resolv_conf"]["kind"], "file")
        self.assertTrue(info["dns"]["nss_resolve"])
        self.assertEqual(len(info["warnings"]), 2)

    def test_hide_me_and_escape(self):
        write(self.p(self.np.RESOLVED_HIDEME), hide_me_dropin())
        self.fake.current_server = [0, 2, [127, 0, 0, 1], TOR_PORT, ""]
        info = self.np.status()
        self.assertEqual((info["dns"]["override"], info["dns"]["encrypted"]), ("hide-me", "tor"))
        self.assertEqual(info["dns"]["servers"], [TOR])
        self.assertEqual(info["dns"]["live"]["current_server"], TOR)
        self.assertEqual(info["errors"], [])
        write(self.p("/proc/cmdline"), "ro quiet shadow.netprivacy=off\n")
        self.assertEqual(self.cli("boot")[0], 0)
        info = self.np.status()
        self.assertTrue(info["escape_hatch"])
        self.assertEqual(info["dns"]["override"], "escape")
        self.assertEqual(info["mac"]["mode"], "hardware")
        dns = self.effective()
        self.assertEqual(dns["servers"], [TOR], "Hide Me (95) still wins over the escape (93)")
        os.unlink(self.p(self.np.RESOLVED_HIDEME))
        dns = self.effective()
        self.assertEqual((dns["mode"], dns["dnssec"], dns["servers"]), ("off", "no", []))

    def test_hide_me_is_tor_only_when_tor_is_the_only_server(self):
        # Hide Me's file of an earlier version, without the "DNS=" reset: Tor
        # comes after Quad9, resolved asks Quad9 in plain text (DoT off).
        write(self.p(self.np.RESOLVED_HIDEME), "[Resolve]\nDNS=127.0.0.1:5353\nDNSOverTLS=no\nDNSSEC=no\nDomains=~.\n")
        self.fake.live = {"DNSOverTLS": "no", "DNSSEC": "no", "LLMNR": "no", "MulticastDNS": "no"}
        self.fake.current_server = [0, 2, [9, 9, 9, 9], 0, "dns.quad9.net"]
        info = self.np.status()
        self.assertEqual(info["dns"]["servers"][0], "9.9.9.9#dns.quad9.net")
        self.assertEqual(info["dns"]["override"], "hide-me")
        self.assertEqual(info["dns"]["encrypted"], "no", "never 'tor' while another server comes first")
        self.assertTrue(any("not in force" in e and "9.9.9.9" in e for e in info["errors"]), info["errors"])
        # The right file, but resolved still runs with the old list (not restarted).
        write(self.p(self.np.RESOLVED_HIDEME), hide_me_dropin())
        info = self.np.status()
        self.assertEqual(info["dns"]["servers"], [TOR])
        self.assertNotEqual(info["dns"]["encrypted"], "tor")
        self.assertTrue(any("in use: 9.9.9.9#dns.quad9.net" in e for e in info["errors"]), info["errors"])
        self.fake.current_server = [0, 2, [127, 0, 0, 1], TOR_PORT, ""]
        info = self.np.status()
        self.assertEqual((info["dns"]["encrypted"], info["errors"]), ("tor", []))

    def test_boot_without_parameter_cleans_up(self):
        write(self.p(self.np.RESOLVED_ESCAPE), "x")
        write(self.p(self.np.NM_ESCAPE), "x")
        self.assertEqual(self.cli("boot")[0], 0)
        self.assertFalse(os.path.exists(self.p(self.np.RESOLVED_ESCAPE)))
        self.assertFalse(os.path.exists(self.p(self.np.NM_ESCAPE)))

    def test_cli_json(self):
        rc, out, _ = self.cli("status", "--json")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["version"], 1)


# --------------------------------------------------------------------------- lock
class LockFile(Base):
    """A lock file other users can open lets any of them hold it (flock works on a
    read-only descriptor) and block every DNS and MAC change."""

    def lock_mode(self):
        return os.stat(self.p(self.np.LOCK_FILE)).st_mode & 0o777

    def test_root_only_whatever_the_umask(self):
        old = os.umask(0)
        try:
            with self.np.Lock():
                self.assertEqual(self.lock_mode(), 0o600)
        finally:
            os.umask(old)
        self.assertEqual(self.lock_mode(), 0o600)

    def test_file_of_an_older_version_is_made_root_only(self):
        write(self.p(self.np.LOCK_FILE), "")
        os.chmod(self.p(self.np.LOCK_FILE), 0o644)
        with self.np.Lock():
            pass
        self.assertEqual(self.lock_mode(), 0o600)

    def test_symlink_is_not_followed(self):
        target = self.p("/elsewhere")
        write(target, "keep\n")
        os.makedirs(os.path.dirname(self.p(self.np.LOCK_FILE)), exist_ok=True)
        os.symlink(target, self.p(self.np.LOCK_FILE))
        with self.assertRaises(self.np.Failure):
            with self.np.Lock():
                pass
        self.assertEqual(read(target), "keep\n")

    def test_held_lock_refuses_a_second_command(self):
        with self.np.Lock():
            with self.assertRaisesRegex(self.np.Failure, "another shadow-netprivacy command"):
                with self.np.Lock():
                    pass
        with self.np.Lock():
            pass


# --------------------------------------------------------------------------- DNS probe
class Probe(unittest.TestCase):
    def serve(self, answer):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(5)

        def loop():
            try:
                data, peer = sock.recvfrom(512)
                sock.sendto(answer(data), peer)
            except OSError:
                pass
            finally:
                sock.close()
        threading.Thread(target=loop, daemon=True).start()
        return sock.getsockname()[1]

    def test_probe(self):
        np = load(TOOL, "shadow_netprivacy_probe")
        good = self.serve(lambda q: q[:2] + struct.pack(">H", 0x8180) + q[4:])
        self.assertTrue(np.dns_probe("127.0.0.1", good))
        servfail = self.serve(lambda q: q[:2] + struct.pack(">H", 0x8182) + q[4:])
        self.assertFalse(np.dns_probe("127.0.0.1", servfail))
        wrong_id = self.serve(lambda q: bytes([q[0] ^ 0xFF, q[1]]) + struct.pack(">H", 0x8180) + q[4:])
        self.assertFalse(np.dns_probe("127.0.0.1", wrong_id))
        silent = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        silent.bind(("127.0.0.1", 0))
        try:
            self.assertFalse(np.dns_probe("127.0.0.1", silent.getsockname()[1], timeout=0.3))
        finally:
            silent.close()


# --------------------------------------------------------------------------- packaging
class Packaging(unittest.TestCase):
    def test_resolver_packages(self):
        hardening = control_stanza("shadow-hardening")
        self.assertIn("systemd-resolved", relation_names(hardening["Depends"]))
        self.assertIn("dnscrypt-proxy", relation_names(hardening.get("Suggests", "")))
        self.assertNotIn("dnscrypt-proxy", relation_names(hardening["Depends"] + "," + hardening["Recommends"]))
        self.assertIn("libnss-resolve", relation_names(hardening.get("Conflicts", "")))

    def test_libnss_resolve_never_pulled(self):
        text = read(CONTROL)
        for match in re.finditer(r"(?ms)^Package: (\S+)\n(.*?)(?=\n\n|\Z)", text):
            for field in ("Depends", "Recommends", "Pre-Depends"):
                value = re.search(rf"(?m)^{field}:(.*(?:\n[ \t].*)*)", match.group(2))
                if value:
                    self.assertNotIn("libnss-resolve", relation_names(value.group(1)), match.group(1))
        lists = os.path.join(ROOT, "distro", "base", "package-lists")
        packages = set()
        for name in os.listdir(lists):
            for line in read(os.path.join(lists, name)).splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    packages.add(line.split()[0])
        self.assertIn("systemd-resolved", packages)
        self.assertNotIn("libnss-resolve", packages)
        # The image build installs nothing it recommends, and fails if it did.
        build = read(os.path.join(ROOT, "build", "iso", "build.sh"))
        self.assertIn("APT::Install-Recommends \"false\"", build)
        stage = read(os.path.join(ROOT, "build", "iso", "stage-rootfs.sh"))
        self.assertIn("libnss-resolve is installed in the image", stage)
        self.assertIn("ln -s ../run/systemd/resolve/stub-resolv.conf", stage)
        self.assertIn(".resolv.conf.systemd-resolved.bak", stage)

    def test_install_lines(self):
        install = read(os.path.join(ROOT, "debian", "shadow-hardening.install"))
        for line in ("security/hardening/resolved.conf.d/80-shadow-privacy.conf usr/lib/systemd/resolved.conf.d",
                     "security/hardening/shadow-netprivacy usr/sbin",
                     "security/hardening/systemd/shadow-netprivacy.service usr/lib/systemd/system"):
            self.assertIn(line, install.splitlines())
        self.assertTrue(os.access(TOOL, os.X_OK))
        self.assertIn("shadow-netprivacy.service", read(os.path.join(ROOT, "debian", "rules")))

    def test_polkit_action(self):
        actions = {a.get("id"): a for a in ET.parse(POLICY).getroot().iter("action")}
        action = actions["org.shadow.netprivacy.manage"]
        self.assertEqual(action.find("defaults/allow_active").text, "auth_admin_keep")
        self.assertEqual(action.find("defaults/allow_any").text, "auth_admin")
        path = [a.text for a in action.iter("annotate") if a.get("key") == "org.freedesktop.policykit.exec.path"]
        self.assertEqual(path, ["/usr/sbin/shadow-netprivacy"])

    def test_unit(self):
        unit = configparser.ConfigParser(interpolation=None, strict=False)
        unit.optionxform = str
        unit.read_string(read(UNIT))
        self.assertEqual(unit.get("Unit", "ConditionKernelCommandLine"), "shadow.netprivacy=off")
        self.assertIn("systemd-resolved.service", unit.get("Unit", "Before").split())
        self.assertIn("NetworkManager.service", unit.get("Unit", "Before").split())
        self.assertEqual(unit.get("Unit", "DefaultDependencies"), "no")
        self.assertEqual(unit.get("Service", "ExecStart"), "/usr/sbin/shadow-netprivacy boot")


if __name__ == "__main__":
    unittest.main()
