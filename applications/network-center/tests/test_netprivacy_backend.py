#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of Network Center's encrypted DNS / MAC address back-end
(applications/network-center/netprivacy.{h,cpp}).

Two programs are driven, with fake programs first on PATH:

  SHADOW_NC_BIN       shadow-network-center; "--dump privacy" shows what the
                      Privacy page reads from "shadow-netprivacy status --json"
  SHADOW_NC_HARNESS   tests/harness/ncprivacy; runs the page's changes and
                      the validation of what the user typed

The fake pkexec records its arguments and runs them; the fake
shadow-netprivacy (SHADOW_NETPRIVACY_TOOL) answers "status --json" from a
file and records every other command. When the tests run as root, the
programs are started as the user "nobody" (setpriv), so the pkexec path is
the one tested; one test checks that root runs the tool directly.

Without SHADOW_NC_BIN / SHADOW_NC_HARNESS the tests are skipped. Run with
PYTHONDONTWRITEBYTECODE=1; nothing is written outside a temporary directory.
"""
import copy
import json
import os
import shutil
import subprocess
import tempfile
import unittest

NC_BIN = os.environ.get("SHADOW_NC_BIN", "")
HARNESS = os.environ.get("SHADOW_NC_HARNESS", "")
NOBODY = 65534
UUID = "3f2b8c1e-5d4a-4e6f-9a1b-2c3d4e5f6a7b"   # made up

FAKE_PKEXEC = """#!/bin/sh
printf 'pkexec' >> "$FAKE_LOG"; printf ' %s' "$@" >> "$FAKE_LOG"; printf '\\n' >> "$FAKE_LOG"
if [ -n "$FAKE_PKEXEC_EXIT" ]; then echo "Error executing command as another user: Not authorized" >&2; exit "$FAKE_PKEXEC_EXIT"; fi
exec "$@"
"""

# "status --json" prints $FAKE_STATUS; any other command is recorded and, on
# success, the status becomes $FAKE_STATUS.next when that file exists (the
# tool really changed something).
FAKE_TOOL = """#!/usr/bin/python3
import os, shutil, sys
status = os.environ["FAKE_STATUS"]
if sys.argv[1:] == ["status", "--json"]:
    if os.environ.get("FAKE_STATUS_EXIT"):
        sys.stderr.write("Traceback (most recent call last):\\nPermissionError: denied\\n")
        sys.exit(int(os.environ["FAKE_STATUS_EXIT"]))
    sys.stdout.write(open(status).read())
    sys.exit(0)
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("netprivacy " + " ".join(sys.argv[1:]) + "\\n")
if os.environ.get("FAKE_EXIT"):
    sys.stderr.write("shadow-netprivacy: systemd-resolved reports DNSOverTLS=no DNSSEC=no, the files say "
                     "DNSOverTLS=yes DNSSEC=yes; the previous setting was put back\\n")
    sys.exit(int(os.environ["FAKE_EXIT"]))
if os.path.exists(status + ".next"):
    shutil.copyfile(status + ".next", status)
print("DNS: DNS-over-TLS only, provider Quad9, DNSSEC allow-downgrade.")
print("Note: shadow.netprivacy=off is on the kernel command line; this setting applies from the next boot.")
"""

STATUS = {
    "version": 1, "escape_hatch": False,
    "dns": {"mode": "dot", "provider": "quad9", "servers": ["9.9.9.9#dns.quad9.net"], "dns_over_tls": "opportunistic",
            "dnssec": "allow-downgrade", "all_names_to_provider": True, "fallback_servers": [], "llmnr": "no",
            "mdns": "no", "source": "default", "override": None, "resolver_installed": True, "resolver_active": True,
            "resolv_conf": {"kind": "stub", "target": "../run/systemd/resolve/stub-resolv.conf", "dangling": False},
            "nss_resolve": False, "files": [],
            "live": {"dns_over_tls": "opportunistic", "dnssec": "allow-downgrade", "llmnr": "no", "mdns": "no",
                     "current_server": "9.9.9.9#dns.quad9.net"},
            "doh": {"installed": False, "configured": False, "active": False, "listen": "127.0.2.1:53"},
            "encrypted": "opportunistic"},
    "mac": {"mode": "per-boot", "stable_id": "${CONNECTION}/${BOOT}", "wifi": "stable", "ethernet": "stable",
            "scan_randomization": True, "source": "default", "override": None, "fixed_drivers": ["eagle_sdio", "wl"],
            "exceptions": [UUID.upper()], "exceptions_known": True, "dns_plugin": "systemd-resolved", "files": []},
    "errors": [], "warnings": [],
}


def have_setpriv():
    return shutil.which("setpriv") is not None


@unittest.skipUnless(NC_BIN and os.access(NC_BIN, os.X_OK), "SHADOW_NC_BIN (shadow-network-center) not given")
class Base(unittest.TestCase):
    as_user = True

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nc-np-")
        os.chmod(self.tmp, 0o755)
        self.fake = os.path.join(self.tmp, "fake")
        self.home = os.path.join(self.tmp, "home")
        for d in (self.fake, self.home):
            os.makedirs(d)
            os.chmod(d, 0o777)
        self.log = self.write("calls.log", "")
        self.status_file = self.write("status.json", json.dumps(STATUS))
        self.write("fake/pkexec", FAKE_PKEXEC, 0o755)
        self.tool = self.write("fake/shadow-netprivacy", FAKE_TOOL, 0o755)
        self.env_extra = {}

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, rel, text, mode=0o666):
        path = os.path.join(self.tmp, rel)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.chmod(path, mode)
        return path

    def set_status(self, obj, name="status.json"):
        self.write(name, obj if isinstance(obj, str) else json.dumps(obj))

    def calls(self):
        with open(self.log, encoding="utf-8") as fh:
            return [line.strip() for line in fh if line.strip()]

    def env(self, tool=True):
        env = {"PATH": self.fake + ":/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
               "HOME": self.home, "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "XDG_RUNTIME_DIR": self.home,
               "QT_QPA_PLATFORM": "offscreen", "FAKE_LOG": self.log, "FAKE_STATUS": self.status_file,
               "PYTHONDONTWRITEBYTECODE": "1"}
        if os.environ.get("LD_LIBRARY_PATH"):
            env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        env["SHADOW_NETPRIVACY_TOOL"] = self.tool if tool else os.path.join(self.tmp, "not-installed")
        env.update(self.env_extra)
        return env

    def command(self, argv):
        if os.geteuid() == 0 and self.as_user:
            if not have_setpriv():
                self.skipTest("running as root without setpriv: cannot test the pkexec path")
            return ["setpriv", f"--reuid={NOBODY}", f"--regid={NOBODY}", "--clear-groups", "--"] + argv
        return argv

    def dump(self, tool=True):
        p = subprocess.run(self.command([NC_BIN, "--dump", "privacy"]), env=self.env(tool),
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def harness(self, *ops, tool=True):
        if not (HARNESS and os.access(HARNESS, os.X_OK)):
            self.skipTest("SHADOW_NC_HARNESS (ncprivacy) not given")
        p = subprocess.run(self.command([HARNESS, *ops]), env=self.env(tool), capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = [json.loads(line) for line in p.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(lines), len(ops), p.stdout + p.stderr)
        return lines

    def tool_calls(self):
        return [c for c in self.calls() if c.startswith("pkexec ") or c.startswith("netprivacy ")]


class Reading(Base):
    def test_not_installed(self):
        d = self.dump(tool=False)
        self.assertEqual(d["status"], "missing")
        self.assertIn("is not installed", d["statusText"])
        self.assertEqual(d["dns"], {})

    def test_status_is_read(self):
        d = self.dump()
        self.assertEqual(d["status"], "ok")
        self.assertEqual(d["statusText"], "")
        self.assertEqual((d["dns"]["mode"], d["dns"]["provider"], d["dns"]["encrypted"]), ("dot", "quad9", "opportunistic"))
        self.assertEqual(d["dns"]["currentServer"], "9.9.9.9#dns.quad9.net")
        self.assertEqual(d["dns"]["live"]["dnssec"], "allow-downgrade")
        self.assertEqual(d["mac"]["mode"], "per-boot")
        self.assertEqual(d["mac"]["exceptions"], [UUID])          # normalised to lower case
        self.assertTrue(d["mac"]["exceptionsKnown"])
        self.assertEqual(self.tool_calls(), [])                   # reading needs no pkexec

    def test_tool_failure(self):
        self.env_extra = {"FAKE_STATUS_EXIT": "1"}
        d = self.dump()
        self.assertEqual(d["status"], "failed")
        self.assertIn("PermissionError", d["statusText"])
        self.assertEqual(d["dns"], {})

    def test_unreadable_documents(self):
        for name, text in (("not json", "{"), ("array", "[]"),
                           ("no mac", json.dumps({k: v for k, v in STATUS.items() if k != "mac"}))):
            with self.subTest(name):
                self.set_status(text)
                d = self.dump()
                self.assertEqual(d["status"], "failed")
                self.assertIn("cannot read", d["statusText"])
                self.assertEqual(d["dns"], {})

    def test_wrong_fields_are_reported_not_guessed(self):
        bad = copy.deepcopy(STATUS)
        bad["version"] = 2
        bad["dns"]["dnssec"] = True
        bad["mac"]["exceptions"] = ["not-a-uuid", UUID]
        bad["errors"] = "resolved stopped"
        self.set_status(bad)
        d = self.dump()
        self.assertEqual(d["status"], "ok")
        self.assertIn("unknown status version 2", d["statusText"])
        self.assertIn("dns.dnssec is not text", d["statusText"])
        self.assertIn("not a connection UUID", d["statusText"])
        self.assertIn("status.errors is not a list", d["statusText"])
        self.assertEqual(d["dns"]["dnssec"], "")
        self.assertEqual(d["mac"]["exceptions"], [UUID])

    def test_problems_are_passed_on(self):
        st = copy.deepcopy(STATUS)
        st["dns"]["live"] = None
        st["dns"]["encrypted"] = "unknown"
        st["errors"] = ["systemd-resolved is not running: no name can be looked up"]
        st["warnings"] = ["libnss-resolve is enabled in /etc/nsswitch.conf"]
        st["escape_hatch"] = True
        st["mac"]["exceptions_known"] = False
        self.set_status(st)
        d = self.dump()
        self.assertEqual(d["errors"], st["errors"])
        self.assertEqual(d["warnings"], st["warnings"])
        self.assertTrue(d["escapeHatch"])
        self.assertEqual(d["dns"]["live"], {})
        self.assertFalse(d["mac"]["exceptionsKnown"])


class Changing(Base):
    def test_dns(self):
        self.write("status.json.next", json.dumps(dict(STATUS, dns=dict(STATUS["dns"], mode="dot-strict"))))
        out = self.harness("dns|dot-strict|quad9|allow-downgrade")
        f = out[0]["finished"]
        self.assertTrue(f["ok"], f)
        # The tool's own confirmation, on one line.
        self.assertIn("DNS-over-TLS only", f["message"])
        self.assertIn("next boot", f["message"])
        self.assertEqual(self.tool_calls()[0], f"pkexec {self.tool} dns dot-strict --provider quad9 --dnssec allow-downgrade")
        self.assertEqual(out[0]["state"]["dns"]["mode"], "dot-strict")   # read again afterwards

    def test_dns_off_passes_no_provider(self):
        self.harness("dns|off|quad9|no")
        self.assertEqual(self.tool_calls()[0], f"pkexec {self.tool} dns off --dnssec no")

    def test_custom_servers_are_normalised(self):
        out = self.harness("dns|dot|custom:192.0.2.53#DNS.Example.NET , 2001:DB8::53#dns2.example.net|yes")
        self.assertTrue(out[0]["finished"]["ok"], out[0])
        self.assertEqual(self.tool_calls()[0], f"pkexec {self.tool} dns dot --provider "
                         "custom:192.0.2.53#dns.example.net,2001:db8::53#dns2.example.net --dnssec yes")

    def test_refused_before_anything_runs(self):
        cases = {
            "dns|dot|custom:dns.example.net#dns.example.net|yes": "not an IP address",
            "dns|dot|custom:127.0.0.1#dns.example.net|yes": "this computer",
            "dns|dot|custom:224.0.0.1#dns.example.net|yes": "cannot be a DNS server",
            "dns|dot|custom:0.0.0.0#dns.example.net|yes": "cannot be a DNS server",
            "dns|dot|custom:192.0.2.53|yes": "address#TLS name",
            "dns|dot|custom:192.0.2.53#localhost|yes": "not a valid TLS server name",
            "dns|dot|custom:" + ",".join(f"192.0.2.{i}#dns.example.net" for i in range(1, 10)) + "|yes": "At most 8",
            "dns|doh|custom:192.0.2.53#dns.example.net|no": "a server of your own needs",
            "dns|doh|quad9|allow-downgrade": "dnscrypt-proxy",       # not installed per status
            "dns|everything|quad9|yes": "Unknown DNS mode",
            "dns|dot|quad9|maybe": "Unknown DNSSEC",
            "dns|dot|google|yes": "Unknown provider",
            "dns|dot|quad9 --dnssec no|yes": "Unknown provider",
            "mac|random": "Unknown address policy",
            "keep|not-a-uuid|1": "not a connection UUID",
            f"keep|{UUID};id|1": "not a connection UUID",
        }
        out = self.harness(*cases)
        for line, (op, expected) in zip(out, cases.items()):
            with self.subTest(op):
                self.assertFalse(line["finished"]["ok"])
                self.assertIn(expected, line["finished"]["message"])
        self.assertEqual(self.tool_calls(), [])

    def test_validation_for_the_form(self):
        out = self.harness("custom|192.0.2.53#dns.example.net 198.51.100.7#b.example.org",
                           "custom|", "check|off|custom:junk|no")
        self.assertEqual(out[0]["error"], "")
        self.assertEqual(out[0]["provider"], "custom:192.0.2.53#dns.example.net,198.51.100.7#b.example.org")
        self.assertIn("at least one server", out[1]["error"])
        self.assertEqual(out[2]["error"], "")                     # "off" ignores the provider

    def test_mac_and_exceptions(self):
        out = self.harness("mac|per-network", f"keep|{UUID.upper()}|1", f"keep|{UUID}|0")
        self.assertTrue(all(o["finished"]["ok"] for o in out))
        self.assertEqual([o["finished"]["operation"] for o in out], ["mac", "mac-exception", "mac-exception"])
        self.assertEqual(out[1]["finished"]["target"], UUID)
        self.assertEqual(self.tool_calls(), [
            f"pkexec {self.tool} mac per-network", "netprivacy mac per-network",
            f"pkexec {self.tool} mac-exception add {UUID}", f"netprivacy mac-exception add {UUID}",
            f"pkexec {self.tool} mac-exception remove {UUID}", f"netprivacy mac-exception remove {UUID}"])

    def test_failures(self):
        self.env_extra = {"FAKE_EXIT": "1"}
        out = self.harness("dns|dot-strict|quad9|yes")
        self.assertFalse(out[0]["finished"]["ok"])
        self.assertTrue(out[0]["finished"]["message"].startswith("systemd-resolved reports DNSOverTLS=no"), out[0])
        self.env_extra = {"FAKE_PKEXEC_EXIT": "126"}
        out = self.harness("mac|hardware")
        self.assertIn("cancelled or you are not authorised", out[0]["finished"]["message"])
        self.assertNotIn("netprivacy mac hardware", self.calls())
        self.env_extra = {"FAKE_PKEXEC_EXIT": "127"}
        out = self.harness("mac|hardware")
        self.assertIn("not available", out[0]["finished"]["message"])

    def test_not_installed(self):
        out = self.harness("mac|hardware", tool=False)
        self.assertFalse(out[0]["finished"]["ok"])
        self.assertIn("is not installed", out[0]["finished"]["message"])
        self.assertEqual(self.tool_calls(), [])


REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
REAL_TOOL = os.path.join(REPO, "security", "hardening", "shadow-netprivacy")

# Runs the real tool with its ROOT constant pointed at a directory (as
# tests/security/test_netprivacy.py does), nothing else replaced.
WRAPPER = """#!/bin/sh
exec python3 -I -B -c '
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader("tool", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)
mod.ROOT = sys.argv[2]
sys.exit(mod.main(sys.argv[3:]))
' "%s" "%s" "$@"
"""


@unittest.skipUnless(os.path.exists(REAL_TOOL), "the real shadow-netprivacy is not in this tree")
class RealTool(Base):
    """The document of the REAL shadow-netprivacy status --json (no system bus,
    systemd or NetworkManager here: it reports them as not running)."""

    def test_on_this_system(self):
        self.tool = REAL_TOOL
        d = self.dump()
        self.assertEqual(d["status"], "ok", d)
        self.assertEqual(d["statusText"], "")               # every field as expected
        self.assertTrue(d["errors"], d)                     # resolved is not running here

    def test_with_the_shipped_defaults(self):
        root = os.path.join(self.tmp, "root")
        for src, dst in (("security/hardening/resolved.conf.d/80-shadow-privacy.conf",
                          "usr/lib/systemd/resolved.conf.d/80-shadow-privacy.conf"),
                         ("security/hardening/NetworkManager/80-shadow-privacy.conf",
                          "usr/lib/NetworkManager/conf.d/80-shadow-privacy.conf")):
            os.makedirs(os.path.dirname(os.path.join(root, dst)), exist_ok=True)
            shutil.copyfile(os.path.join(REPO, src), os.path.join(root, dst))
        for path in (root, *[os.path.join(dp, n) for dp, dns, fns in os.walk(root) for n in dns + fns]):
            os.chmod(path, 0o755 if os.path.isdir(path) else 0o644)
        self.tool = self.write("fake/real-netprivacy", WRAPPER % (REAL_TOOL, root), 0o755)
        d = self.dump()
        self.assertEqual((d["status"], d["statusText"]), ("ok", ""), d)
        dns, mac = d["dns"], d["mac"]
        self.assertEqual((dns["mode"], dns["provider"], dns["dnssec"], dns["dnsOverTls"]),
                         ("dot", "quad9", "allow-downgrade", "opportunistic"))
        self.assertTrue(dns["allNamesToProvider"])
        self.assertEqual((mac["mode"], mac["stableId"], mac["source"]), ("per-boot", "${CONNECTION}/${BOOT}", "default"))
        self.assertTrue(mac["scanRandomization"])


class AsRoot(Base):
    as_user = False

    def setUp(self):
        if os.geteuid() != 0:
            self.skipTest("needs root")
        super().setUp()

    def test_direct(self):
        out = self.harness("mac|hardware")
        self.assertTrue(out[0]["finished"]["ok"])
        self.assertEqual(self.tool_calls(), ["netprivacy mac hardware"])


if __name__ == "__main__":
    unittest.main()
