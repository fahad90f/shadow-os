# Package repositories

Where the packages of a NEXUS OS system come from, how the NEXUS repository
is produced and signed, and what exists today.

**State in one paragraph:** Debian's archive and security archive are
enabled and are the only network sources. **No hosted NEXUS repository
exists.** The NEXUS packages of a release are published into a signed
repository on the build machine; the image is installed from it, and the
desktop image carries a copy of it on disk. The key used so far is a
development key generated on the build machine.

## APT sources on a NEXUS OS system

| File | Source | State |
|---|---|---|
| `/etc/apt/sources.list.d/debian.sources` | `https://deb.debian.org/debian` — `trixie`, `trixie-updates`; `https://deb.debian.org/debian-security` — `trixie-security`; components `main contrib non-free-firmware`; `Signed-By` Debian's archive keyring | enabled. Written by `distro/base/hooks/80-apt-sources.sh` |
| `/etc/apt/sources.list.d/nexus.sources` | the hosted NEXUS repository; `Signed-By: /usr/share/keyrings/nexus-archive-keyring.gpg` | **disabled** (`Enabled: no`, placeholder URL under the reserved `.invalid` domain) unless the packages were built with `NEXUS_REPO_URL`. Package `nexus-archive-keyring` |
| `/etc/apt/sources.list.d/nexus-local.sources` | `file:///usr/share/nexus/repository` — the NEXUS packages that came with the installation | enabled; desktop image only |

`/etc/apt/sources.list` is removed. Exactly one Debian suite is enabled; no
other distribution's repositories are configured, and the image build fails
if the build-time mirror URL leaks into the shipped APT configuration.

## Components

The NEXUS repository has three components
(`distro/repositories/components.conf`; first matching pattern wins,
unmatched packages go to `core`):

| Component | Content | Patterns |
|---|---|---|
| `core` | the operating system: base files, boot, installer, desktop, system applications; also the Arsenal manager and its manifests | default; `nexus-arsenal`, `nexus-arsenal-data` |
| `security` | Arsenal profile metapackages, and tools packaged by NEXUS because Debian does not ship them (none exist) | `nexus-arsenal-*`, `nexus-tool-*` |
| `community` | contributed packages | empty |

## Producing the repository (reprepro)

```
sudo build/packages/repo.sh [--arch amd64|arm64]
```

Creates or updates `build/out/repo`:

* `conf/distributions` — `Origin: NEXUS`, `Label: NEXUS OS`, `Suite:` the
  channel (`dev`), `Codename: obsidian`, `Architectures: amd64 arm64`,
  `Components: core security community`, `SignWith:` the key's fingerprint;
* every `.deb` in `build/out/packages` whose architecture is `all` or the
  requested one is removed from and re-included into its component
  (`reprepro includedeb`), then the indices are exported;
* `dists/obsidian/InRelease` and `Release.gpg` are signed; the script fails
  if `InRelease` is missing afterwards;
* the public key is copied to `build/out/repo/nexus-archive-keyring.gpg`.

reprepro runs inside the build chroot with the output and signing
directories bind-mounted. Because each package is removed before it is
included, the repository holds **one version per package** — there is no
history of older versions.

The result is a complete static repository: it can be served by any web
server or object store.

## Signing keys

`build/packages/signing-key.sh` prints the fingerprint of the signing key
and exports its public half to `build/out/keys/nexus-archive-keyring.gpg`.

* Key material lives in the GnuPG home `$NEXUS_SIGNING_HOME`, default
  `build/work/signing` — a directory that git ignores. The script **refuses**
  a signing home anywhere else inside the repository tree.
* If that home holds no secret key, a **development key** is generated
  there: Ed25519, signing only, valid for two years, no passphrase, with the
  user id "NEXUS OS development archive key (NOT A RELEASE KEY)".
  Development keys make local images installable and verifiable end to end.
  They are not release keys: anyone with access to the build machine can
  sign with them.
* The public key ships in the package `nexus-archive-keyring` as
  `/usr/share/keyrings/nexus-archive-keyring.gpg`. `scripts/gen-apt-source.sh`
  takes it from `NEXUS_ARCHIVE_KEY` (set by `build/packages/build.sh`) or,
  if that is unset, from `distro/signing/nexus-archive-keyring.asc` — a
  public key file that may be committed. No such file is committed today.

Rules:

* **Never commit private keys.** `.gitignore` excludes `*.key`, `*.pem`,
  `*.p12`, `secring.*`, `*.gnupg/` and `distro/signing/private/`; do not
  work around it. Only a *public* key may live in the repository.
* For a release, keep the signing key outside the build machine's work tree
  (ideally on a hardware token or an offline machine), point
  `NEXUS_SIGNING_HOME` at a GnuPG home that can use it, and commit only the
  public key. `conf/options` contains `ask-passphrase`, so reprepro can
  prompt for a protected key.
* A release-signing procedure (key ceremony, rotation, revocation) is **not
  written down or implemented**. All images built so far are signed with a
  development key and must be treated as development builds.

## Release channels

`NEXUS_CHANNEL` in `distro/base/nexus.conf` is `dev`; the intended values are
`dev`, `testing` and `stable`. What the value does today:

* it becomes the `Suite:` field of the repository's `Release` file;
* it is recorded as `NEXUS_CHANNEL` in `/usr/lib/os-release` and
  `/usr/share/nexus/nexus-release`.

Nothing else is implemented: there are no separate `testing` or `stable`
repositories, no promotion process and no channel switch on the installed
system. The APT source selects the repository by codename (`obsidian`), not
by channel.

## The local on-system repository

When the image contains the desktop shell, `build/iso/stage-rootfs.sh`
copies `dists/` and `pool/` of the build repository (without debug-symbol
packages) to `/usr/share/nexus/repository` and writes `nexus-local.sources`.

* Purpose: the `nexus-arsenal-*` profile metapackages and every other NEXUS
  package of the release can be installed or re-installed on a machine that
  has no NEXUS network repository.
* It is authenticated like a remote repository: APT verifies its `InRelease`
  against `/usr/share/keyrings/nexus-archive-keyring.gpg`.
* It is a **snapshot of the release**. It never receives updates. The files
  are not owned by a package.
* The profile metapackages only name Debian packages; the tools themselves
  are downloaded from Debian when a profile is installed.

Installing a profile from this repository on an installed system has not
been verified.

## Enabling a hosted repository (`NEXUS_REPO_URL`)

Not done yet; this is how the build system supports it.

1. Publish `build/out/repo` (the `dists/` and `pool/` directories) at an
   HTTPS URL, signed with a release key.
2. Build the packages with that URL:

   ```sh
   sudo env NEXUS_REPO_URL=https://example.org/nexus build/packages/build.sh
   ```

   `gen-apt-source.sh` then writes `nexus.sources` with that URL and
   `Enabled: yes`; images built from these packages have the repository
   enabled. `NEXUS_REPO_URL` can also be set in `distro/base/nexus.conf`.
3. On a system that was installed with the source disabled, edit
   `/etc/apt/sources.list.d/nexus.sources`: set `URIs:` to the repository
   and `Enabled: yes`, then `sudo apt update`. The key must be the one in
   `nexus-archive-keyring`; a system installed from a development build
   trusts only that build's development key.

Until a repository is published, **NEXUS packages on an installed system do
not receive updates**. Debian base and security updates are unaffected.

## Rollback

There is no NEXUS-specific rollback mechanism. What is and is not available:

| Mechanism | State |
|---|---|
| Filesystem snapshots (btrfs, LVM, Timeshift-style) | **not implemented.** The installer formats ext4. On encrypted installations the volume group is fully allocated, so there is no free space for LVM snapshots |
| Transactional or image-based updates | not implemented |
| Older package versions in the NEXUS repository | not available: the repository keeps one version per package |
| Downgrading a Debian package | standard APT: `sudo apt install <package>=<version>` works while that version is still in an enabled suite. Debian's stable suites normally carry only the current version; older ones are on `snapshot.debian.org`, which is **not** configured |
| Holding a package | standard APT: `sudo apt-mark hold <package>` keeps a package at its installed version; `apt-mark unhold` releases it |
| APT pinning | standard APT: a file in `/etc/apt/preferences.d/` can pin a package or an origin. NEXUS OS ships no pins |
| Previous kernel | Debian keeps the previously installed kernel; choose it under "Advanced options" in the boot menu if a new kernel fails |
| Interrupted updates | `sudo dpkg --configure -a` and `sudo apt -f install`, or the Updater's Recovery page ([../security/updates.md](../security/updates.md)) |

If you depend on being able to return to an earlier state, make your own
backups before updating.

## The build-time repository in the image build

The image build installs the NEXUS packages with APT from
`build/out/repo`, bind-mounted read-only at `/run/nexus-repo` and trusted
through a temporary key file. Both the temporary source and key are removed
again before the image is finished ([iso.md](iso.md)).
