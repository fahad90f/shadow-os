#!/bin/bash
# Publish the built packages into a signed APT repository.
#
#   build/packages/repo.sh [--arch amd64|arm64]
#
# Creates/updates $NEXUS_OUT/repo, a complete repository with the components
# core, security and community for the suite <codename> (distro/base/nexus.conf):
# signed InRelease/Release.gpg, per-architecture Packages indices and the
# package pool. Component assignment: distro/repositories/components.conf.
#
# The repository can be served as static files from any web server or object
# store (docs/packaging/repositories.md); the image build consumes it directly.

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

ARCH="$(host_arch)"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
need_root

: "${NEXUS_SIGNING_HOME:=$NEXUS_WORK/signing}"
export NEXUS_SIGNING_HOME
CHROOT="$NEXUS_WORK/chroot-build-$(host_arch)"
[ -e "$CHROOT/.nexus-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh"
PKG_OUT="$NEXUS_OUT/packages"
REPO="$NEXUS_OUT/repo"
ls "$PKG_OUT"/*.deb >/dev/null 2>&1 || die "no packages in $PKG_OUT — run build/packages/build.sh first"

FPR="$("$NEXUS_ROOT/build/packages/signing-key.sh")"
mkdir -p "$REPO/conf"
cat > "$REPO/conf/distributions" <<CONF
Origin: $NEXUS_VENDOR
Label: $NEXUS_NAME
Suite: $NEXUS_CHANNEL
Codename: $NEXUS_CODENAME
Version: $NEXUS_VERSION
Architectures: $NEXUS_ARCHES
Components: core security community
Description: $NEXUS_NAME $NEXUS_VERSION package repository
SignWith: $FPR
CONF
cat > "$REPO/conf/options" <<CONF
verbose
ask-passphrase
CONF

component_for() {
    local name="$1" pattern component
    while read -r pattern component; do
        case "$pattern" in ''|'#'*) continue ;; esac
        # shellcheck disable=SC2254
        case "$name" in $pattern) echo "$component"; return ;; esac
    done < "$NEXUS_ROOT/distro/repositories/components.conf"
    echo core
}

run_reprepro() {
    in_chroot "$CHROOT" --bind "$NEXUS_OUT:/out" --bind "$NEXUS_SIGNING_HOME:/signing" -- \
        env GNUPGHOME=/signing reprepro -b /out/repo "$@"
}

log "publishing packages into $REPO (signing key $FPR)"
for deb in "$PKG_OUT"/*.deb; do
    name="$(dpkg-deb -f "$deb" Package)"
    arch="$(dpkg-deb -f "$deb" Architecture)"
    # A package build for one architecture must not disturb the other's index.
    if [ "$arch" != "all" ] && [ "$arch" != "$ARCH" ]; then continue; fi
    comp="$(component_for "$name")"
    run_reprepro -C "$comp" remove "$NEXUS_CODENAME" "$name" >/dev/null 2>&1 || true
    run_reprepro -C "$comp" includedeb "$NEXUS_CODENAME" "/out/packages/$(basename "$deb")" >/dev/null
done
run_reprepro export "$NEXUS_CODENAME" >/dev/null
in_chroot "$CHROOT" --bind "$NEXUS_SIGNING_HOME:/signing" -- gpgconf --homedir /signing --kill gpg-agent 2>/dev/null || true

cp "$NEXUS_OUT/keys/nexus-archive-keyring.gpg" "$REPO/nexus-archive-keyring.gpg"
[ -s "$REPO/dists/$NEXUS_CODENAME/InRelease" ] || die "repository metadata was not signed"
log "repository ready:"
run_reprepro list "$NEXUS_CODENAME" | sed 's/^/  /' >&2
