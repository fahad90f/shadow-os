#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shell's panic action (src/panic.h) against stand-ins for systemd,
# NetworkManager, logind and pkexec, without a compositor: a panic released
# elsewhere, a panic pressed while the banner's release waits for the
# administrator, and one pressed after the administrator authenticated (the
# release then runs as root). Builds test.cpp in the build chroot and runs
# it there (inner.sh).
#
#   sudo desktop/horizon-shell/tests/panic/run.sh
. "$(dirname "${BASH_SOURCE[0]}")/../../../../build/lib/common.sh"
need_root
CHROOT="$SHADOW_WORK/chroot-build-$(host_arch)"
[ -e "$CHROOT/.shadow-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh"
# Not in /tmp: the stand-in for an authorised pkexec is installed setuid
# root, which a nosuid /tmp would ignore.
WORK="$(mktemp -d -p "$SHADOW_WORK" panic-test.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
in_chroot "$CHROOT" --bind "$SHADOW_ROOT:/src:ro" --bind "$WORK:/work" -- \
    /bin/bash /src/desktop/horizon-shell/tests/panic/inner.sh /src /work
