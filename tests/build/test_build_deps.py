#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The build chroot is created with every build dependency of the packaging.

    python3 tests/build/test_build_deps.py [-v]

build/chroot/create.sh installs build/chroot/build-deps.list into the build
chroot; dpkg-buildpackage then checks debian/control's Build-Depends inside
it and refuses to build when one is missing. A dependency added to
debian/control alone would pass every local test and stop the next package
build (scripts/publish-updates.sh, CI) after the chroot has been created.

Standard library only; runs as any user and writes nothing.
"""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Virtual packages of debian/control and the real package that provides them.
PROVIDED_BY = {"debhelper-compat": "debhelper"}


def build_depends():
    """Package names of every Build-Depends field of the source stanza (first alternative)."""
    with open(os.path.join(ROOT, "debian", "control"), encoding="utf-8") as fh:
        source = fh.read().split("\n\n")[0] + "\n"
    names = []
    for field in ("Build-Depends", "Build-Depends-Arch", "Build-Depends-Indep"):
        match = re.search(rf"(?ms)^{field}:(.*?)(?=^\S|\Z)", source)
        if not match:
            continue
        for item in match.group(1).replace("\n", " ").split(","):
            name = re.split(r"[\s(\[:|]", item.strip())[0]
            if name:
                names.append(name)
    return names


def listed(name):
    with open(os.path.join(ROOT, "build", "chroot", name), encoding="utf-8") as fh:
        return [line.split("#", 1)[0].strip() for line in fh if line.split("#", 1)[0].strip()]


class BuildDependencies(unittest.TestCase):
    def test_the_source_stanza_was_read(self):
        names = build_depends()
        self.assertIn("cmake", names)
        self.assertIn("qt6-base-dev", names)
        self.assertGreater(len(names), 30)

    def test_every_build_dependency_is_installed_into_the_chroot(self):
        chroot = set(listed("build-deps.list"))
        missing = sorted(n for n in set(build_depends()) if PROVIDED_BY.get(n, n) not in chroot)
        self.assertEqual(missing, [], "in debian/control's Build-Depends but not in build/chroot/build-deps.list")

    def test_no_package_is_listed_twice(self):
        for name in ("build-deps.list", "dev-tools.list"):
            packages = listed(name)
            twice = sorted({p for p in packages if packages.count(p) > 1})
            self.assertEqual(twice, [], name)

    def test_the_package_build_repairs_an_older_chroot(self):
        # build.sh checks the build dependencies itself and installs the list
        # when the chroot predates one of them.
        with open(os.path.join(ROOT, "build", "packages", "build.sh"), encoding="utf-8") as fh:
            script = fh.read()
        self.assertIn("dpkg-checkbuilddeps", script)
        self.assertIn("build/chroot/build-deps.list", script)
        self.assertLess(script.index("dpkg-checkbuilddeps"), script.index("dpkg-buildpackage -us -uc"))


if __name__ == "__main__":
    unittest.main()
