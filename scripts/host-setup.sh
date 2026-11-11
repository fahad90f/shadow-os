#!/bin/bash
# Install what a Debian or Ubuntu build host needs to build and test Shadow OS.
#
#   sudo scripts/host-setup.sh [--no-test-tools]
#
# Everything that is compiled or packaged runs inside Debian chroots created
# by build/chroot/create.sh; the host only needs the tools that create those
# chroots, assemble images, and (optionally) boot them in QEMU.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo $0)" >&2; exit 1; }

TEST_TOOLS=1
[ "${1:-}" = "--no-test-tools" ] && TEST_TOOLS=0

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    mmdebstrap arch-test debian-archive-keyring \
    squashfs-tools xorriso mtools dosfstools cpio zstd xz-utils rsync \
    reprepro gnupg git python3 ca-certificates curl
if [ "$TEST_TOOLS" -eq 1 ]; then
    apt-get install -y --no-install-recommends \
        qemu-system-x86 qemu-utils ovmf ipxe-qemu python3-pil imagemagick
fi
echo "host tools installed"
