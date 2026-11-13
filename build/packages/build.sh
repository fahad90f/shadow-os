#!/bin/bash
# Build the Shadow OS Debian packages inside the build chroot.
#
#   build/packages/build.sh [--arch amd64|arm64] [--indep | --arch-only]
#
#   SHADOW_BUILD_ID=<number>   package version becomes <version>.build<number>,
#                 so a later build of the same release is an upgrade for APT
#                 (scripts/publish-updates.sh passes the time, CI its run
#                 number). Unset: the plain release version.
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
# Output: $SHADOW_OUT/packages/*.deb

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

ARCH="$(host_arch)"
MODE="-b"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --indep) MODE="-A"; shift ;;
        --arch-only) MODE="-B"; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

need_root
ensure_foreign_arch "$ARCH"
CHROOT="$SHADOW_WORK/chroot-build-$ARCH"
[ -e "$CHROOT/.shadow-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh --arch $ARCH"

PKG_OUT="$SHADOW_OUT/packages"
mkdir -p "$PKG_OUT"

VERSION="$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$SHADOW_ROOT/debian/changelog")"
[ "$VERSION" = "$SHADOW_VERSION" ] || die "debian/changelog version ($VERSION) differs from distro/base/shadow.conf ($SHADOW_VERSION)"

SRC="/build/pkg/shadow-os-$VERSION"
log "exporting source tree to chroot:$SRC"
rm -rf "$CHROOT/build/pkg"
mkdir -p "$CHROOT$SRC"
# Tracked files with their working-tree content. Untracked files are left
# out so unfinished work cannot break a package build;
# SHADOW_EXPORT_UNTRACKED=1 includes them.
ls_flags="-c"
[ -n "${SHADOW_EXPORT_UNTRACKED:-}" ] && ls_flags="-co --exclude-standard"
# shellcheck disable=SC2086
# Committed work in progress is left out too (build/packages/wip.exclude).
excludes=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    excludes+=(":(exclude,glob)$line" ":(exclude,glob)$line/**")
done < "$SHADOW_ROOT/build/packages/wip.exclude"
# shellcheck disable=SC2086
(cd "$SHADOW_ROOT" && git ls-files $ls_flags -z -- . "${excludes[@]}") \
    | (cd "$SHADOW_ROOT" && while IFS= read -r -d "" f; do [ -e "$f" ] && printf '%s\0' "$f"; done) \
    | rsync -a --from0 --files-from=- "$SHADOW_ROOT/" "$CHROOT$SRC/"

# APT only upgrades to a higher version. Builds of the same release are told
# apart by the build number, stamped into the exported changelog only. The
# separator is a dot, not the usual "+": some static file hosts mishandle a
# "+" in a download path.
if [ -n "${SHADOW_BUILD_ID:-}" ]; then
    case "$SHADOW_BUILD_ID" in *[!0-9]*) die "SHADOW_BUILD_ID must be a number, not '$SHADOW_BUILD_ID'" ;; esac
    sed -i "1s/($VERSION)/($VERSION.build$SHADOW_BUILD_ID)/" "$CHROOT$SRC/debian/changelog"
    log "package version $VERSION.build$SHADOW_BUILD_ID"
fi

# The archive public key ships in shadow-archive-keyring.
"$SHADOW_ROOT/build/packages/signing-key.sh" >/dev/null
cp "$SHADOW_OUT/keys/shadow-archive-keyring.gpg" "$CHROOT/build/pkg/archive-key.gpg"

log "dpkg-buildpackage $MODE ($ARCH, $SHADOW_JOBS jobs)"
in_chroot "$CHROOT" -- /bin/bash -euc '
    cd "$1"
    export SHADOW_ARCHIVE_KEY=/build/pkg/archive-key.gpg
    export SHADOW_REPO_URL="$2"
    export DEB_BUILD_OPTIONS="parallel=$3"
    dpkg-buildpackage -us -uc --no-sign "$4"
' build "$SRC" "${SHADOW_REPO_URL:-}" "$SHADOW_JOBS" "$MODE"

count=0
for deb in "$CHROOT"/build/pkg/*.deb; do
    [ -f "$deb" ] || continue
    # One version per package: an older build left here would be published too.
    base="${deb##*/}"
    rm -f "$PKG_OUT/${base%%_*}_"*"_${base##*_}"
    cp "$deb" "$PKG_OUT/"
    count=$((count + 1))
done
[ "$count" -gt 0 ] || die "no packages were produced"
log "$count package(s) → $PKG_OUT"
ls -1 "$PKG_OUT"/*.deb | sed 's|.*/|  |' >&2
