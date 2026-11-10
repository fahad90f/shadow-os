#!/bin/bash
# Build the NEXUS OS Debian packages inside the build chroot.
#
#   build/packages/build.sh [--arch amd64|arm64] [--indep | --arch-only]
#
#   (default)     all packages for the architecture, plus architecture-
#                 independent ones
#   --indep       only architecture-independent packages (no compiler run;
#                 seconds instead of minutes)
#   --arch-only   only compiled packages (what a second architecture needs)
#
# The source is exported from the working tree (tracked files plus untracked
# files that are not ignored), so the build never sees editor backups, old
# build directories or anything else that is not part of the project.
#
# Output: $NEXUS_OUT/packages/*.deb

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

ARCH="$(host_arch)"
MODE="-b"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --indep) MODE="-A"; shift ;;
        --arch-only) MODE="-B"; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

need_root
ensure_foreign_arch "$ARCH"
CHROOT="$NEXUS_WORK/chroot-build-$ARCH"
[ -e "$CHROOT/.nexus-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh --arch $ARCH"

PKG_OUT="$NEXUS_OUT/packages"
mkdir -p "$PKG_OUT"

VERSION="$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$NEXUS_ROOT/debian/changelog")"
[ "$VERSION" = "$NEXUS_VERSION" ] || die "debian/changelog version ($VERSION) differs from distro/base/nexus.conf ($NEXUS_VERSION)"

SRC="/build/pkg/nexus-os-$VERSION"
log "exporting source tree to chroot:$SRC"
rm -rf "$CHROOT/build/pkg"
mkdir -p "$CHROOT$SRC"
(cd "$NEXUS_ROOT" && git ls-files -co --exclude-standard -z) \
    | rsync -a --from0 --files-from=- "$NEXUS_ROOT/" "$CHROOT$SRC/"

# The archive public key ships in nexus-archive-keyring.
"$NEXUS_ROOT/build/packages/signing-key.sh" >/dev/null
cp "$NEXUS_OUT/keys/nexus-archive-keyring.gpg" "$CHROOT/build/pkg/archive-key.gpg"

log "dpkg-buildpackage $MODE ($ARCH, $NEXUS_JOBS jobs)"
in_chroot "$CHROOT" -- /bin/bash -euc '
    cd "$1"
    export NEXUS_ARCHIVE_KEY=/build/pkg/archive-key.gpg
    export NEXUS_REPO_URL="$2"
    export DEB_BUILD_OPTIONS="parallel=$3"
    dpkg-buildpackage -us -uc --no-sign "$4"
' build "$SRC" "${NEXUS_REPO_URL:-}" "$NEXUS_JOBS" "$MODE"

count=0
for deb in "$CHROOT"/build/pkg/*.deb; do
    [ -f "$deb" ] || continue
    cp "$deb" "$PKG_OUT/"
    count=$((count + 1))
done
[ "$count" -gt 0 ] || die "no packages were produced"
log "$count package(s) → $PKG_OUT"
ls -1 "$PKG_OUT"/*.deb | sed 's|.*/|  |' >&2
