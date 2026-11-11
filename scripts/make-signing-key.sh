#!/bin/bash
# Create the Shadow OS repository signing key — run this ONCE, on your own
# computer. It makes a key pair that signs every release so your installed
# system can trust online updates.
#
#   scripts/make-signing-key.sh
#
# It does three things:
#   1. writes the PUBLIC key to distro/signing/shadow-archive-keyring.asc
#      (commit this — it is what the installed system uses to verify updates),
#   2. prints the PRIVATE key for you to store as the GitHub Actions secret
#      named  SHADOW_SIGNING_KEY  (Settings -> Secrets and variables -> Actions),
#   3. never sends the private key anywhere — it only prints it in your terminal.
#
# NEVER commit the private key, paste it into a chat, or share it. Anyone with
# it can sign packages your machines will trust. If it leaks, make a new key
# with this script and replace the secret and the committed public key.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
command -v gpg >/dev/null || { echo "gpg is not installed (apt install gnupg)"; exit 1; }

NAME="Shadow OS release signing key"
EMAIL="shadow-os@users.noreply.github.com"
HOME_DIR="$(mktemp -d)"
trap 'rm -rf "$HOME_DIR"' EXIT
chmod 700 "$HOME_DIR"

echo "Generating an Ed25519 signing key (this is quick)..." >&2
gpg --homedir "$HOME_DIR" --batch --pinentry-mode loopback --passphrase '' \
    --quick-generate-key "$NAME <$EMAIL>" ed25519 sign 5y >/dev/null 2>&1
FPR="$(gpg --homedir "$HOME_DIR" --list-secret-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}')"

mkdir -p "$ROOT/distro/signing"
gpg --homedir "$HOME_DIR" --armor --export "$FPR" > "$ROOT/distro/signing/shadow-archive-keyring.asc"
echo "Wrote the PUBLIC key to distro/signing/shadow-archive-keyring.asc  (commit it)." >&2
echo "Key fingerprint: $FPR" >&2
echo >&2
echo "============================================================================" >&2
echo " COPY EVERYTHING BETWEEN THE LINES BELOW into a new GitHub Actions secret" >&2
echo " named  SHADOW_SIGNING_KEY  — then delete it from your screen/clipboard." >&2
echo "============================================================================" >&2
gpg --homedir "$HOME_DIR" --armor --export-secret-keys "$FPR"
echo "============================================================================" >&2
echo >&2
echo "Next: commit distro/signing/shadow-archive-keyring.asc, add the secret, and" >&2
echo "enable Pages (Settings -> Pages -> Source: GitHub Actions). See" >&2
echo "docs/packaging/online-updates.md." >&2
