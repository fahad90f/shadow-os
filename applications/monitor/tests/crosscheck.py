#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-check "nexus-monitor --dump <topic>" against the system's own tools.

    sudo scripts/dev.sh run --dir monitor python3 /src/applications/monitor/tests/crosscheck.py

Compares, on the machine it runs on:

    memory     with  free -b                    (and /proc/meminfo)
    cpu        with  /proc/stat deltas over the same half second, nproc, /proc/loadavg
    disks      with  df -B1 for every listed file system, lsblk and /proc/diskstats
    network    with  /proc/net/dev and "ip -j addr"
    processes  with  ps -e

Values that move while the tools run (used memory, counters, RSS) are compared
with a tolerance; identities (totals, names, owners, command lines) exactly.
Exits 1 if any check fails. Sensors, battery, GPU and services are only
checked for well-formed output here: the build chroot has none of them (see
fixture.py for those).
"""
import json
import os
import shutil
import subprocess
import sys
import time

FAILED = []
CHECKS = 0


def binary():
    exe = shutil.which("nexus-monitor")
    if exe:
        return exe
    # scripts/dev.sh run exports LD_LIBRARY_PATH=<build dir>/lib.
    for path in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        candidate = os.path.join(os.path.dirname(path.rstrip("/")), "bin", "nexus-monitor")
        if os.access(candidate, os.X_OK):
            return candidate
    sys.exit("crosscheck: nexus-monitor not found (PATH or LD_LIBRARY_PATH=<build>/lib)")


EXE = binary()


def dump(topic):
    out = subprocess.run([EXE, "--dump", topic], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        sys.exit(f"crosscheck: --dump {topic} exited {out.returncode}: {out.stderr.strip()}")
    return json.loads(out.stdout)


def run(*argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=60).stdout


def check(ok, what, detail=""):
    global CHECKS
    CHECKS += 1
    if ok:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}  {detail}")
        FAILED.append(what)


def close(a, b, absolute=0, relative=0.0):
    return abs(a - b) <= max(absolute, relative * max(abs(a), abs(b)))


# ---- memory ------------------------------------------------------------------------
def check_memory():
    print("memory  (free -b)")
    free = run("free", "-b").splitlines()
    mem = dump("memory")
    cols = free[1].split()                      # Mem: total used free shared buff/cache available
    total, used, _free, shared, cache, available = (int(x) for x in cols[1:7])
    swap = [int(x) for x in free[2].split()[1:4]]   # Swap: total used free
    slack = 256 << 20                           # other builds share this machine
    check(mem["total"] == total, "total equals free's total", f"{mem['total']} != {total}")
    check(close(mem["used"], used, slack), "used ~ free's used (MemTotal - MemAvailable)", f"{mem['used']} vs {used}")
    check(close(mem["available"], available, slack), "available ~ free's available", f"{mem['available']} vs {available}")
    check(close(mem["buffers"] + mem["cached"], cache, slack), "buffers + cached ~ free's buff/cache",
          f"{mem['buffers'] + mem['cached']} vs {cache}")
    check(close(mem["shared"], shared, 64 << 20), "shared ~ free's shared", f"{mem['shared']} vs {shared}")
    check(mem["used"] + mem["available"] == mem["total"], "used + available == total")
    check(mem["swapTotal"] == swap[0], "swap total equals free's", f"{mem['swapTotal']} != {swap[0]}")
    check(close(mem["swapUsed"], swap[1], 64 << 20), "swap used ~ free's", f"{mem['swapUsed']} vs {swap[1]}")


# ---- cpu ---------------------------------------------------------------------------
def read_stat():
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu"):
                continue
            name, *fields = line.split()
            values = [int(x) for x in fields[:8]]
            out[name] = (sum(values), values[3] + values[4])
    return out


def check_cpu():
    print("cpu  (/proc/stat, nproc, /proc/loadavg)")
    # The dump reads /proc/stat twice, 500 ms apart; bracket it with our own readings.
    before = read_stat()
    proc = subprocess.Popen([EXE, "--dump", "cpu"], stdout=subprocess.PIPE, text=True)
    out, _ = proc.communicate(timeout=120)
    after = read_stat()
    cpu = json.loads(out)
    total = after["cpu"][0] - before["cpu"][0]
    idle = after["cpu"][1] - before["cpu"][1]
    mine = 100.0 * (1 - idle / total) if total else 0.0
    check(cpu["usageValid"] is True, "usage is based on two readings")
    check(close(cpu["usagePercent"], mine, 25), "total utilisation ~ /proc/stat over the same period",
          f"{cpu['usagePercent']} vs {mine:.1f}")
    check(len(cpu["cores"]) == len([k for k in before if k != "cpu"]), "one entry per online CPU",
          f"{len(cpu['cores'])}")
    check(all(0 <= c["usagePercent"] <= 100 for c in cpu["cores"]), "per-CPU utilisation within 0..100")
    mean = sum(c["usagePercent"] for c in cpu["cores"]) / max(1, len(cpu["cores"]))
    check(close(mean, cpu["usagePercent"], 3), "mean of the CPUs ~ total", f"{mean:.1f} vs {cpu['usagePercent']}")
    check(cpu["logicalCpus"] == int(run("nproc", "--all")), "logical CPUs equal nproc --all")
    load = [float(x) for x in open("/proc/loadavg").read().split()[:3]]
    check(all(close(a, b, 1.5) for a, b in zip(cpu["loadAverage"], load)), "load average ~ /proc/loadavg",
          f"{cpu['loadAverage']} vs {load}")
    uptime = float(open("/proc/uptime").read().split()[0])
    check(close(cpu["uptimeSeconds"], uptime, 5), "uptime ~ /proc/uptime")
    model = [l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")]
    if model:
        check(cpu["model"] == " ".join(model[0].split()), "model equals /proc/cpuinfo", cpu["model"])
    check(cpu["temperatureCelsius"] is None or 0 < cpu["temperatureCelsius"] < 150, "temperature is null or plausible")


# ---- disks -------------------------------------------------------------------------
def check_disks():
    print("disks  (df -B1, lsblk, /proc/diskstats)")
    disks = dump("disks")
    check(len(disks["filesystems"]) > 0, "at least one file system listed")
    slack = 512 << 20                           # builds are writing to the same disk
    for fs in disks["filesystems"]:
        row = run("df", "-B1", "--output=source,fstype,size,used,avail,pcent", fs["mountPoint"]).splitlines()[1].split()
        source, fstype, size, used, avail, pcent = row[0], row[1], int(row[2]), int(row[3]), int(row[4]), int(row[5].rstrip("%"))
        where = fs["mountPoint"]
        check(fs["total"] == size, f"{where}: size equals df", f"{fs['total']} != {size}")
        check(close(fs["used"], used, slack), f"{where}: used ~ df", f"{fs['used']} vs {used}")
        check(close(fs["available"], avail, slack), f"{where}: available ~ df", f"{fs['available']} vs {avail}")
        check(close(fs["usedPercent"], pcent, 2), f"{where}: use% ~ df", f"{fs['usedPercent']} vs {pcent}")
        check(fs["type"] == fstype and fs["device"] == source, f"{where}: device and type equal df",
              f"{fs['device']} {fs['type']} vs {source} {fstype}")
    root = disks["root"]
    row = run("df", "-B1", "--output=size,used,avail", "/").splitlines()[1].split()
    check(root is not None and root["total"] == int(row[0]), "root: size equals df /")
    check(root is not None and close(root["used"], int(row[1]), slack), "root: used ~ df /")

    sizes = {}
    for line in run("lsblk", "-b", "-d", "-n", "-o", "NAME,SIZE").splitlines():
        name, size = line.split()
        sizes[name] = int(size)
    stats = {}
    for line in open("/proc/diskstats"):
        f = line.split()
        stats[f[2]] = (int(f[5]) * 512, int(f[9]) * 512)
    check(disks["ratesValid"] is True, "rates are based on two readings")
    for d in disks["devices"]:
        name = d["name"]
        check(name in sizes and d["size"] == sizes[name], f"{name}: size equals lsblk", f"{d['size']}")
        check(os.path.exists(f"/sys/block/{name}/device"), f"{name}: is a hardware-backed whole device")
        check(name in stats and close(d["readBytes"], stats[name][0], 64 << 20)
              and close(d["writeBytes"], stats[name][1], 512 << 20), f"{name}: totals ~ /proc/diskstats")
        check(d["readBytesPerSecond"] >= 0 and d["writeBytesPerSecond"] >= 0 and 0 <= d["busyPercent"] <= 100,
              f"{name}: rates are sane")
    listed = {d["name"] for d in disks["devices"]}
    expected = {n for n, size in sizes.items() if size > 0 and os.path.exists(f"/sys/block/{n}/device")}
    check(listed == expected, "exactly the hardware-backed devices are listed", f"{listed ^ expected}")


# ---- network -----------------------------------------------------------------------
def check_network():
    print("network  (/proc/net/dev, ip -j addr)")
    net = dump("network")
    counters = {}
    for line in open("/proc/net/dev").read().splitlines()[2:]:
        name, rest = line.split(":", 1)
        f = rest.split()
        counters[name.strip()] = (int(f[0]), int(f[8]))
    names = {i["name"] for i in net["interfaces"]}
    check(names == set(counters), "same interfaces as /proc/net/dev", f"{names ^ set(counters)}")
    check(net["ratesValid"] is True, "rates are based on two readings")
    addresses = {}
    try:
        for link in json.loads(run("ip", "-j", "addr")):
            addresses[link["ifname"]] = (
                {f"{a['local']}/{a['prefixlen']}" for a in link.get("addr_info", [])},
                link.get("address", ""), link.get("mtu"), link.get("operstate", "").lower())
    except (ValueError, OSError):
        print("  (ip -j addr not usable; addresses not compared)")
    for i in net["interfaces"]:
        name = i["name"]
        if name not in counters:
            continue
        rx, tx = counters[name]
        check(close(i["rxBytes"], rx, 8 << 20) and close(i["txBytes"], tx, 8 << 20), f"{name}: byte counters ~ /proc/net/dev")
        check(i["rxBytesPerSecond"] >= 0 and i["txBytesPerSecond"] >= 0, f"{name}: rates are sane")
        if name in addresses:
            addrs, mac, mtu, oper = addresses[name]
            check(set(i["addresses"]) == addrs, f"{name}: addresses equal ip addr", f"{i['addresses']} vs {sorted(addrs)}")
            check(i["mtu"] == mtu, f"{name}: mtu equals ip addr")
            if i["kind"] != "loopback":
                check(i["mac"] == mac, f"{name}: MAC equals ip addr", f"{i['mac']} vs {mac}")
            if oper in ("up", "down"):
                check(i["up"] == (oper == "up"), f"{name}: link state equals ip addr", f"{i['state']} vs {oper}")
    lo = [i for i in net["interfaces"] if i["kind"] == "loopback"]
    check(len(lo) == 1 and lo[0]["name"] == "lo", "the loopback interface is recognised")


# ---- processes ---------------------------------------------------------------------
def same_command(ps_args, command):
    """ps prints '?' for control characters (tabs, escapes) where the monitor
    prints a space, and the monitor cuts a command line after 4095 bytes."""
    expected = ps_args.replace("?", " ").rstrip()
    got = command.replace("?", " ").rstrip()
    if got.endswith("…"):
        return expected.startswith(got[:-1].rstrip())
    return expected == got


def check_processes():
    print("processes  (ps -e)")
    fmt = "pid=,ppid=,euser:32=,rss=,nlwp=,ni=,state=,cputimes=,comm="     # :32 — ps cuts longer user names otherwise
    t0 = time.time()
    ps_rows = run("ps", "-e", "-ww", "-o", fmt).splitlines()
    ps_args = run("ps", "-e", "-ww", "-o", "pid=,args=").splitlines()
    data = dump("processes")
    elapsed = time.time() - t0
    procs = {p["pid"]: p for p in data["processes"]}
    check(data["count"] == len(procs), "count equals the number of entries")

    ps = {}
    for line in ps_rows:
        f = line.split(None, 8)
        if len(f) < 9:
            continue
        ps[int(f[0])] = dict(ppid=int(f[1]), user=f[2], rss=int(f[3]) * 1024, threads=int(f[4]),
                             nice=f[5], state=f[6], cputime=int(f[7]), comm=f[8])
    args = {}
    for line in ps_args:
        f = line.strip().split(None, 1)
        args[int(f[0])] = f[1] if len(f) > 1 else ""

    common = sorted(set(ps) & set(procs))
    only_ps = set(ps) - set(procs)
    only_dump = set(procs) - set(ps)
    # ps itself, this script's children and short-lived build processes differ.
    check(len(common) >= 0.9 * max(len(ps), len(procs)), "at least 90 % of the PIDs are in both lists",
          f"common {len(common)}, only ps {len(only_ps)}, only dump {len(only_dump)}")

    bad = {k: [] for k in ("ppid", "user", "name", "command", "threads", "nice", "rss", "cputime", "kthread")}
    stable = 0
    for pid in common:
        a, b = ps[pid], procs[pid]
        # A PID that was reused between the two listings is a different process.
        if a["comm"] != b["name"][:15] and not b["name"].startswith(a["comm"]):
            if a["ppid"] != b["ppid"]:
                continue
            bad["name"].append((pid, a["comm"], b["name"]))
            continue
        stable += 1
        if a["ppid"] != b["ppid"]:
            bad["ppid"].append((pid, a["ppid"], b["ppid"]))
        if a["user"] != b["user"] and a["user"] != str(b["uid"]):
            bad["user"].append((pid, a["user"], b["user"]))
        if a["nice"] not in ("-", str(b["nice"])):
            bad["nice"].append((pid, a["nice"], b["nice"]))
        if a["threads"] != b["threads"] and abs(a["threads"] - b["threads"]) > 4:
            bad["threads"].append((pid, a["threads"], b["threads"]))
        if not close(a["rss"], b["memory"], 48 << 20, 0.35):
            bad["rss"].append((pid, a["rss"], b["memory"]))
        if not close(a["cputime"], b["cpuTime"], 2 + elapsed * 4, 0.05):
            bad["cputime"].append((pid, a["cputime"], b["cpuTime"]))
        ps_kthread = args.get(pid, "").startswith("[") and args.get(pid, "").endswith("]") and a["rss"] == 0
        if b["kernelThread"] != ps_kthread and a["state"] != "Z":
            bad["kthread"].append((pid, args.get(pid), b["kernelThread"]))
        # ps prints "[comm] <defunct>" for zombies; processes may also re-title themselves.
        if a["state"] != "Z" and b["stateCode"] != "Z" and not same_command(args.get(pid, ""), b["command"]):
            bad["command"].append((pid, args.get(pid, "")[:60], b["command"][:60]))
    check(stable >= 0.9 * len(common), "names agree with ps comm (allowing the 15-character cut)", f"{bad['name'][:5]}")
    for key, label in (("ppid", "parent PIDs equal ps"), ("user", "owners equal ps euser"),
                       ("nice", "nice values equal ps"), ("threads", "thread counts ~ ps nlwp"),
                       ("kthread", "kernel threads are the ones ps prints in brackets")):
        check(not bad[key], label, f"{bad[key][:5]}")
    check(len(bad["command"]) <= max(2, len(common) // 50), "command lines equal ps args", f"{bad['command'][:5]}")
    check(len(bad["rss"]) <= max(2, len(common) // 20), "RSS ~ ps rss", f"{bad['rss'][:5]}")
    check(len(bad["cputime"]) <= max(2, len(common) // 20), "CPU time ~ ps cputimes", f"{bad['cputime'][:5]}")

    check(all(p["cpu"] >= 0 for p in procs.values()), "no negative CPU share")
    ncpu = os.cpu_count() or 1
    check(sum(p["cpu"] for p in procs.values()) <= ncpu * 100 * 1.25, "CPU shares add up to at most the CPUs there are",
          f"{sum(p['cpu'] for p in procs.values()):.0f} % on {ncpu} CPUs")
    check(1 in procs and procs[1]["ppid"] == 0, "PID 1 is listed with parent 0")
    me = procs.get(os.getpid())
    check(me is not None and me["name"].startswith("python") and me["user"] == run("id", "-un").strip(),
          "this script is listed with its name and owner", f"{me}")
    # A busy loop must show up with the CPU share it really gets (on a loaded
    # machine that is less than a whole CPU): compare with its own CPU time
    # over the period of the dump.
    def cpu_seconds(pid):
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")

    spinner = subprocess.Popen([sys.executable, "-c", "while True: pass"])
    try:
        time.sleep(0.3)
        c0, w0 = cpu_seconds(spinner.pid), time.time()
        busy = {p["pid"]: p for p in dump("processes")["processes"]}.get(spinner.pid)
        c1, w1 = cpu_seconds(spinner.pid), time.time()
        share = 100.0 * (c1 - c0) / (w1 - w0)
        check(busy is not None and busy["cpu"] > 5 and close(busy["cpu"], share, 25),
              "a busy loop shows the CPU share it gets", f"{busy and busy['cpu']} vs {share:.1f} measured")
        check(busy is not None and busy["stateCode"] == "R", "a busy loop is reported as running")
    finally:
        spinner.kill()
        spinner.wait()


# ---- topics without hardware here ----------------------------------------------------
def check_shapes():
    print("gpu, sensors, battery, services  (shape only)")
    gpu = dump("gpu")
    check(isinstance(gpu["gpus"], list), "gpu: list of devices")
    for g in gpu["gpus"]:
        check(g["utilizationAvailable"] == (g["utilizationPercent"] is not None), f"gpu {g['card']}: null when not reported")
    sensors = dump("sensors")
    check(isinstance(sensors["temperatures"], list) and isinstance(sensors["fans"], list), "sensors: lists")
    check((sensors["cpuTemperatureCelsius"] is None) == (sensors["cpuTemperatureSource"] == ""), "sensors: CPU temperature and its source agree")
    hwmon = os.path.isdir("/sys/class/hwmon") and os.listdir("/sys/class/hwmon")
    zones = [z for z in (os.listdir("/sys/class/thermal") if os.path.isdir("/sys/class/thermal") else []) if z.startswith("thermal_zone")]
    if not hwmon and not zones:
        check(sensors["temperatures"] == [] and sensors["fans"] == [], "sensors: nothing listed without hwmon devices or thermal zones")
    battery = dump("battery")
    supplies = os.listdir("/sys/class/power_supply") if os.path.isdir("/sys/class/power_supply") else []
    if not supplies:
        check(battery["batteries"] == [] and battery["externalPowerKnown"] is False, "battery: nothing listed without power supplies")
    services = dump("services")
    booted = os.path.isdir("/run/systemd/system")
    check(services["available"] == (booted and os.path.exists("/run/dbus/system_bus_socket")),
          "services: available only under a running systemd with a system bus")
    if not booted:
        check(services["services"] == [] and "systemd" in services["unavailableReason"], "services: unavailable state says why")
    else:
        units = json.loads(run("systemctl", "list-units", "--type=service", "--all", "--no-pager", "--output=json"))
        loaded = {u["unit"]: u for u in units if u["load"] != "not-found"}
        listed = {s["unit"]: s for s in services["services"]}
        check(set(loaded) <= set(listed), "services: every loaded unit is listed", f"{sorted(set(loaded) - set(listed))[:5]}")
        same = sum(1 for u, v in loaded.items() if u in listed and listed[u]["active"] == v["active"])
        check(same >= 0.95 * len(loaded), "services: active states equal systemctl list-units")


def main():
    print(f"binary: {EXE}")
    for fn in (check_memory, check_cpu, check_disks, check_network, check_processes, check_shapes):
        fn()
    print(f"\n{CHECKS - len(FAILED)} of {CHECKS} checks passed")
    if FAILED:
        print("failed: " + "; ".join(FAILED))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
