#!/bin/bash
# Create (or refresh) the Debian build chroot used to compile NEXUS packages.
#
#   build/chroot/create.sh [--arch amd64|arm64] [--recreate] [--dev-tools]
#
#   --dev-tools  additionally install build/chroot/dev-tools.list: the runtime
#                tools and services used when running/testing apps in the chroot
#
# Packages are always compiled inside a pristine Debian <suite> chroot of the
# *target* architecture, never against the build host's libraries. For a
# foreign architecture the chroot runs under qemu-user (binfmt_misc), which is
# slow but guarantees that what we link against is what ships.

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

ARCH="$(host_arch)"
RECREATE=0
DEV_TOOLS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --recreate) RECREATE=1; shift ;;
        --dev-tools) DEV_TOOLS=1; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

need_root
need_cmd mmdebstrap
ensure_foreign_arch "$ARCH"
arch_info "$ARCH" gnu >/dev/null

CHROOT="$NEXUS_WORK/chroot-build-$ARCH"
DEPS_FILE="$NEXUS_ROOT/build/chroot/build-deps.list"

STAMP="$CHROOT/.nexus-bootstrap-complete"
if [ -d "$CHROOT" ] && [ "$RECREATE" -eq 1 ]; then
    log "removing existing chroot $CHROOT"
    safe_rmtree "$CHROOT"
elif [ -d "$CHROOT" ] && [ ! -e "$STAMP" ]; then
    warn "found an incomplete chroot (interrupted bootstrap) — recreating it"
    safe_rmtree "$CHROOT"
fi

# Build dependencies: one package per line, '#' comments allowed.
mapfile -t DEPS < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$DEPS_FILE")

if [ ! -d "$CHROOT" ]; then
    log "bootstrapping $NEXUS_DEBIAN_SUITE/$ARCH build chroot at $CHROOT"
    mkdir -p "$NEXUS_WORK"
    mmdebstrap \
        --variant=buildd \
        --architectures="$ARCH" \
        --keyring="$(debian_keyring)" \
        --include="ca-certificates,eatmydata" \
        --aptopt='Acquire::Retries "5"' \
        --aptopt='APT::Install-Recommends "false"' \
        --dpkgopt='force-unsafe-io' \
        --customize-hook='echo nexus-build > "$1/etc/hostname"' \
        "$NEXUS_DEBIAN_SUITE" "$CHROOT" - < <(build_sources)
    date -u +%Y-%m-%dT%H:%M:%SZ > "$STAMP"
else
    log "reusing existing chroot $CHROOT"
fi

log "installing ${#DEPS[@]} build dependencies"
in_chroot "$CHROOT" -- apt-get -q update
in_chroot "$CHROOT" -- eatmydata apt-get -q -y --no-install-recommends \
    -o Acquire::Retries=5 install "${DEPS[@]}"
if [ "$DEV_TOOLS" -eq 1 ]; then
    mapfile -t TOOLS < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$NEXUS_ROOT/build/chroot/dev-tools.list")
    log "installing ${#TOOLS[@]} development/test tools"
    # Nothing may be started inside the chroot.
    printf '#!/bin/sh\nexit 101\n' > "$CHROOT/usr/sbin/policy-rc.d"; chmod 755 "$CHROOT/usr/sbin/policy-rc.d"
    in_chroot "$CHROOT" -- eatmydata apt-get -q -y --no-install-recommends \
        -o Acquire::Retries=5 install "${TOOLS[@]}"
fi
in_chroot "$CHROOT" -- apt-get -q clean

log "build chroot ready: $CHROOT"
