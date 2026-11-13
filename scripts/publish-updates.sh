#!/bin/bash
# Build the Shadow OS packages on this machine and make them available as
# updates, so that systems pick them up with
#
#     sudo apt update && sudo apt upgrade
#
#   scripts/publish-updates.sh --local
#   scripts/publish-updates.sh [--url https://<name>.vercel.app] [--no-deploy]
#
#   --local       for this machine only: register the packages as an APT
#                 source here (/var/lib/shadow-updates). Nothing is uploaded.
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
as_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }

URL_ARG=""
DEPLOY=1
LOCAL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --local) LOCAL=1; shift ;;
        --url) URL_ARG="${2:-}"; shift 2 ;;
        --no-deploy) DEPLOY=0; shift ;;
        -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ "$LOCAL" -eq 0 ] || [ -z "$URL_ARG" ] || die "--local and --url do not go together"

: "${SHADOW_SIGNING_HOME:=/var/lib/shadow-signing}"
: "${SHADOW_VERCEL_PROJECT:=shadow-os-updates}"
# A compiler cache, kept between runs, so a rebuild only recompiles what
# changed. The first build fills it and is no faster; later ones are much
# quicker. Set SHADOW_CCACHE_DIR= (empty) to turn it off.
: "${SHADOW_CCACHE_DIR:=/var/cache/shadow-ccache}"
# Where --local installs to. Only tests point this at another system root.
: "${SHADOW_LOCAL_ROOT:=}"
STAGE="$ROOT/build/out/site"
LINK="$ROOT/website/.vercel"          # which Vercel project this clone publishes to (not in git)
vercel() { npx --yes vercel@latest "$@"; }

# ---- tools -------------------------------------------------------------------
tools="git python3 gpg"
[ "$LOCAL" -eq 1 ] || tools="$tools curl node npm npx"
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
    build_repository
    dest="$SHADOW_LOCAL_ROOT/var/lib/shadow-updates"
    key="$SHADOW_LOCAL_ROOT/usr/share/keyrings/shadow-updates.gpg"
    apt_sources="$SHADOW_LOCAL_ROOT/etc/apt/sources.list.d"
    log "registering the packages with APT ($dest)"
    as_root install -d -m 755 "$dest" "$apt_sources" "$(dirname "$key")"
    as_root rsync -a --delete build/out/repo/dists build/out/repo/pool "$dest/"
    as_root install -m 644 build/out/repo/shadow-archive-keyring.gpg "$key"
    printf '%s\n' \
        "# Shadow OS packages built on this machine (scripts/publish-updates.sh --local)." \
        "Types: deb" \
        "URIs: file:///var/lib/shadow-updates" \
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
    log "build $BUILD_ID is ready. Install it with:"
    printf '\n    sudo apt update && sudo apt upgrade\n\n' >&2
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
    python3 scripts/gen-repo-site.py --repo build/out/repo --out "$STAGE" --url "$SITE_URL" \
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
