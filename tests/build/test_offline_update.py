#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The restart-time updater (shadow-offline-update) against stand-in commands.

    python3 tests/build/test_offline_update.py [-v]

The tool runs with SHADOW_OFFLINE_UPDATE_ROOT pointing at a scratch directory
(its state, the /system-update trigger and its log go there) and with
stand-ins for apt-get, dpkg-query, dpkg, systemctl and plymouth first on PATH.
The stand-ins record how they were called and answer from small files, so the
test decides what "the package sources" offer. No package is installed and no
real package manager is run.

What it covers: preparing and applying an update, and the adoption of the
desktop edition's metapackage (shadow-desktop) by a desktop system that never
had it: once, only where it installs together with the upgrade, never again
after it has been installed or removed.

Standard library only; runs as any user.
"""
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, "applications", "updater", "offline", "shadow-offline-update")

# apt-get: records its arguments; a simulation ("-s") prints "Inst" lines for
# the upgrade (file "upgradable") and for each named package the sources can
# install with it (file "installable": "name: package package…"), and fails
# for a named package they cannot; "-d" and the real run exit with the status
# in "download-rc" / "apply-rc".
APT_GET = r"""#!/bin/bash
here="$(dirname "$0")/.."
printf '%s\n' "$*" >> "$here/calls/apt-get"
sim=0; download=0; cmd=""; named=()
for a in "$@"; do
    case "$a" in
        -s) sim=1 ;;
        -d) download=1 ;;
        update|upgrade|clean) [ -n "$cmd" ] || cmd="$a" ;;
        -*|*=*) ;;
        *) [ "$cmd" = upgrade ] && named+=("$a") ;;
    esac
done
case "$cmd" in
    update) exit "$(cat "$here/update-rc" 2>/dev/null || echo 0)" ;;
    clean) exit 0 ;;
esac
extra=()
for n in "${named[@]}"; do
    line="$(grep "^$n:" "$here/installable" 2>/dev/null)" || { echo "E: Unable to locate package $n" >&2; exit 100; }
    extra+=("$n"); for p in ${line#*:}; do extra+=("$p"); done
done
if [ "$sim" = 1 ]; then
    for p in $(cat "$here/upgradable" 2>/dev/null) "${extra[@]}"; do echo "Inst $p (2 test [all])"; done
    exit 0
fi
[ "$download" = 1 ] && exit "$(cat "$here/download-rc" 2>/dev/null || echo 0)"
exit "$(cat "$here/apply-rc" 2>/dev/null || echo 0)"
"""
DPKG_QUERY = r"""#!/bin/bash
here="$(dirname "$0")/.."
for last; do :; done
grep -qx "$last" "$here/installed" 2>/dev/null && { printf 'ii '; exit 0; }
echo "dpkg-query: no packages found matching $last" >&2; exit 1
"""
RECORDER = r"""#!/bin/bash
printf '%s\n' "$*" >> "$(dirname "$0")/../calls/$(basename "$0")"
[ "$(basename "$0")" = plymouth ] && exit 1
exit 0
"""


class OfflineUpdate(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="offline-update-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.root = os.path.join(self.dir, "root")
        self.fake = os.path.join(self.dir, "fake")
        os.makedirs(os.path.join(self.fake, "bin"))
        os.makedirs(os.path.join(self.fake, "calls"))
        os.makedirs(self.root)
        for name, text in (("apt-get", APT_GET), ("dpkg-query", DPKG_QUERY), ("dpkg", RECORDER),
                           ("systemctl", RECORDER), ("plymouth", RECORDER)):
            path = os.path.join(self.fake, "bin", name)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        self.state = os.path.join(self.root, "var/lib/shadow/offline-update")
        self.link = os.path.join(self.root, "system-update")
        self.record = os.path.join(self.root, "var/lib/shadow/edition-adopted")
        # A desktop system from before the metapackage, with updates waiting
        # and sources that offer the metapackage and one application it adds.
        self.given("installed", "shadow-horizon\nshadow-ui\nshadow-files\n")
        self.given("upgradable", "shadow-horizon shadow-ui shadow-files")
        self.given("installable", "shadow-desktop: shadow-trading\n")

    def given(self, name, text):
        with open(os.path.join(self.fake, name), "w", encoding="utf-8") as fh:
            fh.write(text)

    def run_tool(self, *args):
        env = {"PATH": os.path.join(self.fake, "bin") + ":/usr/bin:/bin",
               "SHADOW_OFFLINE_UPDATE_ROOT": self.root, "LANG": "C.UTF-8"}
        return subprocess.run(["bash", TOOL, *args], env=env, capture_output=True, text=True, timeout=60)

    def calls(self, program="apt-get"):
        path = os.path.join(self.fake, "calls", program)
        if not os.path.exists(path):
            return []
        with open(path, encoding="utf-8") as fh:
            return fh.read().splitlines()

    def state_file(self, name):
        path = os.path.join(self.state, name)
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as fh:
            return fh.read()

    def downloads(self):
        return [c for c in self.calls() if " -d " in f" {c} "]

    def installs(self):
        return [c for c in self.calls() if "--no-download" in c and " -s " not in f" {c} "]

    def prepared(self):
        return os.path.islink(self.link) and os.readlink(self.link) == self.state

    # ---- prepare ---------------------------------------------------------------
    def test_a_desktop_from_before_the_metapackage_adopts_it(self):
        done = self.run_tool("prepare")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(self.downloads()), 1)
        self.assertTrue(self.downloads()[0].endswith(" upgrade shadow-desktop"), self.downloads())
        self.assertEqual(self.state_file("extra"), "shadow-desktop\n")
        self.assertEqual(self.state_file("packages").split(),
                         ["shadow-horizon", "shadow-ui", "shadow-files", "shadow-desktop", "shadow-trading"])
        self.assertTrue(self.prepared())
        self.assertIn("5 update(s) are ready", done.stdout)
        self.assertIn("applications added since this system was installed (shadow-desktop)", done.stdout)
        # The restart notice other tools read.
        with open(os.path.join(self.root, "run/reboot-required.pkgs"), encoding="utf-8") as fh:
            self.assertIn("shadow-trading", fh.read().split())
        # Preparing installs nothing and never writes the record itself.
        self.assertEqual(self.installs(), [])
        self.assertFalse(os.path.exists(self.record))

    def test_adoption_alone_is_an_update(self):
        self.given("upgradable", "")
        done = self.run_tool("prepare")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(self.prepared())
        self.assertEqual(self.state_file("packages").split(), ["shadow-desktop", "shadow-trading"])

    def test_a_system_with_the_record_is_left_alone(self):
        # It had the metapackage once; it is gone now (removed with an application).
        os.makedirs(os.path.dirname(self.record))
        open(self.record, "w").close()
        done = self.run_tool("prepare")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(self.downloads()[0].endswith(" upgrade"), self.downloads())
        self.assertIsNone(self.state_file("extra"))
        self.assertEqual(self.state_file("packages").split(), ["shadow-horizon", "shadow-ui", "shadow-files"])
        self.assertNotIn("applications added", done.stdout)
        self.assertFalse(any("shadow-desktop" in c for c in self.calls()))  # not even asked about

    def test_an_installed_metapackage_is_not_named(self):
        self.given("installed", "shadow-horizon\nshadow-desktop\n")
        self.run_tool("prepare")
        self.assertTrue(self.downloads()[0].endswith(" upgrade"), self.downloads())
        self.assertIsNone(self.state_file("extra"))

    def test_a_system_without_the_desktop_does_not_get_one(self):
        self.given("installed", "shadow-base-files\nshadow-hardening\n")
        self.given("upgradable", "shadow-base-files")
        self.run_tool("prepare")
        self.assertTrue(self.downloads()[0].endswith(" upgrade"), self.downloads())
        self.assertIsNone(self.state_file("extra"))
        self.assertEqual(self.state_file("packages").split(), ["shadow-base-files"])

    def test_sources_without_the_metapackage_still_update(self):
        self.given("installable", "")
        done = self.run_tool("prepare")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(self.downloads()[0].endswith(" upgrade"), self.downloads())
        self.assertIsNone(self.state_file("extra"))
        self.assertTrue(self.prepared())

    def test_nothing_to_do_clears_an_earlier_preparation(self):
        self.run_tool("prepare")
        self.assertTrue(self.prepared())
        self.given("upgradable", "")
        self.given("installable", "")
        done = self.run_tool("prepare")
        self.assertIn("up to date", done.stdout)
        self.assertFalse(os.path.lexists(self.link))
        self.assertIsNone(self.state_file("extra"))
        self.assertIsNone(self.state_file("packages"))

    def test_a_failed_download_prepares_nothing(self):
        self.given("download-rc", "100")
        done = self.run_tool("prepare")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("downloading the updates failed", done.stderr)
        self.assertFalse(os.path.lexists(self.link))
        self.assertIsNone(self.state_file("extra"))

    # ---- apply -----------------------------------------------------------------
    def test_apply_installs_the_upgrade_and_the_adoption(self):
        self.run_tool("prepare")
        done = self.run_tool("apply")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(self.installs()), 1)
        install = self.installs()[0]
        self.assertTrue(install.endswith(" upgrade shadow-desktop"), install)
        self.assertIn("--with-new-pkgs", install)
        self.assertIn("-y", install.split())
        self.assertFalse(os.path.lexists(self.link))  # the trigger is gone: the next boot is a normal one
        for name in ("packages", "prepared", "extra"):
            self.assertIsNone(self.state_file(name), name)
        self.assertTrue(self.state_file("last-result").startswith("success "))
        self.assertEqual(self.calls("systemctl"), ["--no-block reboot"])
        # Nothing is ever asked to be removed.
        for call in self.calls():
            for word in ("remove", "purge", "autoremove", "dist-upgrade", "full-upgrade", "install"):
                self.assertNotIn(word, call.split(), call)

    def test_apply_without_reboot_for_trying_it_out(self):
        self.run_tool("prepare")
        done = self.run_tool("apply", "--no-reboot")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls("systemctl"), [])

    def test_an_adoption_that_stopped_working_does_not_stop_the_upgrade(self):
        self.run_tool("prepare")
        self.given("installable", "")  # the sources changed between preparing and restarting
        done = self.run_tool("apply")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(self.installs()[0].endswith(" upgrade"), self.installs())
        with open(os.path.join(self.root, "var/log/shadow-offline-update.log"), encoding="utf-8") as fh:
            self.assertIn("not installing shadow-desktop", fh.read())
        self.assertTrue(self.state_file("last-result").startswith("success "))

    def test_apply_ignores_anything_that_is_not_a_package_name(self):
        self.run_tool("prepare")
        with open(os.path.join(self.state, "extra"), "w", encoding="utf-8") as fh:
            fh.write("-o=Dir::Etc::Main=/tmp/x\nshadow desktop\n../x\nUPPER\nshadow-desktop\n\n")
        done = self.run_tool("apply")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(self.installs()[0].endswith(" upgrade shadow-desktop"), self.installs())

    def test_a_failed_installation_is_reported_and_repaired(self):
        self.run_tool("prepare")
        self.given("apply-rc", "100")
        done = self.run_tool("apply")
        self.assertNotEqual(done.returncode, 0)
        self.assertTrue(self.state_file("last-result").startswith("failed "))
        self.assertEqual(self.calls("dpkg"), ["--configure -a"])
        self.assertFalse(os.path.lexists(self.link))  # a failure never makes every boot an update boot
        self.assertEqual(self.calls("systemctl"), ["--no-block reboot"])

    def test_apply_leaves_another_updaters_trigger_alone(self):
        os.symlink("/var/lib/PackageKit/prepared-update", self.link)
        done = self.run_tool("apply")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(os.path.lexists(self.link))
        self.assertEqual(self.installs(), [])
        self.assertEqual(self.calls("systemctl"), [])

    # ---- cancel and status -----------------------------------------------------
    def test_cancel_forgets_the_adoption_too(self):
        self.run_tool("prepare")
        done = self.run_tool("cancel")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertFalse(os.path.lexists(self.link))
        for name in ("packages", "prepared", "extra"):
            self.assertIsNone(self.state_file(name), name)
        # Not adopted and not recorded: the next update offers it again.
        self.run_tool("prepare")
        self.assertEqual(self.state_file("extra"), "shadow-desktop\n")

    def test_status_lists_what_is_prepared(self):
        self.assertIn("Nothing is prepared", self.run_tool("status").stdout)
        self.run_tool("prepare")
        out = self.run_tool("status").stdout
        self.assertIn("installed at the next restart", out)
        self.assertIn("shadow-trading", out)

    def test_it_needs_root_on_a_real_system(self):
        if os.geteuid() == 0:
            self.skipTest("runs as root here")
        env = {"PATH": os.path.join(self.fake, "bin") + ":/usr/bin:/bin"}
        done = subprocess.run(["bash", TOOL, "status"], env=env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("must run as root", done.stderr)


if __name__ == "__main__":
    unittest.main()
