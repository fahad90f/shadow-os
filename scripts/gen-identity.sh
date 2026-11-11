#!/bin/sh
# Render the files that identify the system as Shadow OS from the single source
# of truth, distro/base/shadow.conf.
#
#   scripts/gen-identity.sh <output-directory>
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: gen-identity.sh <output-directory>}"
# shellcheck source=../distro/base/shadow.conf
. "$ROOT/distro/base/shadow.conf"
mkdir -p "$OUT"

# Capitalised codename for display ("obsidian" → "Obsidian").
DISPLAY_CODENAME="$(printf '%s' "$SHADOW_CODENAME" | awk '{ print toupper(substr($0,1,1)) substr($0,2) }')"

cat > "$OUT/os-release" <<OSR
PRETTY_NAME="$SHADOW_NAME $SHADOW_VERSION ($DISPLAY_CODENAME)"
NAME="$SHADOW_NAME"
VERSION_ID="$SHADOW_VERSION"
VERSION="$SHADOW_VERSION ($DISPLAY_CODENAME)"
VERSION_CODENAME=$SHADOW_CODENAME
ID=$SHADOW_ID
ID_LIKE=debian
DEBIAN_CODENAME=$SHADOW_DEBIAN_SUITE
HOME_URL="$SHADOW_HOME_URL"
SUPPORT_URL="$SHADOW_BUG_URL"
BUG_REPORT_URL="$SHADOW_BUG_URL"
LOGO=shadow-os
ANSI_COLOR="0;38;2;34;216;245"
SHADOW_BASE="Debian $SHADOW_DEBIAN_VERSION ($SHADOW_DEBIAN_SUITE)"
SHADOW_CHANNEL=$SHADOW_CHANNEL
SHADOW_DESKTOP="$SHADOW_DESKTOP"
OSR

printf '%s %s \\n \\l\n\n' "$SHADOW_NAME" "$SHADOW_VERSION" > "$OUT/issue"
printf '%s %s\n' "$SHADOW_NAME" "$SHADOW_VERSION" > "$OUT/issue.net"

cat > "$OUT/shadow-release" <<REL
# Machine-readable identity of this Shadow OS release (shell syntax).
SHADOW_NAME="$SHADOW_NAME"
SHADOW_VERSION="$SHADOW_VERSION"
SHADOW_CODENAME="$SHADOW_CODENAME"
SHADOW_CHANNEL="$SHADOW_CHANNEL"
SHADOW_DEBIAN_SUITE="$SHADOW_DEBIAN_SUITE"
SHADOW_DEBIAN_VERSION="$SHADOW_DEBIAN_VERSION"
REL
