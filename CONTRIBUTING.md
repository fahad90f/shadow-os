# Contributing to Shadow OS

Thank you for helping. This page describes how work on Shadow OS is done:
the workflow, the rules for code, how to test, and what a change must
contain to be accepted.

Read first, depending on what you touch:

| You work on | Read |
|---|---|
| an application | [docs/applications/development.md](docs/applications/development.md) — the contract every SHADOW application follows |
| user interface | [docs/desktop/shadowui.md](docs/desktop/shadowui.md) — ShadowUI components, theme tokens, icons |
| the shell | [docs/desktop/shell-modules.md](docs/desktop/shell-modules.md) — shell modules; [docs/desktop/architecture.md](docs/desktop/architecture.md) |
| packaging, images | [docs/packaging/building.md](docs/packaging/building.md), [docs/packaging/iso.md](docs/packaging/iso.md) |
| the installer | [docs/installation/unattended.md](docs/installation/unattended.md) |
| security settings | [docs/security/hardening.md](docs/security/hardening.md) |
| Arsenal profiles | [arsenal/docs/adding-tools.md](arsenal/docs/adding-tools.md) |

The honest state of the project is in [docs/limitations.md](docs/limitations.md)
and [docs/testing.md](docs/testing.md). The most valuable contribution
right now is **testing on real hardware**: nothing has run on a physical
machine yet.

## Reporting problems and test results

Open an issue on the project's GitHub repository
(<https://github.com/fahad90fa/Custom-OS/issues>). Include:

* what you did, what you expected and what happened;
* the image you used (file name, SHA-256, or the commit you built from);
* the machine: `shadow-sysinfo --dump report`, or model and firmware mode;
* logs ([docs/troubleshooting/README.md](docs/troubleshooting/README.md)
  lists where they are). Review them first: they contain host names, user
  names and hardware serial numbers.

Reports of things that **work** on real hardware are as useful as bug
reports. Say exactly what you tried.

**Security vulnerabilities are not reported in public issues**: see
[SECURITY.md](SECURITY.md).

## Workflow

1. Fork the repository and create a branch from `main`.
2. Set up a build host and the build chroot
   ([docs/packaging/building.md](docs/packaging/building.md)):

   ```sh
   sudo scripts/host-setup.sh
   sudo build/chroot/create.sh --dev-tools
   ```

3. Make your change. Keep it to one topic.
4. Build and test it (below).
5. Update the documentation that describes what you changed — the
   component's `README.md`, and the pages under `docs/` if behaviour,
   commands, options or file locations changed.
6. Open a pull request. Describe what the change does, how you tested it
   (the exact commands and their results), and what you did not test.

A component that is not finished can be committed but kept out of package
builds by listing its path in `build/packages/wip.exclude`. Remove the line
when the component is integrated: it has a `debian/control` stanza, a
`README.md`, and its tests pass.

## Rules for code

The full rules for applications are in
[docs/applications/development.md](docs/applications/development.md),
section 2. In short, and for the whole repository:

* **Real functionality only.** No mock or hard-coded data, no invented
  scores, no controls that do nothing, no simulated actions. When a
  back-end is unavailable at run time, show an honest "unavailable" state
  with the reason.
* **Privileged and destructive actions** need explicit confirmation. Use an
  existing system service with its own polkit policy where one exists;
  otherwise a helper started through `pkexec` with an **argument vector**
  (never a shell string), a closed set of sub-commands and validated
  arguments. Secrets travel on standard input only. No GUI runs as root.
* **Never weaken a security mechanism to make something work.** If a
  hardening setting breaks your feature, document the exception instead
  ([docs/security/hardening.md](docs/security/hardening.md), "Documented
  exceptions") and get it reviewed.
* **No shell for external programs**; parse machine-readable output
  (`ip -j`, `lsblk -J`, `nmcli -t`, D-Bus) rather than human-oriented text.
* **Never block the UI thread** on I/O.
* **Design tokens only** in UI code: colours, sizes, fonts and durations
  come from `Theme`. Use ShadowUI components; a missing icon is added to the
  generator (`assets/src/icons/gen-symbolic.py`), not replaced by an
  unrelated one.
* **Accessible and keyboard-usable**: tooltips on icon-only buttons,
  `Accessible.name` on custom controls, everything reachable with Tab.
* User-visible strings go through `qsTr()` / `tr()`.
* **Original work.** Do not copy code or artwork from KDE, GNOME, Kali or
  other projects. Using their libraries and public protocols is fine.
* C++20 with the warnings and Qt strictness flags of
  `cmake/ShadowDefaults.cmake`; no new compiler warnings.
* Shell scripts: `set -euo pipefail` (or `set -eu` for POSIX `sh`), quote
  expansions, pass `shellcheck`.
* Python: standard library only for build tools, helpers and tests, unless a
  dependency is already used there.
* Generated files (theme files, icons, `arsenal/docs/profiles.md`,
  `arsenal/packaging/control.in`) are changed by changing their source and
  re-running the generator, never by hand.
* Each component edits only its own directory. A new application never
  needs a change to the top-level `CMakeLists.txt`: components are
  discovered.

### Distribution rules

* Only Debian 13 "trixie" as the base; no packages from other suites or
  distributions.
* Arsenal profiles list only packages from trixie `main`
  ([arsenal/docs/adding-tools.md](arsenal/docs/adding-tools.md)). No exploit
  code, payloads, malware samples, credentials or word lists in the
  repository.
* **Never commit private keys or other secrets.** Signing material lives
  outside the repository
  ([docs/packaging/repositories.md](docs/packaging/repositories.md)).
* `distro/base/shadow.conf` is the only place for the distribution's name,
  version and codename; `debian/changelog` must carry the same version.

## Licence headers

Shadow OS is licensed under GPL-3.0-or-later. Every new source file starts
with an SPDX line in the comment syntax of its language:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

```python
# SPDX-License-Identifier: GPL-3.0-or-later
```

```xml
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
```

For scripts, put it directly after the `#!` line. By contributing you agree
that your contribution is distributed under this licence. Third-party
material must be under a compatible licence and be recorded in
`debian/copyright`.

## Building and testing

### One component

```sh
sudo env SHADOW_JOBS=2 scripts/dev.sh build --dir <name> --only <component>
sudo scripts/dev.sh run --dir <name> --smoke --shot <name>.png <binary>
sudo scripts/dev.sh run --dir <name> <binary> --dump <topic>
```

`<component>` is the directory name (for example `monitor`,
`horizon-shell`); `--dir` names the build tree, one per component.

### Smoke tests

Required for every change to a GUI component. The smoke test starts each
binary listed in the component's `smoke-tests.list` headlessly and fails on
any QML warning or error:

```sh
sudo tests/desktop/smoke.sh --dir <name> <component>
sudo tests/desktop/smoke.sh --dir <name> --shots <component>    # also saves screenshots
```

It must end with `smoke: N passed, 0 failed`. A new application adds a
`smoke-tests.list` next to its sources, one `<binary> [args...]` per line —
ideally one line per page.

The smoke test runs without system services: it proves the UI loads and
that the "unavailable" states are sound, not that the back-ends work. Test
back-ends against the real service where you can (a private D-Bus bus in
the chroot, the nested session of `scripts/dev.sh nested`) and say in the
README what you used.

### Other tests

```sh
python3 tests/installation/test_plan.py          # installer planner; needs no root
python3 scripts/gen-tokens.py check              # design tokens
python3 arsenal/tools/validate.py                # Arsenal manifests
python3 desktop/themes/tools/generate.py --check # generated theme files are up to date
```

For changes to the build system, boot chain, installer engine or session,
build an image and run the QEMU tests that cover your change
([docs/testing.md](docs/testing.md)):

```sh
sudo build/packages/build.sh && sudo build/packages/repo.sh
sudo build/iso/build.sh --variant minimal
sudo tests/boot/run-matrix.sh build/out/iso/shadow-os-0.1.0-amd64-minimal.iso
sudo tests/installation/run-all.sh build/out/iso/shadow-os-0.1.0-amd64-minimal.iso
```

These take a long time without KVM. Run at least the scenarios your change
can affect and say which.

### Say what you tested

Every component README ends with what was tested and how, and what was
not. Keep that honest and current: **never state that something works
unless you ran it and saw it work.** "Implemented, not tested against X" is
an acceptable and useful statement. The same applies to pull-request
descriptions and to `docs/`.

## Commit style

* One logical change per commit.
* Subject line: imperative or descriptive, capitalised, no trailing full
  stop, about 70 characters at most. A component prefix is welcome when it
  helps: `Installer engine: refuse …`, `Boot branding: …`.
* Body, after a blank line, when the subject is not enough: what changed
  and **why**, wrapped at about 72 columns. Use a list when a commit
  integrates several things.
* Mention tests you ran if they are not obvious from the change.
* Do not commit build output (`build/work`, `build/out`), editor files or
  generated packages; `.gitignore` covers the usual ones.

## Documentation

* Document what exists. Do not describe planned features as if they were
  there; list them in [docs/limitations.md](docs/limitations.md) instead.
* Every command you print must exist and take the options you show.
* Verification statements ("tested", "verified") need a test that was
  actually run; say where and how.
* Plain, precise language; second person in guides; British spelling.

## Conduct

Be respectful and factual. Assume good faith, criticise code and not
people, and keep security-sensitive details out of public channels until
they are fixed.
