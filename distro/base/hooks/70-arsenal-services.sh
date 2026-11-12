# Arsenal daemons that cannot run until the administrator configures them
# (Suricata needs a capture interface, USBGuard needs a device policy) are
# installed but not started at boot; otherwise they fail on every start and
# leave the system "degraded". Enable them after configuring:
#   systemctl enable --now suricata    /    systemctl enable --now usbguard
for unit in suricata.service usbguard.service usbguard-dbus.service; do
    if [ -e "/usr/lib/systemd/system/$unit" ] || [ -e "/etc/systemd/system/$unit" ]; then
        systemctl disable "$unit" >/dev/null 2>&1 || true
    fi
done
