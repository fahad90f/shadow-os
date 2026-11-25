#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# with-fakes.sh COMMAND [ARGS...]
#
# Runs COMMAND (normally shadow-trading) against tests/fake_services.py instead
# of a real exchange and real model services: for screenshots and for trying
# the application by hand without any account or key. Nothing reaches the
# network. Options come from the environment:
#
#   FAKE_PORT=18080        port of the stand-in services
#   FAKE_TREND=up          up | down | flat: the shape of the made-up prices
#   FAKE_SCENARIO=FILE     how the models answer (see fake_services.py)
#   FAKE_KEYS=1            give the application the stand-in's test key
#   FAKE_STREAM=0          no live stream
#   FAKE_AUTO_TRADE=1      write a settings file with auto trade on
#   FAKE_HISTORY=1         start from a paper account with a few made-up trades
#   FAKE_LOG=FILE          request log of the stand-in services
set -euo pipefail
here="$(dirname "$(readlink -f "$0")")"
port="${FAKE_PORT:-18080}"
[ $# -ge 1 ] || { sed -n '3,18p' "$0"; exit 2; }

python3 -B "$here/fake_services.py" --port "$port" --trend "${FAKE_TREND:-up}" \
    --scenario "${FAKE_SCENARIO:-}" --log "${FAKE_LOG:-}" >/dev/null 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
for _ in $(seq 50); do
    python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(s.connect_ex(('127.0.0.1', $port)))" && break
    sleep 0.1
done

base="http://127.0.0.1:$port"
export SHADOW_TRADING_DATA_URL="$base"
export SHADOW_TRADING_NVIDIA_URL="$base/nvidia/v1"
# The stand-in's live stream, unless FAKE_STREAM=0 (prices are then asked for every few seconds).
if [ "${FAKE_STREAM:-1}" = 1 ]; then
    export SHADOW_TRADING_STREAM_URL="ws://127.0.0.1:$port"
else
    export SHADOW_TRADING_STREAM_URL=off
fi
if [ "${FAKE_KEYS:-0}" = 1 ]; then
    # The stand-in's own test key: it opens nothing anywhere else.
    export NVIDIA_API_KEY="nvapi-test-00000000000000000000000"
fi
config="${XDG_CONFIG_HOME:-$HOME/.config}/shadow"
data="${XDG_DATA_HOME:-$HOME/.local/share}/shadow/trading"
if [ "${FAKE_AUTO_TRADE:-0}" = 1 ]; then
    mkdir -p "$config"
    printf '[engine]\nauto_trade=true\n' > "$config/trading.conf"
fi
if [ "${FAKE_HISTORY:-0}" = 1 ]; then
    mkdir -p "$data"
    python3 -B "$here/make_history.py" > "$data/paper.json"
fi
"$@"
