#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the SHADOW Arsenal profile manifests.

    arsenal/tools/validate.py [--profiles-dir DIR] [--quiet]

Two layers of checks:

1. Every manifest is validated against profiles/schema.json. Python's standard
   library has no JSON Schema implementation, so this script interprets the
   keywords the schema uses (type, const, enum, pattern, minLength, maxLength,
   minItems, maxItems, uniqueItems, required, properties,
   additionalProperties, items and local $ref). A keyword it does not know is
   an error, so the schema cannot silently grow past the validator.

2. Rules a schema cannot express:
   * the file name, the id and the metapackage name agree;
   * categories exist in categories.json; their glyphs exist in ShadowUI;
   * a package or an external tool is listed once per profile;
   * a package listed by several profiles has the same entry in each;
   * CLI tools name their command, GUI tools their desktop entry;
   * 'includes' refers to existing profiles and does not form a loop;
   * the 'complete' profile includes every other profile and adds nothing;
   * the profile icon exists in assets/icons (when run inside the repository).

Whether the packages exist in Debian is a separate question answered by
check-availability.py, which needs an APT package index.

Exit status: 0 valid, 1 invalid, 2 cannot read the input.
"""
import argparse
import os
import re
import sys

sys.dont_write_bytecode = True      # the repository is read-only in the build chroot

import arsenal_manifest as am

KNOWN_KEYWORDS = {
    "$schema", "$id", "$defs", "$ref", "title", "description", "type", "const", "enum", "pattern",
    "minLength", "maxLength", "minItems", "maxItems", "uniqueItems", "required", "properties",
    "additionalProperties", "items",
}

JSON_TYPES = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "integer": int,
    "number": (int, float),
}

# Fields that must be identical wherever a package is listed. The installable
# identity (what gets installed and how it starts) and the one-line summary are
# shared; 'category' and 'notes' may differ per profile, because the same tool
# is filed under a different, context-appropriate category in each profile
# (e.g. yara under Forensics, Defensive and Reverse Engineering).
SHARED_TOOL_FIELDS = ("name", "summary", "interface", "homepage", "architectures",
                      "command", "help", "desktop-id")


class SchemaChecker:
    """The subset of JSON Schema 2020-12 that profiles/schema.json uses."""

    def __init__(self, schema):
        self.root = schema
        self.errors = []

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise am.ManifestError(f"schema: only local references are supported, not '{ref}'")
        node = self.root
        for part in ref[2:].split("/"):
            if not isinstance(node, dict) or part not in node:
                raise am.ManifestError(f"schema: unresolved reference '{ref}'")
            node = node[part]
        return node

    def check(self, value, schema, where):
        unknown = set(schema) - KNOWN_KEYWORDS
        if unknown:
            raise am.ManifestError(f"schema: keyword(s) {sorted(unknown)} at {where} are not implemented by validate.py")
        if "$ref" in schema:
            self.check(value, self.resolve(schema["$ref"]), where)
        if "const" in schema and value != schema["const"]:
            self.errors.append(f"{where}: must be {schema['const']!r}")
        if "enum" in schema and value not in schema["enum"]:
            self.errors.append(f"{where}: {value!r} is not one of {schema['enum']}")
        if "type" in schema:
            expected = JSON_TYPES[schema["type"]]
            # bool is a subclass of int in Python; JSON keeps them apart.
            wrong_bool = isinstance(value, bool) and schema["type"] in ("integer", "number")
            if not isinstance(value, expected) or wrong_bool:
                self.errors.append(f"{where}: expected {schema['type']}, found {type(value).__name__}")
                return
        if isinstance(value, str):
            if "minLength" in schema and len(value) < schema["minLength"]:
                self.errors.append(f"{where}: shorter than {schema['minLength']} characters")
            if "maxLength" in schema and len(value) > schema["maxLength"]:
                self.errors.append(f"{where}: longer than {schema['maxLength']} characters ({len(value)})")
            if "pattern" in schema and not re.search(schema["pattern"], value):
                self.errors.append(f"{where}: {value!r} does not match {schema['pattern']}")
        if isinstance(value, list):
            if "minItems" in schema and len(value) < schema["minItems"]:
                self.errors.append(f"{where}: fewer than {schema['minItems']} items")
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                self.errors.append(f"{where}: more than {schema['maxItems']} items")
            if schema.get("uniqueItems"):
                seen = []
                for item in value:
                    if item in seen:
                        self.errors.append(f"{where}: {item!r} is listed twice")
                    seen.append(item)
            if "items" in schema:
                for index, item in enumerate(value):
                    self.check(item, schema["items"], f"{where}[{index}]")
        if isinstance(value, dict):
            for name in schema.get("required", []):
                if name not in value:
                    self.errors.append(f"{where}: missing '{name}'")
            properties = schema.get("properties", {})
            for name, item in value.items():
                if name in properties:
                    self.check(item, properties[name], f"{where}.{name}")
                elif schema.get("additionalProperties") is False:
                    self.errors.append(f"{where}: unknown field '{name}'")


def label(tool, index):
    return tool.get("package") or tool.get("name") or f"#{index}"


def check_profile(ident, manifest, category_ids, errors):
    path = os.path.basename(manifest["_path"])
    if path != f"{ident}.json":
        errors.append(f"{path}: the id '{ident}' does not match the file name")
    if manifest.get("metapackage") != f"shadow-arsenal-{ident}":
        errors.append(f"{path}: metapackage must be 'shadow-arsenal-{ident}'")

    seen = set()
    for index, tool in enumerate(manifest.get("tools", [])):
        if not isinstance(tool, dict):
            continue
        where = f"{path}: tool {label(tool, index)}"
        package = tool.get("package")
        if package in seen:
            errors.append(f"{where}: listed twice in this profile")
        seen.add(package)
        if tool.get("category") not in category_ids:
            errors.append(f"{where}: unknown category {tool.get('category')!r}")
        interface = tool.get("interface")
        if interface in ("cli", "both") and "command" not in tool:
            errors.append(f"{where}: interface '{interface}' needs 'command'")
        if interface in ("gui", "both") and "desktop-id" not in tool:
            errors.append(f"{where}: interface '{interface}' needs 'desktop-id'")
        if interface == "gui" and "help" in tool:
            errors.append(f"{where}: 'help' only applies to tools with a command-line interface")
        if "architectures" in tool and set(tool["architectures"]) == set(am.ARCHITECTURES):
            errors.append(f"{where}: 'architectures' lists every architecture; leave it out")

    seen = set()
    for index, entry in enumerate(manifest.get("external", [])):
        if not isinstance(entry, dict):
            continue
        where = f"{path}: external {label(entry, index)}"
        if entry.get("name") in seen:
            errors.append(f"{where}: listed twice in this profile")
        seen.add(entry.get("name"))
        if entry.get("category") not in category_ids:
            errors.append(f"{where}: unknown category {entry.get('category')!r}")


def check_cross_profile(profiles, errors):
    first = {}
    for ident, manifest in profiles.items():
        for tool in manifest.get("tools", []):
            if not isinstance(tool, dict) or "package" not in tool:
                continue
            package = tool["package"]
            if package not in first:
                first[package] = (ident, tool)
                continue
            other_id, other = first[package]
            for field in SHARED_TOOL_FIELDS:
                if tool.get(field) != other.get(field):
                    errors.append(f"{ident}.json: tool {package}: '{field}' differs from {other_id}.json "
                                  f"({tool.get(field)!r} vs {other.get(field)!r})")

    first = {}
    for ident, manifest in profiles.items():
        for entry in manifest.get("external", []):
            if not isinstance(entry, dict) or "name" not in entry:
                continue
            if entry["name"] not in first:
                first[entry["name"]] = (ident, entry)
            elif first[entry["name"]][1] != entry:
                errors.append(f"{ident}.json: external {entry['name']}: entry differs from "
                              f"{first[entry['name']][0]}.json")

    for ident, manifest in profiles.items():
        for other in manifest.get("includes", []):
            if other == ident:
                errors.append(f"{ident}.json: a profile cannot include itself")
            elif other not in profiles:
                errors.append(f"{ident}.json: includes unknown profile '{other}'")
            elif ident in am.included_profiles(profiles, other):
                errors.append(f"{ident}.json: includes '{other}', which includes '{ident}' again")

    if "complete" in profiles:
        complete = profiles["complete"]
        others = [p for p in profiles if p != "complete"]
        missing = [p for p in others if p not in complete.get("includes", [])]
        if missing:
            errors.append(f"complete.json: must include every other profile; missing {missing}")
        if complete.get("tools") or complete.get("external"):
            errors.append("complete.json: is the union of the other profiles and lists nothing of its own")
    for ident, manifest in profiles.items():
        if not manifest.get("includes") and not manifest.get("tools"):
            errors.append(f"{ident}.json: has neither tools nor includes")


def check_categories(categories, errors):
    ids = set()
    for index, category in enumerate(categories):
        where = f"categories.json[{index}]"
        if not isinstance(category, dict):
            errors.append(f"{where}: must be an object")
            continue
        for field in ("id", "name", "icon", "description"):
            if not isinstance(category.get(field), str) or not category.get(field):
                errors.append(f"{where}: missing '{field}'")
        ident = category.get("id")
        if isinstance(ident, str):
            if not re.fullmatch(r"[a-z][a-z-]+", ident):
                errors.append(f"{where}: bad id {ident!r}")
            if ident in ids:
                errors.append(f"{where}: id '{ident}' is defined twice")
            ids.add(ident)

    # Glyph names are checked against the ShadowUI icon set when it is at hand.
    glyph_list = os.path.join(am.REPO_ROOT, "desktop", "ui", "icons", "icons.list")
    if os.path.isfile(glyph_list):
        with open(glyph_list, encoding="utf-8") as handle:
            glyphs = {line.strip()[:-4] for line in handle if line.strip().endswith(".svg")}
        for category in categories:
            if isinstance(category, dict) and category.get("icon") not in glyphs:
                errors.append(f"categories.json: '{category.get('id')}': no ShadowUI glyph named {category.get('icon')!r}")
    return ids


def check_icons(profiles, errors):
    icon_dirs = [os.path.join(am.REPO_ROOT, "assets", "icons", sub) for sub in ("categories", "apps")]
    if not all(os.path.isdir(d) for d in icon_dirs):
        return False
    for ident, manifest in profiles.items():
        icon = manifest.get("icon")
        if isinstance(icon, str) and not any(os.path.isfile(os.path.join(d, icon + ".svg")) for d in icon_dirs):
            errors.append(f"{ident}.json: icon '{icon}' not found in assets/icons/categories or assets/icons/apps")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--profiles-dir", default=am.PROFILES_DIR, help="directory with the manifests")
    parser.add_argument("--quiet", action="store_true", help="print errors only")
    args = parser.parse_args()

    errors = []
    try:
        schema = am.load_json(os.path.join(args.profiles_dir, "schema.json"))
        categories = am.load_categories(args.profiles_dir)
        profiles = am.load_profiles(args.profiles_dir)
        if not profiles:
            raise am.ManifestError(f"{args.profiles_dir}: no profile manifests found")
        category_ids = check_categories(categories, errors)

        for ident, manifest in profiles.items():
            checker = SchemaChecker(schema)
            document = {key: value for key, value in manifest.items() if not key.startswith("_")}
            checker.check(document, schema, os.path.basename(manifest["_path"]))
            errors.extend(checker.errors)
            check_profile(ident, manifest, category_ids, errors)

        missing = [p for p in am.PROFILE_ORDER if p not in profiles]
        if missing:
            errors.append(f"profiles missing from {args.profiles_dir}: {', '.join(missing)}")
        if not errors:
            # The cross-profile rules assume well-formed manifests.
            check_cross_profile(profiles, errors)
        icons_checked = check_icons(profiles, errors)
    except am.ManifestError as error:
        print(f"validate: {error}", file=sys.stderr)
        return 2

    for message in errors:
        print(f"validate: {message}", file=sys.stderr)
    if errors:
        print(f"validate: {len(errors)} problem(s) in {args.profiles_dir}", file=sys.stderr)
        return 1

    if not args.quiet:
        for ident, manifest in profiles.items():
            own, total = len(manifest.get("tools", [])), len(am.effective_tools(profiles, ident))
            external = len(am.effective_external(profiles, ident))
            detail = f"{own} tools" if own == total else f"{total} tools ({own} own, the rest by reference)"
            print(f"  {ident:10s} {detail}, {external} external")
        print(f"validate: {len(profiles)} profiles, {len(am.all_packages(profiles))} distinct packages, "
              f"{len(categories)} categories — valid"
              + ("" if icons_checked else " (profile icons not checked: assets/icons not found)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
