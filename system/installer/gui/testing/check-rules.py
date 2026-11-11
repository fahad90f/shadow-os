#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the installer's mirrored input rules (validation.cpp) with the engine.

Feeds a list of user names, host names, full names, passwords and passphrases
to `shadow-installer --dump rules` and to the engine's own answer validation
(shadow_installer.answers.parse) and reports every disagreement.

    scripts/dev.sh exec --dir installer -- python3 \
        /src/system/installer/gui/testing/check-rules.py /build/installer/bin/shadow-installer

The front-end is allowed to be stricter in exactly one way: it also rejects a
user name that exists as a system account or group on the running system
(validation.h, existingAccountProblem); those cases are listed, not failed. The same goes for names
that end in a line break, which the engine's regular expressions accept by
accident (Python's "$").
"""
import grp
import json
import os
import pwd
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "engine"))
from shadow_installer import answers as A  # noqa: E402
from shadow_installer.util import InstallError  # noqa: E402

USERNAMES = sorted(A.RESERVED_USERS) + [
    "ada", "a", "_svc", "a-b_c9", "a" * 32, "a" * 33, "Ada", "9lives", "-dash", "ada lovelace", "ädä", "ada.l",
    "", "user$", "x_", "tty", "disk", "shadow", "ada\n",
]
HOSTNAMES = ["shadow", "a", "a-b", "-a", "a-", "A", "a_b", "a.b", "a" * 63, "a" * 64, "", "9", "shadow-01", "ho st", "a\n"]
FULLNAMES = ["", "Ada Lovelace", "A:B", "A,B", "A=B", "Zoë Ångström", "x" * 120, "x" * 121, "a\nb", "名前"]
PASSPHRASES = ["", "short", "12345678", "1234567", "éèàùçâê", "éèàùçâêô", "with\nbreak", "correct horse battery staple", "😀" * 8, "😀" * 7]
PASSWORDS = ["", "x", "with\nbreak", "with\rbreak", " ", "pässword"]


def engine_accepts(**override) -> bool:
    doc = {"version": 1, "user": {"username": "ada", "password": "x"},
           "storage": {"mode": "erase-disk", "disk": "/dev/sda"}}
    for key, value in override.items():
        section, _, field = key.partition("__")
        if field:
            doc.setdefault(section, {})[field] = value
        else:
            doc[section] = value
    try:
        A.parse(doc, zoneinfo_dir="/nonexistent", supported_locales="/nonexistent")
        return True
    except InstallError:
        return False


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "shadow-installer"
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
        json.dump({"usernames": USERNAMES, "hostnames": HOSTNAMES, "fullnames": FULLNAMES,
                   "passphrases": PASSPHRASES, "passwords": PASSWORDS}, fh)
    try:
        out = subprocess.run([binary, "--dump", "rules"], env=dict(os.environ, SHADOW_INSTALLER_RULES_INPUT=fh.name),
                             stdout=subprocess.PIPE, check=True).stdout
    finally:
        os.unlink(fh.name)
    gui = json.loads(out)

    def system_name(name: str) -> bool:
        try:
            if pwd.getpwnam(name).pw_uid < 1000:
                return True
        except KeyError:
            pass
        try:
            return grp.getgrnam(name).gr_gid < 1000
        except KeyError:
            return False

    cases = (
        [("usernames", v, engine_accepts(user__username=v)) for v in USERNAMES]
        + [("hostnames", v, engine_accepts(hostname=v)) for v in HOSTNAMES]
        + [("fullnames", v, engine_accepts(user__fullname=v)) for v in FULLNAMES]
        + [("passphrases", v, engine_accepts(storage__encrypt=True, storage__passphrase=v)) for v in PASSPHRASES]
        + [("passwords", v, engine_accepts(user__password=v)) for v in PASSWORDS]
    )
    bad = stricter = 0
    for kind, value, engine_ok in cases:
        gui_ok = gui[kind][value] == ""
        if gui_ok == engine_ok:
            continue
        if kind == "usernames" and engine_ok and not gui_ok and system_name(value):
            stricter += 1
            print(f"stricter (system account/group exists here): {kind} {value!r}: {gui[kind][value]}")
            continue
        if value.endswith("\n") and engine_ok and not gui_ok:
            # Python's "$" also matches before a trailing line break, so the engine's
            # USERNAME_RE/HOSTNAME_RE let "name\n" through. The front-end does not.
            stricter += 1
            print(f"stricter (engine accepts a trailing line break; reported): {kind} {value!r}")
            continue
        bad += 1
        print(f"DISAGREE {kind} {value!r}: engine {'accepts' if engine_ok else 'rejects'}, "
              f"front-end {'accepts' if gui_ok else 'rejects: ' + gui[kind][value]}")
    print(f"check-rules: {len(cases)} cases, {bad} disagreement(s), {stricter} stricter by design")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
