# SPDX-License-Identifier: GPL-3.0-or-later
"""SHADOW Recovery engine: find, mount and repair an installed Shadow OS.

A root command-line tool (Python standard library only) driven by the
graphical front-end shadow-recovery through pkexec, or by hand:

    shadow-recovery-engine scan
    shadow-recovery-engine unlock --device /dev/sda3        (passphrase on stdin)
    shadow-recovery-engine mount --target <id>
    shadow-recovery-engine repair-boot --target <id> [--dry-run]
    shadow-recovery-engine repair-packages --target <id> [--dry-run]
    shadow-recovery-engine reset-password --target <id> --user NAME   (password on stdin)
    shadow-recovery-engine logs --target <id> --output FILE.tar.gz
    shadow-recovery-engine shell --target <id> [--exec]
    shadow-recovery-engine umount --target <id>
    shadow-recovery-engine fsck --target <id> [--device DEV] [--repair]
    shadow-recovery-engine lock [--device DEV]

Output: one JSON object per line on stdout; the last line has
"event": "result" and "ok": true|false. Rules the code keeps to:

  * a target is always named by the id that "scan" reported and looked up
    again before anything is done to it; nothing outside it is touched;
  * everything that changes something is its own sub-command, and is written
    to the log file with the exact commands that were run;
  * what needs an unmounted file system refuses to work on a mounted one,
    the running system's root above all;
  * external programs get an argument vector, never a shell; secrets arrive
    on stdin and are never logged.

It does not depend on the installer engine (package shadow-installer exists
on the live medium only).
"""

__version__ = "1.0"
