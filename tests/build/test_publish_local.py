#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/publish-updates.sh --local registers a build on "this machine".

    python3 tests/build/test_publish_local.py [-v]

The script runs with --no-build against a made-up package repository
(SHADOW_OUT) and installs into a scratch directory (SHADOW_LOCAL_ROOT) instead
of the running system, which is as far as it goes without a system to restart:
the packages are registered as an APT source and the restart-time updater of
the clone is put in place. Nothing is built, no package manager runs and
nothing outside the scratch directory is written.

Standard library only; runs as any user. Skipped where rsync is missing.
"""
import filecmp
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(ROOT, "scripts", "publish-updates.sh")
OFFLINE = os.path.join(ROOT, "applications", "updater", "offline")
UNITS = "usr/lib/systemd/system"


def codename():
    out = subprocess.run(["sh", "-c", '. ./distro/base/shadow.conf; printf %s "$SHADOW_CODENAME"'],
                         cwd=ROOT, capture_output=True, text=True, check=True)
    return out.stdout


@unittest.skipUnless(shutil.which("rsync"), "rsync is not installed")
class PublishLocal(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="publish-local-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.out = os.path.join(self.dir, "out")
        self.root = os.path.join(self.dir, "root")
        os.makedirs(self.root)
        # sudo must never be needed for a scratch root: this one fails loudly.
        self.bin = os.path.join(self.dir, "bin")
        os.makedirs(self.bin)
        sudo = os.path.join(self.bin, "sudo")
        with open(sudo, "w", encoding="utf-8") as fh:
            fh.write("#!/bin/sh\necho 'sudo was called' >&2\nexit 97\n")
        os.chmod(sudo, 0o755)

    def repository(self, build="202611250101"):
        pool = os.path.join(self.out, "repo", "pool", "core", "s", "shadow-base-files")
        dists = os.path.join(self.out, "repo", "dists", codename())
        os.makedirs(pool)
        os.makedirs(dists)
        for path, text in ((os.path.join(pool, f"shadow-base-files_0.2.0.build{build}_all.deb"), "deb"),
                           (os.path.join(dists, "InRelease"), "signed index"),
                           (os.path.join(self.out, "repo", "shadow-archive-keyring.gpg"), "the key of this build")):
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)

    def publish(self, *args):
        env = {"PATH": self.bin + ":" + os.environ.get("PATH", "/usr/bin:/bin"), "HOME": self.dir,
               "SHADOW_OUT": self.out, "SHADOW_LOCAL_ROOT": self.root, "LANG": "C.UTF-8"}
        return subprocess.run(["bash", SCRIPT, *args], env=env, capture_output=True, text=True, timeout=120,
                              stdin=subprocess.DEVNULL)

    def path(self, relative):
        return os.path.join(self.root, relative)

    def test_registers_the_build_and_the_restart_time_updater(self):
        self.repository()
        done = self.publish("--local", "--no-build", "--later")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("sudo was called", done.stderr)
        self.assertIn("build 202611250101", done.stderr)
        # The packages, their index and the key they are signed with.
        self.assertTrue(os.path.isfile(self.path(
            "var/lib/shadow-updates/pool/core/s/shadow-base-files/shadow-base-files_0.2.0.build202611250101_all.deb")))
        self.assertTrue(os.path.isfile(self.path(f"var/lib/shadow-updates/dists/{codename()}/InRelease")))
        key = self.path("usr/share/keyrings/shadow-updates.gpg")
        with open(key, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "the key of this build")
        self.assertEqual(stat.S_IMODE(os.stat(key).st_mode), 0o644)
        with open(self.path("etc/apt/sources.list.d/shadow-updates.sources"), encoding="utf-8") as fh:
            source = fh.read()
        self.assertIn("URIs: copy:///var/lib/shadow-updates\n", source)
        self.assertIn(f"Suites: {codename()}\n", source)
        self.assertIn("Signed-By: /usr/share/keyrings/shadow-updates.gpg\n", source)
        # The restart-time updater: the clone's files, where the Updater package puts them.
        tool = self.path("usr/sbin/shadow-offline-update")
        self.assertTrue(filecmp.cmp(tool, os.path.join(OFFLINE, "shadow-offline-update"), shallow=False))
        self.assertTrue(os.access(tool, os.X_OK))
        self.assertTrue(filecmp.cmp(self.path(f"{UNITS}/shadow-offline-update.service"),
                                    os.path.join(OFFLINE, "shadow-offline-update.service"), shallow=False))
        self.assertTrue(filecmp.cmp(self.path(f"{UNITS}/packagekit-offline-update.service.d/50-shadow.conf"),
                                    os.path.join(OFFLINE, "packagekit-offline-update.conf"), shallow=False))
        link = self.path(f"{UNITS}/system-update.target.wants/shadow-offline-update.service")
        self.assertEqual(os.readlink(link), "../shadow-offline-update.service")
        self.assertIn("bringing the restart-time updater up to date", done.stderr)

    def test_the_updater_files_match_what_the_package_installs(self):
        # The same destinations as applications/updater/CMakeLists.txt.
        with open(os.path.join(ROOT, "applications", "updater", "CMakeLists.txt"), encoding="utf-8") as fh:
            cmake = fh.read()
        for needle in ("install(PROGRAMS offline/shadow-offline-update DESTINATION ${CMAKE_INSTALL_SBINDIR})",
                       "install(FILES offline/shadow-offline-update.service DESTINATION lib/systemd/system)",
                       "DESTINATION lib/systemd/system/packagekit-offline-update.service.d",
                       "RENAME 50-shadow.conf",
                       "lib/systemd/system/system-update.target.wants",
                       "CREATE_LINK ../shadow-offline-update.service"):
            self.assertIn(needle, cmake)

    def test_an_older_updater_is_replaced_and_a_current_one_left_alone(self):
        self.repository()
        os.makedirs(self.path("usr/sbin"))
        with open(self.path("usr/sbin/shadow-offline-update"), "w", encoding="utf-8") as fh:
            fh.write("#!/bin/sh\n# the tool of an earlier build\n")
        first = self.publish("--local", "--no-build", "--later")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(filecmp.cmp(self.path("usr/sbin/shadow-offline-update"),
                                    os.path.join(OFFLINE, "shadow-offline-update"), shallow=False))
        second = self.publish("--local", "--no-build", "--later")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertNotIn("bringing the restart-time updater up to date", second.stderr)

    def test_packages_of_an_earlier_build_do_not_linger(self):
        self.repository()
        stale = self.path("var/lib/shadow-updates/pool/core/s/shadow-base-files")
        os.makedirs(stale)
        with open(os.path.join(stale, "shadow-base-files_0.2.0.build202601010000_all.deb"), "w") as fh:
            fh.write("old")
        self.assertEqual(self.publish("--local", "--no-build", "--later").returncode, 0)
        self.assertEqual(os.listdir(stale), ["shadow-base-files_0.2.0.build202611250101_all.deb"])

    def test_the_image_snapshot_is_turned_off_when_the_key_differs(self):
        self.repository()
        os.makedirs(self.path("etc/apt/sources.list.d"))
        os.makedirs(self.path("usr/share/keyrings"))
        snapshot = self.path("etc/apt/sources.list.d/shadow-local.sources")
        with open(snapshot, "w", encoding="utf-8") as fh:
            fh.write("Types: deb\n")
        with open(self.path("usr/share/keyrings/shadow-archive-keyring.gpg"), "w", encoding="utf-8") as fh:
            fh.write("the key of the image")
        self.assertEqual(self.publish("--local", "--no-build", "--later").returncode, 0)
        self.assertFalse(os.path.exists(snapshot))
        self.assertTrue(os.path.exists(snapshot + ".disabled"))

    def test_the_image_snapshot_stays_when_the_key_is_the_same(self):
        self.repository()
        os.makedirs(self.path("etc/apt/sources.list.d"))
        os.makedirs(self.path("usr/share/keyrings"))
        snapshot = self.path("etc/apt/sources.list.d/shadow-local.sources")
        with open(snapshot, "w", encoding="utf-8") as fh:
            fh.write("Types: deb\n")
        with open(self.path("usr/share/keyrings/shadow-archive-keyring.gpg"), "w", encoding="utf-8") as fh:
            fh.write("the key of this build")
        self.assertEqual(self.publish("--local", "--no-build", "--later").returncode, 0)
        self.assertTrue(os.path.exists(snapshot))

    def test_now_only_registers(self):
        self.repository()
        done = self.publish("--local", "--no-build", "--now")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("sudo apt update && sudo apt upgrade", done.stderr)
        self.assertTrue(os.path.exists(self.path("etc/apt/sources.list.d/shadow-updates.sources")))
        self.assertFalse(os.path.exists(self.path("usr/sbin/shadow-offline-update")))

    def test_no_build_needs_an_earlier_build(self):
        done = self.publish("--local", "--no-build", "--later")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("there are no built packages", done.stderr)
        self.assertEqual(os.listdir(self.root), [])

    def test_options_that_do_not_go_together(self):
        for args in (("--no-build",), ("--restart",), ("--local", "--url", "https://x.vercel.app")):
            done = self.publish(*args)
            self.assertNotEqual(done.returncode, 0, args)
            self.assertIn("go", done.stderr)
        self.assertEqual(os.listdir(self.root), [])


if __name__ == "__main__":
    unittest.main()
