#!/usr/bin/env python3
"""Boot the NEXUS OS live ISO through real firmware and verify the live session.

    tests/boot/test_live_boot.py --iso build/out/iso/nexus-os-<ver>-<arch>.iso \
        [--arch amd64|arm64] [--firmware uefi|uefi-secureboot|bios] [--media cdrom|usb]

What is verified (all from inside the guest, over the serial console):
  * firmware → shim → GRUB → kernel → live-boot → systemd reaches a login prompt
  * the live user can log in and has passwordless sudo
  * the root filesystem is the squashfs+overlay from the media
  * Secure Boot state matches the firmware mode under test
  * systemd reports no failed units
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from qemu import VM, TestFailure, PANIC, main_guard, report  # noqa: E402


def run(args):
    vm = VM(arch=args.arch, firmware=args.firmware, name=f"live-{args.arch}-{args.firmware}-{args.media}",
            workdir=args.workdir, memory_mb=args.memory)
    if args.media == "cdrom":
        vm.add_cdrom(args.iso)
    else:
        vm.add_usb_stick(args.iso)
    t0 = time.time()
    with vm:
        print(f"# accel={vm.accel} firmware={args.firmware} media={args.media} log={vm.serial_log}", flush=True)
        vm.expect(r"Linux version \S+", args.timeout, fail=PANIC)
        report("kernel started", True, f"{time.time() - t0:.0f}s")
        vm.login(args.user, args.password, timeout=args.timeout)
        report("login prompt reached and live user logged in", True, f"{time.time() - t0:.0f}s")

        _, out = vm.run("sudo -n id -u")
        report("passwordless sudo", out.strip().endswith("0"), out.strip())

        _, out = vm.run("findmnt -n -o FSTYPE,SOURCE /; cat /proc/cmdline")
        report("root is live overlay", "overlay" in out, out.strip().replace("\n", " | "))
        if "boot=live" not in out:
            raise TestFailure("kernel command line lacks boot=live")

        _, out = vm.run("uname -srm; . /etc/os-release; echo \"$PRETTY_NAME\"")
        report("system identity", True, out.strip().replace("\n", " | "))

        rc, out = vm.run("test -d /sys/firmware/efi && echo EFI || echo BIOS")
        want = "BIOS" if args.firmware == "bios" else "EFI"
        report(f"booted via {want}", out.strip() == want, out.strip())
        if out.strip() != want:
            raise TestFailure(f"expected {want} boot, got {out.strip()}")

        if args.firmware != "bios":
            _, out = vm.run("mokutil --sb-state 2>&1 || true")
            enabled = "SecureBoot enabled" in out
            want_sb = args.firmware == "uefi-secureboot"
            report("Secure Boot state", enabled == want_sb, out.strip().splitlines()[0] if out.strip() else "?")
            if enabled != want_sb:
                raise TestFailure(f"Secure Boot state mismatch: {out!r}")
            if want_sb:
                _, out = vm.run("cat /sys/kernel/security/lockdown")
                report("kernel lockdown active under Secure Boot", "[integrity]" in out or "[confidentiality]" in out, out.strip())

        # Give late units a moment, then require a clean systemd state.
        rc, out = vm.run("systemctl is-system-running --wait 2>&1; systemctl --failed --no-legend --plain", timeout=600, check=False)
        state = out.strip().splitlines()[0] if out.strip() else "?"
        failed = [l for l in out.strip().splitlines()[1:] if l.strip()]
        report("systemd state", state == "running", f"{state}; failed units: {failed or 'none'}")
        if state != "running":
            raise TestFailure(f"systemd is '{state}', failed units: {failed}")

        off = vm.shell_poweroff()
        report("clean poweroff", off)
        if not off:
            raise TestFailure("guest did not power off")
    print(f"PASS  live boot {args.arch}/{args.firmware}/{args.media} in {time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--arch", default="amd64", choices=["amd64", "arm64"])
    ap.add_argument("--firmware", default="uefi", choices=["uefi", "uefi-secureboot", "bios"])
    ap.add_argument("--media", default="cdrom", choices=["cdrom", "usb"])
    ap.add_argument("--user", default="nexus")
    ap.add_argument("--password", default="live")
    ap.add_argument("--memory", type=int, default=4096)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--workdir", default=None)
    a = ap.parse_args()
    main_guard(lambda: run(a))
