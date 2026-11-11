# Shell module: power

Idle actions (screens off, lock, suspend), low-battery notices and update
notices of the Shadow Horizon shell. Compiled into `horizon-shell`; removing
this directory removes these features and nothing else. No panel item, no
surface, no QML.

Context property: **`power`** (`powerservice.h`): `onBattery`,
`batteryWarning`, `idleInhibited`, `userIdle`.
Diagnostics: **`horizon-shell --dump power`** (no compositor needed).

## What it does

| Part | Files | Source / interface |
|---|---|---|
| Settings | `powerconfig.*` | `~/.config/shadow/power.conf` (`[Power] Idle{ScreenOff,Lock,Suspend}Minutes` and `…OnBattery`; format in `applications/control-center/backends/powerbackend.h`) and `~/.config/shadow/updater.conf` (`Updates/AutoCheck`, `Updates/CheckIntervalHours`). Read through `QSettings` (falls back per key to `/etc/xdg/shadow/…`), never written. Values outside 0–1440 or not numbers get the documented default. Re-read when a file is replaced (directory watch) or edited in place (file watch). |
| Power source | `powersource.*` | UPower, system bus: `OnBattery`, `GetCriticalAction()`, and `WarningLevel`/`Percentage`/`IsPresent` of the display device; `PropertiesChanged`. Without UPower: external power. |
| Idle detection | `poweridle.*` | KF6 IdleTime → on Wayland `ext_idle_notifier_v1.get_idle_notification` (the request that honours Wayland idle inhibitors). One timeout per action that is not 0, with the AC or battery minutes. |
| Screens off | `powerscreen.*` | KWin's `org_kde_kwin_dpms_manager` / `org_kde_kwin_dpms` Wayland protocol (plasma-wayland-protocols `dpms.xml`): `set(off)` per output that reports `supported`. KWin switches the outputs on again at the next input; the module also does it when the idle state ends. |
| Lock | `poweridle.*` | `Session::lock()` → `org.freedesktop.ScreenSaver.Lock` (KWin's screen locker). |
| Suspend | `poweridle.*` | `Session::suspend()` → logind `Suspend(interactive=true)`. Locking before suspend is left to KWin's locker (`kscreenlockerrc [Daemon] LockOnResume`, on logind's `PrepareForSleep`) — verified, see below — so the module does not lock a second time. |
| Inhibitors | `powerinhibitors.*` | see below |
| Low battery | `powerservice.cpp` | UPower `WarningLevel`: *low* → one normal notification; *critical* → a critical, resident one that says what UPower's `CriticalAction` (`UPower.conf`) will do; *action* → "is suspending/shutting down now". Withdrawn when the level returns to none. The module performs no shutdown of its own. |
| Update notices | `powerupdates.*` | Runs `shadow-updater --check` 3 minutes after the shell started and then every `CheckIntervalHours`, when `AutoCheck` is true and the binary is on `PATH`. Exit 10/11 → notification "Updates / Security updates are available" with the counts from the updater's JSON and the action "Open Updater" (starts `shadow-updater`); exit 0 → nothing (an earlier notice is closed); exit 1 or no binary → nothing. Only while the network is up: NetworkManager `State` ≥ 60, or a default route in `/proc/net/route` / `ipv6_route` where NetworkManager is not running; otherwise retried every 5 minutes and on NetworkManager's `StateChanged`. |
| Notifications | `powernotifier.*` | `org.freedesktop.Notifications` (`Notify`, `CloseNotification`, `ActionInvoked`, `NotificationClosed`), asynchronous — works with any daemon, the shell's own `notifications` module included. |

### Inhibitors

* **Applications** call `org.freedesktop.ScreenSaver.Inhibit`. KWin provides
  that service (its screen locker) and forwards each request as
  `org.kde.Solid.PowerManagement.PolicyAgent.AddInhibition(4, app, reason)`
  to the session's power manager. This module *is* that power manager: it
  owns **`org.kde.Solid.PowerManagement.PolicyAgent`** on the session bus
  (object `/org/kde/Solid/PowerManagement/PolicyAgent`: `AddInhibition`,
  `ReleaseInhibition`, `ListInhibitions`, `HasInhibition`,
  `InhibitionsChanged`). Type 4 (screen) holds back all three actions,
  type 1 (suspend) only the suspend. An inhibition whose caller leaves the
  bus is dropped. If another power manager already owns the name, the dump
  says so and application inhibitors are not seen.
* **logind** block inhibitors: `Manager.BlockInhibited` (and
  `ListInhibitors` for display). `idle` holds back all three actions,
  `sleep` the suspend; logind is asked again right before a suspend.
* **Wayland idle inhibitors** (`zwp_idle_inhibit_manager_v1`, e.g. a video
  player's window): the compositor itself reports no idleness meanwhile.

An action that was held back is not lost: when the inhibitor goes away while
the user is still idle, all timeouts start again from that moment. They also
start again when the settings or the power source change and after a wake-up
(`PrepareForSleep(false)`).

### `--dump power`

`settings` (file, both value sets, time scale), `powerSource`, `effective`
(source and the three minutes in force), `inhibitors.logind`, `updates`
(settings, whether the updater is installed) as read by the dump process
itself, plus `shell`: the running shell's own state — `idle.timers` (state
`off | armed | done | held | failed`, with the reason), `idle.events` (last
20 actions), `inhibitors.applications`, `screens`, `updates` (next check,
last result), `notifications`. The shell keeps that state in
`$XDG_RUNTIME_DIR/shadow-horizon-power.json` (removed on exit);
`shellState` is `running`, `not-running` or `stale-state-file`.

### Developer override

`SHADOW_POWER_TIME_SCALE=<factor>` divides every duration the module waits
for; `60` turns the minutes of `power.conf` into seconds (and the 3-minute
start delay into 3 s, an hour of check interval into a minute).

## Run-time dependencies (Debian packages)

`libkf6idletime6` (linked), `kwin-wayland` (idle notifications, DPMS
protocol, screen locker with `libkscreenlocker6`), `systemd` (logind),
`upower`, `libnotify`-compatible notification daemon (the shell's
`notifications` module), `shadow-updater` (optional), `network-manager`
(optional). Build: `libkf6idletime-dev`, `plasma-wayland-protocols`.
Each service is optional at run time; what is missing is reported in the dump.

## Limitations

* Changing the settings or the power source restarts the idle count (the
  compositor's idle clock cannot be read back).
* Screens are only switched off on compositors with `org_kde_kwin_dpms`
  (KWin); elsewhere the action is reported as failed in the dump, never faked.
* The first update check comes 3 minutes after every shell start, whatever
  the time of the last check; metered connections are not treated specially.
* `session/kscreenlockerrc` (core, outside this module) still sets
  `Autolock=true, Timeout=10`: KWin's locker then locks after 10 minutes by
  itself, independently of `IdleLockMinutes`. It should be `Autolock=false`
  now that this module does the idle lock.

## Tested

Build chroot, `scripts/dev.sh build --dir modpower --only horizon-shell --modules power`.

* `tests/desktop/smoke.sh --dir modpower horizon-shell` — 1 passed, no QML warnings.
* `horizon-shell --dump power` without any bus (honest "not reachable"), and
  against a **real `upowerd`** on a private bus (`criticalAction: PowerOff`, no battery).
* `tests/session-test.sh` (see its header), 117 checks, 0 failed:

  ```
  scripts/dev.sh exec --dir modpower -- cmake -S /src/desktop/horizon-shell/modules/power/tests -B /build/modpower-tests -G Ninja
  scripts/dev.sh exec --dir modpower -- ninja -C /build/modpower-tests
  SHADOW_RUN_TIMEOUT=600 scripts/dev.sh nested --dir modpower --wait 1 -- '. /src/desktop/horizon-shell/modules/power/tests/session-test.sh'
  ```

  The nested session's KWin (X11 back-end, `--no-lockscreen`) has neither
  DPMS nor a locker, so each scenario runs the shell in a second KWin 6.3.6
  nested in the first (Wayland back-end, which implements DPMS, with its
  screen locker), with `tests/fake-services.py` standing in for UPower,
  logind and NetworkManager on a private "system" bus and for the
  notification daemon, `tests/fake-updater.sh` for the updater, real
  `xdotool` input, and `SHADOW_POWER_TIME_SCALE`. Verified there:
  * settings: AC/battery values, atomic replacement, in-place edit, invalid values → defaults, everything 0 → nothing happens;
  * screens off after the timeout and on again on input — checked with `kscreen-doctor --dpms show` (off / on) and the outer screenshot (`modpower-idle-screen-off.png`: KWin titles the window "Output dimmed");
  * `org.freedesktop.ScreenSaver.Inhibit` of a real client → forwarded by KWin → action `held`, released by `UnInhibit` → timeouts restart → off; a client that dies is dropped; type 1 does not keep the screen on;
  * a window with a Wayland idle inhibitor (`power-test-idle-inhibit`) → no timeout arrives; gone → screen off;
  * logind `idle` block → held; `sleep` block → screen off but suspend held, `Suspend` not called; block gone → `Suspend(true)` called once;
  * KWin's locker locks by itself on `PrepareForSleep(true)` with `LockOnResume=true` (`GetActive` → true while the module's lock timer is off) and does not with `LockOnResume=false`;
  * idle lock → `GetActive` true;
  * update check: not while NetworkManager says disconnected, runs on `StateChanged`; exit 11 / 10 → notification text, counts, action; action → `shadow-updater` started and notice closed; exit 0, exit 1, missing binary, `AutoCheck=false` → no notification; the **real** `shadow-updater --check` (no PackageKit there → exit 1 → no notification);
  * battery: low → critical (replaces, critical urgency, HybridSleep text) → action → withdrawn.
* With `--modules "power;notifications"`, scenario `popup`
  (`POWER_TEST_SCENARIOS=popup POWER_TEST_CLICK="1131 258"`): the notices in
  the shell's own notification daemon and a click on "Open Updater"
  (`modpower-popup-updates.png`, `modpower-popup-battery.png`).
* DPMS through this protocol is not gated by KWin's interface permissions
  (`kscreen-doctor --dpms off` worked without `KWIN_WAYLAND_NO_PERMISSION_CHECKS`).

## Not tested (needs the VM image / hardware)

* A real logind: `Suspend` actually suspending, polkit's answer, the locker's
  delay inhibitor ordering lock → sleep (the locker did not take a delay lock
  from the stand-in), wake-up; real `systemd-inhibit` locks.
* DPMS on the DRM back-end (real or virtual GPU); only KWin's nested Wayland
  back-end was exercised. KWin's X11-window and `--virtual` back-ends report
  no DPMS support.
* The lock screen itself: `kscreenlocker_greet` cannot start in the chroot
  (no greeter theme/platform), so only the locked state was observed.
* A real battery and UPower's critical action; NetworkManager; PackageKit
  results other than "not reachable"; long real-time intervals (hours).
* Coexistence with PowerDevil (would own the PolicyAgent name) — not part of SHADOW.
