"""Logging: the stderr tee writes voice.log (rotated at the size limit) and passes everything on."""
import os
import subprocess
import sys
import tempfile
import time
import unittest

from harness import TMP, VOICE_DIR, Helper, need_data

LOGTEE = os.path.join(VOICE_DIR, "shadow_voice", "logtee.py")


class Tee(unittest.TestCase):
    def test_rotation_and_passthrough(self):
        d = tempfile.mkdtemp(dir=TMP)
        log = os.path.join(d, "state", "shadow", "voice.log")
        p = subprocess.Popen([sys.executable, "-I", LOGTEE, log, "2000", "tag"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE)
        lines = b"".join(b"line %04d " % i + b"x" * 40 + b"\n" for i in range(100))       # ~5 kB
        out, _ = p.communicate(lines + b"no newline at the end", timeout=10)
        self.assertEqual(out, lines + b"no newline at the end", "stderr must be passed through unchanged")
        self.assertTrue(os.path.exists(log + ".1"), "rotated file missing")
        self.assertLessEqual(os.path.getsize(log), 2000)
        self.assertLessEqual(os.path.getsize(log + ".1"), 2000)
        with open(log, "rb") as f:
            last = f.read().splitlines()[-1]
        self.assertTrue(last.endswith(b"no newline at the end"))
        self.assertIn(b"[tag]", last)

    def test_survives_unwritable_log(self):
        p = subprocess.Popen([sys.executable, "-I", LOGTEE, "/proc/forbidden/voice.log", "2000", "t"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        out, _ = p.communicate(b"still here\n", timeout=10)
        self.assertEqual(out, b"still here\n")
        self.assertEqual(p.returncode, 0)


@need_data
class HelperLog(unittest.TestCase):
    def test_log_file_and_fatal_error_order(self):
        state = tempfile.mkdtemp(dir=TMP)
        env = dict(os.environ, XDG_STATE_HOME=state, SHADOW_VOICED_TEST_HOOKS="1")
        h = Helper("--muted", log_name="crash", env=env)
        h.ready()
        h.send(cmd="_raise")
        err = h.wait("error", fatal=True, timeout=10)
        self.assertIn("simulated internal error", err["message"])
        self.assertEqual(h.p.wait(10), 1, "an internal error must end the helper (the app restarts it)")
        h.close()
        with open(h.err_path, encoding="utf-8", errors="replace") as f:
            stderr = f.read()
        self.assertIn("Traceback", stderr)


@need_data
class HelperLogFile(unittest.TestCase):
    def test_voice_log_written(self):
        from harness import VOICED, DATA
        state = tempfile.mkdtemp(dir=TMP)
        env = dict(os.environ, XDG_STATE_HOME=state)
        r = subprocess.run([VOICED, "--probe", "--data", DATA], env=env, capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 0)
        p = subprocess.Popen([VOICED, "--serve", "--muted", "--data", DATA], env=env, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = p.communicate(b'{"cmd":"quit"}\n', timeout=120)
        self.assertEqual(p.returncode, 0)
        log = os.path.join(state, "shadow", "voice.log")
        for _ in range(50):
            if os.path.exists(log) and b"exit 0" in open(log, "rb").read():
                break
            time.sleep(0.1)
        with open(log, "rb") as f:
            content = f.read()
        self.assertIn(b"models loaded", content)
        self.assertIn(b"models loaded", err, "stderr must carry the same lines")


if __name__ == "__main__":
    unittest.main()
