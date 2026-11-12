#!/bin/bash
# Shadow OS — anonymization engine setup hook
# Installs tools required by shadow-anon and hardens the base system at build time.
set -euo pipefail

echo "[60-anon-setup] Installing anonymization stack..."

apt-get install -y --no-install-recommends \
    tor \
    torsocks \
    obfs4proxy \
    macchanger \
    rfkill \
    iptables \
    iptables-persistent \
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

echo "[60-anon-setup] Blocking Bluetooth at build time (rfkill)..."
rfkill block bluetooth 2>/dev/null || true

echo "[60-anon-setup] Applying kernel hardening sysctl..."
if [ -f /etc/sysctl.d/90-shadow-anon.conf ]; then
    sysctl -p /etc/sysctl.d/90-shadow-anon.conf 2>/dev/null || true
fi

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

echo "[60-anon-setup] Done."
