# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises the shell's "status" module in the nested test session: a real
# KWin Wayland compositor and session bus, a real PipeWire sound server with
# a null output and a virtual microphone, and the MPRIS test player of this
# directory. Sourced by the session script of scripts/dev.sh:
#
#   NEXUS_JOBS=2 scripts/dev.sh build --dir modstatus --only horizon-shell --modules status
#   scripts/dev.sh exec -- cmake -S /src/desktop/horizon-shell/modules/status/tests -B /build/modstatus-tests -G Ninja
#   scripts/dev.sh exec -- ninja -C /build/modstatus-tests
#   NEXUS_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- \
#       '. /src/desktop/horizon-shell/modules/status/tests/session-test.sh'
#
# Results: build/work/shots/nested-logs/modstatus/session-test.log (one
# "ok"/"FAIL" line per check, "FAILED: n" at the end), dump-status.json, and
# the screenshots build/work/shots/modstatus-*.png.
#
# Not covered here, because the build chroot has neither a system bus nor
# hardware: NetworkManager, BlueZ, UPower, power-profiles-daemon, logind
# brightness, /dev/rfkill.
: "${STATUS_TEST_SIZE:=1600}"            # width of the nested screen
LOG="$NEXUS_LOGS/session-test.log"
: > "$LOG"
failures=0
note() { echo "$*" | tee -a "$LOG"; }
check() {   # check <description> <actual> <expected>
    if [ "$2" = "$3" ]; then note "ok    $1 ($2)"
    else note "FAIL  $1: got '$2', expected '$3'"; failures=$((failures + 1)); fi
}
volume() { pactl get-sink-volume @DEFAULT_SINK@ | grep -o '[0-9]*%' | head -n1; }
muted() { pactl get-sink-mute @DEFAULT_SINK@ | awk '{print $2}'; }
mic_muted() { pactl get-source-mute @DEFAULT_SOURCE@ | awk '{print $2}'; }
calls() { grep -c "^call $1 " "$HOME/player.log"; }
dump() { horizon-shell --dump status 2>/dev/null; }
field() { python3 -c 'import json,sys
value = json.load(sys.stdin)
for key in sys.argv[1].split("."):
    value = value[key]
print(json.dumps(value) if not isinstance(value, str) else value)' "$1"; }

# ---- a real sound server ---------------------------------------------------------
pipewire > "$HOME/pipewire.log" 2>&1 &
sleep 1
wireplumber > "$HOME/wireplumber.log" 2>&1 &
sleep 1
pipewire-pulse > "$HOME/pipewire-pulse.log" 2>&1 &
sleep 3
pactl load-module module-null-sink sink_name=nexus_test_out \
    sink_properties=device.description=Test-Speakers > /dev/null
pactl load-module module-null-sink media.class=Audio/Source/Virtual sink_name=nexus_test_mic \
    sink_properties=device.description=Test-Microphone > /dev/null
sleep 1
pactl set-default-sink nexus_test_out
pactl set-default-source nexus_test_mic
# PipeWire only applies a volume to a node that is in use: keep both busy.
pacat --playback /dev/zero > /dev/null 2>&1 &
pacat --record > /dev/null 2>&1 &
sleep 2
pactl set-sink-volume @DEFAULT_SINK@ 40%
pactl set-sink-mute @DEFAULT_SINK@ 0
pactl set-source-mute @DEFAULT_SOURCE@ 0
note "sound server: $(pactl info | sed -n 's/^Server Name: //p'); sink $(pactl get-default-sink); source $(pactl get-default-source)"

horizon-shell > "$HOME/shell.log" 2>&1 &
sleep 8

# ---- panel item and --dump status --------------------------------------------------
shot modstatus-1-panel.png
dump > "$NEXUS_LOGS/dump-status.json"
check "dump: audio available" "$(field audio.available < "$NEXUS_LOGS/dump-status.json")" true
check "dump: output device" "$(field audio.output.name < "$NEXUS_LOGS/dump-status.json")" nexus_test_out
check "dump: output volume" "$(field audio.output.volumePercent < "$NEXUS_LOGS/dump-status.json")" 40
check "dump: input device" "$(field audio.input.name < "$NEXUS_LOGS/dump-status.json")" nexus_test_mic
check "dump: media available, no player" "$(field media.hasPlayer < "$NEXUS_LOGS/dump-status.json")" false
check "dump: network unavailable without a system bus" "$(field network.available < "$NEXUS_LOGS/dump-status.json")" false
note "dump: nightLight $(field nightLight < "$NEXUS_LOGS/dump-status.json")"

# ---- volume, microphone and brightness keys, on-screen display --------------------
xdo key XF86AudioRaiseVolume; sleep 0.7
shot modstatus-2-osd-volume.png
check "volume key up" "$(volume)" 45%
xdo key XF86AudioLowerVolume; sleep 0.3; xdo key XF86AudioLowerVolume; sleep 0.5
check "volume key down twice" "$(volume)" 35%
xdo key XF86AudioMute; sleep 0.7
shot modstatus-3-osd-muted.png
check "mute key" "$(muted)" yes
xdo key XF86AudioRaiseVolume; sleep 0.5
check "volume up unmutes" "$(muted)" no
check "volume after unmute" "$(volume)" 40%
xdo key XF86AudioMicMute; sleep 0.7
shot modstatus-4-osd-mic.png
check "microphone mute key" "$(mic_muted)" yes
xdo key XF86AudioMicMute; sleep 0.5
check "microphone mute key again" "$(mic_muted)" no
xdo key XF86MonBrightnessUp; sleep 0.7
shot modstatus-5-osd-brightness-unavailable.png
xdo key XF86AudioPlay; sleep 0.7
shot modstatus-6-osd-no-player.png
sleep 2.2

# ---- quick settings ----------------------------------------------------------------
xdo key super+a; sleep 1.5
shot modstatus-7-quick-settings.png
# The volume slider follows a change made elsewhere while the popup is open.
pactl set-sink-volume @DEFAULT_SINK@ 80%; sleep 0.8
shot modstatus-8-quick-settings-80.png
# A click on the slider sets the volume and must not close the popup.
right=$((STATUS_TEST_SIZE - 8 - 16))
xdo mousemove $((right - 186)) 158 click 1; sleep 0.8
level="$(volume)"; level="${level%\%}"
check "click in the middle of the volume slider sets about 50 %" \
    "$([ "$level" -ge 44 ] && [ "$level" -le 56 ] && echo yes || echo "no: $level")" yes
xdo mousemove $((right - 354)) 158 click 1; sleep 0.8
check "mute button" "$(muted)" yes
shot modstatus-9-quick-settings-muted.png
xdo mousemove $((right - 354)) 158 click 1; sleep 0.8
check "mute button again" "$(muted)" no

# ---- media player ------------------------------------------------------------------
status-test-mpris-player --playing > "$HOME/player.log" 2>&1 &
sleep 1.5
shot modstatus-10-quick-settings-media.png
check "dump: player found" "$(dump | field media.current.title)" "First Light"
xdo mousemove $((right - 66)) 284 click 1; sleep 0.8
check "pause button of the media card reaches the player" "$(calls PlayPause)" 1
check "dump: player paused" "$(dump | field media.current.playbackStatus)" Paused
xdo key Escape; sleep 0.8
xdo key XF86AudioPlay; sleep 0.8
shot modstatus-11-osd-media.png
check "play key reaches the player" "$(calls PlayPause)" 2
check "dump: player playing" "$(dump | field media.current.playbackStatus)" Playing
xdo key XF86AudioNext; sleep 0.8
shot modstatus-12-osd-next.png
check "next key reaches the player" "$(calls Next)" 1
check "dump: second track" "$(dump | field media.current.title)" "Second Wind"
xdo key XF86AudioPrev; sleep 0.8
check "previous key reaches the player" "$(calls Previous)" 1
sleep 2

# ---- calendar ----------------------------------------------------------------------
xdo mousemove $((STATUS_TEST_SIZE / 2)) 16 click 1; sleep 1.5
shot modstatus-13-calendar.png
xdo key Right; sleep 0.6
shot modstatus-14-calendar-next-month.png
xdo key Escape; sleep 0.8

# ---- panel button, dark mode tile ---------------------------------------------------
xdo mousemove $((STATUS_TEST_SIZE - 80)) 16 click 1; sleep 1.5
shot modstatus-15-opened-by-click.png
xdo mousemove $((STATUS_TEST_SIZE - 130)) 86 click 1; sleep 1.5
shot modstatus-16-light.png
check "dark mode tile switches the appearance" \
    "$(sed -n 's/^ColorScheme=//p' "$HOME/.config/nexus/appearance.conf")" light
xdo mousemove $((STATUS_TEST_SIZE - 130)) 86 click 1; sleep 1
check "dark mode tile switches back" \
    "$(sed -n 's/^ColorScheme=//p' "$HOME/.config/nexus/appearance.conf")" dark
xdo key Escape; sleep 0.5

cp "$HOME/shell.log" "$NEXUS_LOGS/shell.log"
cp "$HOME/player.log" "$NEXUS_LOGS/player.log"
note "shell warnings: $(grep -c -i 'warn\|error\|TypeError\|ReferenceError' "$HOME/shell.log")"
note "FAILED: $failures"
