#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packaging consistency of shadow-hardening and of the Blue Team profile.

    python3 tests/security/test_hardening_packaging.py [-v]

  * every source path in debian/shadow-hardening.install exists in the tree,
    and the programs it installs are executable;
  * every link in debian/shadow-hardening.links points at a file the package
    installs;
  * shadow-arsenal-blueteam does not pull in a second firewall (firewalld,
    ufw: only suggested) nor the stock OpenSnitch UI, the manifests say the
    same, and arsenal/packaging/control.in matches debian/control for it;
  * no image hook installs iptables-persistent / netfilter-persistent;
  * every Shadow program the Control Center, Network Center and the shell run
    is in a package (shadow-harden was only copied into images, so upgrades
    never got its fixes), and the Control Center needs the shadow-hardening
    of its own version;
  * systemd-resolved is only in image variants that install shadow-hardening,
    which configures it (Debian's defaults answer LLMNR, plain DNS);
  * the maintainer scripts use one "running system" test, and prerm's check
    for an engaged panic matches what shadow-panic status --json prints.
    (tests/security/test_hardening_maintscripts.py runs them with dpkg.)

Standard library only; runs as any user and writes nothing outside a
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
from contextlib import redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def stanza(text, package):
    match = re.search(r"(?ms)^Package: " + re.escape(package) + r"\n.*?(?=\n\n|\Z)", text)
    return match.group(0) if match else None


def field(stanza_text, name):
    match = re.search(rf"(?m)^{name}:(.*(?:\n[ \t].*)*)", stanza_text)
    if not match:
        return set()
    return {re.split(r"[\s(\[]", item.strip())[0] for item in match.group(1).split(",") if item.strip()}


def install_entries():
    entries = []
    for number, line in enumerate(read("debian", "shadow-hardening.install").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if line:
            parts = line.split()
            entries.append((number, parts[:-1], parts[-1]))
    return entries


class InstallList(unittest.TestCase):
    def test_every_source_exists(self):
        missing = []
        for number, sources, _ in install_entries():
            for source in sources:
                if not os.path.exists(os.path.join(ROOT, source)):
                    missing.append(f"line {number}: {source}")
        self.assertEqual(missing, [], "debian/shadow-hardening.install names files that are not in the tree")

    def test_programs_are_executable(self):
        for number, sources, dest in install_entries():
            if dest.rstrip("/") in ("usr/sbin", "usr/bin") or dest.startswith("usr/lib/NetworkManager/dispatcher.d"):
                for source in sources:
                    path = os.path.join(ROOT, source)
                    if os.path.isfile(path):
                        self.assertTrue(os.access(path, os.X_OK), f"line {number}: {source} is not executable")

    def test_links_point_at_installed_files(self):
        path = os.path.join(ROOT, "debian", "shadow-hardening.links")
        if not os.path.exists(path):
            self.skipTest("no links file")
        installed = set()
        for _, sources, dest in install_entries():
            for source in sources:
                full = os.path.join(ROOT, source)
                if os.path.isdir(full):
                    installed.add(os.path.join(dest, os.path.basename(source.rstrip("/"))))
                else:
                    installed.add(os.path.join(dest, os.path.basename(source)))
        for line in read("debian", "shadow-hardening.links").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                target = line.split()[0]
                self.assertIn(target, installed, f"link target {target} is not installed")

    def test_no_file_installed_twice(self):
        seen = {}
        for number, sources, dest in install_entries():
            for source in sources:
                target = os.path.join(dest, os.path.basename(source.rstrip("/")))
                self.assertNotIn(target, seen, f"line {number}: {target} also installed by line {seen.get(target)}")
                seen[target] = number


class BlueTeam(unittest.TestCase):
    def test_no_second_firewall(self):
        control = stanza(read("debian", "control"), "shadow-arsenal-blueteam")
        depends, suggests = field(control, "Depends"), field(control, "Suggests")
        for package in ("firewalld", "ufw", "python3-opensnitch-ui", "opensnitch"):
            self.assertNotIn(package, depends)
        self.assertTrue({"firewalld", "ufw"} <= suggests)
        self.assertNotIn("python3-opensnitch-ui", read("debian", "control"))

    def test_control_in_matches(self):
        generated = stanza(read("arsenal", "packaging", "control.in"), "shadow-arsenal-blueteam")
        self.assertEqual(generated, stanza(read("debian", "control"), "shadow-arsenal-blueteam"))

    def test_manifest(self):
        tools = {t["package"]: t for t in json.loads(read("arsenal", "profiles", "blueteam.json"))["tools"]}
        self.assertNotIn("python3-opensnitch-ui", tools)
        for package in ("firewalld", "ufw"):
            self.assertIs(tools[package].get("optional"), True)
            self.assertIn("not installed with the profile", tools[package]["notes"])

    def test_generators_and_validator(self):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        for script in (["arsenal/tools/validate.py", "--quiet"], ["arsenal/tools/gen-control.py", "--check"],
                       ["arsenal/tools/gen-profiles-doc.py", "--check"]):
            proc = subprocess.run([sys.executable, *script], cwd=ROOT, capture_output=True, text=True, env=env)
            self.assertEqual(proc.returncode, 0, f"{script[0]}: {proc.stdout}{proc.stderr}")


def installed_paths():
    """/path of every file a debian/*.install line installs (directories as given)."""
    paths = set()
    for name in sorted(os.listdir(os.path.join(ROOT, "debian"))):
        if not name.endswith(".install"):
            continue
        for line in read("debian", name).splitlines():
            parts = line.split("#", 1)[0].split()
            if len(parts) == 1:
                paths.add("/" + parts[0])
            for source in parts[:-1]:
                paths.add("/" + os.path.join(parts[-1].strip("/"), os.path.basename(source.rstrip("/"))))
    return paths


class Programs(unittest.TestCase):
    APPS = ("applications/control-center", "applications/network-center", "desktop/horizon-shell")

    def test_programs_the_applications_run_are_packaged(self):
        referenced = {}
        for app in self.APPS:
            for directory, _, files in os.walk(os.path.join(ROOT, app)):
                for name in files:
                    if not name.endswith((".cpp", ".h", ".qml")):
                        continue
                    with open(os.path.join(directory, name), encoding="utf-8", errors="replace") as fh:
                        for tool in re.findall(r'"(/usr/s?bin/shadow-[A-Za-z0-9_.-]+)"', fh.read()):
                            referenced.setdefault(tool, os.path.relpath(os.path.join(directory, name), ROOT))
        self.assertIn("/usr/bin/shadow-harden", referenced)
        installed = installed_paths()
        missing = {tool: where for tool, where in referenced.items() if tool not in installed}
        self.assertEqual(missing, {}, "programs the applications run that no package installs")

    def test_control_center_needs_its_own_shadow_harden(self):
        # Its Privacy page describes what shadow-harden of the same version
        # does (hidepid with the group "proc"); an older one does otherwise.
        control = stanza(read("debian", "control"), "shadow-control-center")
        self.assertRegex(control, r"(?m)^Depends:(.*\n[ \t])*.*shadow-hardening \(>= \$\{source:Version\}\)")


class Variants(unittest.TestCase):
    @staticmethod
    def variables(name):
        values = {}
        for key, value in re.findall(r'(?m)^(VARIANT_[A-Z_]+)="([^"]*)"', read("distro", "base", "variants", name)):
            values[key] = value.split()
        return values

    @staticmethod
    def packages(list_name):
        names = set()
        for line in read("distro", "base", "package-lists", list_name + ".list").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line.split()[0])
        return names

    def test_resolved_only_where_shadow_hardening_configures_it(self):
        with_resolved = []
        for name in sorted(os.listdir(os.path.join(ROOT, "distro", "base", "variants"))):
            if not name.endswith(".conf"):
                continue
            values = self.variables(name)
            selected = set().union(*(self.packages(n) for n in values["VARIANT_LISTS"]))
            if "systemd-resolved" in selected:
                with_resolved.append(name)
                self.assertIn("shadow-hardening", values["VARIANT_SHADOW_PACKAGES"],
                              f"{name}: systemd-resolved without shadow-hardening runs with Debian's "
                              "defaults (LLMNR answered on every network, plain DNS)")
        self.assertIn("desktop.conf", with_resolved)


def maintainer_script(name):
    return read("debian", f"shadow-hardening.{name}")


def running_system_function(text):
    match = re.search(r"(?ms)^running_system\(\) \{\n.*?^\}\n", text)
    return match.group(0) if match else None


class MaintainerScripts(unittest.TestCase):
    def test_one_running_system_test(self):
        functions = {name: running_system_function(maintainer_script(name)) for name in ("prerm", "postinst", "postrm")}
        for name, function in functions.items():
            self.assertIsNotNone(function, f"shadow-hardening.{name} has no running_system()")
            self.assertIn("/proc/1/root/", function, name)
        self.assertEqual(len(set(functions.values())), 1, "running_system() differs between the scripts")

    def test_no_other_running_system_guess(self):
        # "[ -d /run/systemd/system ]" alone is true in the installer's and the
        # recovery tools' chroots (they bind-mount the live /run).
        for name in ("prerm", "postinst", "postrm"):
            text = maintainer_script(name).replace(running_system_function(maintainer_script(name)) or "", "")
            self.assertNotRegex(text, r"if \[ -d /run/systemd/system \]", name)

    def test_prerm_recognises_an_engaged_panic(self):
        pattern = re.search(r"shadow-panic status --json [^|]*\| *grep -q '([^']+)'", maintainer_script("prerm"))
        self.assertIsNotNone(pattern, "prerm does not ask shadow-panic status --json")
        sys.dont_write_bytecode = True
        loader = importlib.machinery.SourceFileLoader(
            "shadow_panic_packaging_test", os.path.join(ROOT, "security", "firewall", "shadow-panic"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        panic = importlib.util.module_from_spec(spec)
        loader.exec_module(panic)
        tmp = tempfile.mkdtemp(prefix="packaging-panic.")
        try:
            panic.RUN_DIR = tmp
            printed = {}
            for engaged in (True, False):
                with open(os.path.join(tmp, "panic.json"), "w", encoding="utf-8") as fh:
                    json.dump({"version": 1, "engaged": engaged, "loaded": engaged, "error": None}, fh)
                out = io.StringIO()
                with mock.patch.object(panic.os, "geteuid", return_value=4242), redirect_stdout(out):
                    panic.main(["status", "--json"])
                printed[engaged] = out.getvalue()
        finally:
            shutil.rmtree(tmp)
        self.assertIn(pattern.group(1), printed[True])
        self.assertNotIn(pattern.group(1), printed[False])


class Hooks(unittest.TestCase):
    def test_no_persistent_iptables(self):
        hooks = os.path.join(ROOT, "distro", "base", "hooks")
        for name in sorted(os.listdir(hooks)):
            code = "\n".join(line.split("#", 1)[0] for line in read("distro", "base", "hooks", name).splitlines())
            self.assertNotRegex(code, r"\b(iptables|netfilter)-persistent\b", name)
        for name in os.listdir(os.path.join(ROOT, "distro", "base", "package-lists")):
            code = "\n".join(line.split("#", 1)[0] for line in read("distro", "base", "package-lists", name).splitlines())
            self.assertNotRegex(code, r"\b(iptables|netfilter)-persistent\b", name)


if __name__ == "__main__":
    unittest.main()
