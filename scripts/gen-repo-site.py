#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the Shadow OS package repository for publishing.

    scripts/gen-repo-site.py --repo build/out/repo --out public --url URL
                             [--build N] [--commit SHA] [--source-url URL]
                             [--site-url URL]

Writes the repository itself (dists/, pool/, the archive key) to --out, plus:

    repo.json                   the published build and its packages, read by
                                the website (website/, a React app)
    enable-updates.sh           switches an older installation to this repository
    shadow-archive-keyring.deb  the keyring package under a version-free name
    index.html                  one line saying what this address is

The result is static files: any web server can host them. CI publishes them
with GitHub Pages.
"""
import argparse
import datetime
import gzip
import html
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_conf():
    conf = {}
    with open(os.path.join(ROOT, "distro", "base", "shadow.conf"), encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r'^(SHADOW_[A-Z_]+)="([^"]*)"', line)
            if match:
                conf[match.group(1)] = match.group(2)
    return conf


def parse_packages(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as handle:
        text = handle.read()
    for stanza in text.split("\n\n"):
        fields, key = {}, None
        for line in stanza.splitlines():
            if line[:1] in (" ", "\t"):
                if key:
                    fields[key] += "\n" + line.strip()
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
        if "Package" in fields:
            yield fields


def collect(repo, codename):
    """Packages of the suite: {(name, arch): fields + Component}."""
    found = {}
    dist = os.path.join(repo, "dists", codename)
    if not os.path.isdir(dist):
        sys.exit(f"gen-repo-site: no suite '{codename}' in {repo}")
    for component in sorted(os.listdir(dist)):
        for arch_dir in sorted(os.listdir(os.path.join(dist, component)) if os.path.isdir(os.path.join(dist, component)) else []):
            if not arch_dir.startswith("binary-"):
                continue
            base = os.path.join(dist, component, arch_dir, "Packages")
            path = base if os.path.isfile(base) else base + ".gz"
            if not os.path.isfile(path):
                continue
            for fields in parse_packages(path):
                fields["Component"] = component
                found[(fields["Package"], arch_dir[len("binary-"):])] = fields
    return found


def fingerprint(keyring):
    try:
        out = subprocess.run(["gpg", "--batch", "--show-keys", "--with-colons", keyring],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return ""
    for line in out.splitlines():
        if line.startswith("fpr:"):
            return line.split(":")[9]
    return ""


ENABLE_SCRIPT = """#!/bin/sh
# Shadow OS — point an existing installation at the hosted package repository.
#
# Needed once, and only on a system installed from an image that was built
# before this repository existed: its Shadow OS APT source is disabled and it
# trusts the key of that image build. Systems installed from a current image
# already use this repository.
#
#   sudo sh enable-updates.sh
set -eu
URL="@URL@"

[ "$(id -u)" -eq 0 ] || { echo "Run as root: sudo sh enable-updates.sh" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fetch() {
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"; else wget -qO "$2" "$1"; fi
}
fetch "$URL/shadow-archive-keyring.deb" "$tmp/keyring.deb"
fetch "$URL/shadow-archive-keyring.gpg" "$tmp/keyring.gpg"

# The package snapshot that came with the image is signed with the key this
# system trusted until now. Once the key changes APT can no longer verify it.
snapshot=/etc/apt/sources.list.d/shadow-local.sources
if [ -e "$snapshot" ] && ! cmp -s /usr/share/keyrings/shadow-archive-keyring.gpg "$tmp/keyring.gpg"; then
    mv "$snapshot" "$snapshot.disabled"
    echo "Disabled the package snapshot of the old image ($snapshot.disabled)."
fi

dpkg --force-confnew -i "$tmp/keyring.deb"
apt-get update || echo "apt-get update reported problems; see the messages above." >&2
echo
echo "This system now follows $URL"
echo "Update it with:  sudo apt update && sudo apt upgrade"
"""

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", required=True, help="repository directory (dists/, pool/, the archive key)")
    parser.add_argument("--out", required=True, help="directory to write the site to")
    parser.add_argument("--url", required=True, help="public base URL of the site")
    parser.add_argument("--build", default="", help="build number of the packages")
    parser.add_argument("--commit", default="", help="source commit the packages were built from")
    parser.add_argument("--source-url", default="", help="URL of the source repository")
    parser.add_argument("--site-url", default="", help="URL of the website, linked from index.html")
    args = parser.parse_args()

    conf = read_conf()
    codename = conf["SHADOW_CODENAME"]
    url = args.url.rstrip("/")
    keyring = os.path.join(args.repo, "shadow-archive-keyring.gpg")
    if not os.path.isfile(keyring):
        sys.exit(f"gen-repo-site: {keyring} is missing")

    packages = collect(args.repo, codename)
    visible = {key: value for key, value in packages.items() if not key[0].endswith("-dbgsym")}
    if not visible:
        sys.exit("gen-repo-site: the repository lists no packages")

    os.makedirs(args.out, exist_ok=True)
    for name in ("dists", "pool"):
        target = os.path.join(args.out, name)
        if os.path.exists(target):
            shutil.rmtree(target)
        shutil.copytree(os.path.join(args.repo, name), target)
    shutil.copy(keyring, os.path.join(args.out, "shadow-archive-keyring.gpg"))

    keyring_deb = [value for key, value in packages.items() if key[0] == "shadow-archive-keyring"]
    if not keyring_deb:
        sys.exit("gen-repo-site: shadow-archive-keyring is not in the repository")
    shutil.copy(os.path.join(args.repo, keyring_deb[0]["Filename"]), os.path.join(args.out, "shadow-archive-keyring.deb"))

    with open(os.path.join(args.out, "enable-updates.sh"), "w", encoding="utf-8") as handle:
        handle.write(ENABLE_SCRIPT.replace("@URL@", url))
    open(os.path.join(args.out, ".nojekyll"), "w").close()

    # One entry per package; an architecture-specific package is listed once.
    by_name = {}
    for (name, arch), fields in sorted(visible.items()):
        by_name.setdefault(name, fields)
    versions = sorted({fields.get("Version", "") for fields in by_name.values()})
    build = versions[-1] if len(versions) == 1 else (f"build {args.build}" if args.build else "mixed versions")

    manifest = {
        "schema": 1,
        "name": conf["SHADOW_NAME"],
        "version": conf["SHADOW_VERSION"],
        "codename": codename,
        "channel": conf.get("SHADOW_CHANNEL", ""),
        "build": build,
        "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": args.commit,
        "sourceUrl": args.source_url.rstrip("/"),
        "fingerprint": fingerprint(keyring),
        "architectures": sorted({arch for (_, arch) in visible if arch != "all"}) or ["all"],
        "packages": [{
            "name": name,
            "version": fields.get("Version", ""),
            "summary": fields.get("Description", "").split("\n")[0],
            "size": int(fields.get("Size", "0")),
            "filename": fields.get("Filename", ""),
            "component": fields["Component"],
            "architecture": fields.get("Architecture", ""),
        } for name, fields in by_name.items()],
    }
    with open(os.path.join(args.out, "repo.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=1)
        handle.write("\n")

    site = args.site_url.rstrip("/")
    pointer = ' See <a href="{0}">{0}</a>.'.format(html.escape(site, quote=True)) if site else ""
    with open(os.path.join(args.out, "index.html"), "w", encoding="utf-8") as handle:
        handle.write("<!doctype html>\n<meta charset=\"utf-8\">\n<title>{0} package repository</title>\n"
                     "<p>{0} package repository. APT reads it; it is not meant to be browsed.{1}</p>\n".format(
                         html.escape(conf["SHADOW_NAME"]), pointer))
    print(f"gen-repo-site: {len(by_name)} packages ({build}) → {args.out}")


if __name__ == "__main__":
    sys.exit(main())
