#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-ins for the services the power module talks to, for tests in the
# build chroot (which has no systemd, UPower, NetworkManager or — when only
# this module is built into the shell — notification daemon). They follow
# the services' published D-Bus interfaces; they are not the services.
#
#   fake-services.py --control FIFO --log FILE
#
#   "system" bus (DBUS_SYSTEM_BUS_ADDRESS must point to a PRIVATE bus; the
#   script refuses to run otherwise):
#     org.freedesktop.UPower           OnBattery, GetCriticalAction, DisplayDevice
#     org.freedesktop.login1           CanSuspend, Suspend, Inhibit, ListInhibitors,
#                                      BlockInhibited, PrepareForSleep
#     org.freedesktop.NetworkManager   State, StateChanged
#   session bus (only with --notifications):
#     org.freedesktop.Notifications    Notify, CloseNotification, ActionInvoked
#
# Commands, one per line, written to the FIFO:
#     upower on-battery true|false
#     upower critical-action HybridSleep|PowerOff|...
#     battery <percentage> <warning-level 1..5> [present: true|false]
#     logind block <colon-list or "-">       e.g. "idle", "sleep:idle"
#     logind wake                            emits PrepareForSleep(false)
#     nm state <number>                      20 disconnected, 70 connected
#     notify invoke <id> <action>            the user chose an action
# Everything a client asks for is appended to the log file.
import argparse
import os
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

UPOWER_XML = """<node>
 <interface name="org.freedesktop.UPower">
  <method name="GetCriticalAction"><arg type="s" direction="out"/></method>
  <method name="EnumerateDevices"><arg type="ao" direction="out"/></method>
  <property name="OnBattery" type="b" access="read"/>
  <property name="DaemonVersion" type="s" access="read"/>
  <property name="LidIsPresent" type="b" access="read"/>
 </interface>
</node>"""
DEVICE_XML = """<node>
 <interface name="org.freedesktop.UPower.Device">
  <property name="Type" type="u" access="read"/>
  <property name="IsPresent" type="b" access="read"/>
  <property name="Percentage" type="d" access="read"/>
  <property name="State" type="u" access="read"/>
  <property name="WarningLevel" type="u" access="read"/>
 </interface>
</node>"""
LOGIND_XML = """<node>
 <interface name="org.freedesktop.login1.Manager">
  <method name="CanSuspend"><arg type="s" direction="out"/></method>
  <method name="CanHibernate"><arg type="s" direction="out"/></method>
  <method name="CanReboot"><arg type="s" direction="out"/></method>
  <method name="CanPowerOff"><arg type="s" direction="out"/></method>
  <method name="Suspend"><arg type="b" direction="in"/></method>
  <method name="Inhibit">
   <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
   <arg type="h" direction="out"/>
  </method>
  <method name="ListInhibitors"><arg type="a(ssssuu)" direction="out"/></method>
  <property name="BlockInhibited" type="s" access="read"/>
  <property name="DelayInhibited" type="s" access="read"/>
  <signal name="PrepareForSleep"><arg type="b"/></signal>
 </interface>
</node>"""
NM_XML = """<node>
 <interface name="org.freedesktop.NetworkManager">
  <property name="State" type="u" access="read"/>
  <signal name="StateChanged"><arg type="u"/></signal>
 </interface>
</node>"""
NOTIFY_XML = """<node>
 <interface name="org.freedesktop.Notifications">
  <method name="Notify">
   <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
   <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
   <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg type="u" direction="in"/></method>
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="GetServerInformation">
   <arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/>
  </method>
  <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
  <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
 </interface>
</node>"""


class Fakes:
    def __init__(self, log_path, notifications):
        self.log_file = open(log_path, "a", buffering=1)
        self.upower = {"OnBattery": GLib.Variant("b", False), "DaemonVersion": GLib.Variant("s", "fake"),
                       "LidIsPresent": GLib.Variant("b", False)}
        self.critical_action = "HybridSleep"
        self.device = {"Type": GLib.Variant("u", 2), "IsPresent": GLib.Variant("b", True),
                       "Percentage": GLib.Variant("d", 80.0), "State": GLib.Variant("u", 4),
                       "WarningLevel": GLib.Variant("u", 1)}
        self.block = ""
        self.delay_locks = []       # (what, who, why, read end kept open)
        self.nm_state = 70
        self.next_id = 0

        address = os.environ.get("DBUS_SYSTEM_BUS_ADDRESS", "")
        if not address or "/run/dbus/system_bus_socket" in address or "/var/run/dbus" in address:
            sys.exit("fake-services: DBUS_SYSTEM_BUS_ADDRESS must name a private test bus")
        flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        self.system = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        self.export(self.system, "/org/freedesktop/UPower", UPOWER_XML, self.upower_call, lambda n: self.upower[n])
        self.export(self.system, "/org/freedesktop/UPower/devices/DisplayDevice", DEVICE_XML, None, lambda n: self.device[n])
        self.export(self.system, "/org/freedesktop/login1", LOGIND_XML, self.logind_call, self.logind_property)
        self.export(self.system, "/org/freedesktop/NetworkManager", NM_XML, None,
                    lambda n: GLib.Variant("u", self.nm_state))
        for name in ("org.freedesktop.UPower", "org.freedesktop.login1", "org.freedesktop.NetworkManager"):
            self.own(self.system, name)
        self.session = None
        if notifications:
            self.session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            self.export(self.session, "/org/freedesktop/Notifications", NOTIFY_XML, self.notify_call, None)
            self.own(self.session, "org.freedesktop.Notifications")

    def log(self, text):
        self.log_file.write(text + "\n")

    def export(self, connection, path, xml, call, get):
        interface = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]

        def on_call(conn, sender, object_path, interface_name, method, parameters, invocation):
            call(method, parameters.unpack(), invocation)

        def on_get(conn, sender, object_path, interface_name, name):
            return get(name)

        connection.register_object(path, interface, on_call if call else None, on_get if get else None, None)

    def own(self, connection, name):
        reply = connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                     "RequestName", GLib.Variant("(su)", (name, 4)), None,
                                     Gio.DBusCallFlags.NONE, -1, None)
        if reply.unpack()[0] != 1:
            sys.exit("fake-services: %s is already owned" % name)

    def changed(self, connection, path, interface, values):
        connection.emit_signal(None, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                               GLib.Variant("(sa{sv}as)", (interface, values, [])))

    # ---- UPower ------------------------------------------------------------
    def upower_call(self, method, args, invocation):
        if method == "GetCriticalAction":
            invocation.return_value(GLib.Variant("(s)", (self.critical_action,)))
        else:
            invocation.return_value(GLib.Variant("(ao)", (["/org/freedesktop/UPower/devices/DisplayDevice"],)))

    # ---- logind ------------------------------------------------------------
    def logind_property(self, name):
        if name == "BlockInhibited":
            return GLib.Variant("s", self.block)
        return GLib.Variant("s", ":".join(sorted({what for what, _, _, _ in self.delay_locks})))

    def logind_call(self, method, args, invocation):
        if method.startswith("Can"):
            invocation.return_value(GLib.Variant("(s)", ("yes" if method == "CanSuspend" else "na",)))
        elif method == "Suspend":
            if "sleep" in self.block.split(":"):
                self.log("logind Suspend refused (block inhibitor)")
                invocation.return_dbus_error("org.freedesktop.login1.OperationInProgress", "Operation inhibited")
                return
            self.log("logind Suspend interactive=%s" % args[0])
            invocation.return_value(None)
            # What logind does first: tell everybody, and wait for the delay locks.
            self.system.emit_signal(None, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                    "PrepareForSleep", GLib.Variant("(b)", (True,)))
            self.log("logind PrepareForSleep true")
        elif method == "Inhibit":
            what, who, why, mode = args
            self.log("logind Inhibit what=%s who=%s mode=%s" % (what, who, mode))
            read_end, write_end = os.pipe()
            if mode == "delay":
                self.delay_locks.append((what, who, why, read_end))
            invocation.return_value_with_unix_fd_list(GLib.Variant("(h)", (0,)), Gio.UnixFDList.new_from_array([write_end]))
        elif method == "ListInhibitors":
            locks = [(what, who, why, "delay", 0, 0) for what, who, why, _ in self.delay_locks]
            if self.block:
                locks.append((self.block, "fake-services", "test inhibitor", "block", 0, os.getpid()))
            invocation.return_value(GLib.Variant("(a(ssssuu))", (locks,)))

    # ---- notifications -----------------------------------------------------
    def notify_call(self, method, args, invocation):
        if method == "Notify":
            app, replaces, icon, summary, body, actions, hints, timeout = args
            if replaces:
                ident = replaces
            else:
                self.next_id += 1
                ident = self.next_id
            self.log("notify id=%d replaces=%d app=%r icon=%r urgency=%s timeout=%d actions=%s summary=%r body=%r"
                     % (ident, replaces, app, icon, hints.get("urgency"), timeout, "|".join(actions), summary, body))
            invocation.return_value(GLib.Variant("(u)", (ident,)))
        elif method == "CloseNotification":
            self.log("notify close id=%d" % args[0])
            invocation.return_value(None)
            self.session.emit_signal(None, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                     "NotificationClosed", GLib.Variant("(uu)", (args[0], 3)))
        elif method == "GetCapabilities":
            invocation.return_value(GLib.Variant("(as)", (["actions", "body"],)))
        else:
            invocation.return_value(GLib.Variant("(ssss)", ("fake-services", "NEXUS tests", "1", "1.2")))

    # ---- control -----------------------------------------------------------
    def command(self, line):
        words = line.split()
        if not words:
            return
        self.log("control " + line)
        if words[:2] == ["upower", "on-battery"]:
            self.upower["OnBattery"] = GLib.Variant("b", words[2] == "true")
            self.changed(self.system, "/org/freedesktop/UPower", "org.freedesktop.UPower",
                         {"OnBattery": self.upower["OnBattery"]})
        elif words[:2] == ["upower", "critical-action"]:
            self.critical_action = words[2]
        elif words[0] == "battery":
            self.device["Percentage"] = GLib.Variant("d", float(words[1]))
            self.device["WarningLevel"] = GLib.Variant("u", int(words[2]))
            self.device["State"] = GLib.Variant("u", 2 if int(words[2]) > 1 else 4)
            if len(words) > 3:
                self.device["IsPresent"] = GLib.Variant("b", words[3] == "true")
            self.changed(self.system, "/org/freedesktop/UPower/devices/DisplayDevice", "org.freedesktop.UPower.Device",
                         dict(self.device))
        elif words[:2] == ["logind", "block"]:
            self.block = "" if words[2] == "-" else words[2]
            self.changed(self.system, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                         {"BlockInhibited": GLib.Variant("s", self.block)})
        elif words[:2] == ["logind", "wake"]:
            self.system.emit_signal(None, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                    "PrepareForSleep", GLib.Variant("(b)", (False,)))
        elif words[:2] == ["nm", "state"]:
            self.nm_state = int(words[2])
            self.system.emit_signal(None, "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
                                    "StateChanged", GLib.Variant("(u)", (self.nm_state,)))
        elif words[:2] == ["notify", "invoke"] and self.session:
            self.session.emit_signal(None, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                     "ActionInvoked", GLib.Variant("(us)", (int(words[2]), words[3])))
        else:
            self.log("control: unknown command")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--notifications", action="store_true")
    options = parser.parse_args()
    fakes = Fakes(options.log, options.notifications)

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
