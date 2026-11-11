#!/bin/bash
# Private system bus + polkitd + packagekitd inside this invocation's own /run
# (tmpfs, private mount namespace), then run the given commands.
B=/build/store
# Everything the package manager can change goes to a throw-away overlay: the
# shared build chroot itself is never modified. Abort if that cannot be set up.
set -e
O=/tmp/store-pktest/ovl; rm -rf $O; mkdir -p $O
for d in usr etc var; do
  mkdir -p $O/$d/upper $O/$d/work
  mount -t overlay overlay -o lowerdir=/$d,upperdir=$O/$d/upper,workdir=$O/$d/work /$d
  findmnt -n -t overlay /$d >/dev/null
done
set +e
mkdir -p /run/dbus /tmp/store-pktest/home
cat > /tmp/store-pktest/bus.conf <<'XML'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=/run/dbus/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/>
  </policy>
</busconfig>
XML
dbus-daemon --config-file=/tmp/store-pktest/bus.conf --nofork --nopidfile >/tmp/store-pktest/dbus.log 2>&1 &
DBUS_PID=$!
sleep 1
/usr/lib/polkit-1/polkitd --no-debug >/tmp/store-pktest/polkitd.log 2>&1 &
POLKIT_PID=$!
sleep 1
/usr/libexec/packagekitd --verbose >/tmp/store-pktest/packagekitd.log 2>&1 &
PK_PID=$!
sleep 3
export HOME=/tmp/store-pktest/home XDG_RUNTIME_DIR=/tmp/store-pktest/home/run
mkdir -m 700 -p $XDG_RUNTIME_DIR
export QML_IMPORT_PATH="$B/qml" LD_LIBRARY_PATH="$B/lib" PATH="$B/bin:$PATH"
export QT_QUICK_CONTROLS_STYLE=org.shadow.style QT_LOGGING_RULES="qt.qpa.*=false" LIBGL_ALWAYS_SOFTWARE=1
"$@"
RC=$?
kill $PK_PID $POLKIT_PID $DBUS_PID 2>/dev/null
wait 2>/dev/null
rm -rf $O 2>/dev/null
exit $RC
