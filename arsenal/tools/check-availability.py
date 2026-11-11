#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify every Arsenal package really exists in Debian trixie main.

    arsenal/tools/check-availability.py [--profiles-dir DIR]
                                        [--arch-index arm64=PATH]
                                        [--contents PATH ...]
                                        [--component-index non-free=PATH ...]
                                        [--require-arm64] [--require-contents]
                                        [--json REPORT.json]

For each distinct package named by the manifests this checks that:

  * the package exists and is in component 'main' for amd64 (the build
    chroot's architecture), and
  * for every architecture the tool is declared for (default amd64 and arm64)
    the package is present either as an 'all' package or as a binary built for
    that architecture, and
  * a tool restricted with 'architectures' is really unavailable on the
    architectures it leaves out (so the restriction is justified, not a typo),
  * when a Contents index is available: the 'command' of a tool is an
    executable shipped by its package and the 'desktop-id' is a desktop entry
    shipped by it (so "Open" and "Open in terminal" start something real), and
  * every Debian package name recorded for an 'external' tool is still absent
    from trixie main (the day it is packaged the entry moves to 'tools'), and
    the stated reason matches where the package was found: 'not-in-debian-main'
    when contrib or non-free has it, 'not-in-debian' when nothing has it.

It is designed to run inside the build chroot, where the APT lists are already
present:

    scripts/dev.sh exec --dir arsenal -- \
        python3 /src/arsenal/tools/check-availability.py

In the chroot it uses `apt-cache show`/`policy` for the native architecture and
reads the downloaded arm64 Packages index for the foreign one. The arm64 index
is looked for, in order, at: a path given with --arch-index arm64=PATH, the
environment variable ARSENAL_ARM64_INDEX, /tmp/Packages-arm64, or the chroot's
own apt lists. If no arm64 index is available the arm64 checks are reported as
"skipped" rather than passed, so CI can require them.

The Contents indices (dists/trixie/main/Contents-<arch>.gz and Contents-all.gz)
and the contrib/non-free package indices are optional inputs, looked for at
/tmp/Contents-amd64.gz, /tmp/Contents-all.gz, /tmp/Packages-contrib-amd64.xz
and /tmp/Packages-non-free-amd64.xz. To fetch everything (inside the chroot):

    cd /tmp
    M=http://deb.debian.org/debian/dists/trixie
    curl -fsS $M/main/binary-arm64/Packages.xz | xz -dc > Packages-arm64
    curl -fsS -O $M/main/Contents-amd64.gz -O $M/main/Contents-all.gz
    curl -fsS -o Packages-contrib-amd64.xz  $M/contrib/binary-amd64/Packages.xz
    curl -fsS -o Packages-non-free-amd64.xz $M/non-free/binary-amd64/Packages.xz

Exit status: 0 all packages present where declared, 1 a problem was found,
2 the input could not be read or no package index is available.
"""
import argparse
import gzip
import json
import lzma
import os
import subprocess
import sys

sys.dont_write_bytecode = True

import arsenal_manifest as am

NATIVE_ARCH = "amd64"       # the build chroot
FOREIGN_ARCHES = ("arm64",)


def apt_cache_show(package):
    """Return the stanzas `apt-cache show` prints for 'package' (possibly several)."""
    try:
        output = subprocess.run(["apt-cache", "show", "--no-all-versions", package],
                                capture_output=True, text=True, check=False)
    except FileNotFoundError:
        raise am.ManifestError("apt-cache not found: run this inside the build chroot")
    stanzas = []
    for block in output.stdout.split("\n\n"):
        fields = {}
        key = None
        for line in block.splitlines():
            if line and line[0] in " \t":
                continue
            if ":" in line:
                key, _, value = line.partition(":")
                fields[key.strip()] = value.strip()
        if fields.get("Package"):
            stanzas.append(fields)
    return stanzas


def native_status(package):
    """(exists, in_main, architecture, version) for the native architecture."""
    stanzas = apt_cache_show(package)
    if not stanzas:
        return (False, False, None, None)
    stanza = stanzas[0]
    section = stanza.get("Section", "")
    # In 'main' the Section has no component prefix; contrib/non-free do.
    in_main = "/" not in section or section.startswith("main/")
    return (True, in_main, stanza.get("Architecture"), stanza.get("Version"))


def load_arch_index(path):
    """Set of binary package names in a Packages or Packages.xz/.gz file."""
    names = set()
    opener = open
    mode = "rt"
    if path.endswith(".xz"):
        opener, mode = lzma.open, "rt"
    elif path.endswith(".gz"):
        opener, mode = gzip.open, "rt"
    with opener(path, mode, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("Package: "):
                names.add(line[9:].strip())
    return names


BIN_DIRS = ("usr/bin/", "usr/sbin/", "usr/games/", "bin/", "sbin/")
DESKTOP_DIR = "usr/share/applications/"


def load_contents(paths):
    """{package: set of paths} of the executables and desktop entries in the Contents indices."""
    files = {}
    prefixes = BIN_DIRS + (DESKTOP_DIR, "usr/lib/jvm/")
    for path in paths:
        opener = gzip.open if path.endswith(".gz") else (lzma.open if path.endswith(".xz") else open)
        with opener(path, "rt", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if not line.startswith(prefixes):
                    continue
                name, _, owners = line.rstrip("\n").rpartition(" ")
                name = name.strip()
                if name.startswith("usr/lib/jvm/") and "/bin/" not in name:
                    continue
                for owner in owners.split(","):
                    files.setdefault(owner.rsplit("/", 1)[-1], set()).add(name)
    return files


def ships_command(shipped, command):
    """True if 'command' is an executable in 'shipped'.

    Debian's alternatives system installs e.g. /bin/nc.openbsd and
    /usr/bin/kinit.mit and links the plain name at install time, and JDKs keep
    their tools under /usr/lib/jvm/<name>/bin; both count.
    """
    for path in shipped:
        directory, _, name = path.rpartition("/")
        directory += "/"
        if directory in BIN_DIRS or (directory.startswith("usr/lib/jvm/") and directory.endswith("/bin/")):
            if name == command or name.startswith(command + "."):
                return True
    return False


def provider_among_dependencies(package, providers, depth=3):
    """The first of 'providers' that 'package' pulls in (Depends, Pre-Depends, Recommends).

    Searched level by level, 'depth' levels down, stopping at the first hit.
    """
    seen, frontier = {package}, [package]
    for _ in range(depth):
        if not frontier:
            break
        output = subprocess.run(["apt-cache", "depends"] + frontier, capture_output=True, text=True, check=False)
        following = []
        following_kind = None
        for line in output.stdout.splitlines():
            kind, separator, target = line.strip().partition(": ")
            if not separator:
                # "    python3" under "Depends: <python3:any>": a provider of the
                # virtual package named on the line before.
                kind, target = following_kind, line.strip()
                if not line.startswith("    "):
                    continue
            else:
                kind = kind.lstrip("|")
                following_kind = kind
            if kind in ("Depends", "PreDepends", "Recommends") and target and not target.startswith("<"):
                target = target.partition(":")[0]
                if target not in seen:
                    seen.add(target)
                    following.append(target)
        for target in following:
            if target in providers:
                return target
        frontier = following
    return None


def find_contents(explicit):
    if explicit:
        return [p for p in explicit if os.path.isfile(p)]
    found = [p for p in ("/tmp/Contents-amd64.gz", "/tmp/Contents-all.gz") if os.path.isfile(p)]
    return found if len(found) == 2 else []


def find_component_indices(explicit):
    indices = {}
    for component in ("contrib", "non-free"):
        path = explicit.get(component) or f"/tmp/Packages-{component}-amd64.xz"
        if os.path.isfile(path):
            indices[component] = load_arch_index(path)
    return indices


def find_arm64_index(explicit):
    candidates = []
    if explicit.get("arm64"):
        candidates.append(explicit["arm64"])
    if os.environ.get("ARSENAL_ARM64_INDEX"):
        candidates.append(os.environ["ARSENAL_ARM64_INDEX"])
    candidates += ["/tmp/Packages-arm64", "/tmp/Packages-arm64.xz"]
    lists = "/var/lib/apt/lists"
    if os.path.isdir(lists):
        for name in sorted(os.listdir(lists)):
            if "binary-arm64_Packages" in name and not name.endswith(".diff_Index"):
                candidates.append(os.path.join(lists, name))
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--profiles-dir", default=am.PROFILES_DIR)
    parser.add_argument("--arch-index", action="append", default=[],
                        metavar="ARCH=PATH", help="package index for a foreign architecture")
    parser.add_argument("--contents", action="append", default=[], metavar="PATH",
                        help="Contents index (amd64 and all) to verify commands and desktop entries")
    parser.add_argument("--component-index", action="append", default=[], metavar="COMPONENT=PATH",
                        help="contrib / non-free package index, to verify why an external tool is external")
    parser.add_argument("--require-arm64", action="store_true", help="fail if no arm64 index is available")
    parser.add_argument("--require-contents", action="store_true", help="fail if no Contents index is available")
    parser.add_argument("--json", metavar="FILE", help="write a machine-readable report")
    args = parser.parse_args()

    explicit = {}
    for item in args.arch_index:
        arch, _, path = item.partition("=")
        explicit[arch] = path

    try:
        profiles = am.load_profiles(args.profiles_dir)
    except am.ManifestError as error:
        print(f"check-availability: {error}", file=sys.stderr)
        return 2

    # package -> set of architectures it is declared for, across all profiles.
    declared = {}
    for manifest in profiles.values():
        for tool in am.own_tools(manifest):
            declared.setdefault(tool["package"], set()).update(am.tool_architectures(tool))

    arm64_path = find_arm64_index(explicit)
    arm64_names = load_arch_index(arm64_path) if arm64_path else None

    problems = []
    report = {"native": NATIVE_ARCH, "arm64Index": arm64_path, "packages": {}}
    for package in sorted(declared):
        arches = declared[package]
        exists, in_main, arch, version = native_status(package)
        entry = {"architectures": sorted(arches), "version": version, "nativeArchitecture": arch,
                 "inMain": in_main, "checks": {}}
        if not exists:
            problems.append(f"{package}: not found by apt-cache (not in the enabled repositories)")
            entry["checks"]["exists"] = False
            report["packages"][package] = entry
            continue
        entry["checks"]["exists"] = True
        if not in_main:
            problems.append(f"{package}: present but not in component 'main' (Section without 'main/')")
        entry["checks"]["inMain"] = in_main

        is_all = arch == "all"
        # Native architecture must be usable when the tool is declared for it.
        if NATIVE_ARCH in arches and not (is_all or arch == NATIVE_ARCH):
            problems.append(f"{package}: declared for {NATIVE_ARCH} but built only for {arch}")

        for foreign in FOREIGN_ARCHES:
            wants = foreign in arches
            if is_all:
                available = True
            elif arm64_names is None:
                entry["checks"][foreign] = "skipped"
                if wants:
                    # Cannot confirm; make CI notice rather than silently pass.
                    problems.append(f"{package}: cannot verify {foreign} (no {foreign} package index available)")
                continue
            else:
                available = package in arm64_names
            entry["checks"][foreign] = available
            if wants and not available:
                problems.append(f"{package}: declared for {foreign} but no {foreign} binary in the index")
            if not wants and available:
                problems.append(f"{package}: available on {foreign} — remove it from 'architectures' "
                                f"(which currently excludes {foreign})")
        report["packages"][package] = entry

    # ---- commands and desktop entries ------------------------------------------------
    contents_paths = find_contents(args.contents)
    report["contents"] = contents_paths
    launch_checked = 0
    if contents_paths:
        files = load_contents(contents_paths)
        # A metapackage (keepassxc, bandit, build-essential, default-jdk ...) ships
        # nothing itself: accept what its dependencies ship, a few levels down.
        for manifest in profiles.values():
            for tool in am.own_tools(manifest):
                package = tool["package"]
                if not report["packages"].get(package, {}).get("checks", {}).get("exists"):
                    continue
                launch_checked += 1
                wanted = []
                if tool.get("desktop-id"):
                    path = DESKTOP_DIR + tool["desktop-id"] + ".desktop"
                    wanted.append(("desktop entry", tool["desktop-id"] + ".desktop",
                                   lambda shipped, path=path: path in shipped))
                if tool.get("command"):
                    wanted.append(("command", tool["command"],
                                   lambda shipped, command=tool["command"]: ships_command(shipped, command)))
                for kind, value, test in wanted:
                    if test(files.get(package, set())):
                        continue
                    providers = {name for name, shipped in files.items() if test(shipped)}
                    via = provider_among_dependencies(package, providers) if providers else None
                    if via:
                        report["packages"][package].setdefault("via", {})[value] = via
                    else:
                        problems.append(f"{manifest['id']}.json: {package}: {kind} '{value}' is not shipped "
                                        f"by the package or by what it depends on")
    elif args.require_contents:
        problems.append("no Contents index available (need Contents-amd64.gz and Contents-all.gz)")

    # ---- external tools must really be external ------------------------------------
    components = find_component_indices(dict(i.partition("=")[::2] for i in args.component_index))
    report["external"] = {}
    external_checked = 0
    for manifest in profiles.values():
        for entry in manifest.get("external", []):
            names = entry.get("packages", [])
            where = f"{manifest['id']}.json: external {entry['name']}"
            found_elsewhere = []
            for package in names:
                exists, in_main, _arch, version = native_status(package)
                if exists and in_main:
                    problems.append(f"{where}: package '{package}' ({version}) is in trixie main — "
                                    f"list it under 'tools' instead")
                elif exists:
                    found_elsewhere.append(package)
                for component, index in components.items():
                    if package in index and package not in found_elsewhere:
                        found_elsewhere.append(package)
                external_checked += 1
            reason = entry["why"]["reason"]
            if found_elsewhere and reason == "not-in-debian":
                problems.append(f"{where}: {', '.join(found_elsewhere)} exists outside main (contrib/non-free); "
                                f"the reason should be 'not-in-debian-main'")
            if reason == "not-in-debian-main" and not found_elsewhere and len(components) == 2:
                problems.append(f"{where}: reason is 'not-in-debian-main' but none of {names} is in "
                                f"contrib or non-free")
            report["external"][entry["name"]] = {"packages": names, "outsideMain": found_elsewhere}

    if args.require_arm64 and arm64_names is None:
        problems.append("no arm64 package index available")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)

    for message in problems:
        print(f"check-availability: {message}", file=sys.stderr)

    checked = len(declared)
    if arm64_names is None:
        print(f"check-availability: NOTE no arm64 package index found; arm64 availability was not verified "
              f"(looked for /tmp/Packages-arm64 and the apt lists)", file=sys.stderr)
    if problems:
        print(f"check-availability: {len(problems)} problem(s) across {checked} packages", file=sys.stderr)
        return 1
    print(f"check-availability: {checked} packages verified in trixie main"
          + (" (amd64 + arm64)" if arm64_names is not None else " (amd64; arm64 index not found)"))
    print(f"check-availability: {launch_checked} tool entries: command and desktop entry "
          + ("found in the package contents" if contents_paths else "NOT checked (no Contents index)"))
    print(f"check-availability: {external_checked} package names of external tools confirmed absent from main"
          + ("" if len(components) == 2 else " (contrib/non-free indices not found: reasons not cross-checked)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
