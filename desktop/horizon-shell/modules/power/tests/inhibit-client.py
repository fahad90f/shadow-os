#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A well-behaved application that holds an idle inhibitor, for the power
# module's session test:
#
#   inhibit-client.py screensaver   org.freedesktop.ScreenSaver.Inhibit (KWin's locker)
#   inhibit-client.py agent TYPES   org.kde.Solid.PowerManagement.PolicyAgent.AddInhibition
#
# Prints the cookie, then holds the inhibitor until SIGUSR1 (releases it
# properly and stays connected) or until it is killed (leaves the bus without
# releasing, which the service has to notice).
import signal
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
if sys.argv[1] == "screensaver":
    target = ("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver")
    cookie = bus.call_sync(*target, "Inhibit", GLib.Variant("(ss)", ("test-player", "playing a film")), None,
                           Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
    release = "UnInhibit"
else:
    target = ("org.kde.Solid.PowerManagement.PolicyAgent", "/org/kde/Solid/PowerManagement/PolicyAgent",
              "org.kde.Solid.PowerManagement.PolicyAgent")
    cookie = bus.call_sync(*target, "AddInhibition", GLib.Variant("(uss)", (int(sys.argv[2]), "test-download", "downloading")),
                           None, Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
    release = "ReleaseInhibition"
print(cookie, flush=True)


def on_usr1():
    bus.call_sync(*target, release, GLib.Variant("(u)", (cookie,)), None, Gio.DBusCallFlags.NONE, -1, None)
    print("released", flush=True)
    return False


GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, on_usr1)
GLib.MainLoop().run()
