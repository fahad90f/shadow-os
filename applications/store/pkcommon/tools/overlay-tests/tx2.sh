#!/bin/bash
O=/tmp/store-pktest/out; cd /tmp/store-pktest
CLI=shadow-pkcommon-cli
step() { echo; echo "######## $*"; }
st() { dpkg-query -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' "$@" 2>&1; }
step "E2 install security updates"
$CLI update --security > $O/E2.log 2>&1; echo "rc=$?"; grep -E "Status:|Result|restart|QUESTION" $O/E2.log | cut -c1-160; st curl libcurl4t64
shadow-updater --dump updates 2>/dev/null | head -12

step "F interrupted dpkg state, then repair"
dpkg --unpack hello_2.10-5_amd64.deb > $O/F-dpkg.log 2>&1; st hello
shadow-updater --dump health > $O/F-health1.json 2>/dev/null; python3 -c "
import json; d=json.load(open('$O/F-health1.json')); print('healthy', d.get('healthy'), 'broken', d.get('brokenPackages'), 'audit', (d.get('audit') or {}).get('output','')[:200])"
$CLI repair --yes > $O/F.log 2>&1; echo "rc=$?"; grep -E "Status:|Result|QUESTION|error" $O/F.log | cut -c1-160; st hello
shadow-updater --dump health 2>/dev/null | grep -E '"healthy"'

step "G unsigned local repository: refresh, repos, untrusted refusal"
mkdir -p repo/pkg/DEBIAN repo/pkg/usr/share/shadow-untrusted-test
printf 'Package: shadow-untrusted-test\nVersion: 1.0\nArchitecture: all\nMaintainer: test <test@example.invalid>\nDescription: unsigned test package\n' > repo/pkg/DEBIAN/control
echo test > repo/pkg/usr/share/shadow-untrusted-test/file
dpkg-deb --build repo/pkg repo/shadow-untrusted-test_1.0_all.deb >/dev/null; rm -rf repo/pkg
(cd repo && dpkg-scanpackages . > Packages 2>/dev/null); chmod -R a+rX repo
echo "deb [allow-insecure=yes] file:/tmp/store-pktest/repo ./" > /etc/apt/sources.list.d/untrusted-test.list
$CLI refresh > $O/G-refresh.log 2>&1; echo "refresh rc=$?"; grep -E "Result|error" $O/G-refresh.log | cut -c1-200
$CLI repos
shadow-store --dump repos > $O/G-repos.json 2>/dev/null; python3 -c "
import json; d=json.load(open('$O/G-repos.json'))
for r in d['packagekit']['repositories']: print(r['enabled'], r['category'], r['verification'], r['repoId'])"
$CLI install shadow-untrusted-test > $O/G1.log 2>&1; echo "install (untrusted answered no) rc=$?"; grep -E "QUESTION|ANSWER|Result|AUTHENT|error" $O/G1.log | cut -c1-260; st shadow-untrusted-test
shadow-store --dump component shadow-untrusted-test 2>/dev/null | grep -E '"trust"|"untrustedCount"|"originRepository"|"error"'
$CLI install shadow-untrusted-test --allow-untrusted > $O/G2.log 2>&1; echo "install --allow-untrusted rc=$?"; grep -E "QUESTION|ANSWER|Result|UNTRUSTED|error" $O/G2.log | cut -c1-260; st shadow-untrusted-test
ID=$($CLI repos | grep store-pktest | sed 's/.*\[\(.*\)\].*/\1/'); echo "repo id: $ID"
$CLI repo-enable "$ID" off > $O/G3.log 2>&1; echo "repo off rc=$?"; grep -E "Result|error" $O/G3.log | cut -c1-200; $CLI repos | grep store-pktest; cat /etc/apt/sources.list.d/untrusted-test.list
$CLI repo-enable "$ID" on > $O/G4.log 2>&1; echo "repo on rc=$?"; $CLI repos | grep store-pktest

step "H restart-required flag"
echo "*** System restart required ***" > /run/reboot-required; printf 'linux-image-amd64\nlibc6\n' > /run/reboot-required.pkgs
shadow-updater --dump reboot
SHADOW_SMOKE_TEST=1 SHADOW_SCREENSHOT=/tmp/store-pktest/shots/restart.png SHADOW_WINDOW_SIZE=1280x860 SHADOW_SCREENSHOT_DELAY=9000 QT_QPA_PLATFORM=xcb timeout -s KILL 90 xvfb-run -a dbus-run-session -- shadow-updater > /tmp/store-pktest/shots/restart.log 2>&1; echo "gui rc=$?"
rm -f /run/reboot-required /run/reboot-required.pkgs
