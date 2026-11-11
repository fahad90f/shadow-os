#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A status notifier item written against the bare protocol (no library), the
# way libappindicator-style clients behave: it registers with an object path
# instead of a bus name, names its icon through IconThemePath, exports no
# menu ("/NO_DBUSMENU") and changes its icon while running.
#
#   minimal-item.py THEME_DIR SECONDS
#
# THEME_DIR must contain nexus-test-one.png and nexus-test-two.png. Prints an
# "item ..." line for every call it receives. Needs python3-gi; nested test
# session only.
import sys

from gi.repository import Gio, GLib

PATH = "/org/ayatana/NotificationItem/nexus_test"
IFACE = "org.kde.StatusNotifierItem"
XML = """<node><interface name='org.kde.StatusNotifierItem'>
<property name='Id' type='s' access='read'/>
<property name='Title' type='s' access='read'/>
<property name='Category' type='s' access='read'/>
<property name='Status' type='s' access='read'/>
<property name='IconName' type='s' access='read'/>
<property name='IconThemePath' type='s' access='read'/>
<property name='Menu' type='o' access='read'/>
<property name='ItemIsMenu' type='b' access='read'/>
<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>
<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>
<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>
<signal name='NewIcon'/>
</interface></node>"""

state = {"icon": "nexus-test-one"}


def out(*words):
    print("item", *words, flush=True)


def main():
    theme_dir, seconds = sys.argv[1], float(sys.argv[2])
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def method(_bus, _sender, _path, _iface, name, params, invocation):
        # No Activate on purpose: the tray has to cope with the error.
        out(name, *[str(v) for v in params.unpack()])
        invocation.return_value(None)

    def get(_bus, _sender, _path, _iface, name):
        values = {
            "Id": GLib.Variant("s", "nexus-minimal-item"),
            "Title": GLib.Variant("s", "Minimal item"),
            "Category": GLib.Variant("s", "ApplicationStatus"),
            "Status": GLib.Variant("s", "Active"),
            "IconName": GLib.Variant("s", state["icon"]),
            "IconThemePath": GLib.Variant("s", theme_dir),
            "Menu": GLib.Variant("o", "/NO_DBUSMENU"),
            "ItemIsMenu": GLib.Variant("b", False),
        }
        return values[name]

    bus.register_object(PATH, Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0], method, get, None)
    try:
        bus.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                      "RegisterStatusNotifierItem", GLib.Variant("(s)", (PATH,)), None,
                      Gio.DBusCallFlags.NONE, 5000, None)
        out("registered as", bus.get_unique_name() + PATH)
    except GLib.Error as error:
        out("registration failed:", error.message)

    def change():
        state["icon"] = "nexus-test-two"
        bus.emit_signal(None, PATH, IFACE, "NewIcon", None)
        out("icon changed")
        return False

    loop = GLib.MainLoop()
    GLib.timeout_add(int(seconds * 500), change)
    GLib.timeout_add(int(seconds * 1000), loop.quit)
    loop.run()
    out("leaving")


if __name__ == "__main__":
    main()
