#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a command in front of a synthetic machine with sensors, batteries,
graphics devices and a stand-in for systemd's D-Bus manager.

The build chroot runs in a VM without hwmon devices, thermal zones, batteries,
DRM devices or a running systemd, so the monitor can only show its
"unavailable" states there. This helper replaces — inside the private,
throw-away mount namespace of one "scripts/dev.sh run" — the relevant parts
of /sys and /proc with a hand-written tree that follows the kernel's sysfs
ABI, starts a private D-Bus "system" bus (DBUS_SYSTEM_BUS_ADDRESS) with
mock-systemd.py as org.freedesktop.systemd1 on it, puts a stub "nvidia-smi"
first in PATH, and then runs the given command. The monitor itself is unmodified and has no test
hooks: it reads /sys/class/hwmon, /sys/class/thermal, /sys/class/power_supply,
/sys/class/drm, /sys/devices/system/cpu, /proc/meminfo, /proc/driver/nvidia
and talks to org.freedesktop.systemd1 on the system bus exactly as on a real
machine.

    sudo scripts/dev.sh run --dir monitor python3 /src/applications/monitor/tests/fixture.py \
         python3 /src/applications/monitor/tests/fixture-check.py
    sudo scripts/dev.sh run --dir monitor --smoke --shot monitor-fixture-sensors.png --size 1240x1500 \
         python3 /src/applications/monitor/tests/fixture.py nexus-monitor --page sensors

What it proves: the parsing, unit conversion, de-duplication and presentation
of these sources. What it cannot prove: that real drivers and a real systemd
behave like the stubs.

The synthetic machine:
  hwmon     coretemp (package + 2 cores, one above its "max"), nvme, thinkpad
            (two fans, one stopped), amdgpu; one sensor whose read fails; one
            hwmon device that only mirrors a thermal zone (must not be listed)
  thermal   acpitz (critical trip point), x86_pkg_temp
  power     BAT0 (energy units, discharging), BAT1 (charge units, charging,
            negative current), an empty battery bay, a mouse battery that only
            reports a level, AC adapter offline
  drm       card0 amdgpu (busy %, VRAM, temperature, power, fan), card1 i915
            (nothing reported), card2 nvidia (figures from the nvidia-smi
            stub), card3 amdgpu runtime-suspended (must not be polled)
  cpufreq   scaling_cur_freq for every online CPU (1200 MHz, 2000 MHz, ...)
  memory    /proc/meminfo of a 16 GiB machine with 2 GiB of 8 GiB swap in use
  systemd   mock-systemd.py keeping unit states in a JSON file:
            ListUnitsByPatterns, ListUnitFilesByPatterns, GetUnitFileState,
            Start/Stop/RestartUnit with JobRemoved, Enable/DisableUnitFiles,
            Reload; "denied.service" refuses every change like a failed
            authorisation; the start job of "unstable.service" fails; the
            unit listing fails while /tmp/nexus-monitor-fixture/fail-listing exists
"""
import json
import os
import shutil
import subprocess
import sys
import time

FX = "/tmp/nexus-monitor-fixture"
DEV = f"{FX}/sys/devices"


def write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(f"{value}\n")


def attrs(directory, **values):
    for name, value in values.items():
        write(f"{directory}/{name}", value)


def link(target, name):
    os.makedirs(os.path.dirname(name), exist_ok=True)
    os.symlink(target, name)


def mount(*args):
    subprocess.run(["mount", *args], check=True)


def build_sysfs():
    # Keep the real sysfs reachable, then put a writable /sys/class in front of
    # it: real classes as links, the fixture's classes as directories.
    os.makedirs(f"{FX}/orig-sys")
    mount("--rbind", "/sys", f"{FX}/orig-sys")
    mount("-t", "tmpfs", "tmpfs", "/sys/class")
    replaced = {"hwmon", "thermal", "power_supply", "drm"}
    for entry in os.listdir(f"{FX}/orig-sys/class"):
        if entry not in replaced:
            os.symlink(f"{FX}/orig-sys/class/{entry}", f"/sys/class/{entry}")
    for entry in replaced:
        os.mkdir(f"/sys/class/{entry}")

    # ---- hwmon ---------------------------------------------------------------
    coretemp = f"{DEV}/platform/coretemp.0/hwmon/hwmon0"
    attrs(coretemp, name="coretemp",
          temp1_input=54000, temp1_label="Package id 0", temp1_max=80000, temp1_crit=100000,
          temp2_input=52000, temp2_label="Core 0", temp2_max=0, temp2_crit=100000,     # max left unset by the driver
          temp3_input=97000, temp3_label="Core 1", temp3_max=80000, temp3_crit=100000)
    link("/nonexistent/sensor", f"{coretemp}/temp4_input")                            # a sensor that cannot be read
    write(f"{coretemp}/temp4_label", "Core 2")
    link(f"{DEV}/platform/coretemp.0", f"{coretemp}/device")
    link(coretemp, "/sys/class/hwmon/hwmon0")

    nvme = f"{DEV}/pci0000:00/0000:00:1d.0/0000:04:00.0/nvme/nvme0/hwmon1"
    attrs(nvme, name="nvme", temp1_input=38850, temp1_label="Composite", temp1_max=81850, temp1_crit=84850)
    link(f"{DEV}/pci0000:00/0000:00:1d.0/0000:04:00.0/nvme/nvme0", f"{nvme}/device")
    link(nvme, "/sys/class/hwmon/hwmon1")

    thinkpad = f"{DEV}/platform/thinkpad_hwmon/hwmon/hwmon2"
    attrs(thinkpad, name="thinkpad", fan1_input=2450, fan2_input=0)
    link(f"{DEV}/platform/thinkpad_hwmon", f"{thinkpad}/device")
    link(thinkpad, "/sys/class/hwmon/hwmon2")

    # ---- thermal zones (and the hwmon device the kernel derives from one) ------
    zone0 = f"{DEV}/virtual/thermal/thermal_zone0"
    attrs(zone0, type="acpitz", temp=47000, trip_point_0_type="critical", trip_point_0_temp=103000)
    attrs(f"{zone0}/hwmon4", name="acpitz", temp1_input=47000, temp1_crit=103000)
    link(f"{zone0}/hwmon4", "/sys/class/hwmon/hwmon4")
    link(zone0, "/sys/class/thermal/thermal_zone0")
    zone1 = f"{DEV}/virtual/thermal/thermal_zone1"
    attrs(zone1, type="x86_pkg_temp", temp=55000, trip_point_0_type="passive", trip_point_0_temp=95000)
    link(zone1, "/sys/class/thermal/thermal_zone1")
    os.makedirs(f"{DEV}/virtual/thermal/cooling_device0")
    link(f"{DEV}/virtual/thermal/cooling_device0", "/sys/class/thermal/cooling_device0")

    # ---- power supplies ------------------------------------------------------
    supplies = f"{DEV}/LNXSYSTM:00/power_supply"
    attrs(f"{supplies}/AC", type="Mains", online=0)
    attrs(f"{supplies}/BAT0", type="Battery", present=1, status="Discharging", capacity=73,
          capacity_level="Normal", technology="Li-ion", manufacturer="SMP", model_name="01AV431",
          cycle_count=312, voltage_min_design=11400000, voltage_now=11850000, power_now=11200000,
          energy_full_design=57000000, energy_full=51000000, energy_now=37230000)
    attrs(f"{supplies}/BAT1", type="Battery", present=1, status="Charging",
          technology="Li-poly", manufacturer="LGC", model_name="L18L3P71",
          voltage_min_design=11100000, voltage_now=12000000, current_now=-1500000,
          charge_full_design=4400000, charge_full=4000000, charge_now=2600000, temp=312)
    attrs(f"{supplies}/BAT2", type="Battery", present=0, status="Unknown")
    attrs(f"{supplies}/hidpp_battery_0", type="Battery", scope="Device", status="Discharging", online=1,
          capacity_level="High", manufacturer="Logitech", model_name="MX Master 3")
    for name in ("AC", "BAT0", "BAT1", "BAT2", "hidpp_battery_0"):
        link(f"{supplies}/{name}", f"/sys/class/power_supply/{name}")

    # ---- graphics ------------------------------------------------------------
    def card(index, slot, vendor, device, driver, runtime="active", **extra):
        pci = f"{DEV}/pci0000:00/{slot}"
        attrs(pci, vendor=vendor, device=device, **extra)
        write(f"{pci}/power/runtime_status", runtime)
        os.makedirs(f"{FX}/sys/bus/pci/drivers/{driver}", exist_ok=True)
        link(f"{FX}/sys/bus/pci/drivers/{driver}", f"{pci}/driver")
        os.makedirs(f"{pci}/drm/card{index}")
        link(pci, f"{pci}/drm/card{index}/device")
        link(f"{pci}/drm/card{index}", f"/sys/class/drm/card{index}")
        os.makedirs(f"{pci}/drm/card{index}/card{index}-DP-1")
        link(f"{pci}/drm/card{index}/card{index}-DP-1", f"/sys/class/drm/card{index}-DP-1")   # a connector, not a device
        return pci

    amd = card(0, "0000:03:00.0", "0x1002", "0x73bf", "amdgpu", gpu_busy_percent=37,
               mem_info_vram_used=3221225472, mem_info_vram_total=17163091968)
    attrs(f"{amd}/hwmon/hwmon3", name="amdgpu", temp1_input=61000, temp1_label="edge", temp1_crit=100000,
          power1_average=45000000, fan1_input=1200)
    link(amd, f"{amd}/hwmon/hwmon3/device")
    link(f"{amd}/hwmon/hwmon3", "/sys/class/hwmon/hwmon3")
    card(1, "0000:00:02.0", "0x8086", "0x9a49", "i915")
    card(2, "0000:01:00.0", "0x10de", "0x2206", "nvidia")
    sleeping = card(3, "0000:05:00.0", "0x1002", "0x7480", "amdgpu", runtime="suspended", gpu_busy_percent=99)
    attrs(f"{sleeping}/hwmon/hwmon5", name="amdgpu", temp1_input=99000, temp1_label="edge")
    link(sleeping, f"{sleeping}/hwmon/hwmon5/device")
    link(f"{sleeping}/hwmon/hwmon5", "/sys/class/hwmon/hwmon5")

    # The proprietary NVIDIA driver lists its devices in procfs.
    if os.path.isdir("/proc/driver"):
        mount("-t", "tmpfs", "tmpfs", "/proc/driver")
        os.makedirs("/proc/driver/nvidia/gpus/0000:01:00.0")

    # ---- cpufreq: one frequency per online CPU, 800 MHz apart from 1200 MHz ----
    cpus = sorted(int(d[3:]) for d in os.listdir("/sys/devices/system/cpu") if d[:3] == "cpu" and d[3:].isdigit())
    for n, cpu in enumerate(cpus):
        mount("-t", "tmpfs", "tmpfs", f"/sys/devices/system/cpu/cpu{cpu}")
        write(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq", 1200000 + 800000 * n)

    # ---- memory with swap in use -------------------------------------------------
    with open(f"{FX}/meminfo", "w") as f:
        f.write(MEMINFO)
    mount("--bind", f"{FX}/meminfo", "/proc/meminfo")


MEMINFO = """MemTotal:       16303428 kB
MemFree:         1200000 kB
MemAvailable:    9000000 kB
Buffers:          300000 kB
Cached:          6000000 kB
SwapCached:        40000 kB
Active:          5000000 kB
Inactive:        4000000 kB
Shmem:            250000 kB
Slab:             900000 kB
SReclaimable:     500000 kB
SUnreclaim:       400000 kB
SwapTotal:       8388604 kB
SwapFree:        6291452 kB
Dirty:               120 kB
"""


NVIDIA_SMI = """#!/bin/sh
# Stub of: nvidia-smi --query-gpu=pci.bus_id,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw --format=csv,noheader,nounits
case "$*" in
    *--query-gpu=pci.bus_id,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw*--format=csv,noheader,nounits*)
        echo "00000000:01:00.0, 64, 71, 3072, 10240, 215.30" ;;
    *) echo "nvidia-smi stub: unexpected arguments: $*" >&2; exit 2 ;;
esac
"""

BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=%(socket)s</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
"""


def unit(description, load="loaded", active="active", sub="running", file="enabled", listed=True, enablement=""):
    return dict(description=description, load=load, active=active, sub=sub, file=file, listed=listed, enablement=enablement)


def build_systemd():
    """A private "system" bus with tests/mock-systemd.py as org.freedesktop.systemd1 on it.
    Returns the processes to stop afterwards."""
    os.makedirs("/run/systemd/system", exist_ok=True)       # sd_booted()
    os.makedirs(f"{FX}/bin")
    state = f"{FX}/systemd-state.json"
    units = {
        "ssh.service": unit("OpenBSD Secure Shell server"),
        "cron.service": unit("Regular background program processing daemon"),
        "NetworkManager.service": unit("Network Manager"),
        "systemd-journald.service": unit("Journal Service", file="static"),
        "apparmor.service": unit("Load AppArmor profiles", sub="exited"),
        "backup.service": unit("Nightly backup", active="failed", sub="failed"),
        "denied.service": unit("A service this user may not control", active="inactive", sub="dead", file="disabled"),
        "bluetooth.service": unit("Bluetooth service", active="inactive", sub="dead"),
        # its start job fails
        "unstable.service": unit("A service that fails to start", active="inactive", sub="dead", file="disabled"),
        # installed, never loaded in this boot: only among the unit files
        "cups.service": unit("", load="", active="", sub="", file="disabled"),
        "rsync.service": unit("", load="", active="", sub="", file="masked"),
        # an instance of a template: loaded, no unit-file entry of its own
        "getty@tty1.service": unit("Getty on tty1", listed=False, enablement="enabled"),
        "getty@.service": unit("", load="", active="", sub="", file="enabled"),
        # a name other units refer to, nothing installed
        "auditd.service": unit("auditd.service", load="not-found", active="inactive", sub="dead", listed=False),
        # created at run time
        "run-u42.service": unit("/usr/bin/sleep 600", listed=False, enablement="transient"),
    }
    with open(state, "w") as f:
        json.dump(units, f, indent=1)
    with open(f"{FX}/bin/nvidia-smi", "w") as f:
        f.write(NVIDIA_SMI)
    os.chmod(f"{FX}/bin/nvidia-smi", 0o755)
    os.environ["PATH"] = f"{FX}/bin:" + os.environ.get("PATH", "")

    socket = f"{FX}/system-bus"
    with open(f"{FX}/bus.conf", "w") as f:
        f.write(BUS_CONFIG % {"socket": socket})
    bus = subprocess.Popen(["dbus-daemon", f"--config-file={FX}/bus.conf", "--nofork", "--nopidfile"])
    for _ in range(100):
        if os.path.exists(socket):
            break
        time.sleep(0.05)
    else:
        sys.exit("fixture: the private system bus did not start")
    os.chmod(socket, 0o666)
    for directory in (FX,):
        os.chmod(directory, 0o755)
    os.environ["DBUS_SYSTEM_BUS_ADDRESS"] = f"unix:path={socket}"
    mock = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock-systemd.py"), state])
    for _ in range(200):
        if os.path.exists(f"{FX}/mock-systemd.ready"):
            break
        if mock.poll() is not None:
            sys.exit("fixture: mock-systemd.py exited")
        time.sleep(0.05)
    else:
        sys.exit("fixture: mock-systemd.py did not appear on the bus")
    return [mock, bus]


def main():
    command = sys.argv[1:]
    if not command:
        sys.exit(__doc__)
    if os.geteuid() != 0:
        sys.exit("fixture: needs root inside the build chroot (scripts/dev.sh run)")
    if os.path.exists("/run/systemd/system") and not os.path.exists(FX):
        sys.exit("fixture: this looks like a real system (systemd is running); refusing to mount over /sys")
    shutil.rmtree(FX, ignore_errors=True)
    os.makedirs(FX)
    build_sysfs()
    helpers = build_systemd()

    # "nexus-monitor" means the freshly built binary (dev.sh exports LD_LIBRARY_PATH=<build>/lib).
    for path in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        candidate = os.path.join(os.path.dirname(path.rstrip("/")), "bin", command[0])
        if "/" not in command[0] and os.access(candidate, os.X_OK):
            command[0] = candidate
            break
    try:
        status = subprocess.call(command)
    finally:
        for helper in helpers:
            helper.terminate()
        # The mounts die with the namespace; the tree in /tmp would stay.
        subprocess.run(["umount", "-R", "-l", f"{FX}/orig-sys"], check=False, stderr=subprocess.DEVNULL)
        shutil.rmtree(FX, ignore_errors=True)
    sys.exit(status)


if __name__ == "__main__":
    main()
