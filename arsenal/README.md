# NEXUS Arsenal

Security tool profiles for NEXUS OS: curated sets of Debian packages for
authorised testing, research, defence and forensics, installed **on demand**
through the package manager — never by default — and a manager application to
browse, install and remove them.

```
arsenal/
├── profiles/     the manifests: one JSON file per profile, categories.json, schema.json, sizes.json
├── tools/        validate.py, check-availability.py, profile-size.py, gen-control.py, gen-profiles-doc.py
├── packaging/    control.in (generated) and install rules for debian/
├── manager/      the application nexus-arsenal (org.nexus.arsenal) — see manager/README.md
└── docs/         legal-use.md, lab-setup.md, profiles.md (generated), adding-tools.md
```

## Rules the profiles follow

* Every package is in Debian 13 "trixie" **main** — no contrib/non-free, no
  third-party repositories. `tools/check-availability.py` verifies this for
  amd64 and arm64 and fails on anything it cannot find.
* The repository contains manifests only: no exploit code, payloads, malware
  samples, credentials or word lists.
* Well-known tools that are not in Debian main are listed per profile under
  `external` with their licence, the reason and the vendor's official
  instructions. They are shown as "not installable from NEXUS repositories";
  nothing downloads or repackages them.
* `complete` is the union of the other profiles by reference (`includes`).

## Profiles

| Profile | Tools (trixie main) | External entries |
|---|---:|---:|
| core | 15 | 1 |
| web | 19 | 4 |
| network | 26 | 0 |
| redteam | 25 | 6 |
| blueteam | 20 | 4 |
| forensics | 22 | 1 |
| reversing | 23 (one of them amd64 only) | 3 |
| developer | 25 | 0 |
| complete | 159 distinct, by reference | 18 distinct |

Contents: [docs/profiles.md](docs/profiles.md). How to change them:
[docs/adding-tools.md](docs/adding-tools.md).

## Tools (Python 3 standard library only)

| Script | Purpose | Needs |
|---|---|---|
| `tools/validate.py` | Manifests against `schema.json` plus cross-file rules | nothing |
| `tools/check-availability.py` | Every package in trixie main on amd64 + arm64; `command` / `desktop-id` shipped by the package or its dependencies; external tools absent from main | build chroot, indices in `/tmp` |
| `tools/profile-size.py` | Download / installed size per profile via `apt-get install --simulate` | build chroot |
| `tools/gen-control.py` | `packaging/control.in` | nothing |
| `tools/gen-profiles-doc.py` | `docs/profiles.md` | nothing |

Exact commands: [docs/adding-tools.md](docs/adding-tools.md), section *Verify*.

## Run-time package dependencies

* `nexus-arsenal` (manager): `nexus-ui`, `nexus-arsenal-data`, `dpkg`, `apt`,
  `libpackagekitqt6-2` (via shlibs), Qt 6 QML modules as for every NEXUS
  application; recommended `packagekit`, `polkitd` (with a polkit agent in the
  session), `nexus-terminal`.
* `nexus-arsenal-data`: none.
* `nexus-arsenal-<id>`: `nexus-arsenal-data`; Recommends the tools.
* The scripts in `tools/` are build-time only: `python3`, `apt`, `dpkg`.

## Packaging

See [packaging/README.md](packaging/README.md): which package owns what, why
metapackages are `Architecture: all` unless a tool is architecture-specific,
why tools are Recommends and not Depends, and what `debian/` has to do.

## State and limitations

Tested (build chroot, amd64, Debian trixie; details in manager/README.md):

* `validate.py`: 9 profiles, 159 packages, valid.
* `check-availability.py --require-arm64 --require-contents`: 159 packages in
  main on amd64 and arm64; 175 tool entries with their command and desktop
  entry found in the Contents indices.
* `profile-size.py`: all nine profiles resolve with `apt-get --simulate`.
* Manager: builds, smoke test passes without QML warnings, dumps match
  `dpkg-query`, first-start acknowledgement, navigation, tool details, the apt
  fallback, and starting tools in NEXUS Terminal exercised in a nested session.

Not tested / known limits:

* **PackageKit transactions** (resolve, simulate, install, remove, progress,
  cancellation, polkit prompt, error paths): the chroot has no system bus. The
  code is exercised only up to "PackageKit is not available".
* The `nexus-arsenal-*` metapackages do not exist in any repository yet, so
  "install profile" through the metapackage and "remove profile" are untested;
  `control.in` has not been run through `dpkg-gencontrol`.
* Only one external entry (Nikto) carries the `packages` field that lets
  `check-availability.py` prove it is absent from main. The other seventeen
  were looked up by hand on 2026-11-11 (none found in trixie main); adding the
  field to them is open.
* The tool selection is the previous engineer's and was verified, not
  extended.
* arm64 was checked against the package index only; nothing was run on arm64.
