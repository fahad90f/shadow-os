#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hide Me's log wipe (shadow-anon wipe_logs: Ghost Mode, NUKE, "Wipe logs").

    python3 tests/security/test_shadow_anon_wipe.py [-v]

wipe_logs runs as root and empties ~/.bash_history, ~/.zsh_history and
~/.fish_history of every home and the thumbnail caches. A user controls what
those names are: a link ~/.bash_history -> /etc/udev/rules.d/... (or
/etc/shadow) made root empty that file and set it to 0600. These tests prove
that links, hard links and files of another owner are left alone, that a
home which is a link is skipped, and (as root) that each home is handled
with its owner's uid and gid, so the thumbnail directory stays the user's.

The real function runs against a temporary directory (every path constant of
the tool is redirected there, see shadow_anon_testlib); journalctl and find
are not run. Any user; the privilege tests need root and are skipped
otherwise. Standard library only.
"""
import io
import os
import shutil
import stat
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_anon_testlib as lib  # noqa: E402

USER = 4242


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def read(path):
    with open(path) as f:
        return f.read()


class WipeLogs(unittest.TestCase):
    def setUp(self):
        self.tool = t = lib.load_tool()
        self.dir = tempfile.mkdtemp(prefix="shadow-anon-wipe-")
        os.chmod(self.dir, 0o755)
        self.root = os.path.join(self.dir, "root")
        lib.redirect_paths(t, self.root)
        self.calls = []
        self.patches = [
            mock.patch.object(t, "sh", lambda cmd, timeout=30: (self.calls.append(cmd), (0, "", ""))[1]),
            mock.patch.object(t, "need_root", lambda: None),
        ]
        for p in self.patches:
            p.start()
        self.home = os.path.join(t.HOME_BASE, "alice")
        os.makedirs(self.home)
        # Files outside the home that a link must never reach.
        self.victim = os.path.join(self.root, "etc", "udev", "rules.d", "90-shadow-nousb.rules")
        write(self.victim, "SUBSYSTEM==\"usb\", DRIVER==\"usb-storage\", ATTR{authorized}=\"0\"\n")
        self.victim2 = os.path.join(self.root, "etc", "sysctl.d", "91-shadow-extreme.conf")
        write(self.victim2, "kernel.kptr_restrict = 2\n")
        self.victim_dir = os.path.join(self.root, "var", "lib", "important")
        write(os.path.join(self.victim_dir, "thumbnails", "important.db"), "data\n")

    def tearDown(self):
        for p in self.patches:
            p.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def wipe(self):
        lib.check_redirected(self.tool, self.root)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.tool.wipe_logs()
        return out.getvalue() + err.getvalue()

    def assert_untouched(self, path, text, mode=0o644):
        self.assertEqual(read(path), text)
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), mode)

    def plant(self):
        """Links in the home, as a user (or malware running as that user) could make them."""
        os.symlink(self.victim, os.path.join(self.home, ".bash_history"))
        os.link(self.victim2, os.path.join(self.home, ".zsh_history"))        # a hard link
        os.symlink(self.victim_dir, os.path.join(self.home, ".cache"))
        return read(self.victim), read(self.victim2)

    # ---- any user
    def test_links_in_a_home_reach_nothing(self):
        before, before2 = self.plant()
        write(os.path.join(self.home, ".fish_history"), "- cmd: secret\n", 0o644)       # the user's own
        self.wipe()
        self.assert_untouched(self.victim, before)
        self.assert_untouched(self.victim2, before2)
        self.assertTrue(os.path.islink(os.path.join(self.home, ".bash_history")))
        self.assertEqual(read(os.path.join(self.victim_dir, "thumbnails", "important.db")), "data\n")
        fish = os.path.join(self.home, ".fish_history")
        self.assertEqual(read(fish), "")                                    # a real history is emptied
        self.assertEqual(stat.S_IMODE(os.stat(fish).st_mode), 0o600)

    def test_a_home_that_is_a_link_is_skipped(self):
        target = os.path.join(self.root, "etc")
        write(os.path.join(target, ".bash_history"), "root history\n")
        os.symlink(target, os.path.join(self.tool.HOME_BASE, "mallory"))
        out = self.wipe()
        self.assertEqual(read(os.path.join(target, ".bash_history")), "root history\n")
        self.assertIn("skipped", out)
        self.assertIn("mallory", out)

    def test_histories_and_thumbnails_are_cleared(self):
        t = self.tool
        write(os.path.join(t.ROOT_HOME, ".bash_history"), "ls\n", 0o600)
        write(os.path.join(self.home, ".bash_history"), "ls -la\n", 0o644)
        write(os.path.join(self.home, ".zsh_history"), ": 1:0;ls\n", 0o600)
        thumbs = os.path.join(self.home, ".cache", "thumbnails")
        write(os.path.join(thumbs, "normal", "a.png"), "png")
        write(os.path.join(thumbs, "b.png"), "png")
        os.chmod(thumbs, 0o700)
        os.symlink(self.victim, os.path.join(thumbs, "link.png"))
        out = self.wipe()
        for path in (os.path.join(t.ROOT_HOME, ".bash_history"), os.path.join(self.home, ".bash_history"),
                     os.path.join(self.home, ".zsh_history")):
            self.assertEqual(read(path), "")
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        self.assertEqual(os.listdir(thumbs), [])                            # emptied, the directory kept
        self.assertEqual(stat.S_IMODE(os.stat(thumbs).st_mode), 0o700)
        self.assertTrue(os.path.exists(self.victim))                        # a link inside: only the link goes
        self.assertIn("shell histories and thumbnail caches cleared", out)
        self.assertNotIn("skipped", out)
        self.assertIn(["journalctl", "--vacuum-time=1s"], self.calls)

    # ---- root: each home with its owner's rights
    @unittest.skipUnless(os.geteuid() == 0, "needs root (homes owned by another user)")
    def test_each_home_is_wiped_with_its_owners_rights(self):
        t = self.tool
        for d in (os.path.join(self.root, "home"), self.home):
            os.chmod(d, 0o755)
        os.chown(self.home, USER, USER)
        os.chmod(self.home, 0o700)
        os.chown(self.victim, 0, 0)
        before, _ = self.plant()
        for path in (os.path.join(self.home, ".bash_history"), os.path.join(self.home, ".cache")):
            os.lchown(path, USER, USER)
        os.unlink(os.path.join(self.home, ".cache"))
        thumbs = os.path.join(self.home, ".cache", "thumbnails")
        write(os.path.join(thumbs, "c.png"), "png")
        for path in (os.path.join(self.home, ".cache"), thumbs, os.path.join(thumbs, "c.png")):
            os.chown(path, USER, USER)
        os.chmod(thumbs, 0o700)
        # A history file root left in the home: not the owner's, left alone.
        write(os.path.join(self.home, ".fish_history"), "root's\n", 0o644)
        seen = os.path.join(self.dir, "seen")
        write(seen, "", 0o666)                     # written by the children, as their owners
        real = t._wipe_home_files

        def spy(home, uid, thumbnails=True):
            with open(seen, "a") as f:
                f.write(f"{os.path.basename(home)} {os.geteuid()} {os.getegid()} {os.getgroups()}\n")
            return real(home, uid, thumbnails)
        with mock.patch.object(t, "_wipe_home_files", spy):
            out = self.wipe()
        self.assertIn(f"alice {USER} {USER} []", read(seen).splitlines())
        self.assertIn("root 0 0", read(seen))
        self.assert_untouched(self.victim, before)
        self.assertEqual(read(os.path.join(self.home, ".fish_history")), "root's\n")
        self.assertEqual(os.listdir(thumbs), [])
        st = os.stat(thumbs)
        self.assertEqual((st.st_uid, st.st_gid, stat.S_IMODE(st.st_mode)), (USER, USER, 0o700))
        self.assertNotIn("skipped", out)


if __name__ == "__main__":
    unittest.main()
