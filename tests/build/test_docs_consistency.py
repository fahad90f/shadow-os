#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Documentation that readers act on agrees with the tree.

    python3 tests/build/test_docs_consistency.py [-v]

  * docs/limitations.md gives the value of net.ipv4.ip_unprivileged_port_start
    at which Tor's ports become bindable by any user. A port is privileged only
    while it is below that value (the kernel's check is port < the value), so
    that is the highest of Hide Me's Tor ports (shadow-anon's TPROXY_PORT,
    TOR_SOCKS_PORT and DNS_PORT), read from the tool itself;
  * docs/limitations.md names the apparmor_parser that
    tests/security/test_shadow_anon_apparmor.py compiles Hide Me's profile
    with, and that test;
  * no file of the repository tells a reader to run a script that is not in
    the repository (the scripts of the lab phase 1 was tested in), unless a
    file of that name has been added.

Standard library only; runs as any user and writes nothing.
"""
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIMITATIONS = os.path.join(ROOT, "docs", "limitations.md")
APPARMOR_TEST = os.path.join(ROOT, "tests", "security", "test_shadow_anon_apparmor.py")
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tests", "security"))
import shadow_anon_testlib as lib  # noqa: E402

# Scripts and helpers of the lab, which is not part of the repository.
LAB_ONLY = ("run-shell.sh", "build-app.sh", "run-app.sh", "lab-apt.sh", "lab_run")
SKIP_DIRS = {".git", "work", "node_modules", "__pycache__"}       # build/work: chroots and images


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def flat(text):
    return re.sub(r"\s+", " ", text)


def repository_files():
    """The repository's files (git's list when there is one, else a walk)."""
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, check=True).stdout
        files = [os.path.join(ROOT, p) for p in out.decode().split("\0") if p]
        if files:
            return [f for f in files if os.path.isfile(f)]
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for top, dirs, names in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        found += [os.path.join(top, n) for n in names]
    return found


class HideMeLimitations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = flat(read(LIMITATIONS))
        cls.tool = lib.load_tool()

    def test_the_port_start_threshold_is_the_highest_tor_port(self):
        """Regression: the entry said Tor's ports become bindable at 26 or below.
        The TransPort is 40: any value from 27 to 40 let any user bind TCP
        127.0.0.1:40 and take every redirected connection while Tor restarted."""
        t = self.tool
        highest = max(int(t.TPROXY_PORT), int(t.TOR_SOCKS_PORT), int(t.DNS_PORT))
        stated = [int(n) for n in re.findall(r"ip_unprivileged_port_start`? to (\d+) or below", self.text)]
        self.assertTrue(stated, "docs/limitations.md no longer says when Tor's ports become bindable")
        self.assertEqual(set(stated), {highest}, "docs/limitations.md: ip_unprivileged_port_start threshold")
        self.assertIn(f"Keep it above {highest}.", self.text)

    def test_the_apparmor_note_names_the_parser_the_test_uses(self):
        """Regression: the note said the profile was compiled with Ubuntu's
        apparmor_parser 4.0.1 while the profile test compiles it with trixie's 4.1."""
        tested = re.search(r"apparmor_parser (\d+(?:\.\d+)+)", read(APPARMOR_TEST)).group(1)
        named = set(re.findall(r"apparmor_parser (\d+(?:\.\d+)+)", self.text))
        self.assertEqual(named, {tested}, "docs/limitations.md names another apparmor_parser")
        self.assertIn("tests/security/test_shadow_anon_apparmor.py", self.text)


class NoLabOnlyScripts(unittest.TestCase):
    def test_no_file_names_a_script_that_is_not_in_the_repository(self):
        """Regression: the status module's README and its tile contrast test
        told readers to run the lab's run-shell.sh and build-app.sh."""
        files = repository_files()
        self.assertTrue(any(f.endswith("docs/limitations.md") for f in files))
        present = {os.path.basename(f) for f in files}
        wanted = [n for n in LAB_ONLY if n not in present]
        pattern = re.compile(r"(?<![\w.-])(" + "|".join(re.escape(n) for n in wanted) + r")(?![\w.-])")
        hits = []
        for path in files:
            if os.path.samefile(path, __file__):
                continue
            try:
                with open(path, "rb") as fh:
                    data = fh.read(4 << 20)
            except OSError:
                continue
            if b"\0" in data[:4096]:
                continue                                    # binary
            for no, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
                if pattern.search(line):
                    hits.append(f"{os.path.relpath(path, ROOT)}:{no}: {line.strip()[:100]}")
        self.assertEqual(hits, [], "files name scripts that are not in the repository")


if __name__ == "__main__":
    unittest.main()
