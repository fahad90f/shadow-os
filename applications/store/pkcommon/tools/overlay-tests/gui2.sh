#!/bin/bash
# usage: gui2.sh <prefix> <app command...> ; then reads actions from /tmp/store-pktest/actions.txt
cd /tmp/store-pktest; S=/tmp/store-pktest/shots; mkdir -p $S
Xvfb :78 -screen 0 1280x860x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!; sleep 2; export DISPLAY=:78 QT_QPA_PLATFORM=xcb SHADOW_WINDOW_SIZE=1280x860
dbus-run-session -- bash -c '
  "$@" > /tmp/store-pktest/shots/gui2-app.log 2>&1 &
  APP=$!
  while read -r verb a b c; do
    case "$verb" in
      sleep) sleep "$a" ;;
      shot) import -window root /tmp/store-pktest/shots/$a.png ;;
      click) xdotool mousemove "$a" "$b" click 1 ;;
      wheel) xdotool mousemove "$a" "$b"; for i in $(seq 1 "$c"); do xdotool click 5; done ;;
      type) xdotool type "$a" ;;
      key) xdotool key "$a" ;;
      sh) bash -c "$a $b $c" ;;
    esac
  done < /tmp/store-pktest/actions.txt
  kill $APP 2>/dev/null
' x "$@"
kill $XPID
grep -i "qml\|warn" $S/gui2-app.log | head
