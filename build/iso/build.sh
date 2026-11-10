#!/bin/bash
# Build a NEXUS OS live/install ISO image.
#
#   build/iso/build.sh [--arch amd64|arm64] [--variant NAME] [--fresh]
#                      [--comp zstd|xz] [--no-nexus-packages]
#
#   --arch      target architecture (default: host architecture)
#   --variant   image variant from distro/base/variants/NAME.conf (default: desktop)
#   --fresh     ignore the cached Debian base layer and bootstrap from scratch
#   --comp      squashfs compressor: zstd (default, faster boot) or xz (smaller)
#   --no-nexus-packages
#               do not install packages from the local NEXUS repository
#               (bring-up/debugging of the base image only)
#
# Pipeline (see docs/architecture/image-build.md):
#   1. base layer   mmdebstrap of Debian <suite> + package lists     (cached)
#   2. nexus layer  NEXUS packages from the local signed repo, rootfs
#                   overlay, configuration hooks                     (overlayfs)
#   3. squashfs     live/filesystem.squashfs
#   4. boot         signed shim + GRUB (UEFI), GRUB eltorito (BIOS on amd64)
#   5. iso          hybrid ISO9660 via xorriso, checksums, manifest
#
# Output: $NEXUS_OUT/iso/nexus-os-<version>-<arch>.iso (+ .sha256, .manifest)

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

ARCH="$(host_arch)"
VARIANT="desktop"
FRESH=0
COMP="zstd"
WITH_NEXUS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --variant) VARIANT="$2"; shift 2 ;;
        --fresh) FRESH=1; shift ;;
        --comp) COMP="$2"; shift 2 ;;
        --no-nexus-packages) WITH_NEXUS=0; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

need_root
need_cmd mmdebstrap mksquashfs xorriso mcopy mformat mmd sha256sum unshare
ensure_foreign_arch "$ARCH"
arch_info "$ARCH" efi >/dev/null

VARIANT_FILE="$NEXUS_ROOT/distro/base/variants/$VARIANT.conf"
[ -f "$VARIANT_FILE" ] || die "unknown variant '$VARIANT' (no $VARIANT_FILE)"
# shellcheck disable=SC1090
. "$VARIANT_FILE"        # defines VARIANT_LISTS, VARIANT_NEXUS_PACKAGES, VARIANT_LABEL

IMG="$NEXUS_WORK/image-$ARCH"
STAGING="$IMG/iso"
ISO_NAME="nexus-os-${NEXUS_VERSION}-${ARCH}"
[ "$VARIANT" = "desktop" ] || ISO_NAME="$ISO_NAME-$VARIANT"
ISO_DIR="$NEXUS_OUT/iso"
REPO_DIR="$NEXUS_OUT/repo"
mkdir -p "$IMG" "$ISO_DIR"

# --------------------------------------------------------------------------
# Package list expansion
# --------------------------------------------------------------------------
# expand_list <file>  → package names valid for $ARCH, one per line
expand_list() {
    awk -v arch="$ARCH" '
        { sub(/#.*/, "") }
        NF == 0 { next }
        {
            pkg = $1
            if (match($0, /\[[^]]*\]/)) {
                spec = substr($0, RSTART + 1, RLENGTH - 2)
                n = split(spec, a, /[ \t]+/); ok = 0
                for (i = 1; i <= n; i++) if (a[i] == arch) ok = 1
                if (!ok) next
            }
            print pkg
        }' "$1"
}

PKGS=()
for name in $VARIANT_LISTS; do
    f="$NEXUS_ROOT/distro/base/package-lists/$name.list"
    [ -f "$f" ] || die "variant '$VARIANT' references missing package list $f"
    while read -r p; do PKGS+=("$p"); done < <(expand_list "$f")
done
[ "${#PKGS[@]}" -gt 0 ] || die "empty package set"
mapfile -t PKGS < <(printf '%s\n' "${PKGS[@]}" | sort -u)
log "variant '$VARIANT' for $ARCH: ${#PKGS[@]} explicitly selected Debian packages"

# --------------------------------------------------------------------------
# 1. Debian base layer (cached by content hash of its inputs)
# --------------------------------------------------------------------------
DPKG_EXCLUDES=()
if [ "${NEXUS_KEEP_DOCS:-0}" != "1" ]; then
    # Drop bulky documentation but keep every copyright file (licence
    # compliance) — the same trade-off Debian's own cloud/container images make.
    DPKG_EXCLUDES=(
        --dpkgopt='path-exclude=/usr/share/doc/*'
        --dpkgopt='path-include=/usr/share/doc/*/copyright'
        --dpkgopt='path-exclude=/usr/share/info/*'
        --dpkgopt='path-exclude=/usr/share/lintian/*'
    )
fi

BASE_KEY="$( { echo "$ARCH $NEXUS_DEBIAN_SUITE keepdocs=${NEXUS_KEEP_DOCS:-0}"; build_sources; printf '%s\n' "${PKGS[@]}"; } | sha256sum | cut -c1-16)"
BASE="$IMG/base-$BASE_KEY"

if [ "$FRESH" -eq 1 ] && [ -d "$BASE" ]; then
    safe_rmtree "$BASE"
fi
if [ ! -e "$BASE/.nexus-base-complete" ]; then
    for old in "$IMG"/base-*; do
        [ -d "$old" ] && { log "removing stale base layer $(basename "$old")"; safe_rmtree "$old"; }
    done
    log "bootstrapping Debian $NEXUS_DEBIAN_SUITE/$ARCH base layer (this downloads and unpacks every package)"
    include="$(IFS=,; echo "${PKGS[*]}")"
    mmdebstrap \
        --variant=minbase \
        --architectures="$ARCH" \
        --keyring="$(debian_keyring)" \
        --include="$include" \
        --aptopt='Acquire::Retries "5"' \
        --aptopt='APT::Install-Recommends "false"' \
        --aptopt='APT::Install-Suggests "false"' \
        --dpkgopt='force-unsafe-io' \
        "${DPKG_EXCLUDES[@]}" \
        "$NEXUS_DEBIAN_SUITE" "$BASE" - < <(build_sources)
    date -u +%Y-%m-%dT%H:%M:%SZ > "$BASE/.nexus-base-complete"
else
    log "reusing cached base layer $(basename "$BASE")"
fi

# --------------------------------------------------------------------------
# 2-4. NEXUS layer, squashfs and boot files — inside a private mount namespace
# --------------------------------------------------------------------------
safe_rmtree "$IMG/upper"; safe_rmtree "$IMG/ovl-work"; safe_rmtree "$IMG/merged"; safe_rmtree "$STAGING"
mkdir -p "$IMG/upper" "$IMG/ovl-work" "$IMG/merged" "$STAGING/live" "$STAGING/boot/grub" "$STAGING/.disk"

NEXUS_PKGS=""
if [ "$WITH_NEXUS" -eq 1 ] && [ -n "${VARIANT_NEXUS_PACKAGES:-}" ]; then
    [ -f "$REPO_DIR/dists/$NEXUS_CODENAME/InRelease" ] || die \
        "local NEXUS repository not found at $REPO_DIR — run 'make packages repo' first (or pass --no-nexus-packages)"
    NEXUS_PKGS="$VARIANT_NEXUS_PACKAGES"
fi

export ARCH VARIANT COMP IMG BASE STAGING REPO_DIR NEXUS_PKGS ISO_NAME
export NEXUS_NAME NEXUS_ID NEXUS_VERSION NEXUS_CODENAME NEXUS_CHANNEL NEXUS_DESKTOP \
       NEXUS_DEBIAN_SUITE NEXUS_DEBIAN_VERSION NEXUS_HOME_URL NEXUS_BUG_URL NEXUS_REPO_URL \
       NEXUS_VENDOR NEXUS_KERNEL_SERIES
unshare --mount --propagation private -- "$NEXUS_ROOT/build/iso/stage-rootfs.sh"

# --------------------------------------------------------------------------
# 5. ISO assembly
# --------------------------------------------------------------------------
"$NEXUS_ROOT/build/iso/assemble.sh"

ISO="$ISO_DIR/$ISO_NAME.iso"
log "image complete:"
ls -l "$ISO" "$ISO.sha256" "$ISO_DIR/$ISO_NAME.manifest" >&2
