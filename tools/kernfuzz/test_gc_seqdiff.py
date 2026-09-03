# -*- coding: utf-8 -*-
r"""test_gc_seqdiff.py — unit tests for the GC sequential-diff harness
(task-gc-seqdiff, docs/kernel-fuzzing-design.md §7.2 / DELIV-8a).

Stdlib-only unittest, style follows tools/kernfuzz/test_gcstress.py.
Run:
    python3 tools/kernfuzz/test_gc_seqdiff.py
Exit 0 = all pass.  Requires ./tinyactor for the real-run sections
(small window); classification / signature / findings sections are
pure-logic or stub-binary and always run.

Covers (task acceptance point 4):
  * classify(): pass / mismatch / crash / timeout, incl. precedence
    (timeout masks comparison; crash on either side is a finding).
  * signature(): whitespace-insensitive source hash (morph §5.4 style).
  * record_finding(): writes src + both-side artifacts + run.sh +
    meta.json; dedup by signature (second record -> False);
    load_known_signatures() reloads for cross-run dedup.
  * small real window via main(): exit 0, all pass.
  * timeout path: stub binary sleeping past --timeout -> category
    timeout, exit 1, finding recorded on disk.
  * crash path: stub binary exiting 3 -> category crash, exit 1.
"""

import json
import os
import shutil
import stat
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gc_seqdiff                # noqa: E402
import gc_workloads              # noqa: E402


def _res(rc=0, out=b"", err=b"", timed_out=False):
    return gc_seqdiff.RunRes(rc, out, err, timed_out)


class TestClassify(unittest.TestCase):
    def test_pass(self):
        a = _res(0, b"a\n1\n")
        b = _res(0, b"a\n1\n")
        self.assertEqual(gc_seqdiff.classify(a, b), "pass")

    def test_mismatch_byte_level(self):
        a = _res(0, b"a\n1\n")
        b = _res(0, b"a\n1\r\n")           # byte-level, not line-normalized
        self.assertEqual(gc_seqdiff.classify(a, b), "mismatch")

    def test_mismatch_trailing_newline(self):
        a = _res(0, b"out\n")
        b = _res(0, b"out")                # missing final newline counts
        self.assertEqual(gc_seqdiff.classify(a, b), "mismatch")

    def test_crash_normal_side(self):
        self.assertEqual(
            gc_seqdiff.classify(_res(-11, b""), _res(0, b"x\n")), "crash")

    def test_crash_stress_side(self):
        self.assertEqual(
            gc_seqdiff.classify(_res(0, b"x\n"), _res(1, b"")), "crash")

    def test_timeout_masks_comparison(self):
        # even if both stdout fragments differ, timeout wins the label
        self.assertEqual(
            gc_seqdiff.classify(
                _res(None, b"part", timed_out=True), _res(0, b"other\n")),
            "timeout")

    def test_timeout_stress_side(self):
        self.assertEqual(
            gc_seqdiff.classify(
                _res(0, b"x\n"),
                _res(None, b"", timed_out=True)),
            "timeout")


class TestSignatureAndFindings(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="seqdiff_test_")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def test_signature_ws_insensitive(self):
        t = "fn main() {\n  print(1)\n}\n"
        t2 = "fn  main(){print(1)}"
        self.assertEqual(gc_seqdiff.signature("mismatch", t),
                         gc_seqdiff.signature("mismatch", t2))
        self.assertEqual(gc_seqdiff.signature("crash", t)[0:6], "crash:")

    def test_record_and_dedup(self):
        src = gc_workloads.generate("pure", 10000)
        normal = _res(0, b"a\n")
        stress = _res(0, b"b\n")
        dedup = set()
        self.assertTrue(gc_seqdiff.record_finding(
            self.dir, "mismatch", 10000, 1, src, normal, stress, dedup))
        # same signature -> dedup skip
        self.assertFalse(gc_seqdiff.record_finding(
            self.dir, "mismatch", 10000, 2, src, normal, stress, dedup))
        # different source -> different signature
        src2 = gc_workloads.generate("pure", 10001)
        self.assertTrue(gc_seqdiff.record_finding(
            self.dir, "mismatch", 10001, 1, src2, normal, stress, dedup))

    def test_finding_artifacts(self):
        src = gc_workloads.generate("pure", 10000)
        sig = gc_seqdiff.signature("timeout", src)
        fdir = os.path.join(
            self.dir, "timeout-%s" % sig.split(":", 1)[1][:8])
        gc_seqdiff.record_finding(
            self.dir, "timeout", 10000, 1, src,
            _res(None, b"part", timed_out=True),
            _res(None, b"", timed_out=True), set())
        for name in ("src.ta", "stdout_normal.txt", "stdout_stress.txt",
                     "stderr_normal.txt", "stderr_stress.txt",
                     "exit_normal.txt", "exit_stress.txt",
                     "meta.json", "run.sh"):
            self.assertTrue(os.path.isfile(os.path.join(fdir, name)),
                            name)
        self.assertEqual(
            open(os.path.join(fdir, "exit_normal.txt")).read(), "TIMEOUT")
        meta = json.load(open(os.path.join(fdir, "meta.json")))
        self.assertEqual(meta["category"], "timeout")
        self.assertEqual(meta["seed"], 10000)
        self.assertEqual(meta["stress_n"], 1)
        self.assertEqual(meta["signature"], sig)
        run_sh = open(os.path.join(fdir, "run.sh")).read()
        self.assertIn("TA_GC_STRESS=1", run_sh)
        self.assertTrue(os.access(os.path.join(fdir, "run.sh"), os.X_OK))

    def test_load_known_signatures(self):
        src = gc_workloads.generate("pure", 10000)
        sig = gc_seqdiff.signature("crash", src)
        gc_seqdiff.record_finding(
            self.dir, "crash", 10000, 1, src,
            _res(0, b"a\n"), _res(3, b""), set())
        self.assertEqual(gc_seqdiff.load_known_signatures(self.dir), {sig})
        # empty/missing dir -> empty set
        self.assertEqual(
            gc_seqdiff.load_known_signatures(
                os.path.join(self.dir, "nope")), set())


class TestRealRun(unittest.TestCase):
    """Small window through the real binary (skips when unbuilt)."""

    def setUp(self):
        if not os.path.exists(gc_seqdiff.TINYACTOR):
            self.skipTest("./tinyactor not built")

    def test_check_seed_pass(self):
        for seed in (10000, 10001, 10002):
            res = gc_seqdiff.check_seed(
                gc_seqdiff.TINYACTOR, seed, 1, gc_seqdiff.DEFAULT_TIMEOUT)
            self.assertEqual(res["category"], "pass", "seed=%d" % seed)

    def test_main_small_window_exit0(self):
        out = tempfile.mkdtemp(prefix="seqdiff_out_")
        self.addCleanup(shutil.rmtree, out, ignore_errors=True)
        rc = gc_seqdiff.main(["--seeds", "10000:10004", "--out", out])
        self.assertEqual(rc, 0)
        self.assertEqual(os.listdir(out), [])   # no findings recorded

    def test_main_small_window_jobs_parallel(self):
        out = tempfile.mkdtemp(prefix="seqdiff_out_")
        self.addCleanup(shutil.rmtree, out, ignore_errors=True)
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10004", "--out", out, "--jobs", "3"])
        self.assertEqual(rc, 0)


class TestStubBinary(unittest.TestCase):
    """Timeout / crash paths via a stubbed binary (no real VM needed)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="seqdiff_stub_")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.out = os.path.join(self.dir, "findings")
        self.saved = gc_seqdiff.TINYACTOR
        self.addCleanup(setattr, gc_seqdiff, "TINYACTOR", self.saved)

    def _stub(self, body):
        path = os.path.join(self.dir, "stub.sh")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n%s\n" % body)
        st = os.stat(path)
        os.chmod(path, st.st_mode | stat.S_IEXEC)
        gc_seqdiff.TINYACTOR = path
        return path

    def test_timeout_path(self):
        # sleep 2 with --timeout 0.1 -> timeout on the stress side
        self._stub("sleep 2")
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10001", "--out", self.out,
             "--timeout", "0.1"])
        self.assertEqual(rc, 1)
        names = os.listdir(self.out)
        self.assertEqual(len(names), 1)
        self.assertTrue(names[0].startswith("timeout-"))
        meta_path = os.path.join(self.out, names[0], "meta.json")
        with open(meta_path) as f:
            meta = json.load(f)
        self.assertEqual(meta["category"], "timeout")

    def test_crash_path(self):
        self._stub("exit 3")
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10001", "--out", self.out])
        self.assertEqual(rc, 1)
        names = os.listdir(self.out)
        self.assertEqual(len(names), 1)
        self.assertTrue(names[0].startswith("crash-"))

    def test_dedup_across_runs(self):
        # same window twice against the crashing stub: exit stays 1
        # (dedup suppresses re-RECORDING only, never the exit code —
        # CI must stay red while a mismatch stands) and no second
        # finding dir is created
        self._stub("exit 3")
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10002", "--out", self.out])
        self.assertEqual(rc, 1)
        self.assertEqual(len(os.listdir(self.out)), 2)   # 2 seeds
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10002", "--out", self.out])
        self.assertEqual(rc, 1)
        self.assertEqual(len(os.listdir(self.out)), 2)   # no growth

    def test_skip_when_binary_missing(self):
        gc_seqdiff.TINYACTOR = os.path.join(self.dir, "absent")
        rc = gc_seqdiff.main(
            ["--seeds", "10000:10001", "--out", self.out])
        self.assertEqual(rc, 2)
        self.assertFalse(os.path.exists(self.out))

    def test_bad_stress_n_rejected(self):
        with self.assertRaises(SystemExit):
            gc_seqdiff.main(
                ["--seeds", "10000:10001", "--out", self.out,
                 "--stress-n", "0"])

    def test_bad_seeds_window_rejected(self):
        with self.assertRaises(ValueError):
            gc_seqdiff.parse_seeds("5:5")
        with self.assertRaises(ValueError):
            gc_seqdiff.parse_seeds("10")


if __name__ == "__main__":
    unittest.main(verbosity=2)