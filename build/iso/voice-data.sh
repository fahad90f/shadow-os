#!/bin/bash
# Bake the offline voice engine data of the Shadow Assistant into an image root.
#
#   build/iso/voice-data.sh ROOTFS
#
# The image's own shadow-voice-setup (package shadow-assistant) runs inside
# ROOTFS, so the data is unpacked with the image's Python and proven by the
# image's own engine (shadow-voiced --selftest) before it is kept. It goes to
# /var/lib/shadow-voice: the system data root the engine looks up, and the one
# "sudo shadow-voice-setup --system" checks, updates and removes on the
# installed system (the installer copies it along with the rest of the image).
#
# The pinned, sha256-verified artifacts (~700 MB) are downloaded into a cache
# on the BUILD HOST that is kept between builds, so only the first image build
# needs the network for them:
#   SHADOW_IMAGE_VOICE_CACHE  download cache (default: $SHADOW_CACHE/voice-engine).
#                             The setup downloads from inside the image, which
#                             only sees the proxy variables passed below and not
#                             the host's CA store: behind a TLS-intercepting proxy
#                             fill the cache beforehand with the files of any
#                             "shadow-voice-setup --keep-cache" run (they are
#                             verified against the pinned sha256 here anyway).
#   SHADOW_IMAGE_VOICE_STT    both (default) | moonshine | whisper
#
# Best effort, like the image's other downloads (distro/base/hooks/45-dev-ides.sh):
# when the data cannot be installed (offline, checksum mismatch, shadow-assistant
# not in the image, not amd64) it prints a warning, leaves nothing half-installed
# in ROOTFS and exits 0. The image then ships without the neural voice: the
# assistant speaks with eSpeak NG and offers the download in its settings.
# stage-rootfs.sh calls this unless build.sh --no-voice-data / SHADOW_IMAGE_VOICE=0.

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

[ $# -eq 1 ] || die "usage: build/iso/voice-data.sh ROOTFS"
need_root
ROOT=$(realpath -- "$1")
if [ ! -d "$ROOT/usr" ] || [ ! -d "$ROOT/var/lib" ]; then
    die "$1 is not an image root (no usr/ and var/lib/)"
fi

CACHE=${SHADOW_IMAGE_VOICE_CACHE:-$SHADOW_CACHE/voice-engine}
STT=${SHADOW_IMAGE_VOICE_STT:-both}
case "$STT" in moonshine|whisper|both) ;; *) die "SHADOW_IMAGE_VOICE_STT must be moonshine, whisper or both (not '$STT')" ;; esac
DEST=/var/lib/shadow-voice
SETUP=/usr/bin/shadow-voice-setup
LOGF=${IMG:-$SHADOW_WORK}/voice-data.log

skip() {
    warn "voice engine data NOT included in the image: $*"
    warn "the assistant will speak with eSpeak NG and offer the download (sudo shadow-voice-setup --system)"
    exit 0
}

[ -x "$ROOT$SETUP" ] || skip "$SETUP is not in the image (shadow-assistant not installed)"
# The pinned runtime wheels (sherpa-onnx, numpy) are x86_64 builds.
arch=$(in_chroot "$ROOT" -- dpkg --print-architecture 2>/dev/null) || arch=""
[ "$arch" = amd64 ] || skip "the pinned engine runtime exists for amd64 only (image: ${arch:-unknown})"
mkdir -p "$CACHE" || skip "cannot create the download cache $CACHE"
mkdir -p "$(dirname "$LOGF")"

# Proxy settings of the build environment reach the download (in_chroot clears
# the environment).
proxy_env=()
for v in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY; do
    if [ -n "${!v:-}" ]; then proxy_env+=("$v=${!v}"); fi
done

# --progress-json lines → one build-log line per step and per 10 % of the long
# steps, instead of the setup's two-per-second progress messages.
progress_to_log() {
    local line step prog msg last="" tenth prev=-1
    while IFS= read -r line; do
        step=$(sed -n 's/^{"step":"\([^"]*\)".*/\1/p' <<<"$line")
        prog=$(sed -n 's/.*"progress":\([0-9.]*\).*/\1/p' <<<"$line")
        msg=$(sed -n 's/.*"message":"\(\([^"\\]\|\\.\)*\)".*/\1/p' <<<"$line")
        printf '%s\n' "$line" >> "$LOGF"
        tenth=$(awk -v p="${prog:-0}" 'BEGIN { printf "%d", p * 10 }')
        if [ "$step" != "$last" ] || { [ "$tenth" != "$prev" ] && [ "$step" != verify ]; }; then
            log "  voice: $(awk -v p="${prog:-0}" 'BEGIN { printf "%3d%%", p * 100 }') $step: $msg"
            last=$step
            prev=$tenth
        fi
    done
}

had_dest=0
[ -e "$ROOT$DEST" ] && had_dest=1
: > "$LOGF"
log "voice engine data: installing into $DEST (speech recognition: $STT; download cache $CACHE)"
set +e
in_chroot "$ROOT" --bind "$CACHE:/run/shadow-voice-cache" -- \
    env "${proxy_env[@]}" "$SETUP" --dest "$DEST" --cache /run/shadow-voice-cache --keep-cache \
        --stt "$STT" --yes --progress-json 2>>"$LOGF" | progress_to_log
rc=${PIPESTATUS[0]}
set -e

# Running the engine as root (its selftest) leaves Python byte code next to
# modules that belong to packages or to the manifest; neither should ship.
find "$ROOT/usr/lib/shadow-assistant" "$ROOT$DEST" -name __pycache__ -type d -prune \
    -exec rm -rf {} + 2>/dev/null || true
# Leftovers of an interrupted setup (it normally removes them itself).
rm -rf "$ROOT$DEST".new.* "$ROOT$DEST".old.* "$(dirname "$ROOT$DEST")"/.shadow-voice-work.* 2>/dev/null || true

if [ "$rc" -eq 0 ]; then
    first_ms=$(grep -o '"first_audio_ms": *[0-9]*' "$LOGF" | tail -n 1 | grep -o '[0-9]*$' || true)
    log "voice engine data: $(du -sh "$ROOT$DEST" | cut -f1) in $DEST, selftest passed${first_ms:+ (first audio after $first_ms ms)}"
    exit 0
fi

case "$rc" in
    3) why="a dependency is missing in the image" ;;
    4) why="download failed (offline, or the proxy/TLS cannot be used from inside the image)" ;;
    5) why="checksum mismatch (the cached or downloaded files are not the pinned ones)" ;;
    6) why="not enough disk space" ;;
    9) why="installed, but the engine's selftest failed" ;;
    *) why="shadow-voice-setup exit code $rc" ;;
esac
# Never ship an engine that did not pass its own test, nor a partial tree.
if [ "$had_dest" = 0 ] && [ -e "$ROOT$DEST" ]; then rm -rf "${ROOT:?}$DEST"; fi
grep -v '^{' "$LOGF" | tail -n 5 | while IFS= read -r l; do warn "  $l"; done
skip "$why (log: $LOGF)"
