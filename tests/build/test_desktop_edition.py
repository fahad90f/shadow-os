#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The desktop edition's metapackage names every application the image ships.

    python3 tests/build/test_desktop_edition.py [-v]

An upgrade installs a new package only when something already installed
depends on it. shadow-desktop is that something for the applications: a
package added to the desktop image (distro/base/variants/desktop.conf) but not
to shadow-desktop's Depends would be in new images and never reach a system
that is already installed. This test fails when the two lists drift apart.

Standard library only; runs as any user and writes nothing.
"""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EDITION = "shadow-desktop"

# Packages of the desktop image that are deliberately not in the metapackage,
# and why. Anything else in the image must be one of its dependencies.
NOT_IN_THE_EDITION = {
    # Installed on every system whatever it is used for; nothing would ever
    # add or remove them through the edition.
    "shadow-base-files": "base system",
    "shadow-archive-keyring": "base system",
    "shadow-boot": "base system",
    "shadow-hardening": "base system",
    # Dependencies of the desktop shell and of every application.
    "shadow-ui": "library, pulled in by its users",
    "shadow-artwork": "data, pulled in by the shell",
    "shadow-themes": "data, pulled in by the shell",
    # Only on the live medium; the installer removes them from the installed system.
    "shadow-live": "live medium only",
    "shadow-installer": "live medium only",
    "shadow-installer-gui": "live medium only",
    # The Arsenal is managed by role (its profiles are installed on demand)
    # and must never arrive through an ordinary update.
    "shadow-arsenal": "role-gated",
    "shadow-arsenal-data": "role-gated",
    "shadow-arsenal-complete": "role-gated",
    EDITION: "the metapackage itself",
}


def stanzas():
    """{package: {field: value}} of debian/control's binary packages."""
    with open(os.path.join(ROOT, "debian", "control"), encoding="utf-8") as fh:
        text = fh.read()
    result = {}
    for block in text.split("\n\n"):
        fields = dict(re.findall(r"(?ms)^([A-Za-z-]+):(.*?)(?=^\S|\Z)", block + "\n"))
        if "Package" in fields:
            result[fields["Package"].strip()] = fields
    return result


def names(value):
    """Package names of a Depends value (first alternative), substitution variables left out."""
    found = []
    for item in value.replace("\n", " ").split(","):
        name = re.split(r"[\s(\[:|]", item.strip())[0]
        if name and not name.startswith("${"):
            found.append(name)
    return found


def image_packages():
    with open(os.path.join(ROOT, "distro", "base", "variants", "desktop.conf"), encoding="utf-8") as fh:
        match = re.search(r'^VARIANT_SHADOW_PACKAGES="([^"]*)"', fh.read(), re.M)
    assert match, "VARIANT_SHADOW_PACKAGES not found in desktop.conf"
    return match.group(1).split()


class DesktopEdition(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.control = stanzas()
        cls.image = image_packages()
        cls.edition = names(cls.control[EDITION].get("Depends", ""))

    def test_the_image_installs_the_metapackage(self):
        self.assertIn(EDITION, self.image)

    def test_every_application_of_the_image_is_in_the_metapackage(self):
        missing = [p for p in self.image if p not in self.edition and p not in NOT_IN_THE_EDITION]
        self.assertEqual(missing, [], "in the desktop image but not a dependency of shadow-desktop: "
                         "systems that are already installed would never get them")

    def test_every_dependency_is_a_package_of_the_image(self):
        for package in self.edition:
            self.assertIn(package, self.control, f"{package} has no stanza in debian/control")
            self.assertIn(package, self.image, f"{package} is not in the desktop image")

    def test_the_exceptions_are_real_and_not_dependencies(self):
        for package in NOT_IN_THE_EDITION:
            self.assertIn(package, self.control, f"{package} (listed as an exception) is not a package")
            self.assertNotIn(package, self.edition)

    def test_the_arsenal_never_comes_with_an_update(self):
        # Neither directly nor through any package the metapackage depends on.
        seen, queue = set(), list(self.edition)
        while queue:
            package = queue.pop()
            if package in seen or package not in self.control:
                continue
            seen.add(package)
            fields = self.control[package]
            queue += names(fields.get("Depends", "")) + names(fields.get("Pre-Depends", ""))
        arsenal = sorted(p for p in seen if p.startswith("shadow-arsenal"))
        self.assertEqual(arsenal, [])

    def test_it_is_a_metapackage_that_records_its_installation(self):
        fields = self.control[EDITION]
        self.assertEqual(fields["Architecture"].strip(), "all")
        with open(os.path.join(ROOT, "debian", "shadow-desktop.postinst"), encoding="utf-8") as fh:
            postinst = fh.read()
        # The record the restart-time updater looks for before adopting it.
        self.assertIn("/var/lib/shadow/edition-adopted", postinst)
        with open(os.path.join(ROOT, "applications", "updater", "offline", "shadow-offline-update"),
                  encoding="utf-8") as fh:
            tool = fh.read()
        self.assertIn("EDITION_RECORD=$ROOT/var/lib/shadow/edition-adopted", tool)
        self.assertIn("EDITION=shadow-desktop", tool)
        # Nothing removes the record: removing the package must stay respected.
        for script in ("shadow-desktop.postrm", "shadow-desktop.prerm"):
            self.assertFalse(os.path.exists(os.path.join(ROOT, "debian", script)), script)


if __name__ == "__main__":
    unittest.main()
