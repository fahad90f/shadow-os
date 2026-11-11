# SPDX-License-Identifier: GPL-3.0-or-later
"""Command-line entry point of shadow-recovery-engine."""
from __future__ import annotations

import argparse
import os
import sys

from . import __doc__ as package_doc, __version__, actions, scan as scan_mod, target as target_mod
from .util import Output, RecoveryError

EXIT_OK, EXIT_FAILED, EXIT_USAGE = 0, 1, 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="shadow-recovery-engine", description=package_doc,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument("--pretty", action="store_true", help="indent the final result (for people)")
    sub = parser.add_subparsers(dest="command", required=True)

    def command(name: str, help_text: str, *, target: bool = True):
        p = sub.add_parser(name, help=help_text)
        if target:
            p.add_argument("--target", required=True, metavar="ID",
                           help="id of an installation from 'scan', or 'running' for this system")
        return p

    command("scan", "find installed systems on the local disks (changes nothing)", target=False)
    p = command("unlock", "open a LUKS volume; the passphrase is read from stdin", target=False)
    p.add_argument("--device", required=True)
    p = command("lock", "close the LUKS volumes this tool opened", target=False)
    p.add_argument("--device")
    p = command("activate-lvm", "activate an inactive LVM volume group", target=False)
    p.add_argument("--volume-group", required=True)
    p = command("mount", "assemble the installation under a private mount point")
    p.add_argument("--read-only", action="store_true")
    command("umount", "unmount what 'mount' mounted")
    p = command("repair-boot", "rebuild the initramfs, re-install GRUB, regenerate the boot menu")
    p.add_argument("--dry-run", action="store_true", help="only say which commands would run")
    p = command("repair-packages", "dpkg --configure -a and apt-get -f install in the installation")
    p.add_argument("--dry-run", action="store_true")
    p = command("fsck", "check the installation's unmounted file systems")
    p.add_argument("--device", help="only this file system")
    p.add_argument("--repair", action="store_true", help="repair what the check finds")
    p = command("reset-password", "set a user's password; the password is read from stdin")
    p.add_argument("--user", required=True)
    p = command("logs", "collect the installation's logs into a .tar.gz")
    p.add_argument("--output", required=True)
    p = command("shell", "print the command for a root shell in the installation")
    p.add_argument("--exec", dest="execute", action="store_true", help="start that shell instead")

    args = parser.parse_args(argv)
    out = Output(pretty=args.pretty)
    try:
        if os.geteuid() != 0:
            raise RecoveryError("This command must run as root.", code="usage")
        modifying = args.command not in ("scan", "shell") or getattr(args, "execute", False)
        if modifying:
            out.open_log()
        if args.command == "scan":
            result = scan_mod.scan(out)
        elif args.command == "unlock":
            result = actions.unlock(out, args.device)
        elif args.command == "lock":
            result = actions.lock(out, args.device)
        elif args.command == "activate-lvm":
            result = actions.activate_lvm(out, args.volume_group)
        elif args.command == "mount":
            result = target_mod.mount(out, args.target, args.read_only)
        elif args.command == "umount":
            result = target_mod.umount(out, args.target)
        elif args.command == "repair-boot":
            result = actions.repair_boot(out, args.target, args.dry_run)
        elif args.command == "repair-packages":
            result = actions.repair_packages(out, args.target, args.dry_run)
        elif args.command == "fsck":
            result = actions.fsck(out, args.target, args.device, args.repair)
        elif args.command == "reset-password":
            result = actions.reset_password(out, args.target, args.user)
        elif args.command == "logs":
            result = actions.collect_logs(out, args.target, args.output)
        else:
            result = actions.shell(out, args.target, args.execute)
        ok = result.pop("ok", True)
        out.result(ok, **result)
        return EXIT_OK if ok else EXIT_FAILED
    except RecoveryError as exc:
        out.result(False, error={"code": exc.code, "message": str(exc), "hint": exc.hint})
        return EXIT_USAGE if exc.code == "usage" else EXIT_FAILED
    except Exception as exc:  # noqa: BLE001 — a traceback is of no use to the front-end
        import traceback
        out.log(traceback.format_exc())
        out.result(False, error={"code": "internal", "message": f"Unexpected error: {exc!r}",
                                 "hint": "This is a bug in SHADOW Recovery. Please report it with the log file."})
        return EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main())
