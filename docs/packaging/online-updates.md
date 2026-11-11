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
`https://fahad90fa.github.io/Custom-OS/`, and bakes that address (enabled) into
the images it produces. Images built **without** the secret keep the Shadow
source disabled, so they never point at a repository that can't be verified.

## How an installed system then updates

Images built after setup already have the repository configured. On an existing
install you can point it there yourself — the source file is
`/etc/apt/sources.list.d/shadow.sources`; set `Enabled: yes` (the public key is
already installed as `/usr/share/keyrings/shadow-archive-keyring.gpg`). Then:

```bash
sudo apt update
sudo apt full-upgrade      # or open Shadow Updater
```

## Adding a feature and shipping it

1. Change the code and raise the version in `debian/changelog` (APT only
   installs a version newer than the one installed).
2. `git push`. CI builds the packages, signs the repository and publishes it.
3. Installed systems get the update at the next `apt full-upgrade` / Shadow
   Updater check. No data is lost.

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
