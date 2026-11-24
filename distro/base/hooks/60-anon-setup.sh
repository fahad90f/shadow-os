#!/bin/bash
# Shadow OS — anonymization engine setup hook
# Installs tools required by shadow-anon and hardens the base system at build time.
set -euo pipefail

echo "[60-anon-setup] Installing anonymization stack..."

# No iptables-persistent/netfilter-persistent: shadow-anon's rules exist only
# while Hide Me runs (shadow-anon-boot.service re-creates what must survive a
# restart), the kill switch is shadow-killswitch (nftables), and the package
# would save whatever rules the build host has into the image (its postinst
# autosaves; the chroot shares the build host's network namespace) and
# restore them on every machine. It is also declared broken by ufw.
apt-get install -y --no-install-recommends \
    tor \
    torsocks \
    obfs4proxy \
    macchanger \
    rfkill \
    iptables \
    curl \
    net-tools \
    iproute2 \
    iputils-ping \
    uuid-runtime

echo "[60-anon-setup] Enabling Tor service at boot..."
systemctl enable tor || true
systemctl enable tor@default || true

echo "[60-anon-setup] Ensuring Tor torrc.d include directive..."
TORRC=/etc/tor/torrc
mkdir -p /etc/tor/torrc.d
MARKER="%include /etc/tor/torrc.d/*.conf"
if ! grep -qF "$MARKER" "$TORRC" 2>/dev/null; then
    echo -e "\n$MARKER" >> "$TORRC"
fi

# Nothing here touches rfkill or sysctl: inside the build chroot /dev and the
# network namespace are the build host's own, so "rfkill block" and
# "sysctl -p" changed the build machine, not the image. The image applies
# /etc/sysctl.d/90-shadow-anon.conf at every boot (systemd-sysctl).

echo "[60-anon-setup] Setting shadow-anon executable..."
chmod 755 /usr/bin/shadow-anon

echo "[60-anon-setup] Writing polkit rule for shadow-anon (pkexec)..."
mkdir -p /usr/share/polkit-1/actions
cat > /usr/share/polkit-1/actions/org.shadowos.anon.policy <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC
 "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <action id="org.shadowos.anon.run">
    <description>Run shadow-anon identity protection engine</description>
    <message>Shadow OS needs root to change network routing, DNS, and hardware kill-switches.</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/usr/bin/shadow-anon</annotate>
    <annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
  </action>
</policyconfig>
EOF

echo "[60-anon-setup] Saving initial ghost-mode state file..."
mkdir -p /run 2>/dev/null || true
STATE=/run/shadow-anon-state.json
if [ ! -f "$STATE" ]; then
    cat > "$STATE" <<'JSON'
{
  "ghost_mode": false,
  "threat_model": "basic",
  "transparent_proxy": false,
  "dns_protected": false,
  "bt_killed": false,
  "usb_killed": false,
  "kernel_hardened": true,
  "auto_rotate": false
}
JSON
    chmod 644 "$STATE"
fi

# Tor's SOCKS port: Debian's defaults open 127.0.0.1:9050, which any local
# user may bind while Tor is stopped (Tor then cannot start, and torsocks and
# proxychains4 talk to that user). shadow-anon writes
# /etc/tor/torrc.d/00-shadow-socks.conf (the unix socket and 127.0.0.1:39,
# below 1024) and points torsocks.conf and proxychains4.conf at it. --offline:
# only files are written. Without it, tor-socks also loads the nat redirect of
# 9050 to Tor's port and restarts a running Tor, and this chroot shares the
# build machine's network namespace and services; shadow-anon-boot.service
# does all of it on the installed system at every start.
echo "[60-anon-setup] Moving Tor's SOCKS port below 1024..."
/usr/bin/shadow-anon tor-socks --offline

echo "[60-anon-setup] Done."
