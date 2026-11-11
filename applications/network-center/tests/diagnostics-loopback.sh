#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test aid, not installed. Runs the Diagnostics back-end against
# 127.0.0.1 (ping, cancel, traceroute, troubleshooting) through tests/harness.
#
#   scripts/dev.sh exec --dir netcenter -- /src/applications/network-center/tests/diagnostics-loopback.sh
export QT_QPA_PLATFORM=offscreen HOME=/tmp/nc-diag-home; mkdir -p $HOME
python3 -c '
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",48123)); s.listen(5); time.sleep(120)' &
L=$!
sleep 1
/tmp/nc-harness/b/ncdiag 'pingstop|127.0.0.1' 'trace|127.0.0.1' 'trace|-h' 'ts|' 'ts|localhost' 'ts|no such host'
kill $L
