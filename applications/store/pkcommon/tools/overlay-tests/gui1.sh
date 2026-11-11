#!/bin/bash
cd /tmp/store-pktest; S=/tmp/store-pktest/shots; mkdir -p $S
V=$(apt-cache madison curl | awk '$0 !~ /security/ && /trixie\/main/ {print $3; exit}')
apt-get install -y --allow-downgrades curl=$V libcurl4t64=$V >/dev/null 2>&1
Xvfb :77 -screen 0 1920x1200x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!; sleep 2; export DISPLAY=:77 QT_QPA_PLATFORM=xcb
shot() { local name=$1 delay=$2; shift 2
  SHADOW_SMOKE_TEST=1 SHADOW_SCREENSHOT=$S/$name.png SHADOW_WINDOW_SIZE=1280x860 SHADOW_SCREENSHOT_DELAY=$delay timeout -s KILL 90 dbus-run-session -- "$@" > $S/$name.log 2>&1; echo "$name rc=$? $(grep -c -i 'qml\|warn' $S/$name.log)"; }
shot pk-store-overview 9000 shadow-store
shot pk-store-app-kate 12000 shadow-store org.kde.kate
shot pk-store-pkg-bash 9000 shadow-store bash
shot pk-store-installed 12000 shadow-store --page installed
shot pk-store-updates 12000 shadow-store --page updates
shot pk-store-repos 9000 shadow-store --page repos
shot pk-store-search 9000 shadow-store --search editor
shot pk-upd-updates 9000 shadow-updater
shot pk-upd-history 8000 shadow-updater history
shot pk-upd-sources 8000 shadow-updater sources
shot pk-upd-settings 8000 shadow-updater settings
shot pk-upd-recovery 8000 shadow-updater recovery
kill $XPID
grep -h -i "qml\|warn" $S/*.log | sort | uniq -c | head -20
