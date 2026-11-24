#!/bin/bash
# Build the Shadow OS packages on this machine and make them available as
# updates, so that systems pick them up with
#
#     sudo apt update && sudo apt upgrade
#
#   scripts/publish-updates.sh --local [--no-build] [--restart | --later | --now]
#   scripts/publish-updates.sh [--url https://<name>.vercel.app] [--no-deploy]
#
#   --local       for this machine only: register the packages as an APT
#                 source here (/var/lib/shadow-updates). Nothing is uploaded.
#                 The new build is downloaded and installed during the next
#                 restart; the script asks whether to restart now.
#     --restart   restart without asking
#     --later     do not ask; install whenever the computer is next restarted
#     --now       only register the packages; install them yourself with
#                 sudo apt update && sudo apt upgrade
#     --no-build  do not build: use the packages an earlier run left in
#                 build/out/repo (after an interrupted run, or to install the
#                 same build on this machine again)
#   (default)     for other machines too: upload the packages, with the
#                 website, to Vercel.
#   --url URL     the address of the Vercel site. Needed once; it is stored in
#                 distro/base/shadow.conf and built into the packages.
#   --no-deploy   build the site into build/out/site but upload nothing
#
# Run it as your normal user from a clone of the repository; it asks for sudo
# where the build needs root. Without --local, the first run, before an
# address is known, publishes the website alone so that Vercel assigns one.
#
# The packages are signed with the key in SHADOW_SIGNING_HOME (default
# /var/lib/shadow-signing), created on first use. Systems that take updates
# trust that key: back the directory up, and never copy it into the repository.
#
# See docs/packaging/online-updates.md.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

log() { printf '\033[1;36m[publish]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[publish] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
# Without sudo as root already, and when a test installs into a scratch
# directory of its own (SHADOW_LOCAL_ROOT).
as_root() { if [ "$(id -u)" -eq 0 ] || [ -n "${SHADOW_LOCAL_ROOT:-}" ]; then "$@"; else sudo "$@"; fi; }

URL_ARG=""
DEPLOY=1
LOCAL=0
INSTALL=ask
BUILD=1
while [ $# -gt 0 ]; do
    case "$1" in
        --local) LOCAL=1; shift ;;
        --restart) INSTALL=restart; shift ;;
        --later) INSTALL=later; shift ;;
        --now) INSTALL=now; shift ;;
        --no-build) BUILD=0; shift ;;
        --url) URL_ARG="${2:-}"; shift 2 ;;
        --no-deploy) DEPLOY=0; shift ;;
        -h|--help) sed -n '2,35p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ "$LOCAL" -eq 0 ] || [ -z "$URL_ARG" ] || die "--local and --url do not go together"
[ "$LOCAL" -eq 1 ] || [ "$INSTALL" = ask ] || die "--restart, --later and --now go with --local"
[ "$LOCAL" -eq 1 ] || [ "$BUILD" -eq 1 ] || die "--no-build goes with --local"

: "${SHADOW_SIGNING_HOME:=/var/lib/shadow-signing}"
: "${SHADOW_VERCEL_PROJECT:=shadow-os-updates}"
# A compiler cache, kept between runs, so a rebuild only recompiles what
# changed. The first build fills it and is no faster; later ones are much
# quicker. Set SHADOW_CCACHE_DIR= (empty) to turn it off.
: "${SHADOW_CCACHE_DIR:=/var/cache/shadow-ccache}"
# Where --local installs to. Only tests point this at another system root.
: "${SHADOW_LOCAL_ROOT:=}"
# Where the build scripts put their results (build/lib/common.sh).
OUT="${SHADOW_OUT:-$ROOT/build/out}"
STAGE="$OUT/site"
LINK="$ROOT/website/.vercel"          # which Vercel project this clone publishes to (not in git)
vercel() { npx --yes vercel@latest "$@"; }

# ---- tools -------------------------------------------------------------------
tools="git python3 gpg"
[ "$LOCAL" -eq 1 ] || tools="$tools curl node npm npx"
[ "$BUILD" -eq 1 ] || tools="rsync"
for tool in $tools; do
    command -v "$tool" >/dev/null || die "'$tool' is not installed"
done
if [ "$LOCAL" -eq 0 ]; then
    node -e '
        const [major, minor] = process.versions.node.split(".").map(Number);
        process.exit(major > 22 || (major === 22 && minor >= 12) || (major === 20 && minor >= 19) ? 0 : 1);
    ' || die "Node.js $(node --version) is too old: the website needs 20.19 or newer (22.12 or newer on the 22 line)"
fi

# ---- the address of the site -------------------------------------------------
if [ -n "$URL_ARG" ]; then
    URL_ARG="${URL_ARG%/}"
    [[ "$URL_ARG" =~ ^https://[a-z0-9.-]+$ ]] || die "--url must look like https://name.vercel.app (lower case, no path)"
    grep -q '^: "\${SHADOW_REPO_URL=.*}"$' distro/base/shadow.conf || die "cannot find the SHADOW_REPO_URL line in distro/base/shadow.conf"
    sed -i "s|^: \"\${SHADOW_REPO_URL=.*}\"\$|: \"\${SHADOW_REPO_URL=$URL_ARG}\"|" distro/base/shadow.conf
    log "address stored in distro/base/shadow.conf — commit that file"
fi
SITE_URL="$(env -u SHADOW_REPO_URL sh -c '. ./distro/base/shadow.conf; printf %s "$SHADOW_REPO_URL"')"
CODENAME="$(sh -c '. ./distro/base/shadow.conf; printf %s "$SHADOW_CODENAME"')"

# Build the packages and the signed repository (build/out/repo).
build_repository() {
    if ! as_root gpg --homedir "$SHADOW_SIGNING_HOME" --batch --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; then
        log "creating the signing key in $SHADOW_SIGNING_HOME — back this directory up"
        as_root install -d -m 700 "$SHADOW_SIGNING_HOME"
        as_root gpg --homedir "$SHADOW_SIGNING_HOME" --batch --pinentry-mode loopback --passphrase '' \
            --quick-generate-key "Shadow OS archive signing key <shadow-os@users.noreply.github.com>" ed25519 sign never >/dev/null 2>&1
        as_root gpgconf --homedir "$SHADOW_SIGNING_HOME" --kill gpg-agent 2>/dev/null || true
    fi
    command -v mmdebstrap >/dev/null || { log "installing the build tools"; as_root scripts/host-setup.sh --no-test-tools >&2; }
    local arch; arch="$(dpkg --print-architecture)"
    [ -e "build/work/chroot-build-$arch/.shadow-bootstrap-complete" ] || {
        log "creating the build chroot (once; this downloads Debian packages)"
        as_root build/chroot/create.sh >&2
    }
    BUILD_ID="$(date -u +%Y%m%d%H%M)"
    local build_env=(env "SHADOW_SIGNING_HOME=$SHADOW_SIGNING_HOME" "SHADOW_REPO_URL=$SITE_URL" "SHADOW_BUILD_ID=$BUILD_ID")
    [ -z "${SHADOW_CCACHE_DIR:-}" ] || build_env+=("SHADOW_CCACHE_DIR=$SHADOW_CCACHE_DIR")
    log "building the packages (build $BUILD_ID; about a quarter of an hour)"
    as_root "${build_env[@]}" build/packages/build.sh >&2
    as_root "${build_env[@]}" SHADOW_REPO_NO_DBGSYM=1 build/packages/repo.sh >&2
}

# ---- this machine only -------------------------------------------------------
if [ "$LOCAL" -eq 1 ]; then
    if [ "$BUILD" -eq 1 ]; then
        build_repository
    else
        [ -d "$OUT/repo/dists" ] && [ -d "$OUT/repo/pool" ] && [ -e "$OUT/repo/shadow-archive-keyring.gpg" ] \
            || die "--no-build: there are no built packages in $OUT/repo; run without it first"
        # The build number of what is there, from a package's file name.
        BUILD_ID="$(find "$OUT/repo/pool" -name 'shadow-base-files_*.deb' -printf '%f\n' \
            | sed -n 's/.*\.build\([0-9]*\)_.*/\1/p' | sort -n | tail -n 1)"
        BUILD_ID="${BUILD_ID:-from an earlier run}"
        log "using the packages already built (build $BUILD_ID)"
    fi
    dest="$SHADOW_LOCAL_ROOT/var/lib/shadow-updates"
    key="$SHADOW_LOCAL_ROOT/usr/share/keyrings/shadow-updates.gpg"
    apt_sources="$SHADOW_LOCAL_ROOT/etc/apt/sources.list.d"
    log "registering the packages with APT ($dest)"
    as_root install -d -m 755 "$dest" "$apt_sources" "$(dirname "$key")"
    as_root rsync -a --delete "$OUT/repo/dists" "$OUT/repo/pool" "$dest/"
    as_root install -m 644 "$OUT/repo/shadow-archive-keyring.gpg" "$key"
    printf '%s\n' \
        "# Shadow OS packages built on this machine (scripts/publish-updates.sh --local)." \
        "Types: deb" \
        "URIs: copy:///var/lib/shadow-updates" \
        "Suites: $CODENAME" \
        "Components: core security community" \
        "Signed-By: /usr/share/keyrings/shadow-updates.gpg" \
        | as_root tee "$apt_sources/shadow-updates.sources" >/dev/null
    # The upgrade replaces the archive key with the one used here. The package
    # snapshot that came with the image is signed with the image's own key and
    # could not be verified any more.
    snapshot="$apt_sources/shadow-local.sources"
    if [ -e "$snapshot" ] && ! cmp -s "$SHADOW_LOCAL_ROOT/usr/share/keyrings/shadow-archive-keyring.gpg" "$key"; then
        as_root mv "$snapshot" "$snapshot.disabled"
        log "disabled the package snapshot of the installation image ($snapshot.disabled)"
    fi
    if [ "$INSTALL" = now ]; then
        log "build $BUILD_ID is ready. Install it with:"
        printf '\n    sudo apt update && sudo apt upgrade\n\n' >&2
        exit 0
    fi

    # Install at the next restart, before the desktop starts, so that nothing
    # that is being replaced is running (shadow-offline-update, part of the
    # Updater).
    #
    # The restart-time updater is one script, its service and the two files
    # that tie the service into systemd's update boot. They are put in place
    # from this clone, the same files in the same places as in the Updater
    # package just built, before preparing: a system from before the tool
    # existed gets it without any package being upgraded while the desktop is
    # running, and what the tool has learned since this system was installed
    # (adopting the desktop edition, which brings the applications added
    # later) already applies to this update. The upgrade then installs the
    # same files again.
    # In /usr/sbin, which is not on a normal user's PATH: look it up by path.
    offline_tool="$SHADOW_LOCAL_ROOT/usr/sbin/shadow-offline-update"
    units="$SHADOW_LOCAL_ROOT/usr/lib/systemd/system"
    offline_src=applications/updater/offline
    if ! as_root cmp -s "$offline_src/shadow-offline-update" "$offline_tool" \
        || ! as_root cmp -s "$offline_src/shadow-offline-update.service" "$units/shadow-offline-update.service" \
        || ! as_root test -L "$units/system-update.target.wants/shadow-offline-update.service"; then
        log "bringing the restart-time updater up to date"
        as_root install -D -m 755 "$offline_src/shadow-offline-update" "$offline_tool"
        as_root install -D -m 644 "$offline_src/shadow-offline-update.service" "$units/shadow-offline-update.service"
        as_root install -D -m 644 "$offline_src/packagekit-offline-update.conf" \
            "$units/packagekit-offline-update.service.d/50-shadow.conf"
        as_root install -d -m 755 "$units/system-update.target.wants"
        as_root ln -sfn ../shadow-offline-update.service "$units/system-update.target.wants/shadow-offline-update.service"
        [ -n "$SHADOW_LOCAL_ROOT" ] || [ ! -d /run/systemd/system ] || as_root systemctl daemon-reload
    fi
    if [ -n "$SHADOW_LOCAL_ROOT" ]; then
        log "build $BUILD_ID is registered in $SHADOW_LOCAL_ROOT; prepare it there with: shadow-offline-update prepare"
        exit 0
    fi
    as_root "$offline_tool" prepare >&2
    as_root test -L /system-update || { log "nothing to install: this system already runs build $BUILD_ID"; exit 0; }

    if command -v notify-send >/dev/null && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        notify-send -a "Shadow OS" -i system-software-update "Updates ready" \
            "Build $BUILD_ID will be installed during the next restart." 2>/dev/null || true
    fi
    answer=n
    [ "$INSTALL" = restart ] && answer=y
    if [ "$INSTALL" = ask ] && [ -t 0 ]; then
        printf '\nBuild %s will be installed during the next restart.\nRestart now? [y/N] ' "$BUILD_ID" >&2
        read -r answer || answer=n
    fi
    case "$answer" in
        [yY]*) log "restarting to install build $BUILD_ID"; as_root systemctl reboot; exit 0 ;;
    esac
    log "build $BUILD_ID will be installed the next time you restart the computer"
    log "(not wanted after all: sudo shadow-offline-update cancel)"
    exit 0
fi

# ---- website -----------------------------------------------------------------
log "building the website"
(cd website && npm ci --no-audit --no-fund && npm run build) >&2

as_root rm -rf "$STAGE"
as_root install -d -o "$(id -u)" -g "$(id -g)" "$STAGE"

if [ -n "$SITE_URL" ]; then
    build_repository
    source_url="$(git config --get remote.origin.url 2>/dev/null | sed -n 's|^\(https://github\.com/[^/@]*/[^/]*\)$|\1|p' | sed 's|\.git$||')"
    python3 scripts/gen-repo-site.py --repo "$OUT/repo" --out "$STAGE" --url "$SITE_URL" \
        --build "$BUILD_ID" --commit "$(git rev-parse HEAD)" --source-url "$source_url" >&2

    # What is about to be published must verify with the key that is published.
    if command -v gpgv >/dev/null; then
        gpgv --keyring "$STAGE/shadow-archive-keyring.gpg" "$STAGE/dists/$CODENAME/InRelease" >/dev/null 2>&1 \
            || die "the repository index does not verify against its own key — not publishing"
    fi
fi

cp -r website/dist/. "$STAGE/"
# Plain static files: nothing for Vercel to install or build.
cat > "$STAGE/vercel.json" <<'JSON'
{
  "framework": null,
  "installCommand": null,
  "buildCommand": null
}
JSON
log "site ready in $STAGE ($(du -sh "$STAGE" | cut -f1))"

if [ "$DEPLOY" -eq 0 ]; then
    log "not uploading (--no-deploy)"
    exit 0
fi

# ---- upload ------------------------------------------------------------------
vercel whoami >/dev/null 2>&1 || die "not signed in to Vercel — run:  npx vercel login"
if [ -e "$LINK/project.json" ]; then
    cp -r "$LINK" "$STAGE/.vercel"
else
    log "creating the Vercel project '$SHADOW_VERCEL_PROJECT'"
    (cd "$STAGE" && vercel link --yes --project "$SHADOW_VERCEL_PROJECT") >&2
    rm -rf "$LINK" && cp -r "$STAGE/.vercel" "$LINK"
fi
log "uploading"
(cd "$STAGE" && vercel deploy --prod --yes) >&2

if [ -z "$SITE_URL" ]; then
    cat >&2 <<EOF

The website is online, without packages yet. Vercel has given it an address:
open https://vercel.com, the project '$SHADOW_VERCEL_PROJECT', and copy the
address under "Domains" (it looks like https://$SHADOW_VERCEL_PROJECT.vercel.app).

Then build and publish the packages for that address:

    scripts/publish-updates.sh --url https://<the address>
EOF
    exit 0
fi

# The address must really be this project: installed systems will use it.
if curl -fsS --max-time 30 "$SITE_URL/dists/$CODENAME/InRelease" 2>/dev/null | cmp -s - "$STAGE/dists/$CODENAME/InRelease"; then
    log "published: $SITE_URL serves build $BUILD_ID"
else
    die "uploaded, but $SITE_URL does not serve this build. Check that it is the address of the Vercel project '$SHADOW_VERCEL_PROJECT' (Domains), and correct it with --url."
fi
