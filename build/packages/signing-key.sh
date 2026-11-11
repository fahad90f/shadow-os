#!/bin/bash
# Make sure a repository signing key exists and export its public half.
#
#   build/packages/signing-key.sh        → prints the key fingerprint
#
# Key material lives in $SHADOW_SIGNING_HOME (default: $SHADOW_WORK/signing), a
# GnuPG home directory OUTSIDE the repository. If it holds no secret key, a
# clearly labelled DEVELOPMENT key is generated there. Development keys make
# local and CI images installable and verifiable end to end; they are not
# release keys. Release signing: distro/signing/README.md.
#
# Output: $SHADOW_OUT/keys/shadow-archive-keyring.gpg (binary public key)

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"
need_cmd gpg

: "${SHADOW_SIGNING_HOME:=$SHADOW_WORK/signing}"
export SHADOW_SIGNING_HOME
mkdir -p "$SHADOW_SIGNING_HOME" "$SHADOW_OUT/keys"
chmod 700 "$SHADOW_SIGNING_HOME"

case "$(realpath "$SHADOW_SIGNING_HOME")/" in
    "$(realpath "$SHADOW_ROOT")"/build/work/*|"$(realpath "$SHADOW_ROOT")"/build/out/*) ;;   # ignored by git
    "$(realpath "$SHADOW_ROOT")"/*) die "SHADOW_SIGNING_HOME must not be inside the repository tree" ;;
esac

gpg_() { gpg --homedir "$SHADOW_SIGNING_HOME" --batch --no-tty "$@"; }

fpr="$(gpg_ --list-secret-keys --with-colons 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
if [ -z "$fpr" ]; then
    log "no signing key in $SHADOW_SIGNING_HOME — generating a DEVELOPMENT key"
    gpg_ --pinentry-mode loopback --passphrase '' --quick-generate-key \
        "Shadow OS development archive key (NOT A RELEASE KEY) <shadow-os@users.noreply.github.com>" \
        ed25519 sign 2y >/dev/null 2>&1
    fpr="$(gpg_ --list-secret-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}')"
    [ -n "$fpr" ] || die "key generation failed"
fi
gpg_ --export "$fpr" > "$SHADOW_OUT/keys/shadow-archive-keyring.gpg"
gpgconf --homedir "$SHADOW_SIGNING_HOME" --kill gpg-agent 2>/dev/null || true
[ -s "$SHADOW_OUT/keys/shadow-archive-keyring.gpg" ] || die "public key export failed"
echo "$fpr"
