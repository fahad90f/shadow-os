"""Logging: $XDG_STATE_HOME/shadow/voice.log (rotated at 1 MB) and stderr.

All diagnostics go to fd 2. In the long-running modes fd 2 is a pipe into the
log tee child (logtee.py), which copies everything to the original stderr (the
parent app reads it) and appends it to voice.log. Python logging therefore only
needs a stderr handler, and native messages land in the log too.

Exception hooks guarantee the rule "never a Python traceback without an error
event first": an uncaught exception in any thread emits {"event":"error",
"fatal":true} before the traceback is logged, then the helper exits non-zero
(the app restarts it with backoff) instead of limping on with a dead thread.
"""
import faulthandler
import logging
import os
import subprocess
import sys
import threading

MAX_LOG_BYTES = 1 << 20
log = logging.getLogger("shadow-voiced")


def log_file_path():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "shadow", "voice.log")


def _direct_file_handler(path):
    """Fallback when the tee child cannot start: Python logs still reach the file."""
    from logging.handlers import RotatingFileHandler
    os.makedirs(os.path.dirname(path), exist_ok=True)
    h = RotatingFileHandler(path, maxBytes=MAX_LOG_BYTES, backupCount=1, encoding="utf-8")
    h.setFormatter(logging.Formatter("%(asctime)s [shadow-voiced/%(process)d] %(levelname)s %(threadName)s: %(message)s"))
    return h


def start_tee(path, tag):
    """Make fd 2 a pipe into logtee.py. Returns the tee Popen or raises OSError."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tee_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logtee.py")
    sys.stderr.flush()
    r, w = os.pipe()
    orig = os.dup(2)
    try:
        child = subprocess.Popen([sys.executable, "-I", "-B", tee_py, path, str(MAX_LOG_BYTES), tag],
                                 stdin=r, stdout=orig, stderr=subprocess.DEVNULL, close_fds=True)
    except OSError:
        os.close(r)
        os.close(w)
        os.close(orig)
        raise
    os.close(r)
    os.close(orig)
    os.dup2(w, 2)
    os.close(w)
    return child


def setup(debug=False, tee=True, tag=None):
    """Configure logging; returns the log file path ('' when only stderr is used)."""
    path = log_file_path()
    handlers = []
    tee_ok = False
    if tee:
        try:
            start_tee(path, tag or f"shadow-voiced/{os.getpid()}")
            tee_ok = True
        except OSError as e:
            sys.stderr.write(f"shadow-voiced: cannot start the log tee ({e}); logging to stderr only\n")
    stream = logging.StreamHandler(sys.stderr)
    stream.setFormatter(logging.Formatter("shadow-voiced: %(levelname)s %(threadName)s: %(message)s"))
    handlers.append(stream)
    if tee and not tee_ok:
        try:
            handlers.append(_direct_file_handler(path))
        except OSError:
            path = ""
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    for h in handlers:
        root.addHandler(h)
    root.setLevel(logging.DEBUG if debug else logging.INFO)
    try:
        faulthandler.enable(file=sys.stderr, all_threads=True)
    except (ValueError, OSError, AttributeError):
        pass
    return path if tee else ""


def install_excepthooks(emit_error, on_fatal):
    """emit_error(message, fatal) is called BEFORE the traceback is written."""

    def hook(exc_type, exc, tb):
        if issubclass(exc_type, KeyboardInterrupt):
            on_fatal(130)
            return
        try:
            emit_error(f"internal error: {exc_type.__name__}: {exc}", True)
        finally:
            log.error("uncaught exception", exc_info=(exc_type, exc, tb))
            on_fatal(1)

    def thook(args):
        if args.exc_type is SystemExit:
            return
        name = args.thread.name if args.thread else "thread"
        try:
            emit_error(f"internal error in {name}: {args.exc_type.__name__}: {args.exc_value}", True)
        finally:
            log.error("uncaught exception in %s", name,
                      exc_info=(args.exc_type, args.exc_value, args.exc_traceback))
            on_fatal(1)

    sys.excepthook = hook
    threading.excepthook = thook
