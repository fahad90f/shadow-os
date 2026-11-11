# SPDX-License-Identifier: GPL-3.0-or-later
"""NEXUS Recovery engine: find, mount and repair an installed NEXUS OS.

A root command-line tool (Python standard library only) driven by the
graphical front-end nexus-recovery through pkexec, or by hand:

    nexus-recovery-engine scan
    nexus-recovery-engine unlock --device /dev/sda3        (passphrase on stdin)
    nexus-recovery-engine mount --target <id>
    nexus-recovery-engine repair-boot --target <id> [--dry-run]
    nexus-recovery-engine repair-packages --target <id> [--dry-run]
    nexus-recovery-engine reset-password --target <id> --user NAME   (password on stdin)
    nexus-recovery-engine logs --target <id> --output FILE.tar.gz
    nexus-recovery-engine shell --target <id> [--exec]
    nexus-recovery-engine umount --target <id>
    nexus-recovery-engine fsck --target <id> [--device DEV] [--repair]
    nexus-recovery-engine lock [--device DEV]

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

It does not depend on the installer engine (package nexus-installer exists
on the live medium only).
"""

__version__ = "1.0"
