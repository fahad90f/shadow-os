# Adding or changing tools in the Arsenal profiles

The profiles are data: `arsenal/profiles/<id>.json`, one file per profile, in
the format described by `arsenal/profiles/schema.json`. The manager, the
metapackages (`packaging/control.in`) and `docs/profiles.md` are all derived
from them.

## What may be listed

A tool goes into `tools` only if **all** of this is true:

1. It is a binary package in Debian 13 "trixie", component **`main`**. Not
   `contrib`, not `non-free`, not backports, and never a package from Kali,
   Parrot, Ubuntu, BlackArch or a vendor repository.
2. It exists for every architecture NEXUS OS is built for (amd64, arm64) — or
   the entry says which with `"architectures"`.
3. It is useful to the audience of the profile and still maintained well
   enough to be in a stable release. Prefer one good tool over three that do
   the same.
4. The package is the program. No package whose main content is exploit code,
   payloads, malware samples, credentials or word lists.

A well-known tool that fails rule 1 goes into `external` instead: the manager
lists it as "not installable from NEXUS repositories" with a link to the
vendor's instructions. Nothing is downloaded or repackaged, and `how.text`
must not be a command that pipes a download into a shell.

## A tool entry

```json
{
  "package": "tcpdump",
  "name": "tcpdump",
  "summary": "Command-line packet capture and analysis",
  "category": "sniffing-spoofing",
  "interface": "cli",
  "command": "tcpdump",
  "homepage": "https://www.tcpdump.org"
}
```

| Field | Rule |
|---|---|
| `package` | Debian binary package name. |
| `name` | The name people know the tool by. |
| `summary` | One line, at most 100 characters, no full stop. Write it yourself after reading `apt-cache show <package>`; do not paste the Debian description. |
| `category` | An id from `categories.json`. The same package may be filed under a different category in another profile. |
| `interface` | `cli`, `gui` or `both`. |
| `command` | Required for `cli`/`both`: the executable the user runs. It must be shipped by the package or by something it depends on. |
| `help` | Optional argument vector that prints the usage; default `["--help"]`. Use `[]` for programs that print it when run without arguments. |
| `desktop-id` | Required for `gui`/`both`: the desktop entry without `.desktop`. |
| `homepage` | Upstream site. |
| `architectures` | Only when the package is missing on one architecture, e.g. `["amd64"]`. |
| `notes` | Optional: something the user should know before installing (starts a service, needs a kernel module, needs root). |

A package listed by several profiles must have identical `name`, `summary`,
`interface`, `homepage`, `architectures`, `command`, `help` and `desktop-id`
everywhere. The `complete` profile only has `includes`; never add tools to it.

## An external entry

`name`, `summary`, `category`, `homepage`, `license` (including anything that
restricts redistribution or use), `why` (`reason` + one or two sentences),
`how` (`text` + the official URL), and — recommended — `packages`: the Debian
package names the tool would have. `check-availability.py` fails when one of
them appears in `main`, which is the signal to move the tool to `tools`.

`why.reason` is one of `not-in-debian`, `not-in-debian-main` (it is in contrib
or non-free), `proprietary`, `vendor-terms`.

## Verify

All commands run in the build chroot, where the trixie package lists are
available. Fetch the indices once (they are looked for in `/tmp`):

```
scripts/dev.sh exec --dir arsenal -- sh -c '
  cd /tmp && M=http://deb.debian.org/debian/dists/trixie &&
  curl -fsS $M/main/binary-arm64/Packages.xz | xz -dc > Packages-arm64 &&
  curl -fsS -O $M/main/Contents-amd64.gz -O $M/main/Contents-all.gz &&
  curl -fsS -o Packages-contrib-amd64.xz  $M/contrib/binary-amd64/Packages.xz &&
  curl -fsS -o Packages-non-free-amd64.xz $M/non-free/binary-amd64/Packages.xz'
```

Then:

```
# 1. Look at the package you want to add
scripts/dev.sh exec --dir arsenal -- apt-cache policy <package>
scripts/dev.sh exec --dir arsenal -- apt-cache show <package>

# 2. Format and cross-file rules (needs no chroot)
python3 arsenal/tools/validate.py

# 3. Every package in trixie main, on amd64 and arm64; commands and desktop
#    entries really shipped; external tools really external
scripts/dev.sh exec --dir arsenal -- \
    python3 /src/arsenal/tools/check-availability.py --require-arm64 --require-contents

# 4. Sizes (apt-get --simulate; nothing is installed)
scripts/dev.sh exec --dir arsenal -- python3 /src/arsenal/tools/profile-size.py --json /tmp/sizes.json
cp build/work/chroot-build-amd64/tmp/sizes.json arsenal/profiles/sizes.json

# 5. Regenerate what is derived from the manifests
python3 arsenal/tools/gen-control.py
python3 arsenal/tools/gen-profiles-doc.py
```

`gen-control.py --check` and `gen-profiles-doc.py --check` fail when the
generated files are stale; run them in CI together with steps 2 and 3.

Then merge `arsenal/packaging/control.in` into `debian/control`, and look at
the result in the manager:

```
NEXUS_JOBS=2 scripts/dev.sh build --dir arsenal --only manager
scripts/dev.sh run --dir arsenal nexus-arsenal --profiles-dir /src/arsenal/profiles --dump profiles
tests/desktop/smoke.sh --dir arsenal manager
```

## Adding a profile

Create `profiles/<id>.json` (`id` lower-case letters and digits,
`metapackage` = `nexus-arsenal-<id>`, `icon` = an icon in
`assets/icons/categories` or `assets/icons/apps`), add the id to
`complete.json` `includes` and to `PROFILE_ORDER` in
`tools/arsenal_manifest.py` (and `kOrder` in `manager/manifest.cpp`) if it
should not sort last, then run the steps above.

## Adding a category

Add it to `profiles/categories.json`; `icon` must be a glyph of the NexusUI
icon set (`desktop/ui/icons`).
