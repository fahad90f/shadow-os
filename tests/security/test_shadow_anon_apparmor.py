#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hide Me's AppArmor profile and the kill switch.

    python3 tests/security/test_shadow_anon_apparmor.py [-v]

shadow-anon hands its kill switch to shadow-killswitch ("set tor --no-lan
--runtime", "status --json"). With inherited confinement (ix) the profile would block
shadow-killswitch's own writes to /run/shadow and /etc/shadow-os, so it must
run under its own profile or unconfined (PUx). shadow-anon may read the
world-readable status documents in /run/shadow but never write the kill
switch's or panic's files itself.

The profile is also compiled with Debian trixie's apparmor_parser 4.1 when it
is available ($AA_ROOT: the root of the extracted apparmor .deb; skipped
when unset), without loading anything into the kernel.

Hide Me puts Tor's DNSPort and TransPort below 1024 (so that no user can bind
them): Tor must be allowed to bind such ports. With Debian's tor package
extracted ($TOR_ROOT: the root of the extracted tor .deb; skipped when unset), its AppArmor
profile system_tor (enforced on Debian) must grant capability
net_bind_service, and tor@default's CapabilityBoundingSet must keep it.
"""
import shutil
import tempfile
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PROFILE = os.path.join(ROOT, "distro", "base", "rootfs", "etc", "apparmor.d", "usr.bin.shadow-anon")
AA_ROOT = os.environ.get("AA_ROOT", "")
TOR_ROOT = os.environ.get("TOR_ROOT", "")
TOOL = os.path.join(ROOT, "distro", "base", "rootfs", "usr", "bin", "shadow-anon")


def rules(text):
    """{path: perms} of the file rules of the profile (comments removed)."""
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        m = re.match(r"^(/\S+)\s+([a-zA-Z]+),$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


class Profile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PROFILE) as fh:
            cls.text = fh.read()
        cls.rules = rules(cls.text)

    def test_shadow_killswitch_runs_outside_the_profile(self):
        self.assertEqual(self.rules.get("/usr/sbin/shadow-killswitch"), "PUx")

    def test_the_status_documents_are_readable_only(self):
        self.assertEqual(self.rules.get("/run/shadow/*.json"), "r")
        own = ("/run/shadow/", "/run/shadow/mac-salt", "/run/shadow/.mac-salt.*")     # the MAC salt is its own
        for path, perms in self.rules.items():
            if path.startswith("/run/shadow/") and path not in own:
                self.assertNotRegex(perms, "[wal]", f"{path} {perms}: shadow-anon must not write it")
            if path.startswith("/etc/shadow-os/") and ("killswitch" in path or "panic" in path):
                self.fail(f"{path} {perms}: the kill switch's and panic's files are not shadow-anon's")

    def test_the_socks_clients_it_points_at_tor_are_writable(self):
        # boot/tor-socks rewrite proxychains4.conf (atomically: a temporary
        # file in /etc) and torsocks.conf (/etc/tor/**) for Tor's SOCKS port.
        self.assertEqual(self.rules.get("/etc/proxychains4.conf"), "rw")
        self.assertEqual(self.rules.get("/etc/.proxychains4.conf.*"), "rw")
        self.assertEqual(self.rules.get("/etc/tor/**"), "rw")

    def test_the_tool_still_uses_the_kill_switch_command(self):
        with open(os.path.join(ROOT, "distro", "base", "rootfs", "usr", "bin", "shadow-anon")) as fh:
            tool = fh.read()
        self.assertIn('KILLSWITCH = "shadow-killswitch"', tool)

    def test_compiles_with_the_trixie_parser(self):
        parser = os.path.join(AA_ROOT, "usr", "sbin", "apparmor_parser")
        if not AA_ROOT or not os.access(parser, os.X_OK):
            self.skipTest(f"no apparmor_parser 4.1 at {parser} (set AA_ROOT)")
        base = [parser, "--config-file=/dev/null", "-I", os.path.join(AA_ROOT, "etc", "apparmor.d")]
        p = subprocess.run(base + ["-Q", "-K", "-T", PROFILE], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        p = subprocess.run(base + ["-p", PROFILE], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.assertEqual(p.returncode, 0)
        flat = rules(p.stdout)
        self.assertEqual(flat.get("/usr/sbin/shadow-killswitch"), "PUx")
        self.assertEqual(flat.get("/run/shadow/*.json"), "r")
        # negative control: the same parser rejects a broken rule
        broken = self.text.replace("/run/shadow/*.json                r,", "/run/shadow/*.json                rz,")
        self.assertNotEqual(broken, self.text)
        p = subprocess.run(base + ["-Q", "-K", "-T"], input=broken, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        self.assertNotEqual(p.returncode, 0)


class TorMayBindHideMesPorts(unittest.TestCase):
    """Without this Tor (as root, confined by system_tor) cannot bind Hide
    Me's DNSPort and TransPort, fails to start, and DNS protection and Ghost
    Mode stop working."""

    def test_the_ports_are_below_1024(self):
        with open(TOOL) as fh:
            tool = fh.read()
        ports = {name: int(re.search(rf'^{name}\s*=\s*"(\d+)"', tool, re.M).group(1))
                 for name in ("DNS_PORT", "TPROXY_PORT", "TOR_SOCKS_PORT")}
        for name, port in ports.items():
            self.assertLess(port, 1024, name)
        self.assertIn('TOR_KEEP_BIND = "KeepBindCapabilities 1\\n"', tool)

    def test_debians_tor_unit_keeps_the_capability(self):
        unit = os.path.join(TOR_ROOT, "usr", "lib", "systemd", "system", "tor@default.service")
        if not TOR_ROOT or not os.path.exists(unit):
            self.skipTest(f"no extracted tor package at {TOR_ROOT} (set TOR_ROOT)")
        with open(unit) as fh:
            text = fh.read()
        bounding = [ln for ln in text.splitlines() if ln.startswith("CapabilityBoundingSet=")]
        self.assertTrue(bounding and all("CAP_NET_BIND_SERVICE" in ln for ln in bounding), bounding)
        self.assertIn("AppArmorProfile=-system_tor", text)

    def test_system_tor_grants_net_bind_service(self):
        parser = os.path.join(AA_ROOT, "usr", "sbin", "apparmor_parser")
        profile = os.path.join(TOR_ROOT, "etc", "apparmor.d", "system_tor")
        if not AA_ROOT or not TOR_ROOT or not os.access(parser, os.X_OK) or not os.path.exists(profile):
            self.skipTest(f"no apparmor_parser 4.1 at {parser} or no system_tor at {profile} (set AA_ROOT, TOR_ROOT)")
        tmp = tempfile.mkdtemp(prefix="aa-tor.")
        try:
            shutil.copytree(os.path.join(AA_ROOT, "etc", "apparmor.d"), os.path.join(tmp, "d"))
            shutil.copy(profile, os.path.join(tmp, "d", "system_tor"))
            shutil.copy(os.path.join(TOR_ROOT, "etc", "apparmor.d", "abstractions", "tor"),
                        os.path.join(tmp, "d", "abstractions", "tor"))
            os.makedirs(os.path.join(tmp, "d", "local"), exist_ok=True)
            open(os.path.join(tmp, "d", "local", "system_tor"), "w").close()    # tor's postinst: empty
            base = [parser, "--config-file=/dev/null", "-I", os.path.join(tmp, "d")]
            p = subprocess.run(base + ["-Q", "-K", "-T", os.path.join(tmp, "d", "system_tor")],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            p = subprocess.run(base + ["-p", os.path.join(tmp, "d", "system_tor")], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
            self.assertEqual(p.returncode, 0)
            body = p.stdout.split("profile system_tor", 1)[1]
            caps = {m.group(1) for m in re.finditer(r"^\s*capability\s+([a-z_]+)\s*,", body, re.M)}
            # through abstractions/tor -> nameservice -> nis ("portmapper may
            # ask root processes to do nis/ldap at low ports")
            self.assertIn("net_bind_service", caps)
            self.assertIn("setuid", caps)                               # positive control: abstractions/tor
            self.assertNotIn("sys_admin", caps)                         # negative control
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
