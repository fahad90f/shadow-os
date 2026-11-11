# SHADOW Monitor

System monitor and task manager of Shadow OS: binary `shadow-monitor`, QML module
`org.shadow.monitor`, application id `org.shadow.monitor`. Two launchers start
the same program: **System Monitor** (`org.shadow.monitor.desktop`) opens the
overview, **Task Manager** (`org.shadow.taskmanager.desktop`) runs
`shadow-monitor --page processes`.

Everything on screen is read from the running system. A value the kernel or a
driver does not provide is shown as not available, with the reason — nothing
is estimated or filled in.

| Page | Shows |
|---|---|
| Overview | gauges for processor, memory, graphics and the root file system; load average, processor temperature, process count, uptime; 2-minute histories of processor, memory, network and disk activity; every logical CPU; a hardware summary with a button that opens `shadow-sysinfo` |
| Processes | name, PID, user, CPU share, resident memory, state, nice value, command line; sortable by every column, searchable (name, user, PID, command line), optional tree view, own processes only, kernel threads; end (SIGTERM), force kill (SIGKILL), suspend (SIGSTOP), continue (SIGCONT) and change priority (nice value), each after confirmation |
| Services | systemd service units with state and start-up setting; start, stop, restart, enable, disable through systemd's D-Bus API |
| Disks | usage of every mounted file system; read/write rates, busy time and totals of every physical drive |
| Network | download/upload rates, totals, addresses and link state of every interface |
| Sensors | temperatures, fans, graphics devices (utilisation, temperature, video memory, power) and batteries |

```
shadow-monitor [--page <key> | <key>]      key: overview processes services disks network sensors
shadow-monitor --dump <topic>              topic: cpu memory gpu disks network processes services sensors battery
```

The application is single-instance: a second launch hands its page to the
running window. Keyboard: `Ctrl+F` focuses the search field (Processes,
Services), `Delete` ends the selected process, `Shift+Delete` force-kills it —
both after confirmation; suspend, continue and the priority dialog are in the
row's context menu and behind the “…” button; the arrow keys move the selection in the tables.

## Sampling

`SystemMonitor` takes a sample every 1, 2 or 5 seconds (selectable in the side
bar, stored in `~/.config/shadow/monitor.conf`) on a worker thread; the process
table is scanned on a second thread and only while the Processes page is
shown; file-system usage is read every 5 seconds on a third thread, because
`statvfs()` blocks for as long as a network file server takes to answer.
Histories hold 120 samples, i.e. 2, 4 or 10 minutes; changing the interval
clears them. **Nothing is sampled while the window is not exposed** — the
window system's own notion of "not on screen": hidden or minimised, and under
Wayland whenever the compositor stops presenting the window. The first sample
after a pause only re-primes the counters, so no rate is ever computed across
the gap. Delta-based values (CPU utilisation, rates, per-process CPU share) appear
with the second reading, half a second after the first.

Pages other than Processes and Services exist only while they are shown, so a
hidden page costs neither CPU time nor rendered frames.

## Data sources

| Data | Source |
|---|---|
| CPU utilisation, total and per CPU | `/proc/stat`, difference of two readings; `iowait` counts as idle |
| Load average, runnable tasks, thread count | `/proc/loadavg` |
| Process count, uptime | entries of `/proc`, `/proc/uptime` |
| CPU model, cores, threads | `/proc/cpuinfo`; `lscpu` where cpuinfo has no model name (ARM) |
| CPU frequency | `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`, else `cpu MHz` of `/proc/cpuinfo` (re-read every 5 s); mean over the online CPUs |
| Memory, swap | `/proc/meminfo`; used = `MemTotal − MemAvailable` (as `free`), cached = `Cached + SReclaimable` |
| Temperatures, fans | `/sys/class/hwmon/hwmonN/{name,tempN_input,tempN_label,tempN_max,tempN_crit,fanN_input,fanN_label}`, `/sys/class/thermal/thermal_zoneN/{type,temp,trip_point_N_*}` |
| Processor temperature | chosen from the above: `coretemp` "Package id N", `k10temp`/`zenpower` "Tdie" then "Tctl", thermal zone `x86_pkg_temp`, then zones/chips named after the CPU or SoC |
| Graphics devices | `/sys/class/drm/cardN/device/`: driver, PCI ids, `power/runtime_status`, `gpu_busy_percent`, `mem_info_vram_{used,total}`, `hwmon/hwmonN/{temp1_input,power1_average,power1_input,fan1_input}`; names from `/usr/share/misc/pci.ids` |
| NVIDIA proprietary driver | devices from `/proc/driver/nvidia/gpus/`; figures from `nvidia-smi --query-gpu=pci.bus_id,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw --format=csv,noheader,nounits` — only if `nvidia-smi` is installed |
| File systems | `/proc/self/mountinfo` + `statvfs(3)`; kernel-internal file systems (proc, tmpfs, squashfs, …) are left out, a file system mounted several times is listed once (same rule as `df`) |
| Drive activity | `/proc/diskstats`, `/sys/block/<dev>/{size,removable,queue/rotational,device/model}`; whole devices backed by hardware only |
| Network | `/proc/net/dev`, `/sys/class/net/<if>/{type,operstate,address,mtu,speed,device,wireless}`, `getifaddrs(3)` |
| Processes | `/proc/<pid>/stat` (state, parent, CPU time, threads, nice, start time), `/proc/<pid>/status` (effective UID, `VmRSS`), `/proc/<pid>/cmdline`; user names through `getpwuid_r(3)` |
| Services | `org.freedesktop.systemd1.Manager` on the D-Bus system bus: `ListUnitsByPatterns([], ["*.service"])`, `ListUnitFilesByPatterns([], ["*.service"])`, `GetUnitFileState` for loaded units without a unit-file entry of their own; re-read every 10 s while the page is shown |
| Batteries, external power | `/sys/class/power_supply/*/` (`type`, `status`, `capacity`, `energy_*` or `charge_*`, `power_now`/`current_now`, `voltage_*`, `cycle_count`, `technology`, `scope`, …) |

Conventions worth knowing when comparing with other tools:

* **Process CPU share** is relative to one CPU (100 % = one core busy), like
  `top`; `ps` prints the average since the process started instead.
* **Process memory** is `VmRSS`, the figure `ps -o rss` prints.
* **Process names** are the kernel's `comm`, completed from `argv[0]` where the
  kernel cut it to 15 characters. Kernel threads show `[name]` as command.
* **File-system percentages** are used ÷ (used + available), as `df` computes
  them; reserved blocks make this differ from used ÷ size.
* **Network totals** add up hardware interfaces only (bridges, bonds, VLANs
  and tunnels carry the same bytes again); without any hardware interface, all
  but the loopback.
* A graphics device or sensor chip that is **runtime-suspended** is not read:
  reading would power it up and polling would keep it awake.

## Runtime dependencies

Debian package names, beyond the shared SHADOW UI runtime every application
links (`desktop/ui`: the `org.shadow.ui` and `org.shadow.style` QML modules with
Qt 6 Quick, Quick Controls, Quick Shapes, SVG and D-Bus):

| Package | Needed for |
|---|---|
| `systemd`, `dbus-daemon` or `dbus-broker` (system bus) | Services page; without a running systemd or without the system bus the page says so |
| `pkexec`, `polkitd` and a polkit authentication agent in the session | signalling or re-prioritising another user's process, raising a priority; authorising service actions |
| `procps` | `/usr/bin/kill`, run through `pkexec` |
| `bsdutils` (essential) | `/usr/bin/renice`, run through `pkexec` |
| `pci.ids` | product names of graphics devices (optional: the driver name is shown otherwise) |
| `util-linux` | `lscpu`, only where `/proc/cpuinfo` has no model name |
| `nvidia-smi` (non-free) | figures of the proprietary NVIDIA driver (optional) |
| SHADOW System Information (`shadow-sysinfo`) | the "System Information" button (optional: disabled with an explanation otherwise) |

The launchers use the icons `org.shadow.monitor` and `org.shadow.taskmanager`
from the SHADOW icon set; the application ships none itself.

## Privileged operations

The application never runs as root and installs no helper and no polkit
action of its own.

* **Signalling a process** (end, force kill, suspend, continue). The signal
  is sent directly, to a `pidfd` of the process (so it cannot hit a different
  process that reused the PID; the process is identified by PID and start
  time from the moment it is chosen). PID 1 and kernel threads are never
  touched, and the monitor does not suspend itself — refused in the back-end,
  not only in the UI. If the process belongs to another user, the
  confirmation says so and the signal is sent with
  `pkexec /usr/bin/kill -s TERM|KILL|STOP|CONT <pid>` — an argument vector,
  the PID formatted from an integer. polkit's stock action
  `org.freedesktop.policykit.exec` applies (administrator authentication).
* **Changing a priority.** `setpriority(2)` on the process, as `renice(1)`
  does; the dialog is the confirmation. The kernel lets an unprivileged user
  only raise the nice value of their own processes; a lower value or another
  user's process is announced in the dialog ("Apply as administrator") and
  done with `pkexec /usr/bin/renice -n <nice> -p <pid>`, both numbers
  formatted from range-checked integers.
* **Services.** `StartUnit`, `StopUnit`, `RestartUnit` (mode `replace`; the
  outcome is the `JobRemoved` signal of the job they queue),
  `EnableUnitFiles` / `DisableUnitFiles` followed by `Reload`, all on
  `org.freedesktop.systemd1.Manager`, sent as the user with the D-Bus flag
  "interactive authorisation allowed". systemd authorises the request
  through polkit (`org.freedesktop.systemd1.manage-units`,
  `…manage-unit-files`, `…reload-daemon`) and the session's agent asks for
  authentication. Unit names are checked against systemd's character set
  before they are sent. Stop, restart and disable ask for confirmation first.

## Tests

In `tests/`, all run inside the build chroot through `scripts/dev.sh run`
(usage in each file's header):

| File | What it checks |
|---|---|
| `crosscheck.py` | `--dump memory/cpu/disks/network/processes` against `free`, `/proc/stat`, `nproc`, `df`, `lsblk`, `/proc/diskstats`, `/proc/net/dev`, `ip -j addr`, `ps` |
| `fixture.py` | builds a synthetic machine (hwmon chips, thermal zones, batteries, four graphics devices, cpufreq, a `meminfo` with swap, a stub `nvidia-smi`, and a private D-Bus system bus with `mock-systemd.py` as `org.freedesktop.systemd1`) in the throw-away mount namespace of one test run, then runs a command in front of it; the application has no test hooks |
| `mock-systemd.py` | the part of `org.freedesktop.systemd1.Manager` the monitor uses, with the real interface's signatures; refuses changing calls sent without the interactive-authorisation flag; one unit refuses every change, the start job of another fails |
| `fixture-check.py` | `--dump sensors/memory/cpu/battery/gpu/services` against the values the fixture defines |
| `ui-processes.sh` | real key presses and clicks (xdotool) on the Processes page, one scenario per run: search, Delete / Shift+Delete, confirmation and Escape, SIGTERM and SIGKILL reaching exactly the selected process, the End process button, arrow-key selection across refreshes, suspend and continue (SIGSTOP / SIGCONT) from the context menu, the priority dialog (nice value applied directly; a lower one needs the administrator path), refusal of PID 1, the administrator path without a polkit authority, sorting, menus, tree view, second launch, stored interval |
| `ui-services.sh` | the Services page against `mock-systemd.py`: start, stop with keyboard confirmation, refused action, a job that ends as failed, enable, filters |
| `soak.sh` | runs the monitor on one page for several minutes and prints its own CPU share, resident memory, file descriptors and threads every 30 s; fails if memory or descriptors keep growing |
| `ui-window.sh` | sampling stops while the window is unmapped and resumes; the System Information button starts `shadow-sysinfo` |

## Limitations

* **Not verified on real hardware or a real systemd.** The build environment
  is a virtual machine without hwmon devices, thermal zones, batteries, DRM
  devices, cpufreq or a running systemd. Temperatures, fans, batteries,
  graphics devices, `nvidia-smi` parsing and the Services page have been
  exercised only against the synthetic machine of `tests/fixture.py`, which
  follows the kernel's documented sysfs ABI and systemd's documented D-Bus interface but
  is not the real thing. The polkit prompts (`pkexec`, systemd) have never
  been seen to succeed: there is no polkit authority in the chroot, only the
  failure path is verified. All of this belongs in the QEMU integration tests
  (`--dump` topics exist for every data set).
* GPU utilisation is available only where the driver publishes it in sysfs
  (`amdgpu`) or through `nvidia-smi`. Intel (`i915`, `xe`), `nouveau`,
  virtual and most ARM graphics drivers do not; the page says so. A GPU
  driven by the proprietary NVIDIA driver is found only while that driver is
  loaded.
* The overview's graphics gauge shows one device: the first that reports its
  utilisation. All devices are listed on the Sensors page.
* Histories are not recorded while the window is hidden; the time axis of a
  chart skips such a gap without marking it. (Pausing was verified by
  unmapping the window under X11 and by minimising it under KWin/Wayland in
  the nested test session of `scripts/dev.sh nested`.)
* CPU shares have the resolution of the kernel's clock tick (1 % of one CPU
  at a 1-second interval).
* Command lines longer than 4095 bytes are cut (marked with "…").
* Service templates (`name@.service`) and unit names that are only referenced
  but not installed (`not-found`) are not listed; only `.service` units are
  shown. Unit descriptions exist only for units systemd has loaded.
* With `/proc` mounted `hidepid=1|2`, only the user's own processes are
  visible, and nothing indicates that the list is restricted.
* Acting on another user's process: between the confirmation and the end of
  the authentication dialog, `pkexec kill` / `pkexec renice` can only address
  the process by PID. A PID that is reused in that window would be hit. (The
  direct signal path has no such window; the direct `setpriority` call checks
  the identity immediately before it, which leaves a window of microseconds.)
* The nice value is that of the process's main thread, as `renice -p` sets
  it; threads that changed their own priority keep it.
* The tree view nests the rows that are shown: with a search or "only my
  processes" active, a process whose parent is filtered out starts a new
  top-level branch. Rows cannot be collapsed.
* The service list is polled (every 10 s and after each action); it does not
  follow systemd's `PropertiesChanged` signals.
* Battery "time remaining" is the device's own figure where it has one,
  otherwise energy ÷ power of the moment, labelled "about".
* The gauges and bars animate each change for 340 ms (ShadowUI). With software
  rendering this costs noticeable CPU time on the Overview page. Measured
  with `tests/soak.sh` under llvmpipe on a 4-CPU virtual machine that was
  itself fully loaded, at the 1-second interval and 1280x860: Overview 28–39 %
  of one core over 5 minutes (almost all of it in the llvmpipe and scene-graph
  render threads; the sampling threads used 0.1 s of CPU time in 2 minutes),
  Processes 5 %, Sensors 0.1–0.2 %. Resident memory stayed flat (215 MiB with
  the software renderer, −0.7 MiB over the second half of the 5 minutes), as
  did the file descriptors (14) and threads (17). The "reduce motion"
  appearance setting removes the animation. Not measured with a hardware
  renderer.
