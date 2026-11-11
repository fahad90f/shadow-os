# SHADOW Arsenal

Security tool profiles for Shadow OS: curated sets of Debian packages for
authorised testing, research, defence and forensics, installed **on demand**
through the package manager — never by default — and a manager application to
browse, install and remove them.

```
arsenal/
├── profiles/     the manifests: one JSON file per profile, categories.json, schema.json, sizes.json
├── tools/        validate.py, check-availability.py, profile-size.py, gen-control.py, gen-profiles-doc.py
├── packaging/    control.in (generated) and install rules for debian/
├── manager/      the application shadow-arsenal (org.shadow.arsenal) — see manager/README.md
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
  instructions. They are shown as "not installable from SHADOW repositories";
  nothing downloads or repackages them.
* `complete` is the union of the other profiles by reference (`includes`).

## Profiles

| Profile | Tools (trixie main) | External entries |
|---|---:|---:|
| core | 27 | 1 |
| web | 19 | 18 |
| network | 36 | 13 |
| redteam | 35 | 26 |
| blueteam | 25 | 12 |
| forensics | 22 | 6 |
| reversing | 27 (one of them amd64 only) | 16 |
| developer | 57 | 17 |
| complete | 230 distinct, by reference | 100 distinct |

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

* `shadow-arsenal` (manager): `shadow-ui`, `shadow-arsenal-data`, `dpkg`, `apt`,
  `libpackagekitqt6-2` (via shlibs), Qt 6 QML modules as for every SHADOW
  application; recommended `packagekit`, `polkitd` (with a polkit agent in the
  session), `shadow-terminal`.
* `shadow-arsenal-data`: none.
* `shadow-arsenal-<id>`: `shadow-arsenal-data`; **Depends** on the tools (Shadow
  OS installs every tool of a profile, so they are hard dependencies).
* The scripts in `tools/` are build-time only: `python3`, `apt`, `dpkg`.

## Packaging

See [packaging/README.md](packaging/README.md): which package owns what, why
metapackages are `Architecture: all` unless a tool is architecture-specific,
why tools are Recommends and not Depends, and what `debian/` has to do.

## State and limitations

Tested (build chroot, amd64, Debian trixie; details in manager/README.md):

* `validate.py`: 9 profiles, 230 packages, 16 categories, valid.
* `check-availability.py --require-arm64 --require-contents`: 230 packages in
  main on amd64 and arm64; 248 tool entries with their command and desktop
  entry found in the Contents indices.
* `profile-size.py`: all nine profiles resolve with `apt-get --simulate`; the
  `complete` profile pulls in 1204 packages (≈1.3 GB download, ≈5.4 GB
  installed) in the minimal chroot.
* Manager: builds, smoke test passes without QML warnings, dumps match
  `dpkg-query`, first-start acknowledgement, navigation, tool details, the apt
  fallback, and starting tools in SHADOW Terminal exercised in a nested session.

Not tested / known limits:

* **PackageKit transactions** (resolve, simulate, install, remove, progress,
  cancellation, polkit prompt, error paths): the chroot has no system bus. The
  code is exercised only up to "PackageKit is not available".
* The `shadow-arsenal-*` metapackages do not exist in any repository yet, so
  "install profile" through the metapackage and "remove profile" are untested;
  `control.in` has not been run through `dpkg-gencontrol`.
* Only one external entry (Nikto) carries the `packages` field that lets
  `check-availability.py` prove it is absent from main. The other 99 distinct
  external tools were verified absent from trixie main by `apt-cache` lookup on
  2026-11-11 (see the generator notes); adding the `packages` field to them is
  open but optional.
* The tool set was expanded on 2026-11-11 to cover the full 18-category tool
  list the distribution aims to bundle; every package was re-verified in main.
* arm64 was checked against the package index only; nothing was run on arm64.
