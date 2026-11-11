#!/usr/bin/env python3
"""Keep one test VM running and drive it from separate commands.

Booting under emulation takes minutes, so debugging needs a VM that stays up
between commands:

    tests/lib/vmshell.py start --iso build/out/iso/shadow-os-<ver>-amd64.iso [--disk FILE] [--firmware uefi]
                               [--memory 4096] [--cpu MODEL] [--no-login]
    tests/lib/vmshell.py run 'systemctl --failed'          # as the live user, over the serial console
    tests/lib/vmshell.py sudo 'journalctl -b -p err'
    tests/lib/vmshell.py shot name.png                     # screenshot → build/work/shots/name.png
    tests/lib/vmshell.py key meta_l          |  type 'text'  |  mouse X Y [left|right]
    tests/lib/vmshell.py stop

"start" returns once the live user is logged in on the serial console. State
lives in build/work/tests/vmshell/.
"""
import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu import VM, TestFailure, PANIC  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / "build/work/tests/vmshell"
SOCK = STATE / "control.sock"
SHOTS = ROOT / "build/work/shots"


def serve(args):
    STATE.mkdir(parents=True, exist_ok=True)
    if SOCK.exists():
        SOCK.unlink()
    vm = VM(arch=args.arch, firmware=args.firmware, name="vmshell", workdir=str(STATE),
            memory_mb=args.memory, graphics=True)
    if args.cpu:
        vm.add_args("-cpu", args.cpu)
    if args.iso:
        vm.add_cdrom(args.iso)
    if args.disk:
        vm.add_disk(args.disk)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(SOCK))
    server.listen(4)
    status = {"ok": True, "detail": ""}
    with vm:
        try:
            if not args.no_login:
                vm.expect(r"Linux version \S+", args.timeout, fail=PANIC)
                vm.login(args.user, args.password, timeout=args.timeout)
        except TestFailure as exc:
            status = {"ok": False, "detail": str(exc)}
        (STATE / "ready.json").write_text(json.dumps(status))
        while True:
            conn, _ = server.accept()
            with conn:
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                try:
                    req = json.loads(data.decode())
                    reply = handle(vm, req)
                except TestFailure as exc:
                    reply = {"ok": False, "out": f"FAILED: {exc}"}
                except Exception as exc:  # report, keep serving
                    reply = {"ok": False, "out": f"error: {exc!r}"}
                conn.sendall(json.dumps(reply).encode() + b"\n")
                if req.get("op") == "stop":
                    break
    SOCK.unlink(missing_ok=True)


def handle(vm, req):
    op = req["op"]
    if op == "run":
        rc, out = vm.run(req["cmd"], timeout=req.get("timeout", 300), check=False)
        return {"ok": rc == 0, "rc": rc, "out": out}
    if op == "sudo":
        rc, out = vm.sudo(req["cmd"], timeout=req.get("timeout", 300), check=False)
        return {"ok": rc == 0, "rc": rc, "out": out}
    if op == "shot":
        SHOTS.mkdir(parents=True, exist_ok=True)
        path = vm.screenshot(SHOTS / req["name"])
        return {"ok": True, "out": str(path)}
    if op == "key":
        vm.send_key(*req["keys"])
        return {"ok": True, "out": ""}
    if op == "type":
        vm.type_text(req["text"])
        return {"ok": True, "out": ""}
    if op == "mouse":
        # usb-tablet takes absolute coordinates scaled to 0..32767.
        w, h = req.get("screen", [1280, 800])
        x = int(req["x"] * 32767 / w)
        y = int(req["y"] * 32767 / h)
        vm.qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": x}},
            {"type": "abs", "data": {"axis": "y", "value": y}}])
        time.sleep(0.3)
        if req.get("button"):
            for down in (True, False):
                vm.qmp("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": req["button"]}}])
                time.sleep(0.15)
        return {"ok": True, "out": ""}
    if op == "tail":
        return {"ok": True, "out": vm.tail(req.get("chars", 3000))}
    if op == "stop":
        try:
            vm.shell_poweroff(timeout=60)
        except Exception:
            pass
        return {"ok": True, "out": "stopped"}
    return {"ok": False, "out": f"unknown op {op}"}


def client(req, timeout=900):
    if not SOCK.exists():
        sys.exit("vmshell: no VM running (use: vmshell.py start ...)")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(SOCK))
    s.sendall(json.dumps(req).encode() + b"\n")
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    reply = json.loads(data.decode())
    if reply.get("out"):
        print(reply["out"].rstrip())
    sys.exit(0 if reply.get("ok") else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="op", required=True)
    st = sub.add_parser("start")
    st.add_argument("--iso"); st.add_argument("--disk"); st.add_argument("--arch", default="amd64")
    st.add_argument("--firmware", default="uefi"); st.add_argument("--memory", type=int, default=4096)
    st.add_argument("--cpu"); st.add_argument("--user", default="shadow"); st.add_argument("--password", default="live")
    st.add_argument("--timeout", type=float, default=1200); st.add_argument("--no-login", action="store_true")
    st.add_argument("--foreground", action="store_true")
    for name in ("run", "sudo"):
        p = sub.add_parser(name); p.add_argument("cmd"); p.add_argument("--timeout", type=float, default=300)
    p = sub.add_parser("shot"); p.add_argument("name")
    p = sub.add_parser("key"); p.add_argument("keys", nargs="+")
    p = sub.add_parser("type"); p.add_argument("text")
    p = sub.add_parser("mouse"); p.add_argument("x", type=int); p.add_argument("y", type=int)
    p.add_argument("button", nargs="?", choices=["left", "right", "middle", "wheel-up", "wheel-down"])
    p.add_argument("--screen", default="1280x800")
    p = sub.add_parser("tail"); p.add_argument("--chars", type=int, default=3000)
    sub.add_parser("stop")
    args = ap.parse_args()

    if args.op == "start":
        if SOCK.exists():
            sys.exit("vmshell: a VM is already running (vmshell.py stop)")
        STATE.mkdir(parents=True, exist_ok=True)
        (STATE / "ready.json").unlink(missing_ok=True)
        if not args.foreground:
            if os.fork() > 0:
                # Parent: wait until the VM is logged in (or failed).
                deadline = time.time() + args.timeout + 120
                while time.time() < deadline:
                    if (STATE / "ready.json").exists():
                        status = json.loads((STATE / "ready.json").read_text())
                        print("vmshell: ready" if status["ok"] else f"vmshell: FAILED: {status['detail']}")
                        sys.exit(0 if status["ok"] else 1)
                    time.sleep(2)
                sys.exit("vmshell: timed out waiting for the VM")
            os.setsid()
            log = open(STATE / "daemon.log", "w")
            os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
            sys.stdin.close()
        serve(args)
        return
    req = {"op": args.op}
    if args.op in ("run", "sudo"):
        req.update(cmd=args.cmd, timeout=args.timeout)
    elif args.op == "shot":
        req["name"] = args.name
    elif args.op == "key":
        req["keys"] = args.keys
    elif args.op == "type":
        req["text"] = args.text
    elif args.op == "mouse":
        w, h = args.screen.split("x")
        req.update(x=args.x, y=args.y, button=args.button, screen=[int(w), int(h)])
    elif args.op == "tail":
        req["chars"] = args.chars
    client(req)


if __name__ == "__main__":
    main()
