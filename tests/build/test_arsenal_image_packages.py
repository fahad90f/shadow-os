#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Arsenal metapackages an image installs depend only on packages APT can find.

    SHADOW_TEST_APT_LISTS=build/work/chroot-build-amd64/var/lib/apt/lists \\
        python3 tests/build/test_arsenal_image_packages.py [-v]

The desktop image installs shadow-arsenal-complete (distro/base/variants/),
which depends on every profile's metapackage. If one of those stanzas in
debian/control names a package that none of the image's sources has, the image
build's "apt-get install" fails after hours of building. Copying
arsenal/packaging/control.in's shadow-arsenal-pentest stanza into
debian/control would do that today (docs/limitations.md).

SHADOW_TEST_APT_LISTS is a directory of APT "Packages" indices (plain, .gz or
.xz), for example the build chroot's lists; CI uses those right after creating
the chroot. Without it the check is skipped. Standard library only.
"""
import gzip
import lzma
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LISTS = os.environ.get("SHADOW_TEST_APT_LISTS", "")


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


def relations(value):
    """[[alternative names]] of a Depends value, substitution variables left out."""
    groups = []
    for item in value.replace("\n", " ").split(","):
        names = [re.split(r"[\s(\[:]", alt.strip())[0] for alt in item.split("|") if alt.strip()]
        names = [n for n in names if n and not n.startswith("${")]
        if names:
            groups.append(names)
    return groups


def image_packages():
    """SHADOW packages that any image variant installs by name."""
    names = set()
    variants = os.path.join(ROOT, "distro", "base", "variants")
    for name in os.listdir(variants):
        if name.endswith(".conf"):
            with open(os.path.join(variants, name), encoding="utf-8") as fh:
                match = re.search(r'(?m)^VARIANT_SHADOW_PACKAGES="([^"]*)"', fh.read())
            if match:
                names.update(match.group(1).split())
    return names


def available(directory):
    """Package names and virtual packages in the indices of an APT lists directory."""
    names = set()
    for name in os.listdir(directory):
        path = os.path.join(directory, name)
        if not re.search(r"_Packages(\.gz|\.xz)?$", name):
            continue
        opener = gzip.open if name.endswith(".gz") else lzma.open if name.endswith(".xz") else open
        with opener(path, "rt", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("Package: "):
                    names.add(line.split()[1])
                elif line.startswith("Provides: "):
                    names.update(re.split(r"[\s(]", p.strip())[0] for p in line[10:].split(","))
    return names


def arsenal_closure(control, start):
    """shadow-arsenal-* stanzas reached through Depends from the packages in 'start'."""
    seen, todo = set(), [p for p in start if p in control]
    while todo:
        package = todo.pop()
        if package in seen:
            continue
        seen.add(package)
        for group in relations(control[package].get("Depends", "")):
            todo += [n for n in group if n in control and n not in seen]
    return sorted(p for p in seen if p.startswith("shadow-arsenal"))


class ArsenalDepends(unittest.TestCase):
    def test_parser(self):
        control = stanzas()
        self.assertIn("shadow-arsenal-complete", arsenal_closure(control, image_packages()))
        self.assertIn("shadow-arsenal-pentest", arsenal_closure(control, ["shadow-arsenal-complete"]))
        self.assertEqual(relations(" ${misc:Depends},\n a (>= 1), b | c:any,\n d [amd64]"),
                         [["a"], ["b", "c"], ["d"]])

    @unittest.skipUnless(LISTS, "set SHADOW_TEST_APT_LISTS to an APT lists directory")
    def test_every_dependency_is_in_the_image_sources(self):
        self.assertTrue(os.path.isdir(LISTS), f"SHADOW_TEST_APT_LISTS={LISTS} is not a directory")
        names = available(LISTS)
        self.assertGreater(len(names), 1000, f"{LISTS} holds no Packages index")
        control = stanzas()
        missing = {}
        for package in arsenal_closure(control, image_packages()):
            for group in relations(control[package].get("Depends", "")):
                if not any(n in names or n in control for n in group):
                    missing.setdefault(package, []).append(" | ".join(group))
        self.assertEqual(missing, {}, "the image build's apt-get install would fail on these")


if __name__ == "__main__":
    unittest.main()
