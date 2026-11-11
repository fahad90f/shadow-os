# SPDX-License-Identifier: GPL-3.0-or-later
"""Shadow OS installer engine.

The engine is a root-only, non-interactive program with three verbs:

    probe    describe the machine (disks, firmware, memory, live medium) as JSON
    plan     validate an answer file and print *exactly* what would be done,
             in order, including every destructive operation — touches nothing
    install  carry out a plan; requires the SHA-256 of the plan the caller
             obtained from "plan", so nothing is executed that was not shown

The graphical installer is only a front-end: it turns the user's choices into
an answer file, shows the plan for confirmation and then runs "install",
rendering the JSON progress events. Unattended installation uses the same
three verbs, which is also how the engine is tested end to end in QEMU.
"""

__version__ = "0.1.0"
ANSWERS_VERSION = 1
