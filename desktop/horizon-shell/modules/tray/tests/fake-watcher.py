#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A minimal org.kde.StatusNotifierWatcher that is not the shell, for testing
# that the tray never takes the name from another watcher and works as a
# host of it instead.
#
#   fake-watcher.py SECONDS      own the name for SECONDS, then leave
#
# Prints "watcher ..." lines. Needs python3-gi; nested test session only.
import sys

from gi.repository import Gio, GLib

NAME = "org.kde.StatusNotifierWatcher"
PATH = "/StatusNotifierWatcher"
XML = """<node><interface name='org.kde.StatusNotifierWatcher'>
<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>
<method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>
<property name='RegisteredStatusNotifierItems' type='as' access='read'/>
<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>
<property name='ProtocolVersion' type='i' access='read'/>
<signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>
<signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>
<signal name='StatusNotifierHostRegistered'/>
</interface></node>"""

items = []
hosts = []


def out(*words):
    print("watcher", *words, flush=True)


def emit(bus, name, *args):
    bus.emit_signal(None, PATH, NAME, name, GLib.Variant("(s)", args) if args else None)


def method(bus, sender, _path, _iface, name, params, invocation):
    arg = params.unpack()[0]
    if name == "RegisterStatusNotifierItem":
        item = sender + arg if arg.startswith("/") else arg + "/StatusNotifierItem"
        if item not in items:
            items.append(item)
            out("item registered", item)
            emit(bus, "StatusNotifierItemRegistered", item)
    elif name == "RegisterStatusNotifierHost":
        hosts.append(arg)
        out("host registered", arg)
        emit(bus, "StatusNotifierHostRegistered")
    invocation.return_value(None)


def get(_bus, _sender, _path, _iface, name):
    if name == "RegisteredStatusNotifierItems":
        return GLib.Variant("as", items)
    if name == "IsStatusNotifierHostRegistered":
        return GLib.Variant("b", bool(hosts))
    return GLib.Variant("i", 0)


def vanished(bus, _sender, _path, _iface, _name, params):
    name, _old, new = params.unpack()
    if new:
        return
    for item in [i for i in items if i.split("/")[0] == name]:
        items.remove(item)
        out("item unregistered", item)
        emit(bus, "StatusNotifierItemUnregistered", item)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    bus.register_object(PATH, node.interfaces[0], method, get, None)
    bus.signal_subscribe("org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", None, None,
                         Gio.DBusSignalFlags.NONE, vanished)
    Gio.bus_own_name_on_connection(bus, NAME, Gio.BusNameOwnerFlags.NONE,
                                   lambda _b, n: out("acquired", n), lambda _b, n: out("lost", n))
    loop = GLib.MainLoop()
    GLib.timeout_add(int(seconds * 1000), loop.quit)
    loop.run()
    out("leaving")


if __name__ == "__main__":
    main()
