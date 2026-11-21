"""shadow-voice-setup: options, exit codes, error messages, --check/--uninstall safety,
--progress-json and --install-deps (with a fake apt-get). No network, no real install
(a full install is ~85 s and is exercised separately; see README)."""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

from harness import SETUP, TMP

CACHE = os.environ.get("SHADOW_VOICE_TEST_CACHE")      # offline cache with the pinned artifacts (optional)


def run(*args, env=None, timeout=120):
    r = subprocess.run([SETUP] + list(args), capture_output=True, text=True, timeout=timeout,
                       env=dict(os.environ, **(env or {})))
    return r.returncode, r.stdout, r.stderr


class Options(unittest.TestCase):
    def test_help_and_usage_errors(self):
        code, out, _ = run("--help")
        self.assertEqual(code, 0)
        self.assertIn("Exit codes", out)
        self.assertEqual(run("--bogus")[0], 2)
        self.assertEqual(run("--stt", "klingon")[0], 2)
        self.assertEqual(run("--dest")[0], 2)
        self.assertEqual(run("--dest", "/")[0], 2)
        code, out, _ = run("--version")
        self.assertEqual(code, 0)
        self.assertIn("shadow-voice-setup", out)

    def test_system_needs_root(self):
        if os.geteuid() == 0:
            self.skipTest("running as root")
        code, _, err = run("--system", "--offline-cache", TMP)
        self.assertEqual(code, 7)
        self.assertIn("sudo", err)


class Check(unittest.TestCase):
    def test_check_nothing_installed(self):
        d = tempfile.mkdtemp(dir=TMP)
        code, _, err = run("--check", "--dest", d)
        self.assertEqual(code, 11)
        self.assertIn("no voice engine data", err)

    def test_check_detects_damage(self):
        d = tempfile.mkdtemp(dir=TMP)
        os.makedirs(os.path.join(d, "models", "vad"))
        with open(os.path.join(d, "models", "vad", "silero_vad.onnx"), "wb") as f:
            f.write(b"not the real model")
        manifest = {"format": 1, "installed": {"vad": {"files": {"models/vad/silero_vad.onnx": {
            "size": 18, "sha256": "0" * 64}}}}}
        with open(os.path.join(d, "manifest.json"), "w") as f:
            json.dump(manifest, f)
        code, out, err = run("--check", "--dest", d, "--progress-json")
        self.assertEqual(code, 10)
        last = json.loads(out.strip().splitlines()[-1])
        self.assertEqual(last["step"], "error")
        self.assertEqual(last["code"], 10)
        self.assertIn("sha256", " ".join(last["bad"]))
        for line in out.strip().splitlines():
            json.loads(line)                           # every line is JSON

    def test_uninstall_removes_our_data_only(self):
        parent = tempfile.mkdtemp(dir=TMP)
        d = os.path.join(parent, "voice")
        os.makedirs(os.path.join(d, "models"))
        open(os.path.join(d, "models", "x.onnx"), "w").write("x")
        with open(os.path.join(d, "manifest.json"), "w") as f:
            json.dump({"format": 1, "installed": {}}, f)
        sibling = os.path.join(parent, "other.txt")
        open(sibling, "w").write("keep")
        cache = os.path.join(parent, "cache")
        os.makedirs(cache)
        open(os.path.join(cache, "silero_vad.onnx"), "w").write("cached")
        open(os.path.join(cache, "unrelated.bin"), "w").write("keep")
        code, out, err = run("--uninstall", "--dest", d, "--yes", "--cache", cache, "--progress-json")
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(d))
        self.assertTrue(os.path.exists(sibling))
        self.assertFalse(os.path.exists(os.path.join(cache, "silero_vad.onnx")))
        self.assertTrue(os.path.exists(os.path.join(cache, "unrelated.bin")), "only our artifacts are removed")
        self.assertEqual(json.loads(out.strip().splitlines()[-1])["step"], "done")

    def test_uninstall_refuses_foreign_directory(self):
        d = tempfile.mkdtemp(dir=TMP)
        keep = os.path.join(d, "precious.txt")
        open(keep, "w").write("do not delete")
        code, _, err = run("--uninstall", "--dest", d, "--yes", "--cache", os.path.join(TMP, "no-cache"))
        self.assertEqual(code, 11)
        self.assertTrue(os.path.exists(keep))


class OfflineCache(unittest.TestCase):
    def test_missing_artifacts(self):
        empty = tempfile.mkdtemp(dir=TMP)
        dest = os.path.join(tempfile.mkdtemp(dir=TMP), "voice")
        code, out, err = run("--dest", dest, "--offline-cache", empty, "--no-selftest", "--progress-json",
                             "--numpy", "system")
        if code == 3:
            self.skipTest("python3-numpy missing here; --numpy system refuses (tested in test_install_deps)")
        self.assertEqual(code, 4, err)
        self.assertIn("not in the offline cache", err)
        self.assertIn("kokoro-multi-lang-v1_0.tar.bz2", err)
        self.assertIn("https://github.com/k2-fsa/sherpa-onnx/releases/download/", err)
        err_line = json.loads(out.strip().splitlines()[-1])
        self.assertEqual((err_line["step"], err_line["code"]), ("error", 4))
        self.assertFalse(os.path.exists(dest))

    @unittest.skipUnless(CACHE, "set SHADOW_VOICE_TEST_CACHE to an offline cache with the pinned artifacts")
    def test_corrupted_artifact(self):
        bad = tempfile.mkdtemp(dir=TMP)
        for n in os.listdir(CACHE):
            os.symlink(os.path.join(CACHE, n), os.path.join(bad, n))
        os.unlink(os.path.join(bad, "silero_vad.onnx"))
        data = bytearray(open(os.path.join(CACHE, "silero_vad.onnx"), "rb").read())
        data[1000] ^= 0xFF
        open(os.path.join(bad, "silero_vad.onnx"), "wb").write(bytes(data))
        dest = os.path.join(tempfile.mkdtemp(dir=TMP), "voice")
        code, _, err = run("--dest", dest, "--offline-cache", bad, "--no-selftest", "--stt", "moonshine")
        self.assertEqual(code, 5, err)
        self.assertIn("silero_vad.onnx", err)
        self.assertIn("sha256", err)
        self.assertFalse(os.path.exists(dest), "nothing may be installed from a bad artifact")

    def test_lock(self):
        dest_parent = tempfile.mkdtemp(dir=TMP)
        holder = subprocess.Popen(["flock", dest_parent, "sleep", "30"])
        try:
            import time
            time.sleep(0.3)
            code, _, err = run("--dest", os.path.join(dest_parent, "voice"), "--offline-cache", TMP,
                               "--numpy", "wheel")
            self.assertEqual(code, 12, err)
            self.assertIn("another shadow-voice-setup", err)
        finally:
            holder.kill()
            holder.wait()


def bindir_without(*names, extra=None):
    """A PATH directory with every tool of /usr/bin and /bin except NAMES (+ EXTRA {name: script})."""
    bindir = tempfile.mkdtemp(dir=TMP)
    for d in ("/usr/bin", "/bin"):
        for n in os.listdir(d):
            if n in names or os.path.exists(os.path.join(bindir, n)):
                continue
            os.symlink(os.path.join(d, n), os.path.join(bindir, n))
    for n, body in (extra or {}).items():
        with open(os.path.join(bindir, n), "w") as f:
            f.write(body)
        os.chmod(os.path.join(bindir, n), 0o755)
    return bindir


class Architecture(unittest.TestCase):
    def test_refuses_other_architectures(self):
        """The pinned runtime is x86_64: elsewhere nothing is downloaded or installed."""
        bindir = bindir_without("uname", extra={"uname": "#!/bin/sh\n[ \"$1\" = -m ] && echo aarch64 || echo Linux\n"})
        dest = os.path.join(tempfile.mkdtemp(dir=TMP), "voice")
        cache = os.path.join(TMP, "arch-cache")
        code, out, err = run("--dest", dest, "--cache", cache, "--yes", "--progress-json", env={"PATH": bindir})
        self.assertEqual(code, 13, err)
        self.assertIn("x86_64 only", err)
        self.assertIn("aarch64", err)
        self.assertEqual(json.loads(out.strip().splitlines()[-1])["code"], 13)
        self.assertFalse(os.path.exists(dest))
        self.assertFalse(os.path.exists(cache), "nothing may be downloaded")


@unittest.skipUnless(CACHE, "set SHADOW_VOICE_TEST_CACHE to an offline cache with the pinned artifacts")
class Reinstall(unittest.TestCase):
    """A full cache needs no download tool; a repair downloads only the damaged component;
    a selftest failure of the new files leaves the installation as it was."""

    def test_cache_repair_and_selftest_gate(self):
        dest = os.path.join(tempfile.mkdtemp(dir=TMP), "voice")
        cache = tempfile.mkdtemp(dir=TMP)
        for n in os.listdir(CACHE):
            os.symlink(os.path.join(CACHE, n), os.path.join(cache, n))
        nodl = bindir_without("curl", "wget")
        code, out, err = run("--dest", dest, "--cache", cache, "--stt", "moonshine", "--numpy", "system", "--yes",
                             "--no-selftest", env={"PATH": nodl}, timeout=900)
        if code == 3 and "numpy" in err:
            self.skipTest("python3-numpy missing here")
        self.assertEqual(code, 0, err)
        self.assertNotIn("curl", err)
        code, _, err = run("--check", "--dest", dest)
        self.assertEqual(code, 0, err)

        # one damaged file: only its component is fetched again, by a download tool that logs URLs
        vad = os.path.join(dest, "models", "vad", "silero_vad.onnx")
        os.chmod(vad, 0o644)
        with open(vad, "r+b") as f:
            f.seek(1000)
            f.write(b"\xff\xff")
        urls = os.path.join(TMP, "fetched-urls.txt")
        fake_curl = ("#!/bin/sh\nout=\"\"; url=\"\"\nwhile [ $# -gt 0 ]; do case \"$1\" in -o) out=$2; shift ;; "
                     "--) url=$2; shift ;; esac; shift; done\n"
                     f"echo \"$url\" >> {urls}\ncp \"{CACHE}/${{url##*/}}\" \"$out\"\n")
        dl = bindir_without("curl", "wget", extra={"curl": fake_curl})
        empty = tempfile.mkdtemp(dir=TMP)
        code, out, err = run("--dest", dest, "--cache", empty, "--stt", "moonshine", "--numpy", "system", "--yes",
                             "--no-selftest", env={"PATH": dl}, timeout=900)
        self.assertEqual(code, 0, err)
        with open(urls) as f:
            fetched = f.read().split()
        self.assertEqual([u.rsplit("/", 1)[-1] for u in fetched], ["silero_vad.onnx"], fetched)
        self.assertIn("already installed and intact", err)
        code, _, err = run("--check", "--dest", dest)
        self.assertEqual(code, 0, err)

        # the new files fail the engine's selftest: nothing is replaced, nothing is left behind
        with open(os.path.join(dest, "manifest.json")) as f:
            before = f.read()
        bad = os.path.join(TMP, "failing-voiced")
        with open(bad, "w") as f:
            f.write("#!/bin/sh\necho '{\"ok\": false}'\nexit 1\n")
        os.chmod(bad, 0o755)
        code, out, err = run("--dest", dest, "--cache", empty, "--stt", "moonshine", "--numpy", "system", "--yes",
                             env={"PATH": nodl, "SHADOW_VOICED": bad}, timeout=900)
        self.assertEqual(code, 9, err)
        self.assertIn("were not installed", err)
        with open(os.path.join(dest, "manifest.json")) as f:
            self.assertEqual(f.read(), before)
        parent = os.path.dirname(dest)
        self.assertEqual(sorted(os.listdir(parent)), ["voice"], "no staging or work directory may be left")


class Deps(unittest.TestCase):
    def test_install_deps_uses_apt(self):
        """espeak-ng 'missing' + --install-deps -> sudo apt-get install -y --no-install-recommends espeak-ng"""
        bindir = tempfile.mkdtemp(dir=TMP)
        for d in ("/usr/bin", "/bin"):
            for n in os.listdir(d):
                if n in ("espeak-ng", "sudo", "apt-get") or os.path.exists(os.path.join(bindir, n)):
                    continue
                os.symlink(os.path.join(d, n), os.path.join(bindir, n))
        record = os.path.join(bindir, "apt.log")
        with open(os.path.join(bindir, "apt-get"), "w") as f:
            f.write(f"#!/bin/sh\necho \"$@\" >> {record}\nexit 0\n")
        with open(os.path.join(bindir, "sudo"), "w") as f:
            f.write("#!/bin/sh\n[ \"$1\" = -n ] && shift\nexec \"$@\"\n")
        os.chmod(os.path.join(bindir, "apt-get"), 0o755)
        os.chmod(os.path.join(bindir, "sudo"), 0o755)
        dest = os.path.join(tempfile.mkdtemp(dir=TMP), "voice")
        code, _, err = run("--user", "--install-deps", "--offline-cache", tempfile.mkdtemp(dir=TMP),
                           env={"PATH": bindir, "XDG_DATA_HOME": os.path.dirname(dest)})
        self.assertEqual(code, 4, err)                    # deps done, then the (empty) offline cache stops it
        with open(record) as f:
            args = f.read()
        self.assertIn("install -y --no-install-recommends", args)
        self.assertIn("espeak-ng", args)

    def test_missing_espeak_is_named(self):
        bindir = tempfile.mkdtemp(dir=TMP)
        for d in ("/usr/bin", "/bin"):
            for n in os.listdir(d):
                if n == "espeak-ng" or os.path.exists(os.path.join(bindir, n)):
                    continue
                os.symlink(os.path.join(d, n), os.path.join(bindir, n))
        code, _, err = run("--dest", os.path.join(tempfile.mkdtemp(dir=TMP), "voice"), "--offline-cache",
                           tempfile.mkdtemp(dir=TMP), env={"PATH": bindir})
        self.assertEqual(code, 4)
        self.assertIn("sudo apt install espeak-ng", err)


if __name__ == "__main__":
    unittest.main()
