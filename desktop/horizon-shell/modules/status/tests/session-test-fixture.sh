# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises the status module's NetworkManager, UPower and power-profiles
# back-ends (and what the panel and the popup make of them) against
# status-test-dbus-fixture on a PRIVATE bus that the shell is told is the
# system bus. The fixture follows the services' published D-Bus interfaces;
# it is not the services. Sourced in the nested session:
#
#   NEXUS_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- \
#       '. /src/desktop/horizon-shell/modules/status/tests/session-test-fixture.sh'
#
# Results: build/work/shots/nested-logs/modstatus/session-test-fixture.log,
# fixture.log (what the shell wrote to the "services"), and the screenshots
# build/work/shots/modstatus-f*.png.
: "${STATUS_TEST_SIZE:=1600}"
TESTS=/src/desktop/horizon-shell/modules/status/tests
LOG="$NEXUS_LOGS/session-test-fixture.log"
: > "$LOG"
failures=0
note() { echo "$*" | tee -a "$LOG"; }
check() {
    if [ "$2" = "$3" ]; then note "ok    $1 ($2)"
    else note "FAIL  $1: got '$2', expected '$3'"; failures=$((failures + 1)); fi
}
dump() { horizon-shell --dump status 2>/dev/null; }
field() { python3 -c 'import json,sys
value = json.load(sys.stdin)
for key in sys.argv[1].split("."):
    value = value[key]
print(json.dumps(value) if not isinstance(value, str) else value)' "$1"; }
NM=/org/freedesktop/NetworkManager
UP=/org/freedesktop/UPower
fx() { echo "set $*" > "$HOME/fixture.ctl"; sleep 0.3; }      # fx <path> <interface> <Property> <type> <json>
nm() { fx "$NM" org.freedesktop.NetworkManager "$@"; }
battery() { fx "$UP/devices/DisplayDevice" org.freedesktop.UPower.Device "$@"; }
client_sets() { grep -c "^client-set .* $1 $2\$" "$HOME/fixture.log"; }
right=$((STATUS_TEST_SIZE - 8 - 16))

# ---- a private "system" bus with the fixture on it ---------------------------------
dbus-daemon --session --address="unix:path=$HOME/system-bus" --nofork --nopidfile > "$HOME/system-bus.log" 2>&1 &
sleep 1
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$HOME/system-bus"
mkfifo "$HOME/fixture.ctl"
status-test-dbus-fixture "$TESTS/fixture-laptop.json" --control "$HOME/fixture.ctl" > "$HOME/fixture.log" 2>&1 &
fixture=$!
sleep 1
# A backlight the kernel does not have: read, but not settable without logind.
mkdir -p "$HOME/backlight/fixture_backlight"
echo 1000 > "$HOME/backlight/fixture_backlight/max_brightness"
echo 600 > "$HOME/backlight/fixture_backlight/brightness"
echo raw > "$HOME/backlight/fixture_backlight/type"
export NEXUS_STATUS_BACKLIGHT_DIR="$HOME/backlight"

horizon-shell > "$HOME/shell.log" 2>&1 &
sleep 8

# ---- Wi-Fi, battery on discharge, balanced profile ---------------------------------
dump > "$NEXUS_LOGS/dump-status-fixture.json"
D="$NEXUS_LOGS/dump-status-fixture.json"
check "network available" "$(field network.available < "$D")" true
check "network state" "$(field network.state < "$D")" connected
check "connectivity" "$(field network.connectivity < "$D")" full
check "primary kind" "$(field network.primary.kind < "$D")" wifi
check "Wi-Fi SSID (UTF-8 bytes)" "$(field network.wifi.ssid < "$D")" "Fixture Café"
check "Wi-Fi strength" "$(field network.wifi.strength < "$D")" 82
check "Wi-Fi connected" "$(field network.wifi.connected < "$D")" true
check "no VPN" "$(field network.vpn.active < "$D")" false
check "battery present" "$(field battery.present < "$D")" true
check "battery percentage" "$(field battery.percentage < "$D")" 67
check "battery state" "$(field battery.state < "$D")" discharging
check "seconds to empty" "$(field battery.secondsToEmpty < "$D")" 9240
check "on battery" "$(field battery.onBattery < "$D")" true
check "power profiles" "$(field powerProfiles.profiles < "$D")" '["power-saver", "balanced", "performance"]'
check "active profile" "$(field powerProfiles.activeProfile < "$D")" balanced
check "power profile service name" "$(field powerProfiles.service < "$D")" org.freedesktop.UPower.PowerProfiles
check "bluetooth: BlueZ absent" "$(field bluetooth.reason < "$D")" "The Bluetooth service (BlueZ) is not running."
check "brightness: device found" "$(field brightness.device < "$D")" fixture_backlight
check "brightness: percent" "$(field brightness.percent < "$D")" 60
check "brightness: not available without logind" "$(field brightness.available < "$D")" false
xdo key super+a; sleep 1.5
shot modstatus-f1-wifi-battery.png

# ---- changes announced with PropertiesChanged, popup open --------------------------
fx $NM/AccessPoint/7 org.freedesktop.NetworkManager.AccessPoint Strength y 30
nm ActiveConnections ao "[\"$NM/ActiveConnection/1\", \"$NM/ActiveConnection/2\", \"$NM/ActiveConnection/9\"]"
battery Percentage d 9
battery WarningLevel u 4
sleep 1
shot modstatus-f2-weak-vpn-critical.png
check "VPN (wireguard connection)" "$(dump | field network.vpn.name)" "Office tunnel"

battery State u 1
battery Percentage d 45
battery WarningLevel u 1
battery TimeToFull x 2700
fx $UP org.freedesktop.UPower OnBattery b false
nm Connectivity u 2
fx $NM/AccessPoint/7 org.freedesktop.NetworkManager.AccessPoint Strength y 55
sleep 1
shot modstatus-f3-charging-portal.png
check "battery charging" "$(dump | field battery.state)" charging
check "connectivity portal" "$(dump | field network.connectivity)" portal
nm Connectivity u 4

# ---- writes: power profile and Wi-Fi switch ------------------------------------------
xdo mousemove $((right - 40)) "${STATUS_TEST_PROFILE_Y:-250}" click 1; sleep 1
check "choosing Performance writes ActiveProfile" "$(client_sets ActiveProfile '"performance"')" 1
fx $UP/PowerProfiles org.freedesktop.UPower.PowerProfiles PerformanceDegraded s '"high-operating-temperature"'
sleep 0.8
shot modstatus-f4-performance-degraded.png

xdo mousemove $((right - 300)) 86 click 1; sleep 1
check "the Wi-Fi tile writes WirelessEnabled" "$(client_sets WirelessEnabled false)" 1
# What NetworkManager does next: the connection goes down.
fx $NM/Devices/3 org.freedesktop.NetworkManager.Device State u 20
fx $NM/Devices/3 org.freedesktop.NetworkManager.Device.Wireless ActiveAccessPoint o '"/"'
nm ActiveConnections ao "[\"$NM/ActiveConnection/9\"]"
nm PrimaryConnection o '"/"'
nm State u 20
sleep 1
shot modstatus-f5-wifi-off.png
check "disconnected" "$(dump | field network.state)" disconnected
check "Wi-Fi disabled" "$(dump | field network.wifi.enabled)" false

# ---- wired ---------------------------------------------------------------------------
fx $NM/Devices/2 org.freedesktop.NetworkManager.Device State u 100
nm ActiveConnections ao "[\"$NM/ActiveConnection/3\", \"$NM/ActiveConnection/9\"]"
nm PrimaryConnection o "\"$NM/ActiveConnection/3\""
nm State u 70
sleep 1
shot modstatus-f6-wired.png
check "wired" "$(dump | field network.primary.kind)" wired
check "wired connection name" "$(dump | field network.primary.name)" "Wired connection 1"

# ---- the services go away ------------------------------------------------------------
kill "$fixture"; sleep 1.5
shot modstatus-f7-services-gone.png
check "NetworkManager gone" "$(dump | field network.reason)" "NetworkManager is not running."
check "UPower gone" "$(dump | field battery.reason)" "UPower is not running."
xdo key Escape; sleep 0.5

cp "$HOME/shell.log" "$NEXUS_LOGS/shell-fixture.log"
cp "$HOME/fixture.log" "$NEXUS_LOGS/fixture.log"
note "shell warnings: $(grep -c -i 'warn\|TypeError\|ReferenceError' "$HOME/shell.log")"
note "FAILED: $failures"
