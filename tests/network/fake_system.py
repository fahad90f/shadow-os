#!/usr/bin/env python3
"""A simulated laptop for tests/network/fix-network-test.sh.

One program that answers as nmcli, ip, curl, ping, getent, rfkill, systemctl,
iptables, ip6tables, ss, modinfo, sysctl, chattr, lsattr, shadow-anon,
shadow-killswitch, shadow-panic, nft and update-initramfs (dispatch on the
name it is called by). The machine's state
lives in $FAKE_STATE (JSON); system files live under $FAKE_ROOT, the same
directory scripts/fix-network.sh gets as SHADOW_NETFIX_ROOT.

The "physics" is deliberately small: a Wi-Fi network and an Ethernet network,
each of which reaches the internet unless the scenario's fault applies:
  rp          strict reverse-path filtering drops Wi-Fi answers while Ethernet
              is also connected
  random      the router gives no address to a MAC that changes every time
  macfilter   the router only serves the real hardware address
  powersave   the Wi-Fi chip passes no traffic with power saving on
  routerdns   the router's DNS server does not answer
  rotation    Hide Me's rotation timer keeps tearing Wi-Fi down
  portal      the Wi-Fi network shows a log-in page
  driver      nothing helps: the chip itself does not pass traffic
  dot853      the Wi-Fi network blocks port 853 (DNS-over-TLS) and every
              DNS server but its own

systemd-resolved (newer images) is simulated when the state has "resolved":
{"active": bool, "dot": "opportunistic"|"yes", "fails": bool}; it is
installed when $FAKE_ROOT/usr/lib/systemd/systemd-resolved exists. Its stub
file (run/systemd/resolve/stub-resolv.conf) exists while it runs. Names go to
its global server (the provider, a documentation address) unless the active
Wi-Fi profile carries the routing domain "~." (then its own server answers too).

The Shadow kill switch and panic (nftables tables inet shadow_egress and
inet shadow_panic) are simulated when the state has
  "egress": {"mode": "off"|"tor"|"vpn"|"tor-or-vpn", "table": bool, "lan": bool,
             "changed": bool (the table was edited by hand: loaded=false),
             "escape_hatch": bool, "error": str}
  "panic":  {"engaged": bool, "table": bool}
A panic table drops everything. The kill switch table drops every test bound
to one interface and lets the rest out only through Tor in the Tor modes
(Tor works, "tor_ok", and Hide Me's transparent proxy SHADOWTOR sends a
program's traffic to it) or through the VPN ("vpn_up") in the VPN modes.
"curl --socks5-hostname" goes to Tor's SOCKS port on loopback (127.0.0.1:39,
shadow-anon's TOR_SOCKS_PORT): it works when "tor_ok", whatever the kill
switch, and never during a panic; "socks_answers" is something else that
answers on any SOCKS port (a local user's listener) while Tor may not run.
tor@default is active when "tor_ok" (or "tor_running"); "ss -t" lists Tor's
SOCKS listener then, owned by root, or by "socks_uid" if set. "nft" only lists
tables; every other nft command is refused and recorded in "nft_refused".
"""
import json
import os
import re
import sys

STATE_PATH = os.environ["FAKE_STATE"]
ROOT = os.environ["FAKE_ROOT"]
GW = "192.168.1.1"


def load():
    with open(STATE_PATH) as f:
        return json.load(f)


def save(st):
    with open(STATE_PATH, "w") as f:
        json.dump(st, f, indent=1)


def log(st, argv):
    st.setdefault("calls", []).append(" ".join(argv))


def rpath(p):
    return os.path.join(ROOT, p.lstrip("/"))


# ---- derived state ------------------------------------------------------------
def nm_conf_value(key):
    """Last value of key across NetworkManager conf.d (an /etc file shadows the
    /usr/lib file of the same name), as NetworkManager merges them."""
    files = {}
    for d in ("usr/lib/NetworkManager/conf.d", "etc/NetworkManager/conf.d"):
        full = rpath(d)
        if os.path.isdir(full):
            for n in os.listdir(full):
                if n.endswith(".conf"):
                    files[n] = os.path.join(full, n)
    val = ""
    for n in sorted(files):
        with open(files[n]) as f:
            for line in f:
                line = line.strip()
                if line.startswith(key + "="):
                    val = line.split("=", 1)[1].strip()
    return val


def eff_mac(st, uuid):
    c = st["cons"][uuid]
    if c.get("mac"):
        return c["mac"]
    key = "wifi.cloned-mac-address" if c["type"] == "wifi" else "ethernet.cloned-mac-address"
    return nm_conf_value(key) or "preserve"


def read_int(p, default=0):
    try:
        with open(rpath(p)) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return default


def rp_strict(dev):
    a = read_int("proc/sys/net/ipv4/conf/all/rp_filter")
    d = read_int(f"proc/sys/net/ipv4/conf/{dev}/rp_filter")
    return max(a, d) == 1


def hooked(st, table, chain):
    return chain in st.get("hooks", {}).get(table, [])


def link_ok(st, dev):
    d = st["devices"][dev]
    if d["state"] != "connected":
        return False
    if d["type"] == "wifi":
        if st.get("radio", "enabled") != "enabled" or st.get("rfkill_soft") or st.get("rfkill_hard"):
            return False
    return True


def addr(st, dev):
    if not link_ok(st, dev):
        return None
    d = st["devices"][dev]
    sc = st["scenario"]
    if d["type"] == "wifi":
        m = d.get("mac_now", "random")   # NetworkManager sets the address when it connects
        if sc == "random" and m == "random":
            return None
        if sc == "macfilter" and m != "permanent":
            return None
        if sc == "dhcpdead":
            return None
        return "192.168.1.23/24"
    return "192.168.1.40/24"


def eth_up(st):
    return any(d["type"] == "ethernet" and addr(st, n) for n, d in st["devices"].items())


def gw_answers(st, dev):
    if not addr(st, dev):
        return False
    d = st["devices"][dev]
    if d["type"] == "wifi" and st["scenario"] in ("driver", "powersave") and not (
            st["scenario"] == "powersave" and st["cons"][d["con"]].get("powersave") == "2"):
        return False
    return True


def internet(st, dev, bound):
    """Can traffic leaving through dev reach the internet (no DNS involved)?"""
    if not gw_answers(st, dev):
        return False
    d = st["devices"][dev]
    sc = st["scenario"]
    if d["type"] == "wifi":
        if sc == "rp" and bound and eth_up(st) and rp_strict(dev):
            return False
        if sc == "rotation" and st.get("rotation"):
            return False
    return True


def best_dev(st):
    eth = [n for n, d in st["devices"].items() if d["type"] == "ethernet" and addr(st, n)]
    wifi = [n for n, d in st["devices"].items() if d["type"] == "wifi" and addr(st, n)]
    return (eth + wifi + [None])[0]


def ghost_blocks(st, bound):
    """Ghost Mode: True = blocked, False = let through (via Tor), None = not on."""
    if not (hooked(st, "filter", "SHADOWKILL") or hooked(st, "nat", "SHADOWTOR")):
        return None
    if hooked(st, "nat", "SHADOWTOR") and not bound:
        return not st.get("tor_ok")
    return hooked(st, "filter", "SHADOWKILL") or (hooked(st, "nat", "SHADOWTOR") and not st.get("tor_ok"))


def egress_blocks(st, bound):
    """The Shadow panic or kill switch: True = blocked, False = let through (Tor/VPN), None = no table."""
    if st.get("panic", {}).get("table"):
        return True
    e = st.get("egress") or {}
    if not e.get("table"):
        return None
    if bound:
        return True
    mode = e.get("mode")
    through = (mode in ("tor", "tor-or-vpn") and st.get("tor_ok") and hooked(st, "nat", "SHADOWTOR")) or \
        (mode in ("vpn", "tor-or-vpn") and st.get("vpn_up"))
    return not through


PROVIDER = "192.0.2.53"       # resolved's global DNS server in these tests (RFC 5737)
STUB = "run/systemd/resolve/stub-resolv.conf"


def resolved_installed():
    return os.path.exists(rpath("usr/lib/systemd/systemd-resolved"))


def resolved_set_active(st, active):
    st.setdefault("resolved", {})["active"] = active
    stub = rpath(STUB)
    if active:
        os.makedirs(os.path.dirname(stub), exist_ok=True)
        with open(stub, "w") as f:
            f.write("nameserver 127.0.0.53\noptions edns0 trust-ad\n")
    elif os.path.exists(stub):
        os.unlink(stub)


def resolved_dns_ok(st):
    """A lookup through systemd-resolved (resolv.conf is its stub or 127.0.0.53)."""
    r = st.get("resolved", {})
    if not resolved_installed() or not r.get("active"):
        return False
    hideme = rpath("run/systemd/resolved.conf.d/95-shadow-anon.conf")
    if os.path.exists(hideme):
        return bool(st.get("tor_ok") and st.get("tor_listening"))
    dev = best_dev(st)
    if dev is None or not internet(st, dev, False):
        return False
    d = st["devices"][dev]
    if d["type"] == "wifi" and st["scenario"] == "dot853":
        # Only the network's own DNS server answers here.
        return "~." in st["cons"][d["con"]].get("search", [])
    return True


def resolv_text():
    p = rpath("etc/resolv.conf")
    if os.path.islink(p) and not os.path.exists(p):
        return None
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return None


def dns_ok(st):
    txt = resolv_text()
    if txt is None:
        return False
    servers = re.findall(r"^nameserver\s+(\S+)", txt, re.M)
    if not servers:
        return False
    k = egress_blocks(st, False)
    if k is not None:
        return not k
    g = ghost_blocks(st, False)
    if g is not None:
        return not g
    if servers == ["127.0.0.53"]:
        return resolved_dns_ok(st)
    if servers == ["127.0.0.1"]:
        tor = (hooked(st, "nat", "SHADOWDNS") or hooked(st, "nat", "SHADOWTOR")) and st.get("tor_ok")
        return bool(tor or st.get("listen53"))
    dev = best_dev(st)
    if dev is None or not internet(st, dev, False):
        return False
    if st["scenario"] == "routerdns":
        return any(s in ("9.9.9.9", "1.1.1.1") for s in servers)
    if st["scenario"] == "dot853":
        return False                      # this network answers only from its own server, not listed here
    return True


def nm_rewrite(st):
    """NetworkManager writes /etc/resolv.conf when it manages DNS and the file
    is a plain file or missing (rc-manager=auto without systemd-resolved). With
    the systemd-resolved plugin and resolved installed it never touches it."""
    if os.path.exists(rpath("etc/NetworkManager/conf.d/90-shadow-dns.conf")):
        return
    if nm_conf_value("dns") == "systemd-resolved" and resolved_installed():
        return
    p = rpath("etc/resolv.conf")
    if st.get("immutable") or os.path.islink(p):
        return
    lines = ["# Generated by NetworkManager"]
    for n, d in st["devices"].items():
        if d["state"] == "connected" and d.get("con"):
            for s in filter(None, st["cons"][d["con"]].get("dns", "").split(",")):
                lines.append(f"nameserver {s.split('#')[0]}")    # NetworkManager drops the TLS name
    lines.append(f"nameserver {GW}")
    txt = "\n".join(lines) + "\n"
    with open(p, "w") as f:
        f.write(txt)
    os.makedirs(rpath("run/NetworkManager"), exist_ok=True)
    with open(rpath("run/NetworkManager/resolv.conf"), "w") as f:
        f.write(txt)


# ---- commands --------------------------------------------------------------------
STATE_NAMES = {"connected": "100 (connected)", "disconnected": "30 (disconnected)",
               "unavailable": "20 (unavailable)"}
PS_NAMES = {"0": "0 (default)", "1": "1 (ignore)", "2": "2 (disable)", "3": "3 (enable)"}
PS_IN = {"default": "0", "ignore": "1", "disable": "2", "enable": "3"}


def cmd_nmcli(st, a):
    fields = None
    while a and a[0].startswith("-"):
        o = a.pop(0)
        if o in ("-f", "-g", "--wait"):
            v = a.pop(0)
            if o != "--wait":
                fields = v
    if a[:2] == ["general", "status"]:
        print("connected:full")
        return 0
    if a[:1] == ["general"] and len(a) == 1:
        print("connected:full:enabled:" + st.get("radio", "enabled"))
        return 0
    if a[:2] == ["general", "reload"]:
        nm_rewrite(st)
        return 0
    if a[:2] == ["radio", "wifi"]:
        if len(a) == 3:
            st["radio"] = "enabled" if a[2] == "on" else "disabled"
        else:
            print(st.get("radio", "enabled"))
        return 0
    if a[:2] == ["device", "status"]:
        for n, d in st["devices"].items():
            con = st["cons"][d["con"]]["id"] if d["state"] == "connected" and d.get("con") else ""
            print(f"{n}:{d['type']}:{d['state']}:{con}")
        return 0
    if a[:2] == ["device", "show"]:
        d = st["devices"].get(a[2])
        if not d:
            return 10
        print(STATE_NAMES.get(d["state"], d["state"]))
        return 0
    if a[:2] == ["device", "connect"]:
        dev = a[2]
        d = st["devices"][dev]
        if d["type"] == "wifi" and st.get("saved_wifi") and st.get("radio", "enabled") == "enabled" \
                and not st.get("rfkill_soft") and not st.get("rfkill_hard"):
            d["state"] = "connected"
            d["con"] = st["wifi_uuid"]
            d["mac_now"] = eff_mac(st, d["con"])
            nm_rewrite(st)
            return 0
        print("Error: no network found", file=sys.stderr)
        return 4
    if a[:2] == ["connection", "show"]:
        if "--active" in a:
            for n, d in st["devices"].items():
                if d["state"] == "connected" and d.get("con"):
                    print(f"{d['con']}:{n}")
            return 0
        c = st["cons"][a[2]]
        key = fields
        val = {"connection.id": c["id"],
               "802-11-wireless.cloned-mac-address": c.get("mac", ""),
               "802-11-wireless.powersave": PS_NAMES[c.get("powersave", "0")],
               "ipv4.dns": c.get("dns", ""),
               "ipv4.dns-search": ",".join(c.get("search", []))}[key]
        print(val)
        return 0
    if a[:2] == ["connection", "modify"]:
        c = st["cons"][a[2]]
        prop, val = a[3], a[4]
        if prop.endswith("cloned-mac-address"):
            c["mac"] = val
        elif prop.endswith("powersave"):
            c["powersave"] = PS_IN.get(val, val)
        elif prop == "ipv4.dns":
            c["dns"] = ",".join(val.replace(",", " ").split())
        elif prop == "+ipv4.dns-search":
            if val not in c.setdefault("search", []):
                c["search"].append(val)
        elif prop == "-ipv4.dns-search":
            if val in c.get("search", []):
                c["search"].remove(val)
        else:
            print(f"fake nmcli: unknown property {prop}", file=sys.stderr)
            return 2
        return 0
    if a[:2] == ["connection", "up"]:
        uuid = a[2]
        dev = a[a.index("ifname") + 1]
        d = st["devices"][dev]
        if d["type"] == "wifi" and (st.get("radio", "enabled") != "enabled" or st.get("rfkill_hard")):
            return 4
        d["state"] = "connected"
        d["con"] = uuid
        d["mac_now"] = eff_mac(st, uuid)
        st.setdefault("reconnects", 0)
        st["reconnects"] += 1
        nm_rewrite(st)
        return 0
    print(f"fake nmcli: unhandled {a}", file=sys.stderr)
    return 2


def cmd_ip(st, a):
    a = [x for x in a if x not in ("-4", "-o")]
    if a[:2] == ["addr", "show"]:
        dev = a[a.index("dev") + 1]
        ad = addr(st, dev)
        if ad:
            print(f"3: {dev}    inet {ad} brd 192.168.1.255 scope global dynamic noprefixroute {dev}")
        return 0
    if a[:3] == ["route", "show", "default"]:
        devs = [a[a.index("dev") + 1]] if "dev" in a else list(st["devices"])
        for dev in devs:
            if addr(st, dev):
                metric = 600 if st["devices"][dev]["type"] == "wifi" else 100
                print(f"default via {GW} dev {dev} proto dhcp src {addr(st, dev).split('/')[0]} metric {metric}")
        return 0
    if a[:2] == ["link", "show"]:
        dev = a[a.index("dev") + 1]
        d = st["devices"][dev]
        perm = ""
        if d.get("mac_now", "random") not in ("permanent", "preserve"):
            perm = " permaddr 3c:22:fb:11:22:33"
        print(f"3: {dev}: <BROADCAST,MULTICAST,UP> mtu 1500\n    link/ether 02:aa:bb:cc:dd:ee brd ff:ff:ff:ff:ff:ff{perm}")
        return 0
    if a[:1] == ["neigh"]:
        dev = a[a.index("dev") + 1]
        if gw_answers(st, dev):
            print(f"{GW} lladdr 3c:22:fb:00:00:01 REACHABLE")
        return 0
    print(f"fake ip: unhandled {a}", file=sys.stderr)
    return 2


def cmd_curl(st, a):
    iface = None
    out = None
    fmt = ""
    url = None
    socks = None
    i = 0
    while i < len(a):
        x = a[i]
        if x == "--interface":
            iface = a[i + 1]; i += 2; continue
        if x == "--socks5-hostname":
            socks = a[i + 1]; i += 2; continue
        if x in ("-m", "--connect-timeout"):
            i += 2; continue
        if x == "-o":
            out = a[i + 1]; i += 2; continue
        if x == "-w":
            fmt = a[i + 1].replace("\\n", "\n"); i += 2; continue
        if x.startswith("-"):
            i += 1; continue
        url = x; i += 1
    host = re.sub(r"^https?://([^/]+).*", r"\1", url)

    def fail():
        sys.stdout.write(fmt.replace("%{http_code}", "000").replace("%{time_connect}", "0.000000"))
        return 7

    if url.endswith(":853/"):
        # A TCP connection to the DNS provider's DNS-over-TLS port: it opens
        # (then HTTP over it fails) unless this network blocks the port.
        dev = iface or best_dev(st)
        if dev is None or not internet(st, dev, iface is not None):
            return fail()
        if st["scenario"] == "dot853" and st["devices"].get(dev, {}).get("type") == "wifi":
            sys.stdout.write(fmt.replace("%{time_connect}", "0.000000"))
            return 28
        sys.stdout.write(fmt.replace("%{time_connect}", "0.012000"))
        return 52

    if socks is not None:                 # through Tor: Tor resolves the name and connects
        dev = best_dev(st)
        if st.get("panic", {}).get("table") or dev is None or not internet(st, dev, False):
            return fail()
        if not st.get("socks_answers") and (socks != "127.0.0.1:39" or not st.get("tor_ok")):
            return fail()
        st["tor_checks"] = st.get("tor_checks", 0) + 1
        sys.stdout.write("NetworkManager is online\n" + fmt.replace("%{http_code}", "200"))
        return 0

    by_ip = re.fullmatch(r"[0-9.]+", host) is not None
    if not by_ip and not dns_ok(st):
        return fail()
    g = ghost_blocks(st, iface is not None)
    k = egress_blocks(st, iface is not None)
    if g is True or k is True:
        return fail()
    dev = iface or best_dev(st)
    if k is False and (dev is None or not internet(st, dev, False)):
        return fail()                     # the tunnel needs a working network under it
    if g is None and k is None:
        if dev is None or not internet(st, dev, iface is not None):
            return fail()
    if st["scenario"] == "portal" and st["devices"].get(dev, {}).get("type") == "wifi":
        body, code = "<html>Please sign in</html>\n", "200"
    elif by_ip:
        body, code = "<html>301</html>\n", "301"
    elif "debian" in host:
        body, code = "NetworkManager is online\n", "200"
    else:
        body, code = "success\n", "200"
    if out != "/dev/null":
        sys.stdout.write(body)
    sys.stdout.write(fmt.replace("%{http_code}", code))
    return 0


def cmd_ping(st, a):
    dev = a[a.index("-I") + 1]
    if egress_blocks(st, True) and not (st.get("egress", {}).get("lan") and not st.get("panic", {}).get("table")):
        return 1                          # the ARP check (ip neigh) still sees the router
    return 0 if gw_answers(st, dev) else 1


def cmd_getent(st, a):
    if a[0] == "hosts":
        try:
            with open(rpath("etc/hosts")) as f:
                for line in f:
                    if a[1] in line.split()[1:]:
                        print(line.strip())
                        return 0
        except OSError:
            pass
        return 2
    if a[0] == "ahosts":
        if dns_ok(st):
            print(f"151.101.2.132   STREAM {a[1]}")
            return 0
        return 2
    return 2


def cmd_rfkill(st, a):
    if a and a[0] == "unblock":
        st["rfkill_soft"] = False
        return 0
    cols = a[a.index("-o") + 1].split(",")
    row = []
    for c in cols:
        row.append({"TYPE": "wlan",
                    "SOFT": "blocked" if st.get("rfkill_soft") else "unblocked",
                    "HARD": "blocked" if st.get("rfkill_hard") else "unblocked"}[c])
    print(" ".join(row))
    print(" ".join({"TYPE": "bluetooth", "SOFT": "unblocked", "HARD": "unblocked"}[c] for c in cols))
    return 0


def tor_running(st):
    return bool(st.get("tor_running", st.get("tor_ok")))


def cmd_systemctl(st, a):
    a = [x for x in a if x != "--quiet"]
    if a[0] in ("is-active", "is-enabled") and a[1] == "shadow-anon-rotate.timer":
        return 0 if st.get("rotation") else 3
    if a == ["is-active", "tor@default"]:
        return 0 if tor_running(st) else 3
    if a[-1] == "systemd-resolved.service":
        if not resolved_installed():
            return 5
        if a[0] == "is-active":
            return 0 if st.get("resolved", {}).get("active") else 3
        if a[0] in ("start", "restart"):
            if st.get("resolved", {}).get("fails"):
                resolved_set_active(st, False)
                return 1
            resolved_set_active(st, True)
            st["resolved_restarts"] = st.get("resolved_restarts", 0) + 1
            return 0
    if a[0] == "disable" and a[-1] == "shadow-anon-rotate.timer":
        st["rotation"] = False
        return 0
    if a[0] == "reload" and a[1] == "NetworkManager":
        nm_rewrite(st)
        return 0
    if a[0] == "start":
        return 0
    print(f"fake systemctl: unhandled {a}", file=sys.stderr)
    return 1


def cmd_iptables(st, a, v6=False):
    table = "filter"
    if a[:1] == ["-t"]:
        table = a[1]; a = a[2:]
    chains = st.setdefault("chains", {}).setdefault(table, [])
    hooks = st.setdefault("hooks", {}).setdefault(table, [])
    if v6:   # the fake keeps one IPv6 kill-switch flag only
        if a[:1] == ["-D"]:
            return 1
        return 0
    if a[0] == "-S":
        if a[1] == "OUTPUT":
            print("-P OUTPUT ACCEPT")
            for h in hooks:
                print(f"-A OUTPUT -j {h}")
            return 0
        if a[1] in chains:
            print(f"-N {a[1]}")
            return 0
        return 1
    if a[0] == "-D" and a[1] == "OUTPUT":
        if a[3] in hooks:
            hooks.remove(a[3])
            return 0
        return 1
    if a[0] == "-F":
        return 0 if a[1] in chains else 1
    if a[0] == "-X":
        if a[1] in chains and a[1] not in hooks:
            chains.remove(a[1])
            return 0
        return 1
    return 1


def cmd_ss(st, a):
    if a and a[0].startswith("-") and "t" in a[0]:       # TCP listeners: Tor's SOCKS port
        if tor_running(st):
            owner = " uid:%d" % st["socks_uid"] if st.get("socks_uid") else ""   # root: no uid field
            print("LISTEN 0 4096 127.0.0.1:39 0.0.0.0:*%s ino:4711 sk:1 cgroup:/system.slice/tor@default.service <->"
                  % owner)
        return 0
    if st.get("listen53"):
        print("UNCONN 0 0 127.0.0.1:53 0.0.0.0:*")
    if st.get("resolved", {}).get("active") and resolved_installed():
        print("UNCONN 0 0 127.0.0.53%lo:53 0.0.0.0:*")
        print("UNCONN 0 0 127.0.0.54:53 0.0.0.0:*")
    if st.get("tor_listening"):                 # Tor's DNSPort (shadow-anon's DNS_PORT)
        print("UNCONN 0 0 127.0.0.1:%d 0.0.0.0:*" % st.get("tor_port", 26))
    if st.get("avahi"):                         # mDNS on every address (Tor's DNSPort was 5353 before)
        print("UNCONN 0 0 0.0.0.0:5353 0.0.0.0:*")
    return 0


def cmd_busctl(st, a):
    r = st.get("resolved", {})
    if not resolved_installed() or not r.get("active"):
        print("Failed to get property: Unit dbus-org.freedesktop.resolve1.service not found.", file=sys.stderr)
        return 1
    prop = a[-1]
    value = {"DNSOverTLS": r.get("dot", "opportunistic"), "DNSSEC": "allow-downgrade"}.get(prop)
    if value is None:
        return 1
    print(f's "{value}"')
    return 0


def cmd_resolvectl(st, a):
    r = st.get("resolved", {})
    if not resolved_installed() or not r.get("active"):
        return 1
    if a[:1] == ["dns"]:
        print(f"Global: {PROVIDER}#dns.example.net 2001:db8::53#dns.example.net")
        print(f"Link 3 (wlan0): {GW}")
        return 0
    return 1


MODPARAMS = {"rtw88_core": ["disable_lps_deep:Set Y to disable Deep PS (bool)", "support_bf:Set Y to enable beamformee support (bool)"],
             "rtw88_pci": ["disable_msi:Set Y to disable MSI interrupt support (bool)", "disable_aspm:Set Y to disable PCI ASPM support (bool)"]}


def cmd_modinfo(st, a):
    mod = a[-1]
    if mod in MODPARAMS:
        print("\n".join(MODPARAMS[mod]))
        return 0
    print(f"modinfo: ERROR: Module {mod} not found.", file=sys.stderr)
    return 1


def cmd_sysctl(st, a):
    for x in a:
        if "=" in x:
            k, v = x.split("=", 1)
            p = rpath("proc/sys/" + k.replace(".", "/"))
            with open(p, "w") as f:
                f.write(v + "\n")
    return 0


def cmd_chattr(st, a):
    if a[0] == "-i":
        st["immutable"] = False
    elif a[0] == "+i":
        st["immutable"] = True
    return 0


def cmd_lsattr(st, a):
    f = a[-1]
    print(("----i---------e------- " if st.get("immutable") else "--------------e------- ") + f)
    return 0


KS_MARKER = "shadow-killswitch mode={mode} lan={lan} sha=0123abcd"


def cmd_nft(st, a):
    """Lists the two Shadow tables; refuses (and records) everything else."""
    if a[:3] == ["list", "table", "inet"] and len(a) == 4:
        if a[3] == "shadow_egress" and st.get("egress", {}).get("table"):
            e = st["egress"]
            marker = KS_MARKER.format(mode=e.get("mode", "tor"), lan="yes" if e.get("lan") else "no")
            print("table inet shadow_egress {\n\tcomment \"%s\"\n\tchain output {\n"
                  "\t\ttype filter hook output priority -50; policy drop;\n\t}\n}" % marker)
            return 0
        if a[3] == "shadow_panic" and st.get("panic", {}).get("table"):
            print("table inet shadow_panic {\n\tcomment \"shadow-panic\"\n}")
            return 0
        print(f"Error: No such file or directory\nlist table inet {a[3]}", file=sys.stderr)
        return 1
    st.setdefault("nft_refused", []).append(" ".join(a))
    print("fake nft: only 'list table inet NAME' is allowed here", file=sys.stderr)
    return 1


def cmd_shadow_killswitch(st, a):
    e = st.get("egress")
    if a[:2] == ["status", "--json"]:
        if e is None:
            e = {"mode": "off", "table": False}
        mode = e.get("mode", "off")
        active = mode != "off"
        loaded = bool(e.get("table")) and not e.get("changed")
        error = e.get("error")
        if active and not loaded and not error:
            error = "the kill switch table is not loaded" if not e.get("table") else \
                "the loaded kill switch table is not the one this configuration produces"
        if not active and e.get("table") and not error:
            error = "the kill switch is off but its table is still loaded"
        print(json.dumps({"mode": mode, "active": active, "loaded": loaded if active else bool(e.get("table")),
                          "since": None, "lan": bool(e.get("lan")), "endpoints": 0, "error": error,
                          "escape_hatch": bool(e.get("escape_hatch")), "version": 1,
                          "persistent_mode": e.get("persistent_mode", mode), "runtime_override": False}))
        return 3 if error else 0
    if a[:1] == ["set"]:                  # fix-network.sh must never do this
        st.setdefault("egress", {}).update({"mode": a[1], "table": a[1] != "off", "changed": False})
        return 0
    return 2


def cmd_shadow_panic(st, a):
    p = st.get("panic") or {"engaged": False, "table": False}
    if a[:2] == ["status", "--json"]:
        print(json.dumps({"engaged": bool(p.get("engaged")), "loaded": bool(p.get("table")), "error": None,
                          "since": None, "steps": {}, "version": 1}))
        return 0
    if a[:1] == ["release"]:              # fix-network.sh must never do this
        st["panic"] = {"engaged": False, "table": False}
        return 0
    return 2


def cmd_shadow_anon(st, a):
    hooks = st.setdefault("hooks", {})
    if a[0] == "kill-switch-off":
        for t in hooks.values():
            if "SHADOWKILL" in t:
                t.remove("SHADOWKILL")
        # The Shadow kill switch, if Hide Me turned it on: back to the mode before it.
        state_file = rpath("run/shadow-anon-state.json")
        if os.path.exists(state_file) and not st.get("anon_ks_off_fails"):
            with open(state_file) as f:
                anon = json.load(f)
            if anon.get("killswitch_by") and st.get("egress", {}).get("mode") == "tor":
                before = (anon.get("killswitch_before") or {}).get("mode") or "off"
                st["egress"].update({"mode": before, "table": before != "off", "changed": False})
                for k in ("killswitch_by", "killswitch_before", "killswitch_persisted"):
                    anon.pop(k, None)
                anon["kill_switch"] = False
                with open(state_file, "w") as f:
                    json.dump(anon, f)
    elif a[0] == "transparent-proxy-off":
        if "SHADOWTOR" in hooks.get("nat", []):
            hooks["nat"].remove("SHADOWTOR")
    elif a[0] == "dns-protect-off" and os.path.exists(rpath("run/systemd/resolved.conf.d/95-shadow-anon.conf")):
        # Hide Me with systemd-resolved: only its runtime resolved setting.
        os.unlink(rpath("run/systemd/resolved.conf.d/95-shadow-anon.conf"))
    elif a[0] == "dns-protect-off":
        st["immutable"] = False
        if "SHADOWDNS" in hooks.get("nat", []):
            hooks["nat"].remove("SHADOWDNS")
        lock = rpath("etc/NetworkManager/conf.d/90-shadow-dns.conf")
        if os.path.exists(lock):
            os.unlink(lock)
        bk = rpath("etc/resolv.conf.shadow-backup")
        if os.path.exists(bk):
            os.replace(bk, rpath("etc/resolv.conf"))
        else:
            with open(rpath("etc/resolv.conf"), "w") as f:
                f.write("nameserver 1.1.1.1\nnameserver 9.9.9.9\n")
        nm_rewrite(st)
    return 0


def main():
    name = os.path.basename(sys.argv[0])
    args = sys.argv[1:]
    st = load()
    log(st, [name] + args)
    handlers = {
        "nmcli": cmd_nmcli, "ip": cmd_ip, "curl": cmd_curl, "ping": cmd_ping,
        "getent": cmd_getent, "rfkill": cmd_rfkill, "systemctl": cmd_systemctl,
        "iptables": cmd_iptables, "ip6tables": lambda s, x: cmd_iptables(s, x, True),
        "ss": cmd_ss, "modinfo": cmd_modinfo, "sysctl": cmd_sysctl, "chattr": cmd_chattr,
        "lsattr": cmd_lsattr, "shadow-anon": cmd_shadow_anon,
        "shadow-killswitch": cmd_shadow_killswitch, "shadow-panic": cmd_shadow_panic, "nft": cmd_nft,
        "busctl": cmd_busctl, "resolvectl": cmd_resolvectl,
        "update-initramfs": lambda s, x: 0,
        "hostname": lambda s, x: print(s.get("hostname", "shadow-3ebrpj3t")) or 0,
        "journalctl": lambda s, x: print("NetworkManager[1]: <info> dhcp4 (wlo1): activation: beginning transaction\n"
                                         "NetworkManager[1]: <info> device (wlan0): set-hw-addr: set MAC address to 3C:22:FB:11:22:33 (permanent)") or 0,
    }
    rc = handlers[name](st, list(args))
    save(st)
    sys.stdout.flush()
    sys.exit(rc)


if __name__ == "__main__":
    main()
