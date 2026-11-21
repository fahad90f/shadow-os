"""Log tee: copy the helper's stderr to the real stderr AND to voice.log.

Started by logsetup.start_tee() as a separate tiny process (python3 -I logtee.py
LOGFILE MAXBYTES TAG) whose stdin is a pipe that the helper made its fd 2.

Why a process and not a thread: native code (onnxruntime, sherpa-onnx, libc's
abort message) writes straight to fd 2, often just before the process dies. A
thread inside the dying helper would lose exactly those last lines; this child
keeps reading until every writer has gone, so the last words reach both the
parent app (QProcess shows them) and the log file.

It is the ONLY writer of voice.log for a helper run, so rotation needs no lock;
if another helper run rotated the file under us we notice the inode change and
reopen. Every failure here is swallowed on purpose: logging must never take the
helper down or block it.
"""
import os
import signal
import sys
import time


class RotatingLog:
    def __init__(self, path, max_bytes, tag):
        self.path, self.max_bytes, self.tag = path, max_bytes, tag
        self.f = None
        self.ino = None
        self._open()

    def _open(self):
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            self.f = open(self.path, "ab", buffering=0)
            self.ino = os.fstat(self.f.fileno()).st_ino
        except OSError:
            self.f = None

    def _rotate_if_needed(self, incoming):
        if self.f is None:
            return
        try:
            st = os.stat(self.path)
            if st.st_ino != self.ino:                   # somebody else rotated it
                self.f.close()
                self._open()
                st = os.stat(self.path)
            if st.st_size + incoming > self.max_bytes and st.st_size > 0:
                self.f.close()
                os.replace(self.path, self.path + ".1")
                self._open()
        except OSError:
            # the file vanished (deleted by the user): start a new one
            try:
                self.f.close()
            except OSError:
                pass
            self._open()

    def write_line(self, line):
        if self.f is None:
            self._open()
            if self.f is None:
                return
        now = time.time()
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now)) + ".%03d" % int((now % 1) * 1000)
        data = f"{stamp} [{self.tag}] ".encode() + line.rstrip(b"\n") + b"\n"
        self._rotate_if_needed(len(data))
        try:
            self.f.write(data)
        except OSError:
            pass


def main():
    # Ctrl+C in a terminal hits the whole process group; keep draining until EOF.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    path = sys.argv[1]
    max_bytes = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 20
    tag = sys.argv[3] if len(sys.argv) > 3 else "shadow-voiced"
    log = RotatingLog(path, max_bytes, tag)
    out_ok = True
    pending = b""
    while True:
        try:
            chunk = os.read(0, 65536)
        except InterruptedError:
            continue
        except OSError:
            break
        if not chunk:
            break
        if out_ok:
            try:
                view = memoryview(chunk)
                while view:
                    view = view[os.write(1, view):]
            except OSError:
                out_ok = False          # the parent app stopped reading stderr; keep logging
        pending += chunk
        *lines, pending = pending.split(b"\n")
        for line in lines:
            log.write_line(line)
        if len(pending) > 65536:          # a huge line without newline: write it anyway
            log.write_line(pending)
            pending = b""
    if pending:
        log.write_line(pending)


if __name__ == "__main__":
    main()
