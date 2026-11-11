# SPDX-License-Identifier: GPL-3.0-or-later
"""Command-line entry point: shadow-installer-engine {probe,plan,install}."""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

from . import __version__, answers as answers_mod, plan as plan_mod, probe as probe_mod
from .executor import Executor
from .util import LOG_DIR, InstallError, Reporter, Runner

EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_PLAN_CHANGED = 0, 1, 2, 3


def _load_answers(path: str) -> dict:
    try:
        text = sys.stdin.read() if path == "-" else open(path, encoding="utf-8").read()
        data = json.loads(text)
    except OSError as exc:
        raise InstallError(f"Cannot read the answer file: {exc}")
    except json.JSONDecodeError as exc:
        raise InstallError(f"The answer file is not valid JSON: {exc}")
    if not isinstance(data, dict):
        raise InstallError("The answer file must contain a JSON object.")
    return data


def _need_root():
    if os.geteuid() != 0:
        raise InstallError("This command must run as root.")


def cmd_probe(args) -> int:
    _need_root()
    json.dump(probe_mod.probe(with_other_systems=args.other_systems), sys.stdout, indent=2)
    sys.stdout.write("\n")
    return EXIT_OK


def cmd_plan(args) -> int:
    _need_root()
    parsed = answers_mod.parse(_load_answers(args.answers))
    plan = plan_mod.build(parsed, probe_mod.probe())
    if args.text:
        sys.stdout.write(plan_mod.render_text(plan))
        sys.stdout.write(f"\nTo carry out exactly this plan:\n  shadow-installer-engine install "
                         f"--answers {args.answers} --confirm {plan['hash']}\n")
    else:
        json.dump(plan, sys.stdout, indent=2)
        sys.stdout.write("\n")
    return EXIT_OK


def cmd_install(args) -> int:
    _need_root()
    log_path = LOG_DIR / time.strftime("install-%Y%m%d-%H%M%S.log")
    reporter = Reporter(sys.stdout, log_path)
    reporter.log(f"shadow-installer-engine {__version__} starting")
    executor = None
    try:
        parsed = answers_mod.parse(_load_answers(args.answers))
        report = probe_mod.probe()
        plan = plan_mod.build(parsed, report)
        if plan["hash"] != args.confirm:
            reporter.event("error", code="plan-changed",
                           message="The installation plan differs from the one that was confirmed.",
                           hint="The disks changed since the plan was shown (a device was plugged in or "
                                "removed, or a partition was mounted). Review the plan again.",
                           expected=args.confirm, actual=plan["hash"], log=str(log_path))
            return EXIT_PLAN_CHANGED
        reporter.log("PLAN " + json.dumps(plan, sort_keys=True))
        executor = Executor(parsed, plan, report, reporter, Runner(reporter))
        executor.execute()
        return EXIT_OK
    except InstallError as exc:
        reporter.event("error", code="failed", message=str(exc), hint=exc.hint, log=str(log_path))
    except Exception as exc:  # noqa: BLE001 — report unexpected failures instead of a traceback
        import traceback
        reporter.log(traceback.format_exc())
        reporter.event("error", code="internal", message=f"Unexpected installer error: {exc!r}",
                       hint="This is a bug in the installer. Please report it with the log file.",
                       log=str(log_path))
    if executor is not None:
        executor.abort_cleanup()
    reporter.close()
    return EXIT_FAILED


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="shadow-installer-engine", description=__doc__)
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("probe", help="describe disks, firmware and requirements as JSON")
    p.add_argument("--other-systems", action="store_true",
                   help="also scan for installed operating systems (slow; mounts filesystems read-only)")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("plan", help="validate an answer file and show what would be done")
    p.add_argument("--answers", required=True, help="answer file (JSON), or '-' for standard input")
    p.add_argument("--text", action="store_true", help="human-readable output instead of JSON")
    p.set_defaults(func=cmd_plan)

    p = sub.add_parser("install", help="carry out a previously shown plan")
    p.add_argument("--answers", required=True, help="answer file (JSON), or '-' for standard input")
    p.add_argument("--confirm", required=True, metavar="PLAN-HASH",
                   help="the 'hash' printed by the plan command for these answers")
    p.set_defaults(func=cmd_install)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except InstallError as exc:
        payload = {"event": "error", "code": "invalid", "message": str(exc), "hint": exc.hint}
        sys.stdout.write(json.dumps(payload) + "\n")
        sys.stderr.write(f"shadow-installer-engine: {exc}\n" + (f"  {exc.hint}\n" if exc.hint else ""))
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
