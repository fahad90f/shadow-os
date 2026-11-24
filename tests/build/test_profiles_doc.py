#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""arsenal/docs/profiles.md says when and for which tool list its sizes were computed.

    python3 tests/build/test_profiles_doc.py [-v]

arsenal/profiles/sizes.json (tools/profile-size.py) holds the download and
installed size of each profile. A profile recomputed on its own carries its own
"generated" date, and the figures of a profile whose tool list changed since
are out of date. The generated overview must say both instead of giving one
date for every row, and the checked-in document must match the generator.

Standard library only; runs as any user and writes nothing.
"""
import copy
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(ROOT, "arsenal", "tools")
sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS)

import importlib.util  # noqa: E402

import arsenal_manifest as am  # noqa: E402

spec = importlib.util.spec_from_file_location("gen_profiles_doc", os.path.join(TOOLS, "gen-profiles-doc.py"))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def tool_count(profiles, ident, arch="amd64"):
    return len([t for t in am.effective_tools(profiles, ident)
                if arch in am.tool_architectures(t) and not am.is_optional(t)])


class SizesNote(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = am.load_profiles(am.PROFILES_DIR)
        cls.categories = am.load_categories(am.PROFILES_DIR)

    def sizes(self):
        """sizes.json as profile-size.py --write writes it: one date, current tool counts."""
        return {"schema": 1, "architecture": "amd64", "recommends": True, "generated": "2026-01-01",
                "profiles": {ident: {"tools": tool_count(self.profiles, ident), "packagesToInstall": 10,
                                     "downloadBytes": 1000000, "installedBytes": 5000000}
                             for ident in self.profiles}}

    def overview(self, sizes):
        """The document up to the first profile, in one line."""
        text = gen.generate(self.profiles, self.categories, sizes)
        return " ".join(text.split('<a id="', 1)[0].split())

    def test_one_date(self):
        text = self.overview(self.sizes())
        self.assertIn("computed 2026-01-01", text)
        self.assertNotIn("computed separately", text.lower())
        self.assertNotIn("out of date", text.lower())

    def test_profile_with_its_own_date(self):
        sizes = self.sizes()
        ident = next(iter(self.profiles))
        sizes["profiles"][ident]["generated"] = "2026-02-02"
        text = self.overview(sizes)
        self.assertIn(f"{self.profiles[ident]['name']} (2026-02-02)", text)
        self.assertIn("computed separately", text.lower())

    def test_figures_for_an_older_tool_list(self):
        sizes = self.sizes()
        ident = next(iter(self.profiles))
        current = sizes["profiles"][ident]["tools"]
        sizes["profiles"][ident]["tools"] = current - 2
        text = self.overview(sizes)
        self.assertIn(f"{self.profiles[ident]['name']} ({current - 2} tools; now {current})", text)
        self.assertIn("out of date", text.lower())
        others = copy.deepcopy(sizes)
        others["profiles"][ident]["tools"] = current
        self.assertNotIn("out of date", self.overview(others).lower())

    def test_checked_in_document_is_current(self):
        proc = subprocess.run([sys.executable, "-B", os.path.join(TOOLS, "gen-profiles-doc.py"), "--check"],
                              capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_checked_in_document_names_the_blue_team_date(self):
        sizes = am.load_json(os.path.join(am.PROFILES_DIR, "sizes.json"))
        with open(os.path.join(am.ARSENAL_DIR, "docs", "profiles.md"), encoding="utf-8") as fh:
            doc = fh.read()
        for ident, figures in sizes["profiles"].items():
            if figures.get("generated", sizes["generated"]) != sizes["generated"]:
                expected = f"{self.profiles[ident]['name']} ({figures['generated']})"
                self.assertTrue(expected in doc, f"arsenal/docs/profiles.md does not say {expected!r}")


if __name__ == "__main__":
    unittest.main()
