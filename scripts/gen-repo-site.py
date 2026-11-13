#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble the Shadow OS update site from a signed APT repository.

    scripts/gen-repo-site.py --repo build/out/repo --out public --url URL
                             [--build N] [--commit SHA] [--source-url URL]

The site is the repository itself (dists/, pool/, the archive key) plus:

    index.html                  what this build contains and how to update
    enable-updates.sh           switches an older installation to this repository
    shadow-archive-keyring.deb  the keyring package under a version-free name

It is static: any web server can host it. CI publishes it with GitHub Pages.
"""
import argparse
import datetime
import gzip
import html
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


def human_size(count):
    for unit in ("B", "KB", "MB"):
        if count < 1024 or unit == "MB":
            return f"{count:.0f} {unit}" if unit == "B" else f"{count:.1f} {unit}"
        count /= 1024
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

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@NAME@ Updates</title>
<meta name="description" content="Package repository and update status of @NAME@.">
<style>
:root {
  --bg: #06080C; --surface: #0B0F16; --raised: #111722; --border: #242D40; --border-subtle: #1A2130;
  --text: #E9EEF8; --text-2: #A3B0C6; --muted: #6B7892; --accent: #22D8F5; --on-accent: #03131A;
  --ok: #2EE6A6;
  --ui: "Inter Variable", Inter, "Noto Sans", system-ui, sans-serif;
  --mono: "JetBrains Mono", "Noto Sans Mono", ui-monospace, monospace;
}
@media (prefers-color-scheme: light) {
  :root {
    --bg: #F3F5F9; --surface: #EBEEF4; --raised: #FFFFFF; --border: #CFD6E2; --border-subtle: #E1E6EE;
    --text: #0A1020; --text-2: #45526A; --muted: #6E7A91; --accent: #067C99; --on-accent: #FFFFFF;
    --ok: #0E9F6E;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.6 var(--ui); }
main { max-width: 60rem; margin: 0 auto; padding: 0 1.25rem 4rem; }
a { color: var(--accent); }
code, pre, .mono { font-family: var(--mono); }
header { display: flex; flex-wrap: wrap; align-items: baseline; gap: .5rem 1rem; padding: 2rem 0 0; }
.brand { font-weight: 700; letter-spacing: .14em; text-transform: uppercase; font-size: .8125rem; }
.brand span { color: var(--accent); }
.channel { color: var(--muted); font-size: .8125rem; }
h1 { font-size: clamp(1.9rem, 5vw, 2.75rem); line-height: 1.1; letter-spacing: -.02em; margin: 3.5rem 0 .75rem; text-wrap: balance; }
.lede { color: var(--text-2); max-width: 38rem; margin: 0 0 1.75rem; }
h2 { font-size: 1.125rem; margin: 3.5rem 0 .5rem; }
h2 + p { color: var(--text-2); max-width: 42rem; margin: 0 0 1rem; }
.term { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; overflow: hidden; }
.term-bar { display: flex; justify-content: space-between; align-items: center; padding: .4rem .5rem .4rem .9rem;
  border-bottom: 1px solid var(--border-subtle); color: var(--muted); font-size: .75rem; }
.term pre { margin: 0; padding: 1rem .9rem; overflow-x: auto; font-size: .9375rem; line-height: 1.7; }
.term pre .p { color: var(--accent); user-select: none; }
button.copy { font: 500 .75rem var(--ui); color: var(--text-2); background: var(--raised); border: 1px solid var(--border);
  border-radius: 6px; padding: .25rem .6rem; cursor: pointer; }
button.copy:hover { color: var(--text); border-color: var(--accent); }
button.copy:focus-visible, input:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.facts { display: grid; grid-template-columns: repeat(auto-fit, minmax(9.5rem, 1fr)); gap: 1px; margin: 1.75rem 0 0;
  background: var(--border-subtle); border: 1px solid var(--border-subtle); border-radius: 10px; overflow: hidden; }
.facts div { background: var(--bg); padding: .8rem 1rem; }
.facts dt { color: var(--muted); font-size: .75rem; }
.facts dd { margin: .1rem 0 0; font-family: var(--mono); font-size: .875rem; overflow-wrap: anywhere; }
.live::before { content: ""; display: inline-block; width: .5rem; height: .5rem; border-radius: 50%;
  background: var(--ok); margin-right: .45rem; }
ol.steps { margin: 0; padding-left: 1.2rem; color: var(--text-2); max-width: 42rem; }
ol.steps li { margin: .35rem 0; }
.filter { display: flex; justify-content: space-between; align-items: center; gap: 1rem; flex-wrap: wrap; margin: 0 0 .75rem; }
.filter span { color: var(--muted); font-size: .8125rem; }
input[type=search] { font: inherit; font-size: .875rem; color: var(--text); background: var(--surface);
  border: 1px solid var(--border); border-radius: 6px; padding: .35rem .6rem; width: min(100%, 16rem); }
.table { overflow-x: auto; border: 1px solid var(--border-subtle); border-radius: 10px; }
table { border-collapse: collapse; width: 100%; font-size: .875rem; }
th { text-align: left; font-weight: 500; color: var(--muted); font-size: .75rem; padding: .55rem .9rem;
  border-bottom: 1px solid var(--border-subtle); white-space: nowrap; }
td { padding: .5rem .9rem; border-bottom: 1px solid var(--border-subtle); vertical-align: top; }
tr:last-child td { border-bottom: 0; }
td.n { font-family: var(--mono); white-space: nowrap; }
td.v { font-family: var(--mono); color: var(--text-2); white-space: nowrap; }
td.s { color: var(--text-2); }
td.z { color: var(--muted); white-space: nowrap; text-align: right; font-variant-numeric: tabular-nums; }
th.z { text-align: right; }
@media (max-width: 40rem) {
  thead { display: none; }
  tr { display: grid; grid-template-columns: 1fr auto; gap: .1rem .75rem; padding: .6rem .9rem;
    border-bottom: 1px solid var(--border-subtle); }
  tr:last-child { border-bottom: 0; }
  tr[hidden] { display: none; }
  td { padding: 0; border: 0; }
  td.n { white-space: normal; overflow-wrap: anywhere; }
  td.v, td.s { grid-column: 1 / -1; white-space: normal; font-size: .8125rem; }
  td.z { grid-row: 1; grid-column: 2; }
}
.fpr { font-family: var(--mono); font-size: .875rem; overflow-wrap: anywhere; background: var(--surface);
  border: 1px solid var(--border-subtle); border-radius: 6px; padding: .6rem .8rem; display: inline-block; }
footer { margin-top: 4rem; padding-top: 1.25rem; border-top: 1px solid var(--border-subtle); color: var(--muted); font-size: .8125rem; }
</style>
</head>
<body>
<main>
<header>
  <div class="brand">@NAME_UPPER@ <span>Updates</span></div>
  <div class="channel">@VERSION@ "@CODENAME@" · channel @CHANNEL@</div>
</header>

<h1>One command keeps @NAME@ current.</h1>
<p class="lede">Every change pushed to the source is built, tested and published here.
A system installed from a current image picks it up with APT, together with Debian's own updates.</p>

<div class="term">
  <div class="term-bar"><span>terminal</span><button class="copy" data-copy="sudo apt update &amp;&amp; sudo apt upgrade">Copy</button></div>
  <pre><span class="p">$ </span>sudo apt update &amp;&amp; sudo apt upgrade</pre>
</div>

<dl class="facts">
  <div><dt>Latest build</dt><dd class="live">@BUILD@</dd></div>
  <div><dt>Published (UTC)</dt><dd>@DATE@</dd></div>
  <div><dt>Source commit</dt><dd>@COMMIT@</dd></div>
  <div><dt>Packages</dt><dd>@COUNT@ · @ARCHES@</dd></div>
</dl>

<h2>Packages in this build</h2>
<p>Installed packages older than these are upgraded. Debian packages come from Debian's servers, not from here.</p>
<div class="filter">
  <span id="shown">@COUNT@ packages</span>
  <input type="search" id="q" placeholder="Filter packages" aria-label="Filter packages">
</div>
<div class="table">
<table>
<thead><tr><th>Package</th><th>Version</th><th>What it is</th><th class="z">Download</th></tr></thead>
<tbody id="rows">
@ROWS@
</tbody>
</table>
</div>

<h2>Installed from an older image?</h2>
<p>An image built before this site existed has its Shadow OS source switched off and trusts a different key.
Switch it over once; after that the command above is all it needs.</p>
<div class="term">
  <div class="term-bar"><span>terminal</span><button class="copy" data-copy="curl -fsSLO @URL@/enable-updates.sh &amp;&amp; sudo sh enable-updates.sh">Copy</button></div>
  <pre><span class="p">$ </span>curl -fsSLO @URL@/enable-updates.sh
<span class="p">$ </span>sudo sh enable-updates.sh</pre>
</div>
<ol class="steps">
  <li>Installs the current <code>shadow-archive-keyring</code> package: the repository key and the APT source for this site.</li>
  <li>Turns off the package snapshot that shipped inside the old image, because it is signed with the old key.</li>
  <li>Refreshes the package lists. Read <a href="enable-updates.sh">the script</a> first if you like; it is short.</li>
</ol>

@KEY_SECTION@

<footer>
  This site is the APT repository itself (<a href="dists/@CODENAME@/InRelease">signed index</a>, <code>pool/</code>).
  @SOURCE_LINK@
</footer>
</main>
<script>
for (const button of document.querySelectorAll("button.copy")) {
  button.addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(button.dataset.copy); button.textContent = "Copied"; }
    catch (error) { button.textContent = "Select and copy"; }
    setTimeout(() => { button.textContent = "Copy"; }, 1600);
  });
}
const rows = [...document.querySelectorAll("#rows tr")];
document.getElementById("q").addEventListener("input", (event) => {
  const needle = event.target.value.trim().toLowerCase();
  let shown = 0;
  for (const row of rows) {
    const hit = row.textContent.toLowerCase().includes(needle);
    row.hidden = !hit;
    if (hit) shown += 1;
  }
  document.getElementById("shown").textContent = shown + (shown === 1 ? " package" : " packages");
});
</script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", required=True, help="repository directory (dists/, pool/, the archive key)")
    parser.add_argument("--out", required=True, help="directory to write the site to")
    parser.add_argument("--url", required=True, help="public base URL of the site")
    parser.add_argument("--build", default="", help="build number of the packages")
    parser.add_argument("--commit", default="", help="source commit the packages were built from")
    parser.add_argument("--source-url", default="", help="URL of the source repository")
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

    # One row per package; an architecture-specific package is listed once.
    by_name = {}
    for (name, arch), fields in sorted(visible.items()):
        by_name.setdefault(name, fields)
    rows = []
    for name, fields in by_name.items():
        summary = fields.get("Description", "").split("\n")[0]
        rows.append(
            "<tr><td class=\"n\">{}</td><td class=\"v\">{}</td><td class=\"s\">{}</td>"
            "<td class=\"z\"><a href=\"{}\">{}</a></td></tr>".format(
                html.escape(name), html.escape(fields.get("Version", "")), html.escape(summary),
                html.escape(fields.get("Filename", ""), quote=True), human_size(int(fields.get("Size", "0")))))

    versions = sorted({fields.get("Version", "") for fields in by_name.values()})
    build = versions[-1] if len(versions) == 1 else (f"build {args.build}" if args.build else "mixed versions")
    arches = sorted({arch for (_, arch) in visible if arch != "all"}) or ["all"]
    commit = html.escape(args.commit[:10]) if args.commit else "not recorded"
    source = args.source_url.rstrip("/")
    if source and args.commit:
        commit = '<a href="{}/commit/{}">{}</a>'.format(html.escape(source, quote=True), html.escape(args.commit, quote=True), commit)

    fpr = fingerprint(keyring)
    key_section = ""
    if fpr:
        grouped = " ".join(fpr[i:i + 4] for i in range(0, len(fpr), 4))
        key_section = (
            "<h2>Signing key</h2>\n"
            "<p>APT accepts packages only when the index is signed by this key. "
            "It is installed as <code>/usr/share/keyrings/shadow-archive-keyring.gpg</code> "
            "(<a href=\"shadow-archive-keyring.gpg\">download</a>).</p>\n"
            f"<div class=\"fpr\">{html.escape(grouped)}</div>")

    page = PAGE
    for token, value in {
        "@NAME_UPPER@": html.escape(conf["SHADOW_NAME"].upper()),
        "@NAME@": html.escape(conf["SHADOW_NAME"]),
        "@VERSION@": html.escape(conf["SHADOW_VERSION"]),
        "@CODENAME@": html.escape(codename),
        "@CHANNEL@": html.escape(conf.get("SHADOW_CHANNEL", "")),
        "@BUILD@": html.escape(build),
        "@DATE@": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M"),
        "@COMMIT@": commit,
        "@COUNT@": str(len(by_name)),
        "@ARCHES@": html.escape(", ".join(arches)),
        "@ROWS@": "\n".join(rows),
        "@URL@": html.escape(url, quote=True),
        "@KEY_SECTION@": key_section,
        "@SOURCE_LINK@": 'Source: <a href="{0}">{0}</a>.'.format(html.escape(source, quote=True)) if source else "",
    }.items():
        page = page.replace(token, value)
    with open(os.path.join(args.out, "index.html"), "w", encoding="utf-8") as handle:
        handle.write(page)
    print(f"gen-repo-site: {len(by_name)} packages ({build}) → {args.out}")


if __name__ == "__main__":
    sys.exit(main())
