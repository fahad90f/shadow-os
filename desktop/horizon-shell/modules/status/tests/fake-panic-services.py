#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-ins for the services the shell's panic action talks to, for the
# nested session test (session-test-killswitch.sh). They follow the services'
# published D-Bus interfaces; they are not the services, and nothing here
# touches a network.
#
#   fake-panic-services.py --control FIFO --log FILE --run-dir DIR [--screensaver]
#
#   "system" bus (DBUS_SYSTEM_BUS_ADDRESS must name a PRIVATE bus; refused
#   otherwise):
#     org.freedesktop.systemd1        Manager.StartUnit — for shadow-panic.service
#                                     it plays the unit: writes DIR/panic.json
#                                     the way "shadow-panic engage" does
#     org.freedesktop.NetworkManager  Enable(b)
#     org.freedesktop.login1          /org/freedesktop/login1/session/auto Session.Lock
#   session bus (--screensaver, only if the name is free):
#     org.freedesktop.ScreenSaver     /ScreenSaver Lock
#
# Commands, one per line, written to the FIFO:
#     systemd accept|silent|deny     silent: accept the job, never write panic.json
#     nm accept|deny
#     logind accept|deny
#     screensaver accept|deny
# Every call is logged ("call ..."), with whether the caller allowed
# interactive authorisation (a panic must not).
import argparse
import datetime
import json
import os
import sys
import tempfile

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

SYSTEMD_XML = """<node>
 <interface name="org.freedesktop.systemd1.Manager">
  <method name="StartUnit">
   <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/>
  </method>
 </interface>
</node>"""
NM_XML = """<node>
 <interface name="org.freedesktop.NetworkManager">
  <method name="Enable"><arg type="b" direction="in"/></method>
 </interface>
</node>"""
SESSION_XML = """<node>
 <interface name="org.freedesktop.login1.Session">
  <method name="Lock"/>
 </interface>
</node>"""
SCREENSAVER_XML = """<node>
 <interface name="org.freedesktop.ScreenSaver">
  <method name="Lock"/>
 </interface>
</node>"""


def write_atomic(path, obj):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".panic.")
    with os.fdopen(fd, "w") as fh:
        json.dump(obj, fh, indent=2, sort_keys=True)
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class Fakes:
    def __init__(self, log_path, run_dir, screensaver):
        self.log_file = open(log_path, "a", buffering=1)
        self.run_dir = run_dir
        self.mode = {"systemd": "accept", "nm": "accept", "logind": "accept", "screensaver": "accept"}
        self.jobs = 0

        address = os.environ.get("DBUS_SYSTEM_BUS_ADDRESS", "")
        if not address or "/run/dbus/system_bus_socket" in address or "/var/run/dbus" in address:
            sys.exit("fake-panic-services: DBUS_SYSTEM_BUS_ADDRESS must name a private test bus")
        flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        self.system = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        self.export(self.system, "/org/freedesktop/systemd1", SYSTEMD_XML, self.systemd_call)
        self.export(self.system, "/org/freedesktop/NetworkManager", NM_XML, self.nm_call)
        self.export(self.system, "/org/freedesktop/login1/session/auto", SESSION_XML, self.logind_call)
        for name in ("org.freedesktop.systemd1", "org.freedesktop.NetworkManager", "org.freedesktop.login1"):
            if not self.own(self.system, name):
                sys.exit("fake-panic-services: %s is already owned on the test bus" % name)
        self.session = None
        if screensaver:
            self.session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            self.export(self.session, "/ScreenSaver", SCREENSAVER_XML, self.screensaver_call)
            if self.own(self.session, "org.freedesktop.ScreenSaver"):
                self.log("screensaver: owned by the fake")
            else:
                self.log("screensaver: already owned by another program; not faked")

    def log(self, text):
        self.log_file.write(text + "\n")

    def export(self, connection, path, xml, call):
        interface = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]

        def on_call(conn, sender, object_path, interface_name, method, parameters, invocation):
            interactive = bool(invocation.get_message().get_flags()
                               & Gio.DBusMessageFlags.ALLOW_INTERACTIVE_AUTHORIZATION)
            call(method, parameters.unpack(), invocation, interactive)

        connection.register_object(path, interface, on_call, None, None)

    def own(self, connection, name):
        reply = connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                     "RequestName", GLib.Variant("(su)", (name, 4)), None,
                                     Gio.DBusCallFlags.NONE, -1, None)
        return reply.unpack()[0] == 1

    # ---- systemd: StartUnit plays shadow-panic.service ------------------------------------
    def systemd_call(self, method, args, invocation, interactive):
        unit, mode = args
        self.log("call systemd StartUnit %s %s interactive=%s" % (unit, mode, str(interactive).lower()))
        if self.mode["systemd"] == "deny":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired",
                                         "Interactive authentication required.")
            return
        if unit != "shadow-panic.service":
            invocation.return_dbus_error("org.freedesktop.systemd1.NoSuchUnit", "Unit %s not found." % unit)
            return
        self.jobs += 1
        invocation.return_value(GLib.Variant("(o)", ("/org/freedesktop/systemd1/job/%d" % self.jobs,)))
        if self.mode["systemd"] == "accept":
            # What "shadow-panic engage" publishes, a moment later.
            GLib.timeout_add(400, self.engage)

    def engage(self):
        write_atomic(os.path.join(self.run_dir, "panic.json"), {
            "version": 1, "engaged": True, "loaded": True, "error": None, "since": now_iso(),
            "steps": {"table": True, "conntrack_flushed": True, "sockets_closed": True, "networking_off": True,
                      "rfkill_blocked": 0, "sessions_locked": True},
            "undo": {"networking_was_enabled": True, "rfkill": []}, "warnings": [],
            "clear_clipboard": True, "lock_session": True, "dropped_packets": 0, "updated": now_iso()})
        self.log("unit wrote panic.json engaged")
        return False

    # ---- NetworkManager ------------------------------------------------------------------
    def nm_call(self, method, args, invocation, interactive):
        self.log("call nm Enable %s interactive=%s" % (str(args[0]).lower(), str(interactive).lower()))
        if self.mode["nm"] == "deny":
            invocation.return_dbus_error("org.freedesktop.NetworkManager.PermissionDenied", "Not authorized to enable/disable networking")
        else:
            invocation.return_value(None)

    # ---- locks -----------------------------------------------------------------------------
    def logind_call(self, method, args, invocation, interactive):
        self.log("call logind Session.Lock interactive=%s" % str(interactive).lower())
        if self.mode["logind"] == "deny":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "Access denied")
        else:
            invocation.return_value(None)

    def screensaver_call(self, method, args, invocation, interactive):
        self.log("call screensaver Lock")
        if self.mode["screensaver"] == "deny":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "the screen locker is not running")
        else:
            invocation.return_value(None)

    # ---- control ---------------------------------------------------------------------------
    def command(self, line):
        words = line.split()
        if not words:
            return
        self.log("control " + line)
        if len(words) == 2 and words[0] in self.mode:
            self.mode[words[0]] = words[1]
        else:
            self.log("control: unknown command")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--screensaver", action="store_true")
    options = parser.parse_args()
    fakes = Fakes(options.log, options.run_dir, options.screensaver)

    fd = os.open(options.control, os.O_RDWR | os.O_NONBLOCK)
    pending = [b""]

    def readable(source, condition):
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            return True
        pending[0] += data
        while b"\n" in pending[0]:
            line, pending[0] = pending[0].split(b"\n", 1)
            fakes.command(line.decode())
        return True

    GLib.io_add_watch(fd, GLib.IO_IN, readable)
    fakes.log("ready")
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
