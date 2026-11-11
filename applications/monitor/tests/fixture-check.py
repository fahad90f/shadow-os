#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check "nexus-monitor --dump" against the synthetic machine of fixture.py.

    sudo scripts/dev.sh run --dir monitor python3 /src/applications/monitor/tests/fixture.py \
         python3 /src/applications/monitor/tests/fixture-check.py

Every expected value below follows from the files fixture.py writes; see
there for what the synthetic machine consists of. Exits 1 on any mismatch.
"""
import json
import os
import shutil
import subprocess
import sys

FAILED = []
CHECKS = 0


def binary():
    for path in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        candidate = os.path.join(os.path.dirname(path.rstrip("/")), "bin", "nexus-monitor")
        if os.access(candidate, os.X_OK):
            return candidate
    return shutil.which("nexus-monitor") or sys.exit("fixture-check: nexus-monitor not found")


EXE = binary()


def dump(topic):
    out = subprocess.run([EXE, "--dump", topic], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        sys.exit(f"fixture-check: --dump {topic} exited {out.returncode}: {out.stderr.strip()}")
    return json.loads(out.stdout)


def check(what, got, expected):
    global CHECKS
    CHECKS += 1
    ok = abs(got - expected) < 0.051 if isinstance(expected, float) and isinstance(got, (int, float)) else got == expected
    if ok:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}: got {got!r}, expected {expected!r}")
        FAILED.append(what)


def find(items, **keys):
    matches = [i for i in items if all(i.get(k) == v for k, v in keys.items())]
    return matches[0] if len(matches) == 1 else None


def sensors():
    print("sensors")
    s = dump("sensors")
    temps = s["temperatures"]
    package = find(temps, chip="coretemp", label="Package id 0")
    check("coretemp package: 54.0 °C, high 80, critical 100, normal",
          package and (package["celsius"], package["high"], package["critical"], package["level"], package["device"]),
          (54, 80, 100, "normal", "coretemp.0"))
    core0 = find(temps, chip="coretemp", label="Core 0")
    check("a limit of 0 is treated as unset", core0 and (core0["celsius"], core0["high"], core0["critical"]), (52, None, 100))
    core1 = find(temps, chip="coretemp", label="Core 1")
    check("a reading above its max is graded high", core1 and (core1["celsius"], core1["level"]), (97, "high"))
    check("a sensor that cannot be read is not listed", find(temps, label="Core 2"), None)
    nvme = find(temps, chip="nvme")
    check("nvme composite: 38.9 °C (38850 m°C), on nvme0", nvme and (nvme["label"], nvme["celsius"], nvme["device"]),
          ("Composite", 38.9, "nvme0"))
    gpu = find(temps, chip="amdgpu")
    check("amdgpu edge: 61 °C; the suspended GPU's sensor is not read", gpu and (gpu["label"], gpu["celsius"]), ("edge", 61))
    acpitz = [t for t in temps if t["label"] == "acpitz"]
    check("the thermal zone is listed once (its hwmon mirror is skipped)", len(acpitz), 1)
    check("acpitz: 47 °C, critical trip point 103 °C",
          acpitz and (acpitz[0]["chip"], acpitz[0]["celsius"], acpitz[0]["critical"], acpitz[0]["device"]),
          ("thermal zone", 47, 103, "thermal_zone0"))
    pkg = find(temps, label="x86_pkg_temp")
    check("x86_pkg_temp: 55 °C, a passive trip point is not a critical limit", pkg and (pkg["celsius"], pkg["critical"]), (55, None))
    check("number of temperature readings", len(temps), 7)
    check("fans: thinkpad fan1 2450 RPM, fan2 stopped, amdgpu 1200 RPM",
          sorted((f["chip"], f["label"], f["rpm"]) for f in s["fans"]),
          [("amdgpu", "fan1", 1200), ("thinkpad", "fan1", 2450), ("thinkpad", "fan2", 0)])
    check("processor temperature is the coretemp package sensor",
          (s["cpuTemperatureCelsius"], s["cpuTemperatureSource"]), (54, "coretemp · Package id 0"))
    cpu = dump("cpu")
    check("cpu topic carries the same temperature", (cpu["temperatureCelsius"], cpu["temperatureSource"]),
          (54, "coretemp · Package id 0"))


def memory_and_frequency():
    print("memory, cpufreq")
    kib = 1024
    m = dump("memory")
    check("total and available as in meminfo", (m["total"], m["available"]), (16303428 * kib, 9000000 * kib))
    check("used = MemTotal - MemAvailable", m["used"], (16303428 - 9000000) * kib)
    check("cached = Cached + SReclaimable; buffers, shared, free",
          (m["cached"], m["buffers"], m["shared"], m["free"]), (6500000 * kib, 300000 * kib, 250000 * kib, 1200000 * kib))
    check("used percent 44.8", m["usedPercent"], 44.8)
    check("swap: 2 GiB of 8 GiB used, 25 %", (m["swapTotal"], m["swapUsed"], m["swapFree"], m["swapUsedPercent"]),
          (8388604 * kib, 2097152 * kib, 6291452 * kib, 25))
    cpu = dump("cpu")
    expected = [1200.0 + 800.0 * n for n in range(len(cpu["cores"]))]
    check("per-CPU frequency from cpufreq (kHz → MHz)", [c.get("frequencyMhz") for c in cpu["cores"]], expected)
    check("mean frequency and its source", (cpu["frequencyMhz"], cpu["frequencySource"]),
          (round(sum(expected) / len(expected), 1), "cpufreq"))


def battery():
    print("battery")
    b = dump("battery")
    check("external power is known and offline", (b["externalPowerKnown"], b["externalPowerOnline"]), (True, False))
    names = [x["name"] for x in b["batteries"]]
    check("the empty bay is skipped, the peripheral comes last", names, ["BAT0", "BAT1", "hidpp_battery_0"])
    bat0 = find(b["batteries"], name="BAT0")
    check("BAT0 identity", bat0 and (bat0["manufacturer"], bat0["model"], bat0["technology"], bat0["peripheral"], bat0["status"]),
          ("SMP", "01AV431", "Li-ion", False, "Discharging"))
    check("BAT0 percentage from 'capacity'", bat0 and bat0["percentage"], 73)
    check("BAT0 power draw 11.2 W", bat0 and bat0["rateWatts"], 11.2)
    check("BAT0 energy 37.23 of 51 Wh, 57 Wh new", bat0 and (bat0["energyNowWh"], bat0["energyFullWh"], bat0["energyDesignWh"]),
          (37.23, 51, 57))
    check("BAT0 capacity vs. design 89.5 %", bat0 and bat0["capacityHealthPercent"], 89.5)
    check("BAT0 cycles and voltage", bat0 and (bat0["cycles"], bat0["voltage"]), (312, 11.85))
    check("BAT0 time to empty estimated from energy / power", bat0 and (bat0["secondsRemaining"], bat0["timeEstimated"]),
          (int(37.23 / 11.2 * 3600), True))
    bat1 = find(b["batteries"], name="BAT1")
    check("BAT1 (charge units) percentage 65 %", bat1 and bat1["percentage"], 65.0)
    check("BAT1 energy via design voltage: 28.86 of 44.4 Wh, 48.84 Wh new",
          bat1 and (bat1["energyNowWh"], bat1["energyFullWh"], bat1["energyDesignWh"]), (28.86, 44.4, 48.84))
    check("BAT1 rate from |current| x voltage: 18 W", bat1 and bat1["rateWatts"], 18.0)
    check("BAT1 is charging", bat1 and bat1["status"], "Charging")
    check("BAT1 time to full from the missing energy / power (3108 s)",
          bat1 is not None and abs(bat1["secondsRemaining"] - 3108) <= 1 and bat1["timeEstimated"], True)
    check("BAT1 temperature 31.2 °C, capacity vs. design 90.9 %, no cycle count",
          bat1 and (bat1["temperatureCelsius"], bat1["capacityHealthPercent"], bat1["cycles"]), (31.2, 90.9, None))
    mouse = find(b["batteries"], name="hidpp_battery_0")
    check("the mouse battery reports only a level", mouse and (mouse["peripheral"], mouse["percentage"], mouse["capacityLevel"],
                                                               mouse["model"], mouse["rateWatts"], mouse["secondsRemaining"]),
          (True, None, "High", "MX Master 3", None, None))


def gpu():
    print("gpu")
    gpus = dump("gpu")["gpus"]
    check("four graphics devices, connectors are not devices", [g["card"] for g in gpus], ["card0", "card1", "card2", "card3"])
    amd = find(gpus, card="card0")
    check("card0 identity from the PCI ID database", amd and (amd["name"], amd["driver"], amd["pciSlot"]),
          ("AMD Radeon RX 6800/6800 XT / 6900 XT", "amdgpu", "0000:03:00.0"))
    check("card0 utilisation 37 % from gpu_busy_percent", amd and (amd["utilizationAvailable"], amd["utilizationPercent"], amd["source"]),
          (True, 37, "sysfs"))
    check("card0 temperature, video memory, power, fan",
          amd and (amd["temperatureCelsius"], amd["memoryUsed"], amd["memoryTotal"], amd["powerWatts"], amd["fanRpm"]),
          (61, 3221225472, 17163091968, 45, 1200))
    intel = find(gpus, card="card1")
    check("card1 (i915): nothing reported, and it says so",
          intel and (intel["name"], intel["utilizationAvailable"], intel["utilizationPercent"], intel["temperatureCelsius"], intel["note"]),
          ("Intel Iris Xe Graphics", False, None, None, "Utilisation is not reported by the i915 driver"))
    nvidia = find(gpus, card="card2")
    check("card2 (nvidia): figures from nvidia-smi",
          nvidia and (nvidia["name"], nvidia["source"], nvidia["utilizationPercent"], nvidia["temperatureCelsius"],
                      nvidia["memoryUsed"], nvidia["memoryTotal"], nvidia["powerWatts"]),
          ("NVIDIA GeForce RTX 3080", "nvidia-smi", 64, 71, 3072 * 1048576, 10240 * 1048576, 215.3))
    asleep = find(gpus, card="card3")
    check("card3 (runtime-suspended): not polled",
          asleep and (asleep["suspended"], asleep["utilizationAvailable"], asleep["utilizationPercent"], asleep["temperatureCelsius"]),
          (True, False, None, None))
    check("card3 says why", asleep and asleep["note"], "Powered down to save energy (runtime suspended)")


def services():
    print("services")
    s = dump("services")
    check("systemd is recognised as running", (s["available"], s["error"]), (True, ""))
    by = {x["unit"]: x for x in s["services"]}
    check("listed units (no template, no not-found name)", sorted(by),
          sorted(["ssh.service", "cron.service", "NetworkManager.service", "systemd-journald.service", "apparmor.service",
                  "backup.service", "denied.service", "bluetooth.service", "cups.service", "rsync.service",
                  "getty@tty1.service", "run-u42.service", "unstable.service"]))
    ssh = by.get("ssh.service", {})
    check("ssh: running, enabled", (ssh.get("name"), ssh.get("description"), ssh.get("status"), ssh.get("fileState")),
          ("ssh", "OpenBSD Secure Shell server", "running", "enabled"))
    check("apparmor: active (exited)", (by["apparmor.service"]["active"], by["apparmor.service"]["status"]), ("active", "exited"))
    check("backup: failed", by["backup.service"]["status"], "failed")
    check("journald: static", by["systemd-journald.service"]["fileState"], "static")
    check("cups: installed but not loaded → inactive, disabled",
          (by["cups.service"]["load"], by["cups.service"]["status"], by["cups.service"]["fileState"]), ("", "inactive", "disabled"))
    check("rsync: masked", by["rsync.service"]["fileState"], "masked")
    check("template instance: enablement from GetUnitFileState", by["getty@tty1.service"]["fileState"], "enabled")
    check("run-time unit: transient", by["run-u42.service"]["fileState"], "transient")


def main():
    if not os.path.isdir("/tmp/nexus-monitor-fixture"):
        sys.exit("fixture-check: run through fixture.py")
    print(f"binary: {EXE}")
    for fn in (sensors, memory_and_frequency, battery, gpu, services):
        fn()
    print(f"\n{CHECKS - len(FAILED)} of {CHECKS} checks passed")
    if FAILED:
        print("failed: " + "; ".join(FAILED))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
