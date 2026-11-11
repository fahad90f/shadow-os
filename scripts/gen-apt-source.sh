#!/bin/sh
# Prepare the contents of the shadow-archive-keyring package:
#   <out>/shadow-archive-keyring.gpg   public key of the SHADOW repository
#   <out>/shadow.sources               deb822 APT source for it
#
#   scripts/gen-apt-source.sh <output-directory>
#
# Inputs (environment):
#   SHADOW_ARCHIVE_KEY  path to the public key to ship. Defaults to the release
#                      key committed at distro/signing/shadow-archive-keyring.asc
#                      if present. Local/CI builds pass their development key.
#   SHADOW_REPO_URL     base URL of the published repository; empty → the source
#                      is installed but disabled.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: gen-apt-source.sh <output-directory>}"
# shellcheck source=../distro/base/shadow.conf
. "$ROOT/distro/base/shadow.conf"
mkdir -p "$OUT"

KEY="${SHADOW_ARCHIVE_KEY:-}"
if [ -z "$KEY" ] && [ -s "$ROOT/distro/signing/shadow-archive-keyring.asc" ]; then
    KEY="$ROOT/distro/signing/shadow-archive-keyring.asc"
fi
if [ -z "$KEY" ] || [ ! -s "$KEY" ]; then
    echo "gen-apt-source: no archive public key available." >&2
    echo "  Set SHADOW_ARCHIVE_KEY (build/packages/build.sh does this), or commit the" >&2
    echo "  release public key as distro/signing/shadow-archive-keyring.asc." >&2
    exit 1
fi
# Store the key de-armoured: every APT version reads binary keyrings.
if head -c 40 "$KEY" | grep -q "BEGIN PGP PUBLIC KEY BLOCK"; then
    sed -e '1,/^$/d' -e '/^-----END/,$d' -e '/^=/d' "$KEY" | base64 -d > "$OUT/shadow-archive-keyring.gpg"
else
    cp "$KEY" "$OUT/shadow-archive-keyring.gpg"
fi
[ -s "$OUT/shadow-archive-keyring.gpg" ] || { echo "gen-apt-source: empty keyring produced" >&2; exit 1; }

if [ -n "${SHADOW_REPO_URL:-}" ]; then
    ENABLED=yes
    URL="$SHADOW_REPO_URL"
    NOTE="# Shadow OS package repository."
else
    ENABLED=no
    # .invalid is reserved (RFC 6761) and never resolves.
    URL="https://repository.shadow-os.invalid/shadow"
    NOTE="# Shadow OS package repository — DISABLED: this image was built without a
# published repository URL (SHADOW_REPO_URL). SHADOW packages on this system
# are not updated through APT until a repository is configured: set URIs to
# the repository and Enabled to yes. Debian base and security updates are
# unaffected (see debian.sources)."
fi

cat > "$OUT/shadow.sources" <<SRC
$NOTE
Types: deb
URIs: $URL
Suites: $SHADOW_CODENAME
Components: core security community
Signed-By: /usr/share/keyrings/shadow-archive-keyring.gpg
Enabled: $ENABLED
SRC
