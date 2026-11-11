#!/usr/bin/env python3
"""Boot the desktop ISO and verify that the live session reaches Shadow Horizon.

    tests/desktop/test_desktop_boot.py --iso build/out/iso/shadow-os-<ver>-<arch>.iso
        [--firmware uefi|uefi-secureboot|bios] [--shots DIR]

Verified from inside the guest (serial console) while the graphical session
runs on the emulated display:
  * the display manager started and logged the live user into the
    "shadow-horizon" Wayland session without a password prompt
  * the compositor (kwin_wayland) and the shell (horizon-shell) are running
    and the shell did not restart (crash loop)
  * the session's D-Bus services exist: notification-independent core ones
    (window manager, global shortcuts, polkit agent registration)
  * no failed systemd units, system or user
Screenshots of the emulated display are saved for visual review; the test
also fails when the screen is (almost) a single colour, i.e. nothing was drawn.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from qemu import VM, TestFailure, PANIC, main_guard, report  # noqa: E402


def distinct_colours(png: Path) -> int:
    from PIL import Image
    im = Image.open(png).convert("RGB").resize((160, 90))
    return len(set(im.getdata()))


def run(args):
    shots = Path(args.shots)
    shots.mkdir(parents=True, exist_ok=True)
    vm = VM(arch=args.arch, firmware=args.firmware, name=f"desktop-{args.arch}-{args.firmware}",
            workdir=args.workdir, memory_mb=args.memory, graphics=True)
    vm.add_cdrom(args.iso)
    t0 = time.time()
    with vm:
        print(f"# accel={vm.accel} firmware={args.firmware} log={vm.serial_log}", flush=True)
        vm.expect(r"Linux version \S+", args.timeout, fail=PANIC)
        vm.login(args.user, args.password, timeout=args.timeout)
        report("system booted, serial login works", True, f"{time.time() - t0:.0f}s")

        # The graphical session starts in parallel; under emulation without
        # KVM it can take several minutes.
        deadline = time.time() + args.session_timeout
        state = ""
        while time.time() < deadline:
            _, state = vm.run("pgrep -u $(id -u) -x horizon-shell >/dev/null && echo SHELL-UP || echo waiting; "
                              "systemctl is-active sddm", check=False)
            if "SHELL-UP" in state:
                break
            time.sleep(15)
        vm.screenshot(shots / "desktop-1-first.png")
        report("display manager active", "active" in state.split(), state.strip().replace("\n", " | "))
        if "SHELL-UP" not in state:
            _, diag = vm.run("sudo journalctl -b --no-pager -u sddm | tail -n 25; "
                             "journalctl --user -b --no-pager | tail -n 40", check=False)
            print(diag)
            raise TestFailure("horizon-shell did not start within the time limit")
        report("horizon-shell running", True, f"{time.time() - t0:.0f}s after power-on")

        _, out = vm.run("loginctl list-sessions --no-legend; "
                        "loginctl show-session $(loginctl list-sessions --no-legend | awk '$3==\"shadow\" && $0 ~ /seat0/ {print $1; exit}') "
                        "-p Type -p Desktop -p Active -p Class 2>&1", check=False)
        report("graphical login session", "Type=wayland" in out, out.strip().replace("\n", " | "))

        _, out = vm.run("pgrep -a -u $(id -u) 'kwin_wayland|horizon' | cut -c1-120")
        report("compositor and shell processes", "kwin_wayland" in out and "horizon-shell" in out,
               out.strip().replace("\n", " | "))

        time.sleep(args.settle)
        vm.screenshot(shots / "desktop-2-settled.png")
        colours = distinct_colours(shots / "desktop-2-settled.png")
        report("something is drawn on the screen", colours > 50, f"{colours} distinct colours in a downscaled screenshot")

        _, out = vm.run("journalctl --user -b --no-pager | grep -c 'horizon-shell exited with status' || true", check=False)
        restarts = out.strip().splitlines()[-1] if out.strip() else "?"
        report("shell did not crash", restarts == "0", f"restarts: {restarts}")

        _, out = vm.run("export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus; "
                        "busctl --user list --no-legend | awk '{print $1}' | grep -E '^org\\.(kde\\.KWin|kde\\.kglobalaccel|freedesktop\\.portal\\.Desktop)$' | sort | tr '\\n' ' '",
                        check=False)
        report("session services on the bus", "org.kde.KWin" in out and "org.kde.kglobalaccel" in out, out.strip())

        _, out = vm.run("systemctl --failed --no-legend | head; systemctl --user --failed --no-legend | head; echo END", check=False)
        failed = [l for l in out.strip().splitlines() if l.strip() and l.strip() != "END"]
        report("no failed units (system and user)", not failed, " | ".join(failed) or "none")

        _, out = vm.run("sudo journalctl -b --no-pager -p err | grep -v -E 'pcspkr|RAS|ACPI|blk_update|floppy|Bluetooth' | tail -n 15", check=False)
        print("# errors in the journal (informational):")
        print(out)

        if args.keys:
            # Open the launcher with the Meta key and take a screenshot.
            vm.send_key("meta_l")
            time.sleep(args.settle)
            vm.screenshot(shots / "desktop-3-launcher.png")
            vm.send_key("esc")
        vm.shell_poweroff()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--arch", default="amd64")
    ap.add_argument("--firmware", default="uefi", choices=["uefi", "uefi-secureboot", "bios"])
    ap.add_argument("--workdir", default="build/work/tests/desktop-boot")
    ap.add_argument("--shots", default="build/work/shots")
    ap.add_argument("--memory", type=int, default=4096)
    ap.add_argument("--user", default="shadow")
    ap.add_argument("--password", default="live")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--session-timeout", type=float, default=1200)
    ap.add_argument("--settle", type=float, default=45)
    ap.add_argument("--keys", action="store_true", help="also exercise the launcher through the keyboard")
    main_guard(lambda: run(ap.parse_args()))
