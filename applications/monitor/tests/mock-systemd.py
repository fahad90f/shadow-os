#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for systemd's D-Bus manager, for the monitor's fixture only.

Owns org.freedesktop.systemd1 on the private "system" bus fixture.py starts
and implements the part of org.freedesktop.systemd1.Manager the monitor uses,
with the argument and reply signatures of the real interface. Unit states
live in a JSON file (argv[1]) that the UI tests read back.

  ListUnitsByPatterns, ListUnitFilesByPatterns, GetUnitFileState, Subscribe
  StartUnit / StopUnit / RestartUnit   answer with a job path; JobRemoved
                                       follows 1.2 s later
  EnableUnitFiles / DisableUnitFiles, Reload

Like systemd behind polkit, it refuses a changing call that was sent without
the "allow interactive authorisation" flag. Special units:
  denied.service     every change is refused (AccessDenied), as after a failed
                     authentication
  unstable.service   its start job ends with the result "failed"
While the file "fail-listing" exists next to the state file, ListUnitsByPatterns
fails.
"""
import fnmatch
import json
import os
import sys

from gi.repository import Gio, GLib

STATE = sys.argv[1]
JOB_DELAY_MS = 1200

XML = """
<node>
 <interface name="org.freedesktop.systemd1.Manager">
  <method name="Subscribe"/>
  <method name="Reload"/>
  <method name="ListUnitsByPatterns">
   <arg type="as" direction="in"/><arg type="as" direction="in"/>
   <arg type="a(ssssssouso)" direction="out"/>
  </method>
  <method name="ListUnitFilesByPatterns">
   <arg type="as" direction="in"/><arg type="as" direction="in"/>
   <arg type="a(ss)" direction="out"/>
  </method>
  <method name="GetUnitFileState"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
  <method name="StartUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <method name="StopUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <method name="RestartUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  <method name="EnableUnitFiles">
   <arg type="as" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/>
   <arg type="b" direction="out"/><arg type="a(sss)" direction="out"/>
  </method>
  <method name="DisableUnitFiles">
   <arg type="as" direction="in"/><arg type="b" direction="in"/>
   <arg type="a(sss)" direction="out"/>
  </method>
  <signal name="JobRemoved"><arg type="u"/><arg type="o"/><arg type="s"/><arg type="s"/></signal>
 </interface>
</node>
"""

PATH = "/org/freedesktop/systemd1"
IFACE = "org.freedesktop.systemd1.Manager"
CHANGING = {"StartUnit", "StopUnit", "RestartUnit", "EnableUnitFiles", "DisableUnitFiles", "Reload"}
job_counter = [100]


def load():
    with open(STATE) as f:
        return json.load(f)


def save(units):
    with open(STATE + ".new", "w") as f:
        json.dump(units, f, indent=1)
    os.replace(STATE + ".new", STATE)


def log(text):
    with open(os.path.dirname(STATE) + "/mock-systemd.log", "a") as f:
        f.write(text + "\n")


def matches(name, patterns):
    return not patterns or any(fnmatch.fnmatchcase(name, p) for p in patterns)


def on_call(connection, sender, path, iface, method, params, invocation):
    args = params.unpack()
    log(f"{method}{args}")
    units = load()
    interactive = bool(invocation.get_message().get_flags() & Gio.DBusMessageFlags.ALLOW_INTERACTIVE_AUTHORIZATION)
    if method in CHANGING and not interactive:
        invocation.return_dbus_error("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired",
                                     "Interactive authentication required.")
        return

    if method in ("Subscribe", "Reload"):
        invocation.return_value(None)
    elif method == "ListUnitsByPatterns":
        if os.path.exists(os.path.dirname(STATE) + "/fail-listing"):
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "Failed to list units: Connection timed out")
            return
        rows = [(n, u["description"], u["load"], u["active"], u["sub"], "",
                 PATH + "/unit/u%d" % i, 0, "", "/")
                for i, (n, u) in enumerate(units.items()) if u["load"] and matches(n, args[1])]
        invocation.return_value(GLib.Variant("(a(ssssssouso))", (rows,)))
    elif method == "ListUnitFilesByPatterns":
        rows = [("/usr/lib/systemd/system/" + n, u["file"]) for n, u in units.items()
                if u["listed"] and matches(n, args[1])]
        invocation.return_value(GLib.Variant("(a(ss))", (rows,)))
    elif method == "GetUnitFileState":
        u = units.get(args[0])
        if not u or not u["enablement"]:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.FileNotFound",
                                         "Unit file %s does not exist." % args[0])
        else:
            invocation.return_value(GLib.Variant("(s)", (u["enablement"],)))
    elif method in ("StartUnit", "StopUnit", "RestartUnit"):
        name, mode = args
        if name not in units:
            invocation.return_dbus_error("org.freedesktop.systemd1.NoSuchUnit", "Unit %s not found." % name)
            return
        if name == "denied.service":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "Access denied")
            return
        if mode != "replace":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", "unexpected job mode " + mode)
            return
        job_counter[0] += 1
        job_id = job_counter[0]
        job = "/org/freedesktop/systemd1/job/%d" % job_id
        invocation.return_value(GLib.Variant("(o)", (job,)))

        def finish():
            current = load()
            u = current[name]
            result = "done"
            if method == "StopUnit":
                u.update(active="inactive", sub="dead")
            elif name == "unstable.service":
                u.update(load="loaded", active="failed", sub="failed")
                result = "failed"
            else:
                u.update(load="loaded", active="active", sub="running")
            save(current)
            connection.emit_signal(None, PATH, IFACE, "JobRemoved", GLib.Variant("(uoss)", (job_id, job, name, result)))
            return False
        GLib.timeout_add(JOB_DELAY_MS, finish)
    elif method in ("EnableUnitFiles", "DisableUnitFiles"):
        names = args[0]
        for name in names:
            if name not in units:
                invocation.return_dbus_error("org.freedesktop.systemd1.NoSuchUnit", "Unit %s not found." % name)
                return
            if name == "denied.service":
                invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "Access denied")
                return
        enable = method == "EnableUnitFiles"
        changes = []
        for name in names:
            units[name]["file"] = "enabled" if enable else "disabled"
            changes.append(("symlink" if enable else "unlink",
                            "/etc/systemd/system/multi-user.target.wants/" + name, "/usr/lib/systemd/system/" + name))
        save(units)

        def answer():
            if enable:
                invocation.return_value(GLib.Variant("(ba(sss))", (True, changes)))
            else:
                invocation.return_value(GLib.Variant("(a(sss))", (changes,)))
            return False
        GLib.timeout_add(JOB_DELAY_MS, answer)      # as if the user were authenticating
    else:
        invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", method)


def main():
    loop = GLib.MainLoop()
    node = Gio.DBusNodeInfo.new_for_xml(XML)

    def on_bus(connection, name):
        connection.register_object(PATH, node.interfaces[0], on_call, None, None)

    def on_name(connection, name):
        with open(os.path.dirname(STATE) + "/mock-systemd.ready", "w") as f:
            f.write("ready\n")

    def on_lost(connection, name):
        sys.stderr.write("mock-systemd: could not own %s\n" % name)
        loop.quit()

    Gio.bus_own_name(Gio.BusType.SYSTEM, "org.freedesktop.systemd1", Gio.BusNameOwnerFlags.NONE,
                     on_bus, on_name, on_lost)
    loop.run()


if __name__ == "__main__":
    main()
