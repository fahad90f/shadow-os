# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared loader for the NEXUS Arsenal profile manifests (Python standard library only).

Used by validate.py, check-availability.py, profile-size.py, gen-control.py and
gen-profiles-doc.py so that all of them read the manifests the same way.
"""
import json
import os

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ARSENAL_DIR = os.path.dirname(TOOLS_DIR)
PROFILES_DIR = os.path.join(ARSENAL_DIR, "profiles")
REPO_ROOT = os.path.dirname(ARSENAL_DIR)

# Files in the profiles directory that are not profile manifests.
NON_PROFILE_FILES = {"schema.json", "categories.json", "sizes.json"}

# Architectures NEXUS OS is built for (distro/base/nexus.conf: NEXUS_ARCHES).
ARCHITECTURES = ("amd64", "arm64")

# The order profiles are presented in (cards, documentation, control file).
PROFILE_ORDER = ("core", "web", "network", "redteam", "blueteam", "forensics",
                 "reversing", "developer", "complete")


class ManifestError(Exception):
    """A manifest cannot be read or refers to something that does not exist."""


def load_json(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise ManifestError(f"{path}: {error.strerror}") from error
    except json.JSONDecodeError as error:
        raise ManifestError(f"{path}:{error.lineno}:{error.colno}: {error.msg}") from error


def load_categories(profiles_dir=PROFILES_DIR):
    """Return the category definitions as an ordered list of dicts."""
    data = load_json(os.path.join(profiles_dir, "categories.json"))
    categories = data.get("categories")
    if not isinstance(categories, list):
        raise ManifestError("categories.json: 'categories' must be a list")
    return categories


def profile_files(profiles_dir=PROFILES_DIR):
    """Paths of the profile manifests, in presentation order."""
    try:
        names = [n for n in os.listdir(profiles_dir)
                 if n.endswith(".json") and n not in NON_PROFILE_FILES]
    except OSError as error:
        raise ManifestError(f"{profiles_dir}: {error.strerror}") from error

    def rank(name):
        ident = name[:-5]
        return (PROFILE_ORDER.index(ident) if ident in PROFILE_ORDER else len(PROFILE_ORDER), ident)

    return [os.path.join(profiles_dir, n) for n in sorted(names, key=rank)]


def load_profiles(profiles_dir=PROFILES_DIR):
    """Return {id: manifest} in presentation order (dicts keep insertion order)."""
    profiles = {}
    for path in profile_files(profiles_dir):
        manifest = load_json(path)
        if not isinstance(manifest, dict):
            raise ManifestError(f"{path}: a manifest must be a JSON object")
        ident = manifest.get("id")
        if not isinstance(ident, str):
            raise ManifestError(f"{path}: missing 'id'")
        if ident in profiles:
            raise ManifestError(f"{path}: profile id '{ident}' is defined twice")
        manifest["_path"] = path
        profiles[ident] = manifest
    return profiles


def included_profiles(profiles, ident, _seen=None):
    """Ids of the profiles 'ident' includes, transitively, in a stable order."""
    seen = _seen if _seen is not None else []
    for other in profiles[ident].get("includes", []):
        if other not in profiles:
            raise ManifestError(f"profile '{ident}' includes unknown profile '{other}'")
        if other == ident or other in seen:
            continue
        seen.append(other)
        included_profiles(profiles, other, seen)
    return seen


def own_tools(manifest):
    return list(manifest.get("tools", []))


def effective_tools(profiles, ident):
    """The profile's own tools followed by those of the profiles it includes.

    A package is listed once; the first entry wins (entries of one package are
    required to be identical across profiles, see validate.py).
    """
    tools, names = [], set()
    for source in [ident] + included_profiles(profiles, ident):
        for tool in own_tools(profiles[source]):
            if tool["package"] not in names:
                names.add(tool["package"])
                tools.append(tool)
    return tools


def effective_external(profiles, ident):
    entries, names = [], set()
    for source in [ident] + included_profiles(profiles, ident):
        for entry in profiles[source].get("external", []):
            if entry["name"] not in names:
                names.add(entry["name"])
                entries.append(entry)
    return entries


def tool_architectures(tool):
    """Architectures a tool entry is declared for."""
    return tuple(tool.get("architectures", ARCHITECTURES))


def all_packages(profiles):
    """{package: tool entry} over every profile."""
    packages = {}
    for manifest in profiles.values():
        for tool in own_tools(manifest):
            packages.setdefault(tool["package"], tool)
    return packages


def format_bytes(count):
    """Human-readable size with decimal units, as APT prints them."""
    value = float(count)
    for unit in ("B", "kB", "MB", "GB"):
        if value < 1000 or unit == "GB":
            return f"{value:.0f} {unit}" if unit in ("B", "kB") else f"{value:.1f} {unit}"
        value /= 1000
    return f"{value:.1f} GB"
