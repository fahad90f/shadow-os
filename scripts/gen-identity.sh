#!/bin/sh
# Render the files that identify the system as NEXUS OS from the single source
# of truth, distro/base/nexus.conf.
#
#   scripts/gen-identity.sh <output-directory>
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: gen-identity.sh <output-directory>}"
# shellcheck source=../distro/base/nexus.conf
. "$ROOT/distro/base/nexus.conf"
mkdir -p "$OUT"

# Capitalised codename for display ("obsidian" → "Obsidian").
DISPLAY_CODENAME="$(printf '%s' "$NEXUS_CODENAME" | awk '{ print toupper(substr($0,1,1)) substr($0,2) }')"

cat > "$OUT/os-release" <<OSR
PRETTY_NAME="$NEXUS_NAME $NEXUS_VERSION ($DISPLAY_CODENAME)"
NAME="$NEXUS_NAME"
VERSION_ID="$NEXUS_VERSION"
VERSION="$NEXUS_VERSION ($DISPLAY_CODENAME)"
VERSION_CODENAME=$NEXUS_CODENAME
ID=$NEXUS_ID
ID_LIKE=debian
DEBIAN_CODENAME=$NEXUS_DEBIAN_SUITE
HOME_URL="$NEXUS_HOME_URL"
SUPPORT_URL="$NEXUS_BUG_URL"
BUG_REPORT_URL="$NEXUS_BUG_URL"
LOGO=nexus-os
ANSI_COLOR="0;38;2;34;216;245"
NEXUS_BASE="Debian $NEXUS_DEBIAN_VERSION ($NEXUS_DEBIAN_SUITE)"
NEXUS_CHANNEL=$NEXUS_CHANNEL
NEXUS_DESKTOP="$NEXUS_DESKTOP"
OSR

printf '%s %s \\n \\l\n\n' "$NEXUS_NAME" "$NEXUS_VERSION" > "$OUT/issue"
printf '%s %s\n' "$NEXUS_NAME" "$NEXUS_VERSION" > "$OUT/issue.net"

cat > "$OUT/nexus-release" <<REL
# Machine-readable identity of this NEXUS OS release (shell syntax).
NEXUS_NAME="$NEXUS_NAME"
NEXUS_VERSION="$NEXUS_VERSION"
NEXUS_CODENAME="$NEXUS_CODENAME"
NEXUS_CHANNEL="$NEXUS_CHANNEL"
NEXUS_DEBIAN_SUITE="$NEXUS_DEBIAN_SUITE"
NEXUS_DEBIAN_VERSION="$NEXUS_DEBIAN_VERSION"
REL
