#!/bin/bash
# Real PackageKit transactions — inside the throw-away overlay only.
O=/tmp/store-pktest/out; mkdir -p $O; cd /tmp/store-pktest
CLI=nexus-pkcommon-cli
step() { echo; echo "######## $*"; }
st() { dpkg-query -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' "$@" 2>&1; }

step "A remove ethtool, question answered no"
$CLI remove ethtool > $O/A.log 2>&1; echo "rc=$?"; grep -E "QUESTION|ANSWER|Result" $O/A.log | cut -c1-200; st ethtool

step "B install hello"
$CLI install hello > $O/B.log 2>&1; echo "rc=$?"; grep -E "Status:|Result|error" $O/B.log | cut -c1-160; st hello

step "B2 history after install"
nexus-updater --dump history > $O/B2.json 2>/dev/null; python3 - <<'PY'
import json
d=json.load(open('/tmp/store-pktest/out/B2.json'))
e=d['entries'][0]; print({k:e[k] for k in e if k not in('packages','changes','installed','upgraded','removed')})
print('pk transactions', len(d['packagekit'].get('transactions',[])), d['packagekit'].get('error'))
PY

step "C remove hello --yes"
$CLI remove hello --yes > $O/C.log 2>&1; echo "rc=$?"; grep -E "QUESTION|ANSWER|Result" $O/C.log | cut -c1-200; st hello

step "D install kate, cancel after 2 s"
$CLI install kate --cancel-after 2 > $O/D.log 2>&1; echo "rc=$?"; grep -E "CANCEL|Cancel|cancel|Result|PackageKit finished" $O/D.log | cut -c1-200; st kate

step "E downgrade curl to the release version (apt, overlay only), then check"
V=$(apt-cache madison curl | awk '$0 !~ /security/ && /trixie\/main/ {print $3; exit}'); echo "release version: $V; installed: $(st curl)"
apt-get install -y --allow-downgrades curl=$V libcurl4t64=$V > $O/E-apt.log 2>&1; echo "apt rc=$?"; st curl libcurl4t64
nexus-updater --check > $O/E-check.json 2> $O/E-check.err; echo "--check rc=$? (expect 11)"; cat $O/E-check.json; cat $O/E-check.err
nexus-updater --dump settings | head -20
nexus-updater --dump updates > $O/E-updates.json 2>/dev/null
nexus-updater --dump update-details > $O/E-details.json 2>/dev/null
nexus-store --dump updates > $O/E-st-updates.json 2>/dev/null
