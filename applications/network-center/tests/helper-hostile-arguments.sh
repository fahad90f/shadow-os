#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test aid, not installed. Calls network-helper directly (as root, in the
# build chroot) with hostile arguments, compares "sockets" with ss and runs a
# real capture on lo, printing the uid and capabilities tcpdump ends up with.
#
#   scripts/dev.sh exec --dir netcenter -- /src/applications/network-center/tests/helper-hostile-arguments.sh
H=/src/applications/network-center/network-helper
t() { printf '%-70s ' "$*"; out=$("$H" "$@" </dev/null 2>&1); rc=$?; echo "rc=$rc ${out:0:200}"; }
echo "== rejected"
t
t bogus
t sockets extra
t sockets --help
t capture
t capture --interface
t capture --interface 'lo;id'
t capture --interface '../../etc'
t capture --interface '-w'
t capture --interface 'lo' --interface lo
t capture --interface nosuchif0
t capture --interface 'lo
x'
t capture --interface lo -w /tmp/evil.pcap
t capture --interface lo --write /tmp/evil.pcap
t capture --interface lo --filter '-w /tmp/evil.pcap'
t capture --interface lo --filter ' -z /bin/sh'
t capture --interface lo --filter 'tcp; touch /tmp/pwned'
t capture --interface lo --filter '$(touch /tmp/pwned)'
t capture --interface lo --filter '`id`'
t capture --interface lo --filter "host 'a'"
t capture --interface lo --filter ''
t capture --interface lo --filter "$(printf 'tcp\nport 80')"
t capture --interface lo --filter "$(head -c 1100 /dev/zero | tr '\0' a)"
t capture --interface lo --snaplen 10
t capture --interface lo --snaplen 9999999
t capture --interface lo --snaplen '100 -w /tmp/x'
t capture --interface lo --snaplen -5
t capture --filter tcp
t capture --interface lo --filter 'not a valid filter expression'
ls /tmp/evil.pcap /tmp/pwned 2>&1
echo "== sockets: helper vs ss"
"$H" sockets | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ok"], len(d["lines"]), sum(1 for l in d["lines"] if "users:" in l))'
ss -H -O -t -u -n -a -p | awk '{n++} /users:/{u++} END{print "ss direct:", n, u}'
echo "== capture on lo (helper stops when stdin closes)"
( sleep 6 ) | "$H" capture --interface lo --filter 'icmp and host 127.0.0.1' --snaplen 128 > /tmp/nc-cap.pcap 2>/tmp/nc-cap.err &
sleep 2
for p in $(pgrep -x tcpdump); do echo "tcpdump pid $p uid: $(awk '/^Uid/{print $2,$3}' /proc/$p/status) CapEff $(awk '/^CapEff/{print $2}' /proc/$p/status) CapBnd $(awk '/^CapBnd/{print $2}' /proc/$p/status) NoNewPrivs $(awk '/^NoNewPrivs/{print $2}' /proc/$p/status) cmd: $(tr '\0' ' ' < /proc/$p/cmdline)"; done
ping -c 3 -i 0.3 127.0.0.1 >/dev/null
wait
cat /tmp/nc-cap.err; ls -la /tmp/nc-cap.pcap; tcpdump -n -r /tmp/nc-cap.pcap 2>&1 | tail -4
