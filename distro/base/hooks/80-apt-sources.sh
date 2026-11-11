# Final APT configuration of the shipped system: exactly one Debian suite,
# over HTTPS, authenticated against the Debian archive keyring. No other
# distribution's repositories are ever enabled (docs/packaging/repositories.md).
rm -f /etc/apt/sources.list
cat > /etc/apt/sources.list.d/debian.sources <<SRC
# Debian ${SHADOW_DEBIAN_VERSION} "${SHADOW_DEBIAN_SUITE}" — the upstream base of Shadow OS.
Types: deb
URIs: https://deb.debian.org/debian
Suites: ${SHADOW_DEBIAN_SUITE} ${SHADOW_DEBIAN_SUITE}-updates
Components: main contrib non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp

Types: deb
URIs: https://deb.debian.org/debian-security
Suites: ${SHADOW_DEBIAN_SUITE}-security
Components: main contrib non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
SRC
[ -s /usr/share/keyrings/debian-archive-keyring.pgp ] || {
    echo "debian-archive-keyring.pgp missing from image" >&2; exit 1; }
