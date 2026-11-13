# Online updates for installed Shadow OS systems

Shadow OS installs are updated the same way as any Debian system — with
packages, which **never touch your personal files** in `/home`, your installed
apps, or your settings. Program files under `/usr`, the kernel and the boot
loader are replaced; the old kernel stays selectable in the boot menu.

Two kinds of update reach an installed system:

- **Debian packages** (kernel, browser, security fixes): already automatic,
  from Debian's official repositories. `sudo apt full-upgrade` or Shadow
  Updater.
- **Shadow OS's own packages** (desktop, apps, installer, arsenal): delivered
  from the **Shadow update site**, which you publish from your own computer
  with one command. This needs a one-time setup.

The update site is one Vercel project that holds two things: the website
(`website/`, a React + TypeScript app showing the published build and its
packages) and the signed APT repository that `apt` downloads from.

## One-time setup

You need a Debian-based computer (Parrot, Debian, Ubuntu) with this repository
cloned, Node.js 20.19 or newer, and a free Vercel account.

1. Sign in to Vercel from the terminal:

   ```bash
   npx vercel login
   ```

2. Publish the website alone, so that Vercel assigns it an address:

   ```bash
   scripts/publish-updates.sh
   ```

3. On vercel.com open the project `shadow-os-updates` and copy the address
   under **Domains** (it looks like `https://shadow-os-updates.vercel.app`).

4. Build and publish the packages for that address:

   ```bash
   scripts/publish-updates.sh --url https://shadow-os-updates.vercel.app
   ```

   The address is written to `distro/base/shadow.conf` (commit it) and built
   into the packages. The first run creates the build environment and the
   signing key, so it takes a while.

### The signing key
The repository is signed with a key the script creates in
`/var/lib/shadow-signing` on your computer. Installed systems accept updates
only from that key, so:

- **back the directory up** (`sudo tar czf shadow-signing-backup.tgz -C /var/lib
  shadow-signing`, kept somewhere safe). Without it you cannot publish updates
  that existing installations accept; they would have to be switched to a new
  key with `enable-updates.sh` again;
- never commit it, upload it or paste it anywhere. Anyone who has it can sign
  packages your machines will install.

## Shipping a change

```bash
scripts/publish-updates.sh
```

It builds the packages from the current checkout, signs the repository, and
uploads it with the website. Installed systems get the change at the next
`sudo apt update && sudo apt upgrade` or Shadow Updater check. No data is lost.

APT only installs a version newer than the one installed, so every build is
stamped: the packages are versioned `<release>.build<time>` (for example
`0.2.0.build202611130542`). There is no need to edit `debian/changelog` for an
ordinary change; raise the version there and in `distro/base/shadow.conf` for
a new release. `apt upgrade` never removes packages; when an update needs that,
APT says packages were "kept back" and `sudo apt full-upgrade` applies it.

`scripts/publish-updates.sh --no-deploy` does everything except the upload; the
result is in `build/out/site`.

## How an installed system then updates

An image built after the packages were published (`sudo env
SHADOW_SIGNING_HOME=/var/lib/shadow-signing build/iso/build.sh`) has the update
site configured:

```bash
sudo apt update && sudo apt upgrade      # or open Shadow Updater
```

A system installed from an older image has the source disabled and trusts the
throw-away key of that image build. Switch it over once:

```bash
curl -fsSLO https://<the address>/enable-updates.sh
sudo sh enable-updates.sh
```

The script installs the current `shadow-archive-keyring` package (the key and
the enabled source) and turns off the package snapshot that came with the old
image (`shadow-local.sources`), which is signed with the old key. The first key
is taken on trust over HTTPS; the site shows its fingerprint.

## What CI does

The GitHub Actions pipeline builds and tests images but publishes nothing. Its
images are signed with a per-run development key and ship the Shadow source
disabled. To have CI sign with the release key instead, store an export of it
as the `SHADOW_SIGNING_KEY` secret (`scripts/make-signing-key.sh` creates a key
for that purpose; use the same key in `/var/lib/shadow-signing`).

## Without the online repository

Every build also uploads the built `.deb` packages as the
`shadow-packages-amd64` artifact on the run page. You can download them and
install manually at any time — this works with no setup and never touches your
data:

```bash
sudo apt install ./shadow-*.deb
```

## Safety habits

- Back up `/home` to external storage occasionally — updates are safe, disks
  are not.
- In a VM, take a snapshot before a large update so you can roll back.
- If an update ever breaks booting, pick the previous kernel under **Advanced
  options** in the boot menu, or run **Shadow Recovery** from the installation
  medium — it repairs the system without touching your files.
