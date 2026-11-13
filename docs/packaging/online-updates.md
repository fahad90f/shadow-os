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
  from the **Shadow package repository**, which GitHub Actions publishes to
  GitHub Pages after every successful build. This needs a one-time setup.

## One-time setup (do this once)

### 1. Create the release signing key
On your own computer, in a clone of this repository:

```bash
scripts/make-signing-key.sh
```

It writes the **public** key to `distro/signing/shadow-archive-keyring.asc`
(commit that) and prints the **private** key once. Copy the private key into a
new GitHub Actions secret and then clear it from your screen:

- GitHub → your repo → **Settings → Secrets and variables → Actions → New
  repository secret**
- Name: `SHADOW_SIGNING_KEY`
- Value: the entire private-key block the script printed

Never commit the private key or paste it anywhere else. If it ever leaks, run
the script again to make a new key and replace both the secret and the
committed public key.

```bash
git add distro/signing/shadow-archive-keyring.asc
git commit -m "Add Shadow OS release signing public key"
git push
```

### 2. Enable GitHub Pages
GitHub → **Settings → Pages → Build and deployment → Source: GitHub Actions.**

That's it. The next build signs the repository with your key, publishes it to
the repository's GitHub Pages address (`https://<owner>.github.io/<repository>/`,
for example `https://fahad90f.github.io/shadow-os/`), and bakes that address
(enabled) into the images it produces. Images built **without** the secret keep
the Shadow source disabled, so they never point at a repository that can't be
verified, and nothing is published: a build without the secret signs with a
throw-away key that no installed system trusts.

`scripts/gen-repo-site.py` prepares what is published: the repository,
`repo.json` (the published build and its packages) and `enable-updates.sh`
(below).

### 3. Deploy the website (optional)
`website/` is a React + TypeScript (Vite) app that shows the published build,
its packages and how to update. It holds no packages itself: it reads
`repo.json` from the repository address in the visitor's browser, so it never
needs redeploying when a new build is published.

On Vercel: **Add New → Project**, import this repository, set **Root
Directory** to `website`, keep the detected Vite settings, deploy. Only if the
repository is not at `https://fahad90f.github.io/shadow-os`, set the
environment variable `VITE_REPO_URL` to its address (`website/.env.example`).

Locally: `cd website && npm install && npm run dev`.

## How an installed system then updates

Images built after setup already have the repository configured:

```bash
sudo apt update && sudo apt upgrade      # or open Shadow Updater
```

A system installed from an image built **before** setup has the source disabled
and trusts the throw-away key of that image build. Switch it over once:

```bash
curl -fsSLO https://<owner>.github.io/<repository>/enable-updates.sh
sudo sh enable-updates.sh
```

The script installs the current `shadow-archive-keyring` package (the release
key and the enabled source) and turns off the package snapshot that came with
the old image (`shadow-local.sources`), which is signed with the old key. The
first key is taken on trust over HTTPS; compare the fingerprint shown on the
site with the one `scripts/make-signing-key.sh` printed if that matters to you.

## Adding a feature and shipping it

1. Change the code and `git push`. CI builds the packages, boots the image
   made from them, signs the repository and publishes it (about an hour).
2. Installed systems get the update at the next `sudo apt update && sudo apt
   upgrade` / Shadow Updater check. No data is lost.

APT only installs a version newer than the one installed, so CI stamps every
build: the packages are versioned `<release>+build<run number>` (for example
`0.2.0+build48`). There is no need to edit `debian/changelog` for an ordinary
change; raise the version there and in `distro/base/shadow.conf` for a new
release. `apt upgrade` never removes packages; when an update needs that, APT
says packages were "kept back" and `sudo apt full-upgrade` applies it.

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
