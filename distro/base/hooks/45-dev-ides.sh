# Pre-install developer IDEs and AI coding agents.
#
#   * VSCodium        — the de-telemetried build of VS Code (privacy friendly),
#                       from its own signed APT repository so it keeps updating.
#   * Claude Code     — Anthropic's terminal coding agent (npm).
#   * OpenAI Codex    — OpenAI's terminal coding agent (npm).
#   * Antigravity     — Google's agentic IDE; no stable Debian repo to bake, so
#                       a one-command installer is placed on the system instead.
#
# Everything here is BEST EFFORT: it reaches external networks (VSCodium repo,
# npm registry) which can be unavailable, so each step is guarded and the hook
# always exits 0 — a failed download must never break the image build. What
# actually installed is printed to the build log.
set +e
export DEBIAN_FRONTEND=noninteractive

log() { echo "[dev-ides] $*"; }
have() { command -v "$1" >/dev/null 2>&1; }

log "ensuring download helpers"
apt-get install -y --no-install-recommends ca-certificates curl gnupg >/dev/null 2>&1

# ---- VSCodium (APT repo) -----------------------------------------------------
log "installing VSCodium"
install -d -m 0755 /usr/share/keyrings
if curl -fsSL --retry 3 https://gitlab.com/paulcarroty/vscodium-deb-rpm-repo/raw/master/pub.gpg \
        | gpg --dearmor -o /usr/share/keyrings/vscodium-archive-keyring.gpg 2>/dev/null; then
    echo 'deb [arch=amd64,arm64 signed-by=/usr/share/keyrings/vscodium-archive-keyring.gpg] https://download.vscodium.com/debs vscodium main' \
        > /etc/apt/sources.list.d/vscodium.list
    apt-get update >/dev/null 2>&1
    if apt-get install -y codium >/dev/null 2>&1; then
        log "VSCodium installed ($(codium --version 2>/dev/null | head -1))"
    else
        log "VSCodium package failed to install (left the repo configured for later)"
    fi
else
    log "could not fetch the VSCodium signing key; skipping"
fi

# ---- Node.js + npm (for the AI agents) ---------------------------------------
if ! have node; then
    log "installing Node.js + npm"
    apt-get install -y --no-install-recommends nodejs npm >/dev/null 2>&1
fi

install_npm() {   # install_npm <pkg> <friendly name>
    log "installing $2 (npm $1)"
    if npm install -g --no-fund --no-audit "$1" >/dev/null 2>&1; then
        log "$2 installed"
    else
        log "$2 failed to install from npm"
    fi
}
if have npm; then
    npm config set fund false --global >/dev/null 2>&1
    install_npm "@anthropic-ai/claude-code" "Claude Code"
    install_npm "@openai/codex"            "OpenAI Codex"
else
    log "npm not available; skipping Claude Code and Codex"
fi

# ---- Google Antigravity (on-demand installer) --------------------------------
# No stable .deb/repo to bake reliably; ship a one-command installer + menu
# entry that fetches and installs the latest build when the user wants it.
install -d -m 0755 /usr/local/bin /usr/share/applications
cat > /usr/local/bin/install-antigravity <<'SH'
#!/bin/sh
# Download and install Google Antigravity (agentic IDE). Run once:
#   sudo install-antigravity
set -e
echo "Fetching Google Antigravity..."
url="https://storage.googleapis.com/antigravity/stable/linux-x64/antigravity-latest.deb"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if curl -fL --retry 3 -o "$tmp/antigravity.deb" "$url"; then
    apt-get install -y "$tmp/antigravity.deb" || dpkg -i "$tmp/antigravity.deb" || {
        echo "Install failed. See https://antigravity.google/ for the current download."; exit 1; }
    echo "Antigravity installed. Launch it from the menu or run 'antigravity'."
else
    echo "Could not download Antigravity automatically."
    echo "Get the current build from https://antigravity.google/ and install the .deb with:"
    echo "  sudo apt-get install ./<downloaded>.deb"
    exit 1
fi
SH
chmod 0755 /usr/local/bin/install-antigravity
cat > /usr/share/applications/shadow-install-antigravity.desktop <<'DESK'
[Desktop Entry]
Type=Application
Name=Install Google Antigravity
Comment=Download and install Google's Antigravity agentic IDE
Exec=pkexec /usr/local/bin/install-antigravity
Icon=applications-development
Terminal=true
Categories=Development;
DESK
log "Antigravity: one-command installer placed at /usr/local/bin/install-antigravity"

# Trim apt caches this hook pulled in.
apt-get clean >/dev/null 2>&1
log "done"
exit 0
