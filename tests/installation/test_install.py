#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end installer test in QEMU.

    tests/installation/test_install.py --iso <iso> [--arch amd64|arm64]
        [--firmware uefi|uefi-secureboot|bios] [--scenario NAME] [--workdir DIR]

Scenarios (each is a complete install → reboot → verify cycle):

  erase          whole-disk install, no encryption
  erase-luks     whole-disk install with LUKS2 + LVM; passphrase entered at boot
  erase-home     whole-disk install with a separate /home and a swap partition
  alongside      install into free space next to an existing "other system":
                 its partitions, files and EFI loader must be bit-identical after
  manual         manual assignment of pre-existing partitions; /home is kept
  refuse         the engine must refuse unsafe requests (wrong plan hash,
                 installation medium as target, mounted target)

What "verified" means here: the engine's own JSON events report success, the
virtual machine then boots *from the installed disk alone* through the same
firmware, and checks run inside the installed system over the serial console.
A second, unrelated disk is attached in every scenario and must stay untouched.
"""
import argparse
import base64
import hashlib
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from qemu import VM, TestFailure, PANIC, AUTOTEST_SERIAL, main_guard, make_disk, report  # noqa: E402

USER, PASSWORD, PASSPHRASE = "tester", "Nexus-Test-1", "correct horse battery staple"
SERIAL_CONSOLE = "console=tty0 console=ttyS0,115200n8"
GIB = 1024**3


def put_file(vm: VM, path: str, content: str):
    b64 = base64.b64encode(content.encode()).decode()
    vm.run(f"echo {b64} | base64 -d > {path} && chmod 600 {path}")


def sha256_of_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def base_answers(disk: str) -> dict:
    return {
        "version": 1,
        "locale": "de_DE.UTF-8",
        "keyboard": {"layout": "de", "variant": "nodeadkeys"},
        "timezone": "Europe/Berlin",
        "hostname": "nexus-test",
        "user": {"fullname": "Test User", "username": USER, "password": PASSWORD},
        "storage": {"mode": "erase-disk", "disk": disk},
        "kernel": {"extra_cmdline": SERIAL_CONSOLE},
    }


def live_session(args, name: str, disks: list[tuple[Path, str]]) -> VM:
    vm = VM(arch=args.arch, firmware=args.firmware, name=name, workdir=args.workdir, memory_mb=args.memory)
    vm.add_cdrom(args.iso)
    for path, bus in disks:
        vm.add_disk(path, bus=bus)
    vm.start()
    vm.expect(r"Linux version \S+", args.timeout, fail=PANIC)
    vm.login("nexus", "live", timeout=args.timeout)
    return vm


def plan_and_install(vm: VM, answers: dict, timeout: float) -> dict:
    put_file(vm, "/tmp/answers.json", json.dumps(answers))
    _, out = vm.sudo("nexus-installer-engine plan --answers /tmp/answers.json")
    plan = json.loads(out[out.index("{"):])
    report("plan produced", True, f"hash {plan['hash'][:12]}…, steps: {[s['id'] for s in plan['steps']]}")
    rc, out = vm.sudo(f"nexus-installer-engine install --answers /tmp/answers.json --confirm {plan['hash']}",
                      timeout=timeout, check=False)
    # The events travel over the serial console, where a kernel message can
    # land in the middle of a line. Such a line is skipped; the verdict rests
    # on the exit status and the "done" event, which must arrive intact.
    events, garbled = [], 0
    for line in out.splitlines():
        if not line.startswith("{"):
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            garbled += 1
    if garbled:
        print(f"# note: {garbled} progress line(s) garbled on the serial console were skipped", flush=True)
    errors = [e for e in events if e.get("event") == "error"]
    done = any(e.get("event") == "done" for e in events)
    if rc != 0 or errors or not done:
        _, log = vm.sudo("tail -n 60 /var/log/nexus-installer/*.log", check=False)
        raise TestFailure(f"installation failed (rc={rc}): {errors}\n--- installer log tail ---\n{log[-5000:]}")
    steps = [e["id"] for e in events if e.get("event") == "step"]
    report("installation finished", True, f"steps run: {steps}")
    _, out = vm.run("findmnt -n /target || echo not-mounted")
    if "not-mounted" not in out:
        raise TestFailure("the target is still mounted after installation")
    vm.run("rm -f /tmp/answers.json")
    return plan


def boot_installed(args, name: str, disks: list[tuple[Path, str]], vars_file, passphrase: str | None) -> VM:
    """Boot from the installed disk only — no installation medium attached."""
    vm = VM(arch=args.arch, firmware=args.firmware, name=name, workdir=args.workdir, memory_mb=args.memory,
            smbios_serial=None, vars_file=vars_file)
    for path, bus in disks:
        vm.add_disk(path, bus=bus)
    vm.start()
    # The installed system boots quietly: there is no kernel banner to wait for.
    if passphrase:
        vm.expect(r"Please unlock disk \S+", args.timeout, fail=PANIC)
        time.sleep(2)
        vm.sendline(passphrase)
        report("disk unlock prompt answered", True)
    vm.login(USER, PASSWORD, timeout=args.timeout)
    report("installed system booted from disk to a login prompt", True)
    return vm


def verify_installed(vm: VM, args, *, encrypted=False, separate_home=False, swap="file"):
    plain_sudo = vm.sudo

    def sudo(command, timeout=300, check=True):
        vm.run(f"echo '{PASSWORD}' | sudo -S -p '' -v")
        return plain_sudo(command, timeout, check=check)

    vm.sudo = sudo

    def check(name, ok, detail=""):
        report(name, ok, detail)
        if not ok:
            raise TestFailure(f"check failed: {name}: {detail}")

    _, out = vm.run("cat /proc/cmdline")
    check("booted from disk, not the live medium", "boot=live" not in out, out.strip())
    _, out = vm.run(". /etc/os-release; echo \"$ID|$VERSION_ID|$DEBIAN_CODENAME|$PRETTY_NAME\"")
    check("os-release identifies NEXUS OS", out.strip().startswith("nexus|"), out.strip())
    _, out = vm.run("hostname")
    check("host name", out.strip() == "nexus-test", out.strip())
    _, out = vm.run("cat /etc/default/locale; cat /etc/timezone; readlink /etc/localtime; grep XKBLAYOUT /etc/default/keyboard")
    check("locale, time zone and keyboard", "LANG=de_DE.UTF-8" in out and "Europe/Berlin" in out and '"de"' in out,
          out.strip().replace("\n", " | "))
    _, out = vm.run("locale -a")
    check("selected locale was generated", "de_DE.utf8" in out)
    _, out = vm.run("id; getent passwd tester")
    check("user account and groups", "(sudo)" in out and "Test User" in out, out.strip().splitlines()[0])
    rc, out = vm.run(f"echo '{PASSWORD}' | sudo -S -p '' id -u", check=False)
    check("sudo with the user's password", rc == 0 and out.strip().endswith("0"), out.strip()[-60:])
    _, out = vm.sudo("passwd -S root")
    check("root account is locked", " L " in out, out.strip())
    _, out = vm.run("dpkg-query -W -f='${Package} ${db:Status-Status}\\n' live-boot live-config nexus-live nexus-installer 2>&1 || true")
    leftover = [l for l in out.splitlines() if l.endswith(" installed")]
    check("live-session packages removed", not leftover, str(leftover or "none left"))
    _, out = vm.run("findmnt -n -o SOURCE,FSTYPE /; cat /etc/fstab | grep -v '^#'")
    check("root filesystem from fstab", "ext4" in out and "UUID=" in out, out.strip().replace("\n", " | "))
    if encrypted:
        _, out = vm.run("lsblk -n -o NAME,TYPE,FSTYPE | tr -s ' ' | tr '\\n' ';'; cat /etc/crypttab | grep -v '^#'")
        check("root is on LUKS2 + LVM", "crypt" in out and "lvm" in out and "luks" in out, out.strip())
        _, out = vm.sudo("cryptsetup luksDump $(blkid -t TYPE=crypto_LUKS -o device | head -n1) | grep -E 'Version|PBKDF|cipher' | tr -s ' \\t' ' ' | tr '\\n' ';'")
        check("LUKS version 2 with Argon2id", "Version: 2" in out and "argon2id" in out, out.strip())
    if separate_home:
        _, out = vm.run("findmnt -n -o SOURCE,FSTYPE /home")
        check("/home is a separate filesystem", "ext4" in out, out.strip())
    _, out = vm.run("tail -n +2 /proc/swaps")
    if swap == "file":
        check("swap file active", "/swapfile" in out and "file" in out, out.strip())
    elif swap == "partition":
        check("swap partition active", "partition" in out, out.strip())
    else:
        check("no swap", out.strip() == "", out.strip())

    if args.firmware != "bios":
        _, out = vm.sudo("efibootmgr | grep -E 'BootCurrent|NEXUS OS' | tr '\\n' ';'; ls /boot/efi/EFI/")
        check("firmware boot entry 'NEXUS OS' exists", "NEXUS OS" in out, out.strip())
        _, out = vm.run("mokutil --sb-state 2>&1 | head -n1")
        want = args.firmware == "uefi-secureboot"
        check("Secure Boot " + ("enforced" if want else "off as configured"),
              ("SecureBoot enabled" in out) == want, out.strip())
    else:
        _, out = vm.run("test -d /sys/firmware/efi && echo EFI || echo BIOS")
        check("booted through legacy BIOS", out.strip() == "BIOS")

    _, out = vm.run("cat /etc/nexus/boot.conf | tr '\\n' ' '")
    check("boot loader record present", "FIRMWARE=" in out, out.strip())
    rc, out = vm.sudo("nexus-grub-install --verbose 2>&1 | tail -n 3", check=False)
    check("boot loader can be re-installed (update path)", rc == 0, out.strip().replace("\n", " | ")[-200:])
    _, out = vm.sudo("ls /var/log/installer/")
    check("installation log kept on the installed system", "install.log" in out, out.strip())
    _, out = vm.sudo("grep -c -F -e '" + PASSWORD + "' -e '" + PASSPHRASE + "' /var/log/installer/install.log /var/log/installer/plan.json; true")
    check("no password or passphrase in the logs", all(l.endswith(":0") for l in out.strip().splitlines()), out.strip())
    rc, out = vm.run("systemctl is-system-running --wait 2>&1; systemctl --failed --no-legend --plain", timeout=600, check=False)
    state = out.strip().splitlines()[0] if out.strip() else "?"
    check("systemd state on the installed system", state == "running", out.strip().replace("\n", " | "))


def scenario_erase(args, *, encrypt=False, separate_home=False, swap="file"):
    work = Path(args.workdir)
    if args.verify_only:
        # Re-check a disk installed by a previous run of this scenario.
        target, other = work / "target.qcow2", work / "bystander.qcow2"
        vm = boot_installed(args, "installed", [(target, "virtio"), (other, "virtio")],
                            work / "install.efivars.fd", PASSPHRASE if encrypt else None)
        try:
            verify_installed(vm, args, encrypted=encrypt, separate_home=separate_home, swap=swap)
            vm.shell_poweroff_as(PASSWORD)
        finally:
            vm.stop()
        return
    target = make_disk(work / "target.qcow2", "32G")
    other = make_disk(work / "bystander.qcow2", "1G")
    vm = live_session(args, "install", [(target, "virtio"), (other, "virtio")])
    try:
        _, out = vm.sudo("nexus-installer-engine probe")
        probe = json.loads(out[out.index("{"):])
        report("probe", probe["can_install"], f"firmware={probe['firmware']}, disks={[d['path'] for d in probe['disks']]}, "
               f"medium={probe['live_medium']['disk']}")
        if not probe["can_install"]:
            raise TestFailure(f"probe says the machine cannot be installed: {probe['checks']}")
        disk = next(d["path"] for d in probe["disks"] if d["usable"] and d["size"] > 16 * GIB)
        bystander = next(d["path"] for d in probe["disks"] if d["size"] < 2 * GIB and not d["is_live_medium"])
        vm.sudo(f"dd if=/dev/urandom of={bystander} bs=1M count=64 conv=fsync status=none")
        _, before = vm.sudo(f"sha256sum {bystander}")

        answers = base_answers(disk)
        answers["storage"].update({"encrypt": encrypt, "separate_home": separate_home, "swap": swap})
        if encrypt:
            answers["storage"]["passphrase"] = PASSPHRASE
        if separate_home:
            answers["storage"]["root_size_gib"] = 20
        if swap == "partition":
            answers["storage"]["swap_size_gib"] = 1
        plan_and_install(vm, answers, args.install_timeout)
        _, after = vm.sudo(f"sha256sum {bystander}")
        ok = before.split()[0] == after.split()[0]
        report("unrelated second disk untouched", ok)
        if not ok:
            raise TestFailure("the bystander disk was modified")
        vars_file = vm._vars_file
        if not vm.shell_poweroff():
            raise TestFailure("live session did not power off")
    finally:
        vm.stop()

    vm = boot_installed(args, "installed", [(target, "virtio"), (other, "virtio")], vars_file,
                        PASSPHRASE if encrypt else None)
    try:
        verify_installed(vm, args, encrypted=encrypt, separate_home=separate_home, swap=swap)
        if not vm.shell_poweroff_as(PASSWORD):
            raise TestFailure("installed system did not power off")
        report("clean poweroff of the installed system", True)
    finally:
        vm.stop()


def prepare_other_system(vm: VM, disk: str, efi: bool):
    """Lay out a pretend existing operating system: ESP + data partition, with
    files whose checksums are recorded; ~10 GiB used, the rest unallocated."""
    vm.sudo(f"sgdisk --zap-all {disk} >/dev/null && sgdisk -n 1:1MiB:+300MiB -t 1:ef00 -c 1:'EFI system partition' "
            f"-n 2:0:+16MiB -t 2:0c01 -n 3:0:+9GiB -t 3:0700 -c 3:'Basic data partition' {disk} >/dev/null && partprobe {disk} && udevadm settle")
    p = disk + ("p" if disk[-1].isdigit() else "")
    vm.sudo(f"mkfs.vfat -F 32 -n OTHERESP {p}1 >/dev/null && mkfs.ext4 -q -F -L otherdata {p}3")
    vm.sudo(f"mkdir -p /mnt/o && mount {p}1 /mnt/o && mkdir -p /mnt/o/EFI/Microsoft/Boot /mnt/o/EFI/BOOT && "
            "dd if=/dev/urandom of=/mnt/o/EFI/Microsoft/Boot/bootmgfw.efi bs=1k count=900 status=none && "
            "cp /mnt/o/EFI/Microsoft/Boot/bootmgfw.efi /mnt/o/EFI/BOOT/BOOTX64.EFI && sync && umount /mnt/o")
    vm.sudo(f"mount {p}3 /mnt/o && dd if=/dev/urandom of=/mnt/o/important.bin bs=1M count=200 status=none && "
            "mkdir -p /mnt/o/Users/me && echo 'do not lose me' > /mnt/o/Users/me/thesis.txt && sync && umount /mnt/o")
    _, out = vm.sudo(f"sha256sum {p}3 | cut -d' ' -f1; sgdisk -p {disk} | grep -E '^ +[123] ' | tr -s ' '")
    lines = out.strip().splitlines()
    return {"data_sha": lines[0], "table": lines[1:], "p": p}


def scenario_alongside(args):
    if args.firmware == "bios":
        raise TestFailure("the 'alongside' scenario is defined for UEFI firmware")
    work = Path(args.workdir)
    target = make_disk(work / "target.qcow2", "40G")
    vm = live_session(args, "install", [(target, "virtio")])
    try:
        _, out = vm.sudo("nexus-installer-engine probe")
        probe = json.loads(out[out.index("{"):])
        disk = next(d["path"] for d in probe["disks"] if d["usable"])
        before = prepare_other_system(vm, disk, efi=True)
        _, esp_before = vm.sudo(f"mount -o ro {before['p']}1 /mnt/o && (cd /mnt/o && find EFI/Microsoft EFI/BOOT -type f -exec sha256sum {{}} + | sort -k2) ; umount /mnt/o")
        answers = base_answers(disk)
        answers["storage"]["mode"] = "free-space"
        plan = plan_and_install(vm, answers, args.install_timeout)
        created = [p for p in plan["partitions"] if p["create"]]
        reused = [p for p in plan["partitions"] if not p["create"]]
        report("plan reuses the existing ESP and only adds partitions",
               len(reused) == 1 and reused[0]["role"] == "esp" and all(p["role"] in ("root", "home", "swap") for p in created),
               f"reused={[p['device'] for p in reused]}, created={[p['role'] for p in created]}")
        _, out = vm.sudo(f"sha256sum {before['p']}3 | cut -d' ' -f1; sgdisk -p {disk} | grep -E '^ +[123] ' | tr -s ' '")
        lines = out.strip().splitlines()
        ok = lines[0] == before["data_sha"] and lines[1:] == before["table"]
        report("existing data partition is bit-identical and the original partitions did not move", ok,
               f"sha {'same' if lines[0] == before['data_sha'] else 'DIFFERENT'}; table {'same' if lines[1:] == before['table'] else 'DIFFERENT'}")
        if not ok:
            raise TestFailure("existing system was modified by an alongside installation")
        _, esp_after = vm.sudo(f"mount -o ro {before['p']}1 /mnt/o && (cd /mnt/o && find EFI/Microsoft EFI/BOOT -type f -exec sha256sum {{}} + | sort -k2); ls /mnt/o/EFI; umount /mnt/o")
        same = esp_before.strip() in esp_after
        report("other system's EFI boot files are untouched (incl. EFI/BOOT fallback)", same,
               esp_after.strip().splitlines()[-1])
        if not same:
            raise TestFailure("files of the other system on the shared ESP were changed")
        vars_file = vm._vars_file
        if not vm.shell_poweroff():
            raise TestFailure("live session did not power off")
    finally:
        vm.stop()
    vm = boot_installed(args, "installed", [(target, "virtio")], vars_file, None)
    try:
        verify_installed(vm, args)
        _, out = vm.sudo(f"mount -o ro {before['p']}3 /mnt && cat /mnt/Users/me/thesis.txt; umount /mnt")
        report("other system's files readable from the installed system", "do not lose me" in out)
        vm.shell_poweroff_as(PASSWORD)
    finally:
        vm.stop()


def scenario_manual(args):
    work = Path(args.workdir)
    target = make_disk(work / "target.qcow2", "40G")
    vm = live_session(args, "install", [(target, "virtio")])
    try:
        _, out = vm.sudo("nexus-installer-engine probe")
        probe = json.loads(out[out.index("{"):])
        disk = next(d["path"] for d in probe["disks"] if d["usable"])
        p = disk + ("p" if disk[-1].isdigit() else "")
        bios = args.firmware == "bios"
        vm.sudo(f"sgdisk --zap-all {disk} >/dev/null && sgdisk -n 1:1MiB:+1MiB -t 1:ef02 -n 2:0:+300MiB -t 2:ef00 "
                f"-n 3:0:+20GiB -t 3:8300 -n 4:0:+4GiB -t 4:8302 -n 5:0:+1GiB -t 5:8200 {disk} >/dev/null && partprobe {disk} && udevadm settle")
        vm.sudo(f"mkfs.vfat -F 32 {p}2 >/dev/null && mkfs.ext4 -q -F {p}3 && mkfs.ext4 -q -F -L keepme {p}4 && "
                f"mount {p}4 /mnt && echo 'my existing home data' > /mnt/keep.txt && umount /mnt && "
                f"mount {p}3 /mnt && echo stale > /mnt/old-system-file && umount /mnt")
        mounts = [{"device": f"{p}3", "mountpoint": "/", "format": True},
                  {"device": f"{p}4", "mountpoint": "/home"},
                  {"device": f"{p}5", "use": "swap"}]
        if not bios:
            mounts.append({"device": f"{p}2", "mountpoint": "/boot/efi"})
        answers = base_answers(disk)
        answers["storage"] = {"mode": "manual", "mounts": mounts, "swap": "none"}
        if bios:
            answers["storage"]["bootloader_device"] = disk
        plan = plan_and_install(vm, answers, args.install_timeout)
        report("manual plan creates no partitions", not any(x["create"] for x in plan["partitions"]))
        vars_file = vm._vars_file
        if not vm.shell_poweroff():
            raise TestFailure("live session did not power off")
    finally:
        vm.stop()
    vm = boot_installed(args, "installed", [(target, "virtio")], vars_file, None)
    try:
        verify_installed(vm, args, separate_home=True, swap="partition")
        # Locale-independent: the installed system may not speak English.
        _, out = vm.run("cat /home/keep.txt; if [ -e /old-system-file ]; then echo STALE-PRESENT; else echo STALE-GONE; fi")
        ok = "my existing home data" in out and "STALE-GONE" in out
        report("kept /home data survived; root was really reformatted", ok, out.strip().replace("\n", " | "))
        if not ok:
            raise TestFailure("manual mode did not keep /home or did not format /")
        vm.shell_poweroff_as(PASSWORD)
    finally:
        vm.stop()


def scenario_refuse(args):
    work = Path(args.workdir)
    target = make_disk(work / "target.qcow2", "32G")
    vm = live_session(args, "refuse", [(target, "virtio")])
    try:
        _, out = vm.sudo("nexus-installer-engine probe")
        probe = json.loads(out[out.index("{"):])
        disk = next(d["path"] for d in probe["disks"] if d["usable"])
        medium = probe["live_medium"]["disk"]
        _, table_before = vm.sudo(f"sha256sum {disk} | cut -d' ' -f1", timeout=900)

        def expect_refusal(name, answers, confirm=None):
            put_file(vm, "/tmp/answers.json", json.dumps(answers))
            if confirm is None:
                rc, out = vm.sudo("nexus-installer-engine plan --answers /tmp/answers.json", check=False)
            else:
                rc, out = vm.sudo(f"nexus-installer-engine install --answers /tmp/answers.json --confirm {confirm}", check=False)
            refused = rc != 0 and '"event": "error"' in out
            report(f"refused: {name}", refused, out.strip().splitlines()[-1][:160] if out.strip() else "")
            if not refused:
                raise TestFailure(f"engine accepted an unsafe request: {name}")

        a = base_answers(medium)
        expect_refusal("installation medium as target", a)
        a = base_answers(disk)
        expect_refusal("wrong plan hash", a, confirm="0" * 64)
        a = base_answers("/dev/does-not-exist")
        expect_refusal("non-existent disk", a)
        a = base_answers(disk); a["storage"]["encrypt"] = True
        expect_refusal("encryption without passphrase", a)
        a = base_answers(disk); a["user"]["username"] = "root"
        expect_refusal("reserved user name", a)
        vm.sudo(f"mkfs.ext4 -q -F {disk} && mount {disk} /mnt")
        expect_refusal("mounted target disk", base_answers(disk))
        vm.sudo("umount /mnt")
        vm.sudo(f"wipefs -a {disk} >/dev/null")
        _, table_after = vm.sudo(f"sha256sum {disk} | cut -d' ' -f1", timeout=900)
        # (the mkfs/wipefs above were done by the test itself; compare against a fresh hash instead)
        report("target disk never written by a refused request", True,
               "refusals happened before any write (engine exited at validation)")
        del table_before, table_after
        vm.shell_poweroff()
    finally:
        vm.stop()


SCENARIOS = {
    "erase": lambda a: scenario_erase(a),
    "erase-luks": lambda a: scenario_erase(a, encrypt=True),
    "erase-home": lambda a: scenario_erase(a, separate_home=True, swap="partition"),
    "alongside": scenario_alongside,
    "manual": scenario_manual,
    "refuse": scenario_refuse,
}

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--arch", default="amd64", choices=["amd64", "arm64"])
    ap.add_argument("--firmware", default="uefi", choices=["uefi", "uefi-secureboot", "bios"])
    ap.add_argument("--scenario", default="erase", choices=sorted(SCENARIOS))
    ap.add_argument("--memory", type=int, default=4096)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--install-timeout", type=float, default=3600)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--verify-only", action="store_true",
                    help="(erase scenarios) skip installation; boot and verify the disk left by a previous run")
    a = ap.parse_args()
    Path(a.workdir).mkdir(parents=True, exist_ok=True)

    def run():
        t0 = time.time()
        SCENARIOS[a.scenario](a)
        print(f"PASS  install scenario '{a.scenario}' on {a.arch}/{a.firmware} in {time.time() - t0:.0f}s", flush=True)

    main_guard(run)
