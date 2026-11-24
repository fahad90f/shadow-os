#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The repository's CI runs the test suites, and a skipped suite is not a pass.

    python3 tests/build/test_ci_suites.py [-v]

  * every suite under tests/security, tests/network and tests/build is run by
    tests/run-suites.sh (the network-namespace ones only as root);
  * .github/workflows/build.yml runs tests/run-suites.sh as a user and with
    sudo as "--root --no-skip", and the in_chroot resolver test against the
    build chroot;
  * the network-namespace suites exit 77 (skipped), not 0, without root;
  * the runner reports exit status 77, and a unittest suite whose every test
    was skipped, as SKIP, not PASS, and as FAIL with --no-skip.

Standard library only; runs as any user. Nothing is run as root here: the
namespace suites are started with os.geteuid() answering an ordinary uid.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RUNNER = os.path.join(ROOT, "tests", "run-suites.sh")
WORKFLOW = os.path.join(ROOT, ".github", "workflows", "build.yml")


def suites(*directories):
    found = []
    for directory in directories:
        path = os.path.join(ROOT, "tests", directory)
        found += [f"tests/{directory}/{name}" for name in sorted(os.listdir(path))
                  if name.startswith("test_") and name.endswith(".py")]
    return found


def listed(mode):
    proc = subprocess.run(["bash", RUNNER, mode, "--list"], capture_output=True, text=True, cwd=ROOT)
    assert proc.returncode == 0, proc.stderr
    return [line.split("\t", 1)[1] for line in proc.stdout.splitlines()]


class RunnerCoversSuites(unittest.TestCase):
    def test_user_suites(self):
        commands = listed("--user")
        for path in suites("security", "build"):
            self.assertTrue(any(path in c.split() for c in commands), f"{path} is not run by tests/run-suites.sh")
        for path in suites("network"):
            self.assertFalse(any(path in c.split() for c in commands), f"{path} needs root, but --user runs it")
        for path in ("tests/network/fix-network-test.sh", "arsenal/tools/validate.py",
                     "tests/installation/test_plan.py"):
            self.assertTrue(any(path in c.split() for c in commands), path)

    def test_root_adds_the_namespace_suites(self):
        commands = listed("--root")
        self.assertEqual(commands[:len(listed("--user"))], listed("--user"))
        for path in suites("network", "security", "build"):
            self.assertTrue(any(path in c.split() for c in commands), f"{path} is not run by --root")


def workflow_runs():
    """(job, step name, the step's run: text) for every step of build.yml."""
    steps, job, name, run, block = [], None, None, None, None
    for line in open(WORKFLOW, encoding="utf-8").read().splitlines():
        if re.match(r"^  [A-Za-z0-9_-]+:\s*$", line):
            job = line.strip().rstrip(":")
            continue
        step = re.match(r"^      - (?:name: (.*)|uses: .*)$", line)
        if step:
            if run is not None:
                steps.append((job, name, run))
            name, run, block = step.group(1), None, None
            continue
        inline = re.match(r"^        run: (?!\|)(.*)$", line)
        if inline:
            run = inline.group(1)
            continue
        if re.match(r"^        run: \|\s*$", line):
            run, block = "", True
            continue
        if block and line.startswith("          "):
            run += line.strip() + "\n"
        elif block and line.strip():
            block = None
    if run is not None:
        steps.append((job, name, run))
    return steps


class Workflow(unittest.TestCase):
    def test_runner_runs_as_user_and_as_root(self):
        runs = [(job, run) for job, _, run in workflow_runs() if "tests/run-suites.sh" in run]
        user = [job for job, run in runs if re.search(r"(?m)^(?!sudo).*tests/run-suites\.sh --user\b", run)]
        root = [job for job, run in runs
                if re.search(r"(?m)^sudo (-E )?tests/run-suites\.sh --root --no-skip\b", run)]
        self.assertTrue(user, "no CI step runs tests/run-suites.sh --user as the runner's user")
        self.assertTrue(root, "no CI step runs sudo tests/run-suites.sh --root --no-skip")
        self.assertEqual(set(user), set(root), "both steps belong in the same job")

    def test_resolver_test_against_the_build_chroot(self):
        steps = [(job, run) for job, _, run in workflow_runs()]
        jobs = [job for job, run in steps if "tests/build/in-chroot-resolver-test.sh" in run]
        self.assertIn("iso-amd64", jobs)
        order = [run for job, run in steps if job == "iso-amd64"]
        chroot = next(i for i, run in enumerate(order) if "build/chroot/create.sh" in run)
        test = next(i for i, run in enumerate(order) if "in-chroot-resolver-test.sh" in run)
        self.assertGreater(test, chroot, "the resolver test needs the build chroot")


class NamespaceSuitesSkip(unittest.TestCase):
    def test_without_root_exit_77(self):
        for path in suites("network"):
            with self.subTest(path=path):
                env = {k: v for k, v in os.environ.items() if not k.startswith(("SHADOW_KILLSWITCH", "SHADOW_ANON"))}
                env["PYTHONDONTWRITEBYTECODE"] = "1"
                code = ("import os, runpy, sys; os.geteuid = lambda: 4242; sys.argv = [sys.argv[1]]; "
                        "runpy.run_path(sys.argv[0], run_name='__main__')")
                proc = subprocess.run([sys.executable, "-B", "-c", code, os.path.join(ROOT, path)],
                                      capture_output=True, text=True, env=env, timeout=120)
                self.assertEqual(proc.returncode, 77, f"{path}: {proc.stdout}{proc.stderr}")
                self.assertIn("SKIP", proc.stderr)


class RunnerVerdicts(unittest.TestCase):
    ALL_SKIPPED = ("import unittest\n"
                   "class T(unittest.TestCase):\n"
                   "    @unittest.skip('needs root')\n"
                   "    def test_a(self): pass\n"
                   "unittest.main()\n")

    def run_suites(self, no_skip, *suite_lines):
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "all_skipped.py")
            with open(script, "w", encoding="utf-8") as fh:
                fh.write(self.ALL_SKIPPED)
            some = os.path.join(tmp, "some_skipped.py")
            with open(some, "w", encoding="utf-8") as fh:
                fh.write(self.ALL_SKIPPED.replace("unittest.main()", "    def test_b(self): pass\nunittest.main()"))
            body = "\n".join(line.replace("@ALL@", script).replace("@SOME@", some) for line in suite_lines)
            program = (f". {RUNNER}\nLOG_DIR={tmp}\nNO_SKIP={1 if no_skip else 0}\n{body}\nsummary\n")
            proc = subprocess.run(["bash", "-c", program], capture_output=True, text=True, cwd=ROOT, timeout=120)
            return proc.returncode, proc.stdout

    def verdict(self, out, name):
        match = re.search(rf"(?m)^(PASS|SKIP|FAIL)\s+{re.escape(name)}\s", out)
        self.assertIsNotNone(match, out)
        return match.group(1)

    def test_verdicts(self):
        lines = ["suite ok true", "suite skipped bash -c 'exit 77'", "suite all-skipped python3 @ALL@",
                 "suite some-skipped python3 @SOME@"]
        rc, out = self.run_suites(False, *lines)
        self.assertEqual(rc, 0, out)
        self.assertEqual([self.verdict(out, n) for n in ("ok", "skipped", "all-skipped", "some-skipped")],
                         ["PASS", "SKIP", "SKIP", "PASS"])
        self.assertIn("passed: 2  skipped: 2  failed: 0", out)

        rc, out = self.run_suites(True, *lines)
        self.assertEqual(rc, 1, out)
        self.assertEqual([self.verdict(out, n) for n in ("ok", "skipped", "all-skipped", "some-skipped")],
                         ["PASS", "FAIL", "FAIL", "PASS"])

        rc, out = self.run_suites(False, "suite broken bash -c 'exit 1'")
        self.assertEqual((rc, self.verdict(out, "broken")), (1, "FAIL"), out)

    def test_root_mode_needs_root(self):
        if os.geteuid() == 0:
            self.skipTest("runs as root")
        proc = subprocess.run(["bash", RUNNER, "--root"], capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(proc.returncode, 2, proc.stderr)


if __name__ == "__main__":
    unittest.main()
