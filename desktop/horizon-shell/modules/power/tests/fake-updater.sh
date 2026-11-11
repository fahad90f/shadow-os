#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test double of "nexus-updater" for the power module's session test: speaks
# the documented --check contract (one JSON object, exit status 0/10/11/1)
# with the result named in $FAKE_UPDATER_DIR/result, and records every start
# in $FAKE_UPDATER_DIR/runs. It checks nothing.
dir="${FAKE_UPDATER_DIR:?}"
echo "run $*" >> "$dir/runs"
[ "$1" = "--check" ] || exit 0
case "$(cat "$dir/result" 2>/dev/null)" in
    security) echo '{"ok":true,"updates":5,"securityUpdates":2,"heldBack":0,"refreshed":true,"restartRequired":false}'; exit 11 ;;
    updates)  echo '{"ok":true,"updates":1,"securityUpdates":0,"heldBack":0,"refreshed":true,"restartRequired":false}'; exit 10 ;;
    failed)   echo '{"ok":false,"error":"test: the check failed"}'; exit 1 ;;
    *)        echo '{"ok":true,"updates":0,"securityUpdates":0,"heldBack":0,"refreshed":true,"restartRequired":false}'; exit 0 ;;
esac
