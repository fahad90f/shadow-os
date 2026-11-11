#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test aid, not installed. Drives every Nm operation (D-Bus) against the
# NetworkManager of nm-sandbox.sh and prints nmcli's view after each step.
# Build the harness first (tests/harness), then:
#
#   scripts/dev.sh exec --dir netcenter -- unshare -n -m --propagation unchanged \
#       /src/applications/network-center/tests/nm-sandbox.sh /src/applications/network-center/tests/nm-operations.sh
H=/tmp/nc-harness/b/ncharness
export QT_QPA_PLATFORM=offscreen HOME=/tmp/nc-nm/home; mkdir -p $HOME
sleep 2
for c in "Wired connection 1" "Wired connection 2"; do nmcli con delete "$c" >/dev/null 2>&1; done
nmcli con add type ethernet ifname nctest0 con-name nc-eth ipv4.method manual ipv4.addresses 10.77.0.2/24 ipv6.method disabled connection.autoconnect no >/dev/null
printf '[Interface]\nPrivateKey = YAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=\nAddress = 10.99.0.2/24\n[Peer]\nPublicKey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=\nAllowedIPs = 10.99.0.0/24\nEndpoint = 10.77.0.1:51820\n' > /tmp/nc-nm/wgtest.conf
$H 'up|nc-eth' 'ip|nc-eth|{"ipv4":{"addresses":"10.77.0.3/24, 10.77.0.4/24","gateway":"10.77.0.1","dns":"9.9.9.9 1.1.1.1","search":"example.test","ignoreAutoDns":true},"ipv6":{"method":"manual","addresses":"fd00:77::3/64","dns":"2620:fe::fe"},"dnsOverTls":"opportunistic"}|1'
nmcli -t -f connection.dns-over-tls,ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,ipv4.dns-search,ipv4.ignore-auto-dns,ipv6.method,ipv6.addresses,ipv6.dns con show nc-eth
ip -br addr show nctest0; ip route show default; ping -c1 -W1 10.77.0.1 | tail -2 | head -1
$H 'ip|nc-eth|{"ipv4":{"dns":"","search":"","gateway":""},"ipv6":{"method":"disabled"},"dnsOverTls":"default"}|0' 
nmcli -t -f connection.dns-over-tls,ipv4.addresses,ipv4.gateway,ipv4.dns,ipv4.dns-search,ipv6.method,ipv6.addresses,ipv6.dns con show nc-eth
$H 'import|wireguard|/tmp/nc-nm/wgtest.conf' 'import|wireguard|/etc/hostname' 'auto|wgtest|0' 'up|wgtest'
nmcli -t -f NAME,TYPE,DEVICE,STATE,AUTOCONNECT con show; ip -br addr | grep -i wg
/build/netcenter/bin/shadow-network-center --dump vpn | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["activeCount"], [(c["name"],c["vpnLabel"],c["state"],c["device"],c["autoconnect"]) for c in d["connections"]])'
nmcli con add type wifi con-name nc-wifi2 ssid "Other" wifi-sec.key-mgmt wpa-psk connection.autoconnect no >/dev/null
$H 'down|wgtest' 'del|wgtest' 'secret|nc-wifi2|some password' 'del|nc-eth' 'devup|nctest0'
nmcli -t -f NAME,TYPE,DEVICE,STATE con show
