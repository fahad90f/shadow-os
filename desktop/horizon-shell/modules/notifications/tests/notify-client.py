#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test client for the notification service (org.freedesktop.Notifications).
# Unlike notify-send it stays on the bus, so it hears the signals the server
# sends to the owner of a notification, and it can send what notify-send
# cannot: oversized fields, raw image data, floods.
#
#   notify-client.py send [options]        one notification; prints "id N"
#   notify-client.py flood COUNT [options] COUNT notifications as fast as possible
#   notify-client.py oversized             a series of hostile notifications
#   notify-client.py occupy SECONDS        own the service name (a competing daemon)
#   notify-client.py state                 the shell's state (JSON)
#
# Every signal received is printed as "signal <name> <args>".
# Needs python3-gi (GLib/Gio); used in the nested test session only.
import argparse
import json
import sys
import time

from gi.repository import Gio, GLib

NAME = "org.freedesktop.Notifications"
PATH = "/org/freedesktop/Notifications"
IFACE = "org.freedesktop.Notifications"


def out(*words):
    print(*words, flush=True)


class Client:
    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.bus.signal_subscribe(None, IFACE, None, PATH, None, Gio.DBusSignalFlags.NONE, self.on_signal)
        self.loop = GLib.MainLoop()
        self.quiet = False
        self.signals = {}

    def on_signal(self, _bus, _sender, _path, _iface, name, params):
        self.signals[name] = self.signals.get(name, 0) + 1
        if not self.quiet:
            out("signal", name, *[str(v) for v in params.unpack()])

    def call(self, method, params, reply=None, timeout=20000):
        return self.bus.call_sync(NAME, PATH, IFACE, method, params,
                                  GLib.VariantType(reply) if reply else None,
                                  Gio.DBusCallFlags.NONE, timeout, None)

    def notify(self, app="notify-client", replaces=0, icon="", summary="", body="", actions=(), hints=None,
               timeout=-1):
        params = GLib.Variant("(susssasa{sv}i)", (app, replaces, icon, summary, body, list(actions),
                                                  hints or {}, timeout))
        return self.call("Notify", params, "(u)").unpack()[0]

    def notify_async(self, done, app, summary, body, hints=None):
        params = GLib.Variant("(susssasa{sv}i)", (app, 0, "", summary, body, [], hints or {}, -1))
        self.bus.call(NAME, PATH, IFACE, "Notify", params, GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE,
                      60000, None, done)

    def close(self, ident):
        self.call("CloseNotification", GLib.Variant("(u)", (ident,)))

    def wait(self, seconds):
        if seconds <= 0:
            return
        GLib.timeout_add(int(seconds * 1000), self.loop.quit)
        self.loop.run()

    def state(self):
        reply = self.bus.call_sync(NAME, PATH, "org.shadow.horizon.Notifications", "GetState", None,
                                   GLib.VariantType("(s)"), Gio.DBusCallFlags.NONE, 20000, None)
        return json.loads(reply.unpack()[0])


def raw_image(width, height, alpha=True, rgb=(34, 216, 245), claim=None):
    """An image-data hint: (iiibiiay). 'claim' overrides the announced size."""
    channels = 4 if alpha else 3
    pixel = bytes(rgb) + (b"\xff" if alpha else b"")
    data = pixel * (width * height)
    w, h = claim or (width, height)
    # Built from GBytes: converting megabytes of pixels element by element
    # would take minutes in Python.
    pixels = GLib.Variant.new_from_bytes(GLib.VariantType("ay"), GLib.Bytes.new(data), True)
    return GLib.Variant.new_tuple(GLib.Variant("i", w), GLib.Variant("i", h), GLib.Variant("i", w * channels),
                                  GLib.Variant("b", alpha), GLib.Variant("i", 8), GLib.Variant("i", channels),
                                  pixels)


def hints_from(args):
    hints = {}
    if args.urgency is not None:
        hints["urgency"] = GLib.Variant("y", args.urgency)
    if args.value is not None:
        hints["value"] = GLib.Variant("i", args.value)
    if args.transient:
        hints["transient"] = GLib.Variant("b", True)
    if args.resident:
        hints["resident"] = GLib.Variant("b", True)
    if args.category:
        hints["category"] = GLib.Variant("s", args.category)
    if args.desktop_entry:
        hints["desktop-entry"] = GLib.Variant("s", args.desktop_entry)
    if args.image_path:
        hints["image-path"] = GLib.Variant("s", args.image_path)
    if args.image_data:
        side = int(args.image_data)
        hints["image-data"] = raw_image(side, side)
    return hints


def cmd_send(client, args):
    ident = client.notify(args.app, args.replaces, args.icon, args.summary, args.body, args.action or [],
                          hints_from(args), args.timeout)
    out("id", ident)
    for step in args.then or []:
        # "replace:SECONDS:SUMMARY:BODY[:VALUE]" or "close:SECONDS"
        parts = step.split(":")
        client.wait(float(parts[1]))
        if parts[0] == "replace":
            hints = hints_from(args)
            if len(parts) > 4:
                hints["value"] = GLib.Variant("i", int(parts[4]))
            again = client.notify(args.app, ident, args.icon, parts[2], parts[3], args.action or [], hints,
                                  args.timeout)
            out("replaced", ident, "->", again)
        elif parts[0] == "close":
            client.close(ident)
            out("close-requested", ident)
    client.wait(args.wait)


def cmd_flood(client, args):
    client.quiet = True
    pending = [args.count]
    ids = []
    started = time.monotonic()

    def done(bus, result):
        try:
            ids.append(bus.call_finish(result).unpack()[0])
        except GLib.Error as error:
            out("error", error.message)
        pending[0] -= 1
        if pending[0] == 0:
            client.loop.quit()

    for i in range(args.count):
        app = args.app if args.apps <= 1 else "%s %d" % (args.app, i % args.apps)
        hints = {"urgency": GLib.Variant("y", args.urgency)} if args.urgency is not None else {}
        client.notify_async(done, app, "Flood %d" % i, "Notification <b>%d</b> of %d" % (i, args.count), hints)
    sent = time.monotonic()
    client.loop.run()
    finished = time.monotonic()
    out("flood count=%d distinct_ids=%d queued_in=%.3fs answered_in=%.3fs" % (
        args.count, len(set(ids)), sent - started, finished - started))
    # Is the service still answering promptly?
    before = time.monotonic()
    client.call("GetServerInformation", None, "(ssss)")
    out("after-flood GetServerInformation %.3fs" % (time.monotonic() - before))
    client.wait(args.wait)
    out("flood signals received:", json.dumps(client.signals, sort_keys=True))


def cmd_oversized(client, args):
    def attempt(label, **kw):
        before = time.monotonic()
        try:
            ident = client.notify(app=kw.pop("app", "Hostile client"), **kw)
            out("sent %-28s id=%d %.3fs" % (label, ident, time.monotonic() - before))
        except GLib.Error as error:
            out("sent %-28s ERROR %s" % (label, error.message))

    attempt("summary-1MB", summary="S" * (1 << 20), body="one megabyte of summary")
    attempt("body-8MB", summary="Huge body", body="word " * (8 << 20 >> 2))
    attempt("app-name-100k", app="A" * 100000, summary="Long application name", body="x")
    attempt("whitespace-4MB", summary=" \t\n" * (1 << 20) + "after white space", body="\n" * (1 << 20) + "end")
    attempt("actions-400", summary="Four hundred actions", body="only a few are shown",
            actions=[s for i in range(400) for s in ("key%d" % i, "Action number %d with a long label " % i * 4)])
    attempt("action-key-1MB", summary="Megabyte action key", body="dropped",
            actions=["k" * (1 << 20), "Big key", "ok", "Fine"])
    attempt("image-2048", summary="Image 2048x2048 (refused)", body="16 MB of pixels",
            hints={"image-data": raw_image(2048, 2048)})
    attempt("image-lying-header", summary="Image header lies (refused)", body="claims 512x512, sends 8x8",
            hints={"image-data": raw_image(8, 8, claim=(512, 512))})
    attempt("image-negative", summary="Image with negative size (refused)", body="-5 x 7",
            hints={"image-data": GLib.Variant("(iiibiiay)", (-5, 7, 20, True, 8, 4, b"\0" * 64))})
    attempt("image-1024", summary="Image 1024x1024 (accepted, scaled)", body="4 MB of pixels",
            hints={"image-data": raw_image(1024, 1024, rgb=(155, 107, 255))})
    attempt("image-remote", summary="Remote image path (never fetched)", body="http URL as image-path",
            hints={"image-path": GLib.Variant("s", "http://127.0.0.1:%d/image.png" % args.port)},
            icon="https://127.0.0.1:%d/icon.png" % args.port)
    attempt("image-device", summary="Device as image", body="/dev/zero and /proc/self/mem",
            hints={"image-path": GLib.Variant("s", "/dev/zero")}, icon="file:///proc/self/mem")
    attempt("image-smb", summary="file: URL with a host", body="file://server/share/x.png",
            hints={"image-path": GLib.Variant("s", "file://fileserver/share/x.png")})
    attempt("hints-wrong-types", summary="Hints of the wrong types", body="urgency as a string, value as text",
            hints={"urgency": GLib.Variant("s", "critical"), "value": GLib.Variant("s", "lots"),
                   "transient": GLib.Variant("s", "yes"), "image-data": GLib.Variant("s", "nope"),
                   "desktop-entry": GLib.Variant("i", 7), "category": GLib.Variant("ay", b"\xff\xfe")})
    attempt("markup-injection", summary="<b>bold?</b> <img src='http://127.0.0.1:%d/s.png'>" % args.port,
            body="<img src=\"http://127.0.0.1:%d/b.png\" alt=\"[alt text]\"/> "
                 "<a href=\"javascript:alert(1)\">js link</a> "
                 "<a href=\"file:///etc/passwd\">file link</a> "
                 "<a href=\"https://user:pw@evil.example/\">credentials link</a> "
                 "<a href=\"https://example.org/ok\">good link</a> "
                 "<style>body{background:url(http://127.0.0.1:%d/c.png)}</style>"
                 "<script>while(1){}</script> <table><tr><td>cell</td></tr></table> "
                 "<h1>heading</h1> <font size=\"72\" color=\"red\">font</font> &lt;b&gt;escaped&lt;/b&gt; "
                 "&#60;i&#62;numeric&#60;/i&#62; ‮right-to-left override" % (args.port, args.port))
    attempt("deep-nesting", summary="Deeply nested markup", body="<b>" * 5000 + "deep" + "</b>" * 5000)
    attempt("unterminated", summary="Unterminated markup", body="<a href=\"" + "x" * 20000 + " <b <i <<<< &&&& &#xFFFFFFFF; end")
    # Last, because the message bus itself may refuse a message of this size
    # and disconnect the sender.
    attempt("image-4096", summary="Image 4096x4096 (refused)", body="64 MB of pixels",
            hints={"image-data": raw_image(4096, 4096)})
    client.wait(args.wait)


def cmd_occupy(client, args):
    def acquired(_bus, name):
        out("occupy acquired", name)

    def lost(_bus, name):
        out("occupy lost", name)

    def method(_bus, _sender, _path, _iface, name, _params, invocation):
        if name == "GetServerInformation":
            invocation.return_value(GLib.Variant("(ssss)", ("other-daemon", "test", "0", "1.2")))
        elif name == "Notify":
            out("occupy got Notify")
            invocation.return_value(GLib.Variant("(u)", (777,)))
        else:
            invocation.return_value(None)

    node = Gio.DBusNodeInfo.new_for_xml(
        "<node><interface name='%s'>"
        "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
        "<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
        "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
        "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
        "<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
        "<arg type='u' direction='out'/></method></interface></node>" % IFACE)
    client.bus.register_object(PATH, node.interfaces[0], method, None, None)
    Gio.bus_own_name_on_connection(client.bus, NAME, Gio.BusNameOwnerFlags.NONE, acquired, lost)
    client.wait(args.seconds)
    out("occupy leaving")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    send = sub.add_parser("send")
    send.add_argument("--app", default="notify-client")
    send.add_argument("--summary", default="Summary")
    send.add_argument("--body", default="")
    send.add_argument("--icon", default="")
    send.add_argument("--replaces", type=int, default=0)
    send.add_argument("--action", action="append", help="key and label alternate: --action reply --action Reply")
    send.add_argument("--urgency", type=int)
    send.add_argument("--value", type=int)
    send.add_argument("--timeout", type=int, default=-1)
    send.add_argument("--transient", action="store_true")
    send.add_argument("--resident", action="store_true")
    send.add_argument("--category")
    send.add_argument("--desktop-entry")
    send.add_argument("--image-path")
    send.add_argument("--image-data", help="side of a generated square image")
    send.add_argument("--then", action="append", help="replace:SECONDS:SUMMARY:BODY[:VALUE] | close:SECONDS")
    send.add_argument("--wait", type=float, default=0, help="keep listening for this many seconds")

    flood = sub.add_parser("flood")
    flood.add_argument("count", type=int)
    flood.add_argument("--app", default="Flood")
    flood.add_argument("--apps", type=int, default=1, help="spread over this many application names")
    flood.add_argument("--urgency", type=int)
    flood.add_argument("--wait", type=float, default=0)

    oversized = sub.add_parser("oversized")
    oversized.add_argument("--port", type=int, default=9, help="local port remote-image URLs point to")
    oversized.add_argument("--wait", type=float, default=0)

    occupy = sub.add_parser("occupy")
    occupy.add_argument("seconds", type=float)

    sub.add_parser("state")

    args = parser.parse_args()
    client = Client()
    if args.command == "send":
        cmd_send(client, args)
    elif args.command == "flood":
        cmd_flood(client, args)
    elif args.command == "oversized":
        cmd_oversized(client, args)
    elif args.command == "occupy":
        cmd_occupy(client, args)
    elif args.command == "state":
        json.dump(client.state(), sys.stdout, indent=1)
        out()
    return 0


if __name__ == "__main__":
    sys.exit(main())
