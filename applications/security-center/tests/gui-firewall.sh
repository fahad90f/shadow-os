#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# GUI round trip of the Firewall page, driven with xdotool inside the nested
# Shadow Horizon session (scripts/dev.sh nested). The application runs in the
# isolation of tests/isolated.sh, so the firewall it enables, opens ports in,
# resets and disables is that of a throw-away network namespace.
#
#   scripts/dev.sh nested --dir seccenter --size 1280x860 --wait 1 -- \
#       '. /src/applications/security-center/tests/gui-firewall.sh'
#
# Screenshots: build/work/shots/seccenter-fw-*.png; the state the tool reports
# after every step is appended to $SHADOW_LOGS/gui-firewall.log.
T=/src/applications/security-center/tests
LOG="$SHADOW_LOGS/gui-firewall.log"; : > "$LOG"
export SHADOW_SECURITY_STATUS_TOOL="$T/status-with-ruleset" SHADOW_FIREWALL_TOOL=/src/security/firewall/shadow-firewall
"$T/isolated.sh" shadow-security-center --page firewall > "$SHADOW_LOGS/gui-firewall-app.log" 2>&1 &
sleep 9
# The app (and the tools it starts) live in the namespaces of this process.
app="$(pgrep -n -x shadow-security-)"
state() { echo "== $1" >> "$LOG"; nsenter -t "$app" -n -m -r -w /src/security/firewall/shadow-firewall status --json >> "$LOG" 2>&1; }
click() { xdo mousemove "$1" "$2" click 1; sleep "${3:-1}"; }
state "start"
shot seccenter-fw-1-inactive.png
# --- enable (confirmation dialog) -------------------------------------------------
click 1176 266 2; shot seccenter-fw-2-enable-dialog.png
click 784 521 1.5; shot seccenter-fw-2b-enabled-toast.png; sleep 4; state "enable"
# --- open 22/tcp "SSH" ---------------------------------------------------------------
click 382 620; xdo type 22; click 830 620; xdo type "SSH"; sleep 1; click 1160 620 4; state "allow 22/tcp SSH"
# --- validation: 70000 is refused before anything is run -------------------
click 382 620; xdo type 70000; sleep 1; shot seccenter-fw-3-invalid.png
# --- open 6000-6010/udp ----------------------------------------------------------
xdo key ctrl+a; xdo type "6000-6010"; click 514 620 1; xdo key Down; sleep 0.5; xdo key Return; sleep 1
click 1160 620 4; state "allow 6000-6010/udp"; shot seccenter-fw-4-ports.png
# --- reload, then close 6000-6010/udp with its trash button -----------
click 1170 399 4; state "reload"
click 1202 572 1.5; shot seccenter-fw-5-removed-toast.png; sleep 3; state "remove 6000-6010/udp"
# --- close all ports (confirmation dialog) -------------------------------------
click 1183 468 2; shot seccenter-fw-6-reset-dialog.png
click 785 497 5; state "reset"
# --- disable (confirmation dialog) -----------------------------------------------
click 1174 184 2; shot seccenter-fw-7-disable-dialog.png
click 782 497 5; state "disable"; shot seccenter-fw-8-disabled.png
