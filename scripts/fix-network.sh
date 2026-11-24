#!/usr/bin/env bash
# fix-network.sh - get Wi-Fi (and Ethernet) on Shadow OS back on the internet,
# and prove that it works.
#
#     bash scripts/fix-network.sh [--yes] [--keep-hide-me] [--report-only]
#
# Run it from a terminal as your normal user; it asks for your password (sudo).
# Keep the network cable plugged in if Ethernet works: the script tests Wi-Fi
# on its own even while the cable is in. It ends with one verdict:
#   PASS            every connected network reaches the internet (exit 0)
#   FAIL            something still does not work; the summary says what, and
#                   the report to send is named at the end (exit 1)
#   NOT VERIFIED    Wi-Fi is not connected to a network, so it could not be
#                   tested; connect from the top bar and run again (exit 3)
#   RESTART NEEDED  a Wi-Fi driver setting was changed; restart the computer,
#                   then run this again (exit 4)
# Running it again is safe: what is already fixed is left alone.
#
#   --yes           no questions (Hide Me's Ghost Mode is turned off if no
#                   traffic gets through Tor). Without --yes and without a
#                   terminal to ask in, nothing Hide Me does is turned off.
#   --keep-hide-me  never change Hide Me (Ghost Mode, kill switch, Tor DNS,
#                   identity rotation)
#   --report-only   change nothing: only test and write the report
#
# What goes wrong on Shadow OS, and what this does about it:
#  1. The reverse-path filter was "strict" (/etc/sysctl.d/90-shadow-anon.conf).
#     With the cable and Wi-Fi both connected, every answer that arrives over
#     Wi-Fi is dropped, because the best way back is the cable: Wi-Fi shows
#     "connected" with no internet. -> "loose" mode, which still drops forged
#     addresses (the setting Shadow's own hardening file intends).
#  2. Wi-Fi took a NEW random hardware (MAC) address at every connection. Many
#     routers then block the "new device" (MAC filter, device limit, used-up
#     address pool), log-in pages forget you, and some Wi-Fi chips stop
#     receiving after the address changes. -> a private address per network
#     that stays the same ("stable"): still not your real address, and still
#     different on every network. Only if the network still fails is the real
#     address tried, for that one network, and kept only if it works.
#  3. Hide Me can leave the network unusable: identity rotation reconnects
#     Wi-Fi every few seconds; DNS-over-Tor locks /etc/resolv.conf to a Tor
#     port that no longer answers after a restart; Ghost Mode sends everything
#     through Tor, which some networks block. -> stopped or repaired, with your
#     say-so (or --yes); --keep-hide-me leaves Hide Me alone.
#     Hide Me's random hostname was not added to /etc/hosts (its AppArmor
#     profile did not allow it), so sudo says "unable to resolve host".
#     -> the hostname is mapped to 127.0.1.1 again.
#  4. A blocked Wi-Fi radio, Wi-Fi power saving, and (Realtek, MediaTek) chip
#     power-saving that drops the connection. -> unblocked / switched off, one
#     at a time, testing after each, keeping only what was needed.
#  5. DNS. Newer images look names up through systemd-resolved (encrypted
#     DNS; /etc/resolv.conf is a link to its stub, 127.0.0.53). If resolved is
#     not running, it is started; the link is never replaced by a file with
#     plain DNS servers. With strict DNS-over-TLS on a network that blocks
#     port 853, that one network may answer names itself (the change is
#     listed and undone by undo.sh); everywhere else DNS stays encrypted.
#  6. The kill switch (shadow-killswitch: nothing leaves except through Tor or
#     a VPN) and a panic (shadow-panic: cut off from every network) block
#     traffic on purpose. -> named as the cause, with the command that turns
#     them off. This script never turns them off (that would let traffic out
#     without Tor or the VPN) and never flushes the firewall. With a panic
#     engaged it changes nothing at all. A kill switch that Hide Me turned on
#     for Ghost Mode is Hide Me's (3. above).
#
# Everything is written to ~/.local/state/shadow-fix-network/ (only you can
# read it): last-run.log, report.txt (what to send if it still fails; no
# passwords, no hardware addresses) and backup-*/ with every file this changed
# and undo.sh, which puts the old settings back.

# shellcheck disable=SC2317  # functions called only through a trap
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -u
export LC_ALL=C.UTF-8 SYSTEMD_PAGER=cat

SCRIPT=$(readlink -f -- "${BASH_SOURCE[0]}")

YES=0
KEEP_HIDE_ME=0
REPORT_ONLY=0
for a in "$@"; do
    case "$a" in
        --yes|-y) YES=1 ;;
        --keep-hide-me) KEEP_HIDE_ME=1 ;;
        --report-only) REPORT_ONLY=1 ;;
        -h|--help) sed -n '2,/^$/p' "$SCRIPT"; exit 0 ;;
        *) echo "unknown option '$a' (see --help)" >&2; exit 2 ;;
    esac
done

# Everything below needs root (network settings, firewall tables, sysctl).
if [ "$(id -u)" != 0 ]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "fix-network: run this as root (sudo is not installed)" >&2
        exit 2
    fi
    echo "fix-network: asking for your password to change network settings..."
    exec sudo -- bash "$SCRIPT" "$@"
fi

# Test hooks: a root directory for every system file, and the user whose
# ~/.local/state receives the log. Never set on a real computer.
R=${SHADOW_NETFIX_ROOT:-}
INVOKER=${SUDO_USER:-root}
if [ -n "${SHADOW_NETFIX_STATE:-}" ]; then
    STATE_DIR=$SHADOW_NETFIX_STATE
else
    INVOKER_HOME=$(getent passwd "$INVOKER" | cut -d: -f6)
    STATE_DIR=${INVOKER_HOME:-/root}/.local/state/shadow-fix-network
fi
WAIT_SCALE=${SHADOW_NETFIX_WAIT:-1}     # tests shrink the waits

MARK='# Written by Shadow OS scripts/fix-network.sh.'
SYSCTL_FIX=$R/etc/sysctl.d/95-shadow-network.conf
NM_MAC_FIX=$R/etc/NetworkManager/conf.d/81-shadow-wifi-mac.conf
NM_DNS_LOCK=$R/etc/NetworkManager/conf.d/90-shadow-dns.conf
RESOLV=$R/etc/resolv.conf
RESOLV_ANON_BACKUP=$R/etc/resolv.conf.shadow-backup
NM_RESOLV=/run/NetworkManager/resolv.conf
# systemd-resolved (shadow-hardening): the stub link, the daemon, and Hide Me's
# runtime setting that sends its DNS to Tor.
RESOLVED_STUB=../run/systemd/resolve/stub-resolv.conf
RESOLVED_BIN=$R/usr/lib/systemd/systemd-resolved
RESOLVED_HIDE_ME=$R/run/systemd/resolved.conf.d/95-shadow-anon.conf
MODPROBE_FIX=$R/etc/modprobe.d/shadow-wifi-fix.conf
HOSTS=$R/etc/hosts
SYS_NET=$R/sys/class/net
PROC_SYS=$R/proc/sys
# Two independent connectivity checks (the same ones Debian and Firefox use);
# a network passes if either answers exactly as expected.
CHECK1_URL=http://network-test.debian.org/nm
CHECK1_BODY='NetworkManager is online'
CHECK2_URL=http://detectportal.firefox.com/success.txt
CHECK2_BODY=success
# Internet reachability without DNS: web servers of three public resolvers.
PROBE_IPS="1.1.1.1 8.8.8.8 9.9.9.9"

# ---- output -------------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
    C_OK=$'\033[32m' C_WARN=$'\033[33m' C_FAIL=$'\033[1;31m' C_STEP=$'\033[1;36m' C_OFF=$'\033[0m'
else
    C_OK="" C_WARN="" C_FAIL="" C_STEP="" C_OFF=""
fi

INTERACTIVE=0
if [ "$YES" = 0 ] && [ -t 0 ]; then INTERACTIVE=1; fi

mkdir -p "$STATE_DIR" && chmod 700 "$STATE_DIR"
[ "$INVOKER" = root ] || chown "$INVOKER": "$STATE_DIR" 2>/dev/null || true
LOG=$STATE_DIR/last-run.log
REPORT=$STATE_DIR/report.txt
: >"$LOG"; : >"$REPORT"
chmod 600 "$LOG" "$REPORT"
TS=$(date +%Y%m%d-%H%M%S)
BACKUP=$STATE_DIR/backup-$TS
UNDO=$BACKUP/undo.sh
CHANGED=()          # what this run changed, for the summary
RESULTS=()          # "STATUS|what|detail" for the summary
RESTART=0
RECONNECT_FAILED=0

# Everything printed also goes to the log, without colours.
exec > >(tee >(sed -u 's/\x1b\[[0-9;]*m//g' >>"$LOG")) 2>&1

say()   { printf '%s\n' "$*"; }
step()  { printf '\n%s== %s%s\n' "$C_STEP" "$*" "$C_OFF"; }
ok()    { printf '  %sOK%s    %s\n' "$C_OK" "$C_OFF" "$*"; }
info()  { printf '        %s\n' "$*"; }
warn()  { printf '  %sNOTE%s  %s\n' "$C_WARN" "$C_OFF" "$*"; }
bad()   { printf '  %sFAIL%s  %s\n' "$C_FAIL" "$C_OFF" "$*"; }
rep()   { printf '%s\n' "$*" >>"$REPORT"; }
have()  { command -v "$1" >/dev/null 2>&1; }
pause() { sleep "$(awk -v s="$1" -v k="$WAIT_SCALE" 'BEGIN { printf "%.2f", s * k }')"; }

ask() {   # ask "question" -> 0 for yes (the default); --yes is yes, no terminal to ask in is no
    if [ "$YES" = 1 ]; then return 0; fi
    if [ "$INTERACTIVE" = 0 ]; then
        info "not done without asking (no terminal; --yes allows it): $1"
        return 1
    fi
    local reply
    printf '  ?     %s [Y/n] ' "$1"
    read -r reply </dev/tty || reply=""
    case "$reply" in [nN]*) return 1 ;; *) return 0 ;; esac
}

undo_line() {   # remember how to put one change back (undo.sh runs them newest first)
    mkdir -p "$BACKUP"
    printf '%s\n' "$*" >>"$BACKUP/.undo-lines"
}
finish_undo() {
    [ -f "$BACKUP/.undo-lines" ] || return 0
    { printf '#!/bin/sh\n# Puts back what scripts/fix-network.sh changed on %s (run with sudo).\nset -x\n' "$TS"
      tac "$BACKUP/.undo-lines"
      printf 'nmcli general reload conf 2>/dev/null || systemctl reload NetworkManager\n'; } >"$UNDO"
    chmod 700 "$UNDO"
    rm -f "$BACKUP/.undo-lines"
    [ "$INVOKER" = root ] || chown -R "$INVOKER": "$BACKUP" 2>/dev/null
}
trap finish_undo EXIT
backup_file() {   # keep the original of a file before it changes
    local f=$1 rel
    rel=${f#"$R"}
    if [ -e "$f" ] || [ -L "$f" ]; then
        mkdir -p "$BACKUP/files$(dirname "$rel")"
        cp -a "$f" "$BACKUP/files$rel"
        undo_line "$(printf 'chattr -i %q 2>/dev/null; rm -f %q; cp -a %q %q' "$rel" "$rel" "$BACKUP/files$rel" "$rel")"
    else
        undo_line "$(printf 'rm -f %q' "$rel")"
    fi
}
changed()  { CHANGED+=("$*"); }

nm_reload() {   # make NetworkManager re-read its configuration files
    nmcli general reload conf >/dev/null 2>&1 || systemctl reload NetworkManager >/dev/null 2>&1 || true
    pause 1
}

# ---- what the system looks like -------------------------------------------------
# Devices NetworkManager knows: "iface|type|state|connection".
nm_devices() {
    nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null |
        awk -F: '$2 == "wifi" || $2 == "ethernet" { print $1 "|" $2 "|" $3 "|" $4 }'
}
active_uuid() {   # UUID of the profile active on a device
    nmcli -t -f UUID,DEVICE connection show --active 2>/dev/null |
        awk -F: -v d="$1" '$2 == d { print $1; exit }'
}
con_get() {   # one property of a profile; "\:" unescaped, "0 (default)" -> "0"
    local v
    v=$(nmcli -g "$2" connection show "$1" 2>/dev/null | head -n1)
    v=${v//\\:/:}
    case "$v" in [0-9]*" ("*")") v=${v%% *} ;; esac
    printf '%s' "$v"
}
ipv4_of() { ip -4 -o addr show dev "$1" scope global 2>/dev/null | awk '{ print $4; exit }'; }
gw_of()   { ip -4 route show default dev "$1" 2>/dev/null | awk '/via/ { for (i = 1; i <= NF; i++) if ($i == "via") { print $(i + 1); exit } }'; }
driver_of() {
    local l
    l=$(readlink "$SYS_NET/$1/device/driver" 2>/dev/null) || { echo unknown; return; }
    basename "$l"
}
mac_kind() {   # "real" when the device uses its burnt-in address, else "private"
    local link
    link=$(ip link show dev "$1" 2>/dev/null)
    case "$link" in *permaddr*) echo private ;; *) echo real ;; esac
}
sysctl_get() { cat "$PROC_SYS/$(echo "$1" | tr . /)" 2>/dev/null; }

nat_chain_hooked() {   # is a Hide Me iptables chain in use?
    have iptables || return 1
    iptables -t "$1" -S "$2" >/dev/null 2>&1 || return 1
    iptables -t "$1" -S OUTPUT 2>/dev/null | grep -q -- "-j $2\b"
}
rotation_on() { systemctl is-active --quiet shadow-anon-rotate.timer 2>/dev/null || systemctl is-enabled --quiet shadow-anon-rotate.timer 2>/dev/null; }
# Hide Me's kill switch: its old iptables chain, or the Shadow kill switch in
# the Tor mode Hide Me turned on (egress_probe sets KS_HIDE_ME).
kill_switch_on()   { nat_chain_hooked filter SHADOWKILL || [ "${KS_HIDE_ME:-0}" = 1 ]; }
tproxy_on()        { nat_chain_hooked nat SHADOWTOR; }
tor_dns_redirect() { nat_chain_hooked nat SHADOWDNS || nat_chain_hooked nat SHADOWTOR; }
resolv_is_tor() { grep -q 'DNS routed through Tor' "$RESOLV" 2>/dev/null; }

# The Shadow kill switch (shadow-killswitch, nftables table inet shadow_egress)
# and a panic (shadow-panic, inet shadow_panic). Only read here: this script
# never turns either off, never runs "nft" other than "list table", never
# flushes the ruleset.
HIDE_ME_STATE=$R/run/shadow-anon-state.json
json_fields() {   # json_fields KEY... <JSON -> "v1|v2|..." (true/false, text, a list's length, "" if missing)
    have python3 || return 1
    python3 -c 'import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    d = None
d = d if isinstance(d, dict) else {}
def f(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return ""
    if isinstance(v, (list, dict)):
        return str(len(v))
    return " ".join(str(v).replace("|", "/").split())
print("|".join(f(d.get(k)) for k in sys.argv[1:]))' "$@" 2>/dev/null
}
egress_probe() {   # -> KS_TABLE (1: inet shadow_egress is in the kernel), KS_MODE, KS_LAN, KS_ERROR, KS_HATCH, KS_HIDE_ME
    KS_TABLE=0 KS_MODE="" KS_LAN="" KS_ERROR="" KS_HATCH="" KS_HIDE_ME=0
    local json active="" loaded="" text by
    if have shadow-killswitch; then
        # As root it re-checks the kernel (exit 0, or 3 when something is wrong).
        json=$(shadow-killswitch status --json 2>/dev/null)
        IFS='|' read -r KS_MODE active loaded KS_LAN KS_ERROR KS_HATCH \
            < <(printf '%s' "$json" | json_fields mode active loaded lan error escape_hatch)
        [ "$loaded" = true ] && KS_TABLE=1
        # On, but not the table its configuration produces: a table changed by hand still blocks.
        if [ "$active" = true ] && [ "$loaded" != true ] && have nft &&
           nft list table inet shadow_egress >/dev/null 2>&1; then KS_TABLE=1; fi
    elif have nft && text=$(nft list table inet shadow_egress 2>/dev/null); then
        KS_TABLE=1
        KS_MODE=$(printf '%s\n' "$text" | sed -n 's/.*comment "shadow-killswitch mode=\([a-z-]*\).*/\1/p' | head -n1)
        case "$(printf '%s\n' "$text" | sed -n 's/.*comment "shadow-killswitch .*lan=\(yes\|no\).*/\1/p' | head -n1)" in
            yes) KS_LAN=true ;; no) KS_LAN=false ;;
        esac
        [ -n "$KS_MODE" ] || KS_MODE=unknown
    fi
    # Turned on by Hide Me (Ghost Mode, its kill switch, the transparent proxy):
    # Hide Me's own "off" puts back the mode that was in force before it.
    if [ "$KS_TABLE" = 1 ] && [ "$KS_MODE" = tor ] && [ -s "$HIDE_ME_STATE" ]; then
        by=$(json_fields killswitch_by <"$HIDE_ME_STATE")
        case "$by" in ""|0) ;; *) KS_HIDE_ME=1 ;; esac
    fi
    return 0
}
panic_probe() {   # -> PANIC_ON (1: a panic is engaged or its table is loaded), PANIC_TABLE (1: inet shadow_panic is loaded)
    PANIC_ON=0 PANIC_TABLE=0
    local json engaged="" loaded=""
    if have shadow-panic; then
        json=$(shadow-panic status --json 2>/dev/null)
        IFS='|' read -r engaged loaded < <(printf '%s' "$json" | json_fields engaged loaded)
        [ "$loaded" = true ] && PANIC_TABLE=1
        [ "$engaged" = true ] && PANIC_ON=1
    fi
    if [ "$PANIC_TABLE" = 0 ] && have nft && nft list table inet shadow_panic >/dev/null 2>&1; then PANIC_TABLE=1; fi
    [ "$PANIC_TABLE" = 1 ] && PANIC_ON=1
    return 0
}
ks_way() {   # the way out the kill switch in force leaves open
    case "$KS_MODE" in
        tor) echo "Tor" ;;
        vpn) echo "the VPN" ;;
        tor-or-vpn) echo "Tor or the VPN" ;;
        *) echo "its rules" ;;
    esac
}
ks_lets() {   # "the kill switch lets out only ..."
    case "$KS_MODE" in
        tor) echo "Tor's traffic" ;;
        vpn) echo "traffic through the VPN" ;;
        tor-or-vpn) echo "Tor's traffic and traffic through the VPN" ;;
        off) echo "loopback traffic (its table is loaded although the kill switch is off)" ;;
        *) echo "what its rules allow (its mode is not known)" ;;
    esac
}
ks_advice() {   # the lines that tell how to get out of a blocking kill switch (never done by this script)
    case "$KS_MODE" in
        tor|tor-or-vpn) say "  - is Tor running and connected? systemctl status tor@default (Hide Me shows it too)" ;;
    esac
    case "$KS_MODE" in
        vpn|tor-or-vpn) say "  - connect the VPN (top bar or Network Center); a VPN server the kill switch does not know is blocked too (docs/security/kill-switch.md, \"VPNs and NetworkManager\")" ;;
    esac
    case "$KS_MODE" in
        tor|vpn|tor-or-vpn)
            say "  - or turn the kill switch off until the next restart, knowing that programs then reach the internet without $(ks_way):"
            say "      pkexec shadow-killswitch set off --runtime     (or Control Center -> Privacy -> Network kill switch)" ;;
        off)
            say "  - its table is loaded although the kill switch is off; remove it with:"
            say "      pkexec shadow-killswitch set off" ;;
        *)
            if have shadow-killswitch; then
                # A table deleted with nft is put back within ~20 s by shadow-killswitch-watch.timer
                # while a mode is recorded; "set off" records the change, so it stays off.
                say "  - its table (inet shadow_egress) is loaded but shadow-killswitch cannot say which mode; turn it off with:"
                say "      sudo shadow-killswitch set off --runtime     (until the next restart; without --runtime also after it)"
            else
                say "  - its table (inet shadow_egress) is loaded but shadow-killswitch is not installed; remove it with:"
                say "      sudo nft delete table inet shadow_egress; sudo nft delete table bridge shadow_egress"
            fi ;;
    esac
}
resolv_immutable() { lsattr -d "$RESOLV" 2>/dev/null | awk '{ exit !($1 ~ /i/) }'; }
listening() {   # listening ip port -> 0 if a UDP socket is bound to ip:port (or to every address)
    have ss || return 1
    ss -lnuH "sport = :$2" 2>/dev/null | awk -v ip="$1" -v port="$2" '
        { a = $4; sub(/%[^:]*:/, ":", a) }
        a == ip ":" port || a == "*:" port || a == "0.0.0.0:" port || a == "[::]:" port { found = 1 }
        END { exit !found }'
}
something_on_53() { listening 127.0.0.1 53; }      # what a resolv.conf with "nameserver 127.0.0.1" reaches
hide_me_tor_port() {   # Tor's DNSPort as Hide Me's drop-in names it (DNS=127.0.0.1:PORT; shadow-anon's DNS_PORT)
    local port
    port=$(sed -n 's/^DNS=127\.0\.0\.1:\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$RESOLVED_HIDE_ME" 2>/dev/null | tail -n 1)
    echo "${port:-26}"
}
# systemd-resolved
resolved_installed() { [ -e "$RESOLVED_BIN" ]; }
resolved_active() { systemctl is-active --quiet systemd-resolved.service 2>/dev/null; }
resolv_is_stub() {
    [ -L "$RESOLV" ] || return 1
    case "$(readlink "$RESOLV")" in ../run/systemd/resolve/stub-resolv.conf|/run/systemd/resolve/stub-resolv.conf) return 0 ;; esac
    return 1
}
resolved_prop() {   # resolved_prop DNSOverTLS -> yes | opportunistic | no | (empty: unknown)
    busctl get-property org.freedesktop.resolve1 /org/freedesktop/resolve1 \
        org.freedesktop.resolve1.Manager "$1" 2>/dev/null | sed -n 's/^s "\(.*\)"$/\1/p'
}
global_dns_ip() {   # first IPv4 address of resolved's global DNS servers (the provider)
    resolvectl dns 2>/dev/null | awk '$1 == "Global:" {
        for (i = 2; i <= NF; i++) { ip = $i; sub(/#.*/, "", ip); sub(/:[0-9]+$/, "", ip)
            if (ip ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/) { print ip; exit } } }'
}
port853_open() {   # port853_open iface ip -> 0 if a TCP connection to ip:853 opens over iface
    local t
    t=$(curl -s -o /dev/null --connect-timeout 5 -m 8 --interface "$1" -w '%{time_connect}' "https://$2:853/" 2>/dev/null)
    awk -v t="${t:-0}" 'BEGIN { exit !(t + 0 > 0) }'
}

# ---- tests ----------------------------------------------------------------------
# Each prints one line of findings and returns 0 when the test passed.
http_check() {   # http_check [iface] -> 0 if a connectivity check answers correctly
    local bind=() body code url want
    [ -n "${1:-}" ] && bind=(--interface "$1")
    for pair in "$CHECK1_URL|$CHECK1_BODY" "$CHECK2_URL|$CHECK2_BODY"; do
        url=${pair%%|*}; want=${pair#*|}
        body=$(curl -sS -m 8 "${bind[@]}" -w '\n%{http_code}' "$url" 2>/dev/null) || continue
        code=${body##*$'\n'}; body=${body%$'\n'*}
        if [ "$code" = 200 ] && [[ "$(printf '%s' "$body" | tr -d '\r\n')" == "$want"* ]]; then
            HTTP_NOTE="answers"
            return 0
        fi
        HTTP_NOTE="answered with something else (HTTP $code): a log-in page?"
        PORTAL=1
    done
    [ -n "${HTTP_NOTE:-}" ] || HTTP_NOTE="no answer"
    return 1
}
tcp_check() {   # tcp_check iface -> 0 if a web server on the internet answers by IP
    local ip code
    for ip in $PROBE_IPS; do
        code=$(curl -sS -m 6 --interface "$1" -o /dev/null -w '%{http_code}' "http://$ip/" 2>/dev/null) || continue
        [ "$code" != 000 ] && return 0
    done
    return 1
}
gw_reachable() {   # gw_reachable iface gw -> 0 if the router answers (ping or ARP)
    ping -q -c 2 -W 2 -I "$1" "$2" >/dev/null 2>&1 && return 0
    ip neigh show "$2" dev "$1" 2>/dev/null | grep -qE 'REACHABLE|STALE|DELAY|PROBE'
}
dns_check() { getent ahosts deb.debian.org >/dev/null 2>&1 || getent ahosts debian.org >/dev/null 2>&1; }
# Tor's SOCKS port on loopback, which the kill switch lets through: shadow-anon's
# TOR_SOCKS_PORT (/etc/tor/torrc.d/00-shadow-socks.conf), below 1024 so only
# root may listen there (Tor binds it as root). Debian's 9050 is not Tor's any
# more: any local user may listen there and answer for Tor.
TOR_SOCKS_PORT=39
TOR_SOCKS=127.0.0.1:$TOR_SOCKS_PORT
tor_socks_is_tors() {   # -> 0 if tor@default runs and the listener on Tor's SOCKS port is root's (or debian-tor's)
    local line tor_uid
    systemctl is-active --quiet tor@default 2>/dev/null || return 1
    have ss || return 1
    line=$(ss -Hltne "sport = :$TOR_SOCKS_PORT" 2>/dev/null | awk -v a="$TOR_SOCKS" '$4 == a { print; exit }')
    [ -n "$line" ] || return 1
    case " $line " in
        *" uid:"*)   # ss prints no uid for root's sockets
            tor_uid=$(id -u debian-tor 2>/dev/null) || return 1
            [[ " $line " == *" uid:$tor_uid "* ]] ;;
        *) return 0 ;;
    esac
}
tor_check() {   # tor_check -> 0 if a connectivity check answers through Tor (names resolved by Tor)
    local body code
    tor_socks_is_tors || return 1         # never an answer from whoever else listens there
    body=$(curl -sS -m 30 --socks5-hostname "$TOR_SOCKS" -w '\n%{http_code}' "$CHECK1_URL" 2>/dev/null) || return 1
    code=${body##*$'\n'}; body=${body%$'\n'*}
    [ "$code" = 200 ] && [[ "$(printf '%s' "$body" | tr -d '\r\n')" == "$CHECK1_BODY"* ]]
}

# test_iface iface -> sets T_RESULT (pass|noaddr|nogw|noinet|portal) and T_TEXT
test_iface() {
    local ifc=$1 addr gw
    PORTAL=0 HTTP_NOTE=""
    addr=$(ipv4_of "$ifc")
    if [ -z "$addr" ]; then
        T_RESULT=noaddr; T_TEXT="no IPv4 address: the router's DHCP did not give one"
        return 1
    fi
    case "$addr" in 169.254.*) T_RESULT=noaddr; T_TEXT="only a self-assigned address ($addr): DHCP failed"; return 1 ;; esac
    gw=$(gw_of "$ifc")
    if [ -z "$gw" ]; then
        T_RESULT=nogw; T_TEXT="address $addr but no default route (gateway) on $ifc"
        return 1
    fi
    if http_check "$ifc"; then
        T_RESULT=pass; T_TEXT="address $addr, the internet answers"
        return 0
    fi
    if [ "$PORTAL" = 1 ]; then
        T_RESULT=portal; T_TEXT="this network shows a log-in page first: open http://neverssl.com in the browser and sign in"
        return 1
    fi
    if tcp_check "$ifc"; then
        T_RESULT=nodns; T_TEXT="the internet answers over $ifc, but names cannot be looked up (DNS)"
    elif gw_reachable "$ifc" "$gw"; then
        T_RESULT=noinet; T_TEXT="the router answers but nothing on the internet does over $ifc"
    else
        T_RESULT=nogw; T_TEXT="address $addr but the router does not answer over $ifc"
    fi
    return 1
}

reconnect() {   # reconnect iface uuid -> 0 when the profile is active again
    # In a terminal nmcli may ask for the Wi-Fi password if NetworkManager
    # does not have it (a network saved "for this user only").
    local rc
    if [ "$INTERACTIVE" = 1 ]; then
        nmcli --ask --wait 45 connection up "$2" ifname "$1" 2>>"$LOG"; rc=$?
    else
        nmcli --wait 45 connection up "$2" ifname "$1" >/dev/null 2>>"$LOG"; rc=$?
    fi
    if [ "$rc" != 0 ]; then
        RECONNECT_FAILED=1
        return 1
    fi
    # NetworkManager reports "activated" as soon as IPv6 works; the IPv4
    # address from DHCP can follow much later (it tries for 45 seconds, and
    # phone hotspots are slow). Wait for it before testing.
    local waited=0
    while [ -z "$(gw_of "$1")" ] && [ "$waited" -lt 40 ]; do
        [ "$waited" = 3 ] && info "waiting for an address from the router (up to 40 seconds)..."
        pause 1
        waited=$((waited + 1))
    done
    return 0
}

# ---- report (sent by the user when something still fails) ----------------------
snapshot() {   # snapshot "title"
    rep ""
    rep "---- $1 ----"
    rep "networkmanager: $(nmcli -t -f STATE,CONNECTIVITY,WIFI-HW,WIFI general 2>/dev/null)"
    local line ifc type state con
    while IFS='|' read -r ifc type state con; do
        [ -n "$ifc" ] || continue
        rep "$type $ifc: state=$state profile=${con:+yes} driver=$(driver_of "$ifc") mac=$(mac_kind "$ifc") ipv4=$(ipv4_of "$ifc" | sed 's#\.[0-9]*/#.x/#') gw=$( [ -n "$(gw_of "$ifc")" ] && echo yes || echo no)"
    done < <(nm_devices)
    rep "routes: $(ip -4 route show default 2>/dev/null | sed -E 's/via [0-9.]+/via GW/g; s/src [0-9.]+/src X/g' | tr '\n' ';')"
    rep "rp_filter all/default: $(sysctl_get net.ipv4.conf.all.rp_filter)/$(sysctl_get net.ipv4.conf.default.rp_filter)"
    rep "resolv.conf: $(grep -E '^nameserver' "$RESOLV" 2>/dev/null | awk '{ print $2 }' | sed -E 's/^(192\.168|10|172)\..*/private-LAN/' | tr '\n' ' ') immutable=$(lsattr -d "$RESOLV" 2>/dev/null | cut -c5 | grep -q i && echo yes || echo no) link=$( [ -L "$RESOLV" ] && readlink "$RESOLV" || echo no)"
    if resolved_installed; then
        rep "resolver: systemd-resolved $(resolved_active && echo running || echo stopped) stub=$(listening 127.0.0.53 53 && echo listening || echo silent) dot=$(resolved_prop DNSOverTLS) dnssec=$(resolved_prop DNSSEC) hide-me-tor=$( [ -e "$RESOLVED_HIDE_ME" ] && echo on || echo off)"
    else
        rep "resolver: none (NetworkManager writes resolv.conf)"
    fi
    egress_probe; panic_probe
    rep "killswitch: mode=${KS_MODE:-none} table=$( [ "$KS_TABLE" = 1 ] && echo loaded || echo none) lan=${KS_LAN:-?} by-hide-me=$( [ "$KS_HIDE_ME" = 1 ] && echo yes || echo no) escape-hatch=${KS_HATCH:-?} problem=$( [ -n "$KS_ERROR" ] && echo yes || echo no)"
    rep "panic: $( [ "$PANIC_ON" = 1 ] && echo engaged || echo no) table=$( [ "$PANIC_TABLE" = 1 ] && echo loaded || echo none)"
    rep "hide-me: rotation=$(rotation_on && echo on || echo off) killswitch=$(kill_switch_on && echo on || echo off) tproxy=$(tproxy_on && echo on || echo off) tor-dns=$(tor_dns_redirect && echo on || echo off)"
    for line in $(rfkill -n -o TYPE,SOFT,HARD 2>/dev/null | awk '$1 == "wlan" { print $2 "/" $3 }'); do rep "rfkill wlan soft/hard: $line"; done
    rep "dns: $(dns_check && echo works || echo fails)"
}
report_nm_log() {   # NetworkManager's last Wi-Fi / DHCP lines, hardware addresses masked
    local ifc
    rep ""
    rep "---- NetworkManager log (Wi-Fi and DHCP, this run) ----"
    for ifc in "${WIFI_IFS[@]}"; do
        journalctl -b --since "$START_TIME" --no-pager -u NetworkManager 2>/dev/null |
            grep -E "$ifc|dhcp4" | tail -n 60 |
            sed -E 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/XX:XX:XX:XX:XX:XX/g; s/(client-id|duid|iaid)[ =:]+[^ ,]+/\1=X/g' >>"$REPORT"
    done
}

# ================================================================================
START_TIME=$(date -d '-10 min' '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date '+%Y-%m-%d %H:%M:%S')
say "Shadow OS network fix - $(date '+%Y-%m-%d %H:%M')"
say "Log: $LOG"
rep "Shadow OS network fix report $TS"
# shellcheck disable=SC1091
rep "os: $(. "$R/etc/os-release" 2>/dev/null; echo "${PRETTY_NAME:-unknown}") kernel: $(uname -r)"

for tool in nmcli ip curl; do
    if ! have "$tool"; then
        bad "$tool is missing: this needs NetworkManager, iproute2 and curl (all part of Shadow OS)"
        exit 1
    fi
done
if ! nmcli -t general status >/dev/null 2>&1; then
    warn "NetworkManager is not running - starting it"
    systemctl start NetworkManager 2>/dev/null; pause 5
    nmcli -t general status >/dev/null 2>&1 || { bad "NetworkManager does not start: journalctl -u NetworkManager -b"; exit 1; }
fi

snapshot "before"

# ---- 1. look ----------------------------------------------------------------------
step "1/5  What is connected"
WIFI_IFS=() ETH_IFS=()
while IFS='|' read -r ifc type state con; do
    [ -n "$ifc" ] || continue
    if [ "$type" = wifi ]; then WIFI_IFS+=("$ifc"); else ETH_IFS+=("$ifc"); fi
    info "$(printf '%-9s %-11s %-28s driver %s' "$type" "$ifc" "$state${con:+ ($con)}" "$(driver_of "$ifc")")"
done < <(nm_devices)
if [ ${#WIFI_IFS[@]} = 0 ]; then
    warn "no Wi-Fi device found (no driver or firmware for the Wi-Fi chip?)"
    rep "no wifi device"
fi

# ---- 2. system-wide causes --------------------------------------------------------
step "2/5  Shadow OS settings that break the network"

# 2.0 A panic or the kill switch block traffic on purpose. A panic: nothing is
#     tested or changed (turning the radio or NetworkManager back on would
#     undo part of it). The kill switch: named here, never turned off.
panic_probe
if [ "$PANIC_ON" = 1 ]; then
    bad "a panic is engaged: this computer is cut off from every network on purpose"
    [ "$PANIC_TABLE" = 1 ] || info "(its firewall table is missing right now; the kill switch watchdog puts it back within 20 seconds)"
    info "nothing can reach a network until it is released, so nothing was tested or changed"
    RESULTS+=("FAIL|panic|engaged: every network is cut off on purpose")
    for r in "${RESULTS[@]}"; do rep "result: $r"; done
    [ "$INVOKER" = root ] || chown "$INVOKER": "$LOG" "$REPORT" 2>/dev/null
    say ""
    say "Nothing was changed."
    say ""
    say "${C_FAIL}FAIL${C_OFF} - a panic is engaged: every network is cut off on purpose. This script does not release it."
    say "To reconnect (administrator password), then run this again if something still fails:"
    say "      pkexec shadow-panic release     (or Control Center -> Privacy -> Panic -> Reconnect)"
    exit 1
fi
egress_probe
if [ "$KS_TABLE" = 1 ] && [ "$KS_HIDE_ME" = 1 ]; then
    :   # Hide Me's, for Ghost Mode: below (2c)
elif [ "$KS_TABLE" = 1 ]; then
    case "$KS_MODE" in
        off) warn "the kill switch's firewall table is loaded although the kill switch is off: it blocks traffic" ;;
        tor|vpn|tor-or-vpn) warn "the kill switch is on (mode $KS_MODE): it lets out only $(ks_lets)" ;;
        *) warn "the kill switch's firewall table (inet shadow_egress) is loaded: it lets out only $(ks_lets)" ;;
    esac
elif [ -n "$KS_MODE" ] && [ "$KS_MODE" != off ]; then
    warn "the kill switch is set to mode $KS_MODE but its table is not loaded: it does not protect this computer right now${KS_ERROR:+ ($KS_ERROR)}"
    info "it comes back within 20 seconds (shadow-killswitch-watch.timer), or now with: sudo shadow-killswitch reassert"
fi

# 2a. Strict reverse-path filtering drops Wi-Fi answers when the cable is in.
rp_all=$(sysctl_get net.ipv4.conf.all.rp_filter)
rp_def=$(sysctl_get net.ipv4.conf.default.rp_filter)
if [ "$rp_all" = 1 ] || [ "$rp_def" = 1 ]; then
    if [ "$REPORT_ONLY" = 1 ]; then
        warn "reverse-path filter is strict (would set loose)"
    else
        backup_file "$SYSCTL_FIX"
        mkdir -p "$(dirname "$SYSCTL_FIX")"
        cat >"$SYSCTL_FIX" <<EOF
$MARK
# Loose reverse-path filtering. "Strict" (1, set by 90-shadow-anon.conf) drops
# every answer that arrives over Wi-Fi while a cable is also connected, because
# the best route back is the cable. Loose (2) still drops packets from
# addresses that are not reachable at all (forged sources).
net.ipv4.conf.all.rp_filter = 2
net.ipv4.conf.default.rp_filter = 2
EOF
        sysctl -q -w net.ipv4.conf.all.rp_filter=2 net.ipv4.conf.default.rp_filter=2 >/dev/null 2>&1
        undo_line "sysctl -w net.ipv4.conf.all.rp_filter=$rp_all net.ipv4.conf.default.rp_filter=$rp_def"
        ok "reverse-path filter: strict -> loose (Wi-Fi answers are no longer dropped)"
        changed "reverse-path filter set to loose ($SYSCTL_FIX)"
    fi
else
    ok "reverse-path filter is not strict"
fi

# 2b. Wi-Fi switched off (software switch, airplane mode).
if [ ${#WIFI_IFS[@]} -gt 0 ]; then
    if [ "$(nmcli radio wifi 2>/dev/null)" = disabled ]; then
        if [ "$REPORT_ONLY" = 1 ]; then warn "Wi-Fi is switched off in NetworkManager"
        else nmcli radio wifi on && ok "Wi-Fi was switched off - switched on" && changed "Wi-Fi radio switched on"; pause 4; fi
    fi
    if have rfkill; then
        if rfkill -n -o TYPE,HARD 2>/dev/null | awk '$1 == "wlan" && $2 == "blocked"' | grep -q .; then
            bad "Wi-Fi is blocked by a hardware switch or key (airplane key, Fn+F2, a switch on the side): switch it on"
            RESULTS+=("FAIL|Wi-Fi radio|blocked by the hardware switch or key")
        elif rfkill -n -o TYPE,SOFT 2>/dev/null | awk '$1 == "wlan" && $2 == "blocked"' | grep -q .; then
            if [ "$REPORT_ONLY" = 1 ]; then warn "Wi-Fi is blocked (rfkill)"
            else rfkill unblock wlan && ok "Wi-Fi was blocked (rfkill) - unblocked" && changed "Wi-Fi unblocked (rfkill)"; pause 4; fi
        fi
    fi
fi

# 2c. Hide Me.
HIDE_NOTE=""
if [ "$KEEP_HIDE_ME" = 1 ]; then
    info "Hide Me left as it is (--keep-hide-me)"
    if rotation_on; then
        HIDE_NOTE="Hide Me's identity rotation is on: it reconnects Wi-Fi with a new address every few seconds. Stop it in Hide Me, or run this without --keep-hide-me."
    fi
else
    if rotation_on; then
        warn "Hide Me's identity rotation is on: it reconnects every network every few seconds with a new address"
        if [ "$REPORT_ONLY" = 0 ] && ask "Stop the automatic rotation? (Rotate by hand still works)"; then
            systemctl disable --now shadow-anon-rotate.timer >/dev/null 2>&1
            undo_line "systemctl enable --now shadow-anon-rotate.timer"
            ok "identity rotation stopped"
            changed "Hide Me identity rotation stopped (turn it on again in Hide Me)"
        fi
    fi
    if kill_switch_on || tproxy_on; then
        warn "Hide Me's Ghost Mode is on (all traffic through Tor$(kill_switch_on && echo ', kill switch on'))"
        if http_check; then
            ok "traffic gets through Tor - Ghost Mode left on"
            HIDE_NOTE="Ghost Mode is on: every connection goes through Tor (slower, and some sites block Tor)."
        elif [ "$REPORT_ONLY" = 0 ] && ask "Nothing gets through Tor on this network. Turn Ghost Mode's network part off (kill switch, Tor routing, Tor DNS)?"; then
            if have shadow-anon; then
                shadow-anon kill-switch-off >/dev/null 2>&1
                shadow-anon transparent-proxy-off >/dev/null 2>&1
                shadow-anon dns-protect-off >/dev/null 2>&1
            fi
            # If shadow-anon could not (or is missing), remove its chains directly.
            # Never the Shadow kill switch's table: only Hide Me's "off" may
            # change it, because it puts back the mode in force before Ghost Mode.
            for t in iptables ip6tables; do
                have "$t" || continue
                while $t -D OUTPUT -j SHADOWKILL 2>/dev/null; do :; done
                $t -F SHADOWKILL 2>/dev/null; $t -X SHADOWKILL 2>/dev/null
            done
            for c in SHADOWTOR SHADOWDNS; do
                while iptables -t nat -D OUTPUT -j "$c" 2>/dev/null; do :; done
                iptables -t nat -F "$c" 2>/dev/null; iptables -t nat -X "$c" 2>/dev/null
            done
            if have ip6tables; then   # DNS protection's IPv6 side (it refuses IPv6 DNS)
                while ip6tables -t nat -D OUTPUT -j SHADOWDNS 2>/dev/null; do :; done
                ip6tables -t nat -F SHADOWDNS 2>/dev/null; ip6tables -t nat -X SHADOWDNS 2>/dev/null
            fi
            egress_probe
            if [ "$KS_HIDE_ME" = 1 ]; then
                bad "Hide Me could not turn its kill switch off (the kill switch is still in its Tor mode)"
            else
                ok "Ghost Mode's network part is off (turn it on again in Hide Me)"
                changed "Hide Me Ghost Mode network part turned off: Tor could not get through"
                if [ "$KS_TABLE" = 1 ] && [ "$KS_MODE" != off ]; then
                    warn "the kill switch is back in mode $KS_MODE, as before Ghost Mode: it lets out only $(ks_lets)"
                fi
            fi
        else
            HIDE_NOTE="Ghost Mode is on and nothing gets through Tor: that alone stops the internet."
        fi
    fi
fi

# 2d. DNS. With systemd-resolved (newer images) a stopped resolver looks
#     exactly like "no internet": start it before anything else is tried.
if resolved_installed && ! resolved_active; then
    warn "the DNS service (systemd-resolved) is not running - names cannot be looked up"
    if [ "$REPORT_ONLY" = 0 ]; then
        if systemctl restart systemd-resolved.service >/dev/null 2>&1 && { pause 2; resolved_active; }; then
            ok "systemd-resolved started"
            changed "DNS service systemd-resolved started (it was not running)"
        else
            bad "systemd-resolved does not start: see journalctl -u systemd-resolved -b"
        fi
    fi
fi

# DNS locked to Tor without Tor answering (DNS-over-Tor, then a restart), or
# /etc/resolv.conf pointing nowhere or past systemd-resolved.
dns_lock=0
if resolv_is_tor || [ -e "$NM_DNS_LOCK" ]; then dns_lock=1
elif [ -e "$RESOLVED_HIDE_ME" ] && ! dns_check; then dns_lock=4
elif resolv_immutable && ! dns_check; then dns_lock=2
elif [ -L "$RESOLV" ] && [ ! -e "$RESOLV" ]; then dns_lock=3
elif resolved_installed && resolved_active && [ ! -L "$RESOLV" ] &&
     ! grep -qE '^[[:space:]]*nameserver[[:space:]]+127\.0\.0\.53([[:space:]]|$)' "$RESOLV" 2>/dev/null; then dns_lock=5; fi
hand_to_resolved() {   # /etc/resolv.conf -> resolved's stub link, never a file with plain DNS servers
    rm -f "$RESOLV"
    ln -s "$RESOLVED_STUB" "$RESOLV"
    systemctl restart systemd-resolved.service >/dev/null 2>&1 ||
        bad "systemd-resolved did not restart: see journalctl -u systemd-resolved -b"
    nm_reload
    pause 2
}
case "$dns_lock" in
    1|2)
        if [ "$KEEP_HIDE_ME" = 0 ] && ! tor_dns_redirect && ! something_on_53; then
            if [ "$dns_lock" = 1 ]; then warn "DNS is locked to Tor (Hide Me) but nothing answers there any more - names cannot be looked up"
            else warn "/etc/resolv.conf is locked (chattr +i) and names cannot be looked up"; fi
            if [ "$REPORT_ONLY" = 0 ]; then
                backup_file "$RESOLV"
                backup_file "$NM_DNS_LOCK"
                [ -e "$RESOLV_ANON_BACKUP" ] && backup_file "$RESOLV_ANON_BACKUP"
                chattr -i "$RESOLV" 2>/dev/null || true
                rm -f "$NM_DNS_LOCK" "$RESOLV_ANON_BACKUP" "$RESOLV"
                if resolved_installed; then
                    hand_to_resolved
                    ok "DNS handed back to systemd-resolved (encrypted DNS)"
                    changed "DNS handed back to systemd-resolved (the locked DNS no longer answered)"
                else
                    # No systemd-resolved (images up to 0.2.0): NetworkManager
                    # writes /etc/resolv.conf itself when it is a plain file or missing.
                    nm_reload
                    nmcli general reload dns-rc >/dev/null 2>&1 || true
                    pause 2
                    if [ ! -s "$RESOLV" ]; then
                        if [ -s "$R$NM_RESOLV" ]; then ln -sf "$NM_RESOLV" "$RESOLV"
                        else printf '%s\n# NetworkManager did not write DNS servers; public ones as a fallback.\nnameserver 9.9.9.9\nnameserver 1.1.1.1\n' "$MARK" >"$RESOLV"; fi
                    fi
                    ok "DNS handed back to NetworkManager (the network's own DNS servers)"
                    changed "DNS handed back to NetworkManager (the locked DNS no longer answered)"
                fi
            fi
        elif [ "$KEEP_HIDE_ME" = 0 ]; then
            ok "DNS goes through Tor (Hide Me) and Tor's redirect is in place"
        fi ;;
    4)  # Hide Me with systemd-resolved: resolved forwards to Tor's DNSPort.
        if listening 127.0.0.1 "$(hide_me_tor_port)"; then
            warn "Hide Me sends DNS to Tor; Tor's DNS port is open but lookups through it fail (Tor may still be connecting)"
        elif [ "$KEEP_HIDE_ME" = 1 ]; then
            HIDE_NOTE="Hide Me sends DNS to Tor, but Tor does not answer, so no name can be looked up. Turn DNS protection off in Hide Me, or run this without --keep-hide-me."
        else
            warn "Hide Me sends DNS to Tor, but Tor does not answer - names cannot be looked up"
            if [ "$REPORT_ONLY" = 0 ]; then
                have shadow-anon && shadow-anon dns-protect-off >/dev/null 2>&1
                if [ -e "$RESOLVED_HIDE_ME" ]; then
                    backup_file "$RESOLVED_HIDE_ME"
                    rm -f "$RESOLVED_HIDE_ME"
                fi
                resolv_is_stub || backup_file "$RESOLV"
                hand_to_resolved
                ok "DNS handed back to systemd-resolved (encrypted DNS)"
                changed "Hide Me's DNS-over-Tor turned off: Tor did not answer (turn it on again in Hide Me)"
            fi
        fi ;;
    3)
        warn "/etc/resolv.conf points at $(readlink "$RESOLV"), which does not exist: names cannot be looked up"
        if [ "$REPORT_ONLY" = 0 ]; then
            if resolved_installed; then
                if resolv_is_stub; then
                    # The link is right; only a running systemd-resolved can
                    # fill it. Never replaced by a file with plain DNS servers.
                    if resolved_active; then bad "systemd-resolved runs but has not written its stub file: see journalctl -u systemd-resolved -b"
                    else bad "names cannot be looked up while systemd-resolved does not run"; fi
                else
                    backup_file "$RESOLV"
                    hand_to_resolved
                    ok "/etc/resolv.conf points at systemd-resolved again"
                    changed "/etc/resolv.conf points at systemd-resolved again (it pointed at a missing file)"
                fi
            else
                backup_file "$RESOLV"
                rm -f "$RESOLV"
                nm_reload
                nmcli general reload dns-rc >/dev/null 2>&1 || true
                pause 2
                if [ ! -s "$RESOLV" ]; then
                    if [ -s "$R$NM_RESOLV" ]; then ln -sf "$NM_RESOLV" "$RESOLV"
                    else printf '%s\n# NetworkManager did not write DNS servers; public ones as a fallback.\nnameserver 9.9.9.9\nnameserver 1.1.1.1\n' "$MARK" >"$RESOLV"; fi
                fi
                ok "DNS handed back to NetworkManager (the network's own DNS servers)"
                changed "DNS handed back to NetworkManager (/etc/resolv.conf pointed at a missing file)"
            fi
        fi ;;
    5)
        warn "/etc/resolv.conf lists DNS servers directly: every lookup bypasses systemd-resolved (no encryption)"
        if [ "$REPORT_ONLY" = 0 ]; then
            backup_file "$RESOLV"
            chattr -i "$RESOLV" 2>/dev/null || true
            hand_to_resolved
            ok "/etc/resolv.conf points at systemd-resolved again (encrypted DNS)"
            changed "/etc/resolv.conf points at systemd-resolved again (it bypassed encrypted DNS)"
        fi ;;
esac

# 2e. This computer's own name does not resolve (Hide Me's random hostname
#     was never added to /etc/hosts): sudo warns at every use.
HOSTNAME_NOW=$(hostname 2>/dev/null)
if [ -n "$HOSTNAME_NOW" ] && ! getent hosts "$HOSTNAME_NOW" >/dev/null 2>&1; then
    if [ "$REPORT_ONLY" = 1 ]; then
        warn "the computer's name '$HOSTNAME_NOW' is not in /etc/hosts"
    else
        backup_file "$HOSTS"
        tmp_hosts=$(mktemp)
        if [ -f "$HOSTS" ] && grep -qE '^127\.0\.1\.1[[:space:]]' "$HOSTS"; then
            sed -E "s/^127\.0\.1\.1[[:space:]].*/127.0.1.1\t$HOSTNAME_NOW/" "$HOSTS" >"$tmp_hosts"
        elif [ -f "$HOSTS" ] && grep -qE '^127\.0\.0\.1[[:space:]]' "$HOSTS"; then
            awk -v h="$HOSTNAME_NOW" '{ print } !done && /^127\.0\.0\.1[[:space:]]/ { print "127.0.1.1\t" h; done = 1 }' "$HOSTS" >"$tmp_hosts"
        else
            { [ -f "$HOSTS" ] && cat "$HOSTS"; printf '127.0.0.1\tlocalhost\n127.0.1.1\t%s\n' "$HOSTNAME_NOW"; } >"$tmp_hosts"
        fi
        cat "$tmp_hosts" >"$HOSTS"; rm -f "$tmp_hosts"
        chmod 644 "$HOSTS"
        ok "the computer's name '$HOSTNAME_NOW' resolves again (sudo stops saying \"unable to resolve host\")"
        changed "/etc/hosts: 127.0.1.1 -> $HOSTNAME_NOW"
    fi
fi

# 2f. Wi-Fi: a new random address at every connection -> one private address per network.
if [ ${#WIFI_IFS[@]} -gt 0 ]; then
    if [ "$REPORT_ONLY" = 1 ]; then
        info "Wi-Fi address policy: $(grep -h '^wifi.cloned-mac-address' "$R"/usr/lib/NetworkManager/conf.d/*.conf "$R"/etc/NetworkManager/conf.d/*.conf 2>/dev/null | tail -n1)"
    elif [ ! -f "$NM_MAC_FIX" ] || ! grep -qF "$MARK" "$NM_MAC_FIX"; then
        backup_file "$NM_MAC_FIX"
        mkdir -p "$(dirname "$NM_MAC_FIX")"
        cat >"$NM_MAC_FIX" <<EOF
$MARK
# Wi-Fi uses a private hardware (MAC) address of its own on every network that
# stays the same for that network ("stable"), instead of a new random one at
# every connection: routers then do not block a "new device" after every
# reconnect, and log-in pages remember you. It is still not the real address
# and it differs between networks, so networks cannot follow you around.
# Change it per network in Control Center -> Wi-Fi, or delete this file to go
# back to the image default.
[connection]
wifi.cloned-mac-address=stable
EOF
        nm_reload
        ok "Wi-Fi now keeps one private address per network (no new address at every connection)"
        changed "Wi-Fi address policy: random at every connection -> private and stable per network ($NM_MAC_FIX)"
    else
        ok "Wi-Fi address policy already fixed"
    fi
fi

# ---- 3. test ----------------------------------------------------------------------
step "3/5  Testing each connection on its own"
declare -A IF_RESULT IF_TEXT
test_all() {
    local ifc type state con
    while IFS='|' read -r ifc type state con; do
        [ -n "$ifc" ] || continue
        if [[ "$state" == connecting* ]]; then
            # Still joining or waiting for an address: give it half a minute,
            # then test what there is (no address is a finding too).
            info "$ifc is still connecting - waiting up to 30 seconds..."
            for _ in $(seq 30); do
                pause 1
                state=$(nmcli -t -f DEVICE,STATE device status 2>/dev/null | awk -F: -v d="$ifc" '$1 == d { print $2 }')
                [[ "$state" == connecting* ]] || break
            done
        fi
        case "$state" in
            connected*|connecting*) ;;
            *) IF_RESULT[$ifc]=down; IF_TEXT[$ifc]="not connected ($state)"; continue ;;
        esac
        test_iface "$ifc"
        IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
    done < <(nm_devices)
}
show_results() {
    local ifc
    for ifc in "${WIFI_IFS[@]}" "${ETH_IFS[@]}"; do
        case "${IF_RESULT[$ifc]:-down}" in
            pass) ok "$ifc: ${IF_TEXT[$ifc]}" ;;
            down) info "$ifc: ${IF_TEXT[$ifc]}" ;;
            *) bad "$ifc: ${IF_TEXT[$ifc]}" ;;
        esac
    done
}
GHOST_ONLY=0
KS_ONLY=0 KS_NET=""
egress_probe
if kill_switch_on || tproxy_on; then
    # Direct tests would be blocked or redirected by Ghost Mode on purpose.
    GHOST_ONLY=1
    if http_check; then ok "internet works (through Tor)"; else bad "no internet, even through Tor"; fi
elif [ "$KS_TABLE" = 1 ]; then
    # The kill switch drops a test of one connection on its own on purpose:
    # only what leaves through Tor or the VPN can be tested.
    # A program that does not use Tor gets through in the Tor modes only with
    # Hide Me's transparent proxy; Tor itself is tested through its SOCKS port.
    GHOST_ONLY=1 KS_ONLY=1 KS_NET=none
    if http_check; then
        KS_NET=direct
        ok "internet works (the kill switch is on and lets out only $(ks_lets))"
    elif [[ "$KS_MODE" == tor* ]] && tor_check; then
        KS_NET=tor
        ok "Tor works: programs that use Tor reach the internet (tested through Tor's SOCKS port)"
        warn "every other program is cut off: the kill switch (mode $KS_MODE) lets out only $(ks_lets)"
    else
        bad "no internet: the kill switch lets out only $(ks_lets), and none of it gets through - the likely cause"
    fi
else
    # A disconnected Wi-Fi with a saved network: let NetworkManager pick it.
    for ifc in "${WIFI_IFS[@]}"; do
        st=$(nmcli -g GENERAL.STATE device show "$ifc" 2>/dev/null)
        case "$st" in
            *"(connected)"*|*"(connecting"*) ;;
            *) if [ "$REPORT_ONLY" = 0 ]; then
                   info "$ifc is not connected - asking NetworkManager to join a saved network"
                   nmcli --wait 40 device connect "$ifc" >/dev/null 2>&1 || true
               fi ;;
        esac
    done
    test_all
    show_results
fi

# ---- 4. repair Wi-Fi --------------------------------------------------------------
step "4/5  Repairing Wi-Fi"
wifi_fixed_note=()
repair_wifi() {   # repair_wifi iface
    local ifc=$1 uuid name old_mac old_ps drv opts=() mod p
    uuid=$(active_uuid "$ifc")
    [ -n "$uuid" ] || return 1
    name=$(con_get "$uuid" connection.id)
    old_mac=$(con_get "$uuid" 802-11-wireless.cloned-mac-address)
    old_ps=$(con_get "$uuid" 802-11-wireless.powersave)
    info "network '$name': ${IF_TEXT[$ifc]}"

    # a. Reconnect once with the stable address (an explicit "random" on this
    #    network is replaced too: that is what breaks it).
    if [ "$old_mac" = random ]; then
        nmcli connection modify "$uuid" 802-11-wireless.cloned-mac-address stable
        undo_line "nmcli connection modify $uuid 802-11-wireless.cloned-mac-address random"
        changed "network '$name': address random -> stable"
    fi
    info "reconnecting '$name' with its stable private address..."
    reconnect "$ifc" "$uuid"; pause 2
    if test_iface "$ifc"; then IF_RESULT[$ifc]=pass; IF_TEXT[$ifc]=$T_TEXT; wifi_fixed_note+=("stable private address"); return 0; fi
    IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
    [ "$T_RESULT" = portal ] && return 1   # nothing to repair: the user must sign in

    # b. Names do not resolve on this network.
    #    With systemd-resolved every name goes to the DNS provider, encrypted
    #    (port 853). Only if this network blocks the way to the provider may
    #    this one network's own DNS server answer too ("~." on the profile),
    #    and only if that helps; DNS stays encrypted everywhere else.
    if [ "$T_RESULT" = nodns ] && resolved_installed && resolv_is_stub; then
        local provider dot
        provider=$(global_dns_ip)
        dot=$(resolved_prop DNSOverTLS)
        if [ -n "$provider" ] && ! port853_open "$ifc" "$provider"; then
            if [ "$dot" = yes ]; then
                info "'$name' blocks port 853, which encrypted DNS needs - letting this network's own DNS server answer on '$name' only..."
            else
                info "'$name' lets no DNS server but its own answer - using it on '$name' only..."
            fi
            if nmcli connection modify "$uuid" +ipv4.dns-search "~." 2>>"$LOG"; then
                reconnect "$ifc" "$uuid"; pause 2
                if test_iface "$ifc"; then
                    IF_RESULT[$ifc]=pass; IF_TEXT[$ifc]=$T_TEXT
                    undo_line "nmcli connection modify $uuid -ipv4.dns-search '~.'"
                    changed "network '$name': its own DNS server answers there (it blocks encrypted DNS on port 853); every other network keeps encrypted DNS"
                    wifi_fixed_note+=("this network's own DNS server (port 853 blocked)"); return 0
                fi
                IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
                nmcli connection modify "$uuid" -ipv4.dns-search "~." 2>>"$LOG"   # it did not help
            fi
        fi
    elif [ "$T_RESULT" = nodns ]; then
        # No systemd-resolved (images up to 0.2.0): the network's DNS server
        # does not answer, so public ones first for this network (Quad9, then
        # Cloudflare; with their TLS names, for DNS-over-TLS once resolved is
        # installed), the network's own after them.
        local old_dns
        old_dns=$(con_get "$uuid" ipv4.dns)
        nmcli connection modify "$uuid" ipv4.dns "9.9.9.9#dns.quad9.net 1.1.1.1#cloudflare-dns.com"
        info "the network's DNS server does not answer - adding public ones for '$name'..."
        reconnect "$ifc" "$uuid"; pause 2
        if test_iface "$ifc"; then
            IF_RESULT[$ifc]=pass; IF_TEXT[$ifc]=$T_TEXT
            undo_line "nmcli connection modify $uuid ipv4.dns '$old_dns'"
            changed "network '$name': DNS servers 9.9.9.9 and 1.1.1.1 added (its own did not answer)"
            wifi_fixed_note+=("public DNS servers"); return 0
        fi
        IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
        nmcli connection modify "$uuid" ipv4.dns "$old_dns"   # it did not help
    fi
    # Only names fail: the network itself carries traffic (tested by address),
    # so power saving, the hardware address and the driver are not the cause.
    [ "$T_RESULT" = nodns ] && return 1

    # c. Wi-Fi power saving off for this network.
    if [ "$old_ps" != 2 ] && [ "$old_ps" != disable ]; then
        nmcli connection modify "$uuid" 802-11-wireless.powersave 2
        info "turning Wi-Fi power saving off for '$name' and reconnecting..."
        reconnect "$ifc" "$uuid"; pause 2
        if test_iface "$ifc"; then
            IF_RESULT[$ifc]=pass; IF_TEXT[$ifc]=$T_TEXT
            undo_line "nmcli connection modify $uuid 802-11-wireless.powersave ${old_ps:-0}"
            changed "network '$name': Wi-Fi power saving off"
            wifi_fixed_note+=("power saving off"); return 0
        fi
        IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
        nmcli connection modify "$uuid" 802-11-wireless.powersave "${old_ps:-0}"   # it did not help
    fi

    # d. The real hardware address, for this network only: MAC-filtering
    #    routers and chips that cannot receive after an address change.
    nmcli connection modify "$uuid" 802-11-wireless.cloned-mac-address permanent
    info "trying the real hardware address on '$name' only..."
    reconnect "$ifc" "$uuid"; pause 2
    if test_iface "$ifc"; then
        IF_RESULT[$ifc]=pass; IF_TEXT[$ifc]=$T_TEXT
        undo_line "nmcli connection modify $uuid 802-11-wireless.cloned-mac-address '${old_mac}'"
        changed "network '$name': uses the real hardware address (this router or Wi-Fi chip does not work with a private one)"
        wifi_fixed_note+=("real hardware address on this network")
        return 0
    fi
    IF_RESULT[$ifc]=$T_RESULT; IF_TEXT[$ifc]=$T_TEXT
    # Not the cause: do not show the real address to this network for nothing.
    nmcli connection modify "$uuid" 802-11-wireless.cloned-mac-address "$( [ "$old_mac" = random ] && echo stable || echo "$old_mac")"
    reconnect "$ifc" "$uuid"

    # e. Chip power saving in the driver (Realtek rtw88/rtw89, MediaTek): only
    #    options the installed driver has; they take effect after a restart.
    drv=$(driver_of "$ifc")
    case "$drv" in
        rtw88_*) opts=("rtw88_core disable_lps_deep=y" "rtw88_pci disable_aspm=y") ;;
        rtw89_*) opts=("rtw89_core disable_ps_mode=y" "rtw89_pci disable_aspm_l1=y disable_aspm_l1ss=y disable_clkreq=y") ;;
        mt7921e|mt7925e) opts=("$drv disable_aspm=y") ;;
    esac
    local lines=()
    for o in "${opts[@]}"; do
        mod=${o%% *}
        local keep=()
        for p in ${o#* }; do
            modinfo -p "$mod" 2>/dev/null | grep -q "^${p%%=*}:" && keep+=("$p")
        done
        [ ${#keep[@]} -gt 0 ] && lines+=("options $mod ${keep[*]}")
    done
    if [ ${#lines[@]} -gt 0 ] && ! grep -qF "${lines[0]}" "$MODPROBE_FIX" 2>/dev/null; then
        backup_file "$MODPROBE_FIX"
        mkdir -p "$(dirname "$MODPROBE_FIX")"
        { echo "$MARK"
          echo "# Power saving of the $drv Wi-Fi chip off: with it on, this chip stops"
          echo "# passing traffic while Wi-Fi still shows as connected."
          printf '%s\n' "${lines[@]}"; } >"$MODPROBE_FIX"
        if have update-initramfs; then info "updating the boot image (a minute)..."; update-initramfs -u >/dev/null 2>&1 || true; fi
        warn "the $drv Wi-Fi chip's power saving is now off - it takes effect after a restart"
        changed "Wi-Fi chip power saving off ($MODPROBE_FIX) - restart needed"
        RESTART=1
    fi
    return 1
}

WIFI_TESTED=0
if [ "$KS_ONLY" = 1 ]; then
    info "skipped while the kill switch lets out only $(ks_lets): a connection tested on its own would be dropped on purpose"
elif [ "$GHOST_ONLY" = 1 ]; then
    info "skipped while Ghost Mode routes everything through Tor"
elif [ "$REPORT_ONLY" = 1 ]; then
    info "skipped (--report-only)"
else
    for ifc in "${WIFI_IFS[@]}"; do
        case "${IF_RESULT[$ifc]:-down}" in
            pass) WIFI_TESTED=1; ok "$ifc works - nothing to repair" ;;
            down) info "$ifc: ${IF_TEXT[$ifc]}" ;;
            *) WIFI_TESTED=1
               if repair_wifi "$ifc"; then ok "$ifc works now (${wifi_fixed_note[*]})"
               else bad "$ifc: ${IF_TEXT[$ifc]}"; fi ;;
        esac
    done
fi

# ---- 5. verdict ---------------------------------------------------------------------
step "5/5  Result"
dns_ok=0
if dns_check; then dns_ok=1; ok "names are looked up (DNS works)"
elif [ "$KS_ONLY" = 1 ]; then warn "names cannot be looked up except through $(ks_way): the kill switch drops other DNS on purpose"
else bad "names cannot be looked up (DNS)"; fi
VERDICT=PASS
if [ "$KS_ONLY" = 1 ]; then
    if [ "$KS_NET" = none ]; then
        VERDICT=FAIL
        RESULTS+=("FAIL|kill switch|mode ${KS_MODE:-unknown}: it lets out only $(ks_lets), and none of it gets through")
    fi
elif [ "$GHOST_ONLY" = 1 ]; then
    http_check || VERDICT=FAIL
else
    any_conn=0
    for ifc in "${WIFI_IFS[@]}" "${ETH_IFS[@]}"; do
        r=${IF_RESULT[$ifc]:-down}
        [ "$r" = down ] && continue
        any_conn=1
        [ "$r" = pass ] || VERDICT=FAIL
        RESULTS+=("$( [ "$r" = pass ] && echo PASS || echo FAIL)|$ifc|${IF_TEXT[$ifc]}")
    done
    if [ "$any_conn" = 0 ]; then
        if [ ${#WIFI_IFS[@]} -gt 0 ]; then VERDICT="NOT VERIFIED"; else VERDICT=FAIL; RESULTS+=("FAIL|network|nothing is connected"); fi
    fi
    [ "$dns_ok" = 1 ] || [ "$any_conn" = 0 ] || VERDICT=FAIL
    if [ "$VERDICT" = PASS ] && [ ${#WIFI_IFS[@]} -gt 0 ] && [ "$WIFI_TESTED" = 0 ]; then
        wifi_up=0
        for ifc in "${WIFI_IFS[@]}"; do [ "${IF_RESULT[$ifc]:-down}" = pass ] && wifi_up=1; done
        [ "$wifi_up" = 1 ] || VERDICT="NOT VERIFIED"
    fi
fi
for r in "${RESULTS[@]}"; do case "$r" in "FAIL|Wi-Fi radio|"*) VERDICT=FAIL ;; esac; done
[ "$RESTART" = 1 ] && [ "$VERDICT" != PASS ] && VERDICT="RESTART NEEDED"
for r in "${RESULTS[@]}"; do rep "result: $r"; done
snapshot "after"
[ "$VERDICT" = PASS ] || report_nm_log
[ "$INVOKER" = root ] || chown "$INVOKER": "$LOG" "$REPORT" 2>/dev/null

say ""
if [ ${#CHANGED[@]} -gt 0 ]; then
    say "Changed:"
    for c in "${CHANGED[@]}"; do say "  - $c"; done
    say "  (undo: sudo sh $UNDO)"
else
    say "Nothing was changed."
fi
[ -n "$HIDE_NOTE" ] && say "" && say "$HIDE_NOTE"
say ""
case "$VERDICT" in
    PASS)
        if [ "$KS_NET" = tor ]; then
            say "${C_OK}PASS${C_OFF} - Tor works: programs that use Tor (Tor Browser, torsocks) reach the internet."
            say "The kill switch (mode $KS_MODE) cuts off every other program on purpose; that is not a fault."
            say "To let them out without Tor until the next restart: pkexec shadow-killswitch set off --runtime"
        elif [ "$KS_ONLY" = 1 ]; then
            say "${C_OK}PASS${C_OFF} - the internet works through $(ks_way) (the kill switch is on, so each connection was not tested on its own)."
        else
            say "${C_OK}PASS${C_OFF} - the internet works on every connected network$( [ "$GHOST_ONLY" = 1 ] && echo ' (through Tor)')."
        fi
        eth_up=0
        for ifc in "${ETH_IFS[@]}"; do [ "${IF_RESULT[$ifc]:-down}" = pass ] && eth_up=1; done
        if [ ${#WIFI_IFS[@]} -gt 0 ] && [ "$eth_up" = 1 ]; then
            say "While the cable is plugged in the computer prefers it; unplug it and Wi-Fi takes over."
        fi
        say "No restart needed."
        exit 0 ;;
    "NOT VERIFIED")
        say "${C_WARN}NOT VERIFIED${C_OFF} - Wi-Fi is not connected to a network, so it could not be tested."
        say "Connect to your Wi-Fi from the top bar, then run this again: bash $SCRIPT"
        exit 3 ;;
    "RESTART NEEDED")
        say "${C_WARN}RESTART NEEDED${C_OFF} - restart the computer, then run this again: bash $SCRIPT"
        exit 4 ;;
    *)
        say "${C_FAIL}FAIL${C_OFF} - still not working:"
        for r in "${RESULTS[@]}"; do
            case "$r" in FAIL*) IFS='|' read -r _ w d <<<"$r"; say "  - $w: $d" ;; esac
        done
        [ "$dns_ok" = 0 ] && [ "$KS_ONLY" = 0 ] && say "  - names cannot be looked up (DNS)"
        if [ "$KS_ONLY" = 1 ]; then
            say ""
            say "The kill switch is the likely cause. This script does not turn it off: that would let"
            say "programs reach the internet without $(ks_way)."
            ks_advice
        fi
        [ "$RECONNECT_FAILED" = 1 ] && say "  - Wi-Fi could not reconnect by itself: join your network again from the top bar, then run this again"
        for ifc in "${WIFI_IFS[@]}"; do
            if [ "${IF_RESULT[$ifc]:-}" = noaddr ]; then
                say ""
                say "The Wi-Fi network gives this computer no address, even with its real hardware"
                say "address: the router has probably run out of addresses (every new random address"
                say "used one up) or blocks this computer. Restart the router (power off 10 seconds;"
                say "for a phone's hotspot: hotspot off and on), wait a minute, then run this again."
                break
            fi
        done
        say "Send me this file (no passwords or hardware addresses in it): $REPORT"
        exit 1 ;;
esac
