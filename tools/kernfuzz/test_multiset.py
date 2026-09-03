#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""test_multiset.py — unit tests for tools/kernfuzz/multiset.py
(task-multiset, DELIV-8b; docs/kernel-fuzzing-design.md §7.3).

Stdlib-only unittest, style follows tools/kernfuzz/test_gc_workloads.py.
Run:
    python3 tools/kernfuzz/test_multiset.py
Exit 0 = all pass.  Requires ./tinyactor for the live-run tests (skipped
gracefully on a bare checkout).

Covers (task acceptance points):
  * driver generation: structure (zero-arg spawn + first-message config,
    monitor barrier), K/M/total reflected in source, determinism per seed,
    seed sensitivity (spawn order / payload lengths vary).
  * expected_multiset: exact K*M cartesian enumeration, sorted.
  * JUDGE with fabricated stdout (acceptance #3 — direct fake feed, no VM):
    conserved / dropped / extra / malformed / duplicate / combined verdicts.
  * timeout path (acceptance #4): a spinning (infinite tail-recursive) TA
    program with a 1s timeout reports category "timeout" (+ dropped, since
    no lines arrive).  Uses timeout=1 not the production default 10 so the
    suite stays fast; the classification code path is identical.
  * live conservation: a real small case (K=2, M=10) conserves.
  * CLI: real run exits 0; a broken binary under test (empty stdout) makes
    every line "dropped" -> exit 1 with a novel finding JSON on disk; bad
    args rejected.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))
sys.path.insert(0, _HERE)

import multiset as ms             # noqa: E402

TINYACTOR = ms.TINYACTOR
CAT = "/bin/cat"                  # echoes the .ta source back (garbage out)
TRUE = shutil.which("true") or "/usr/bin/true"  # truly empty stdout


def spin_source():
    """Infinite tail-recursive spin (probed: never terminates, TCO'd)."""
    return "fn spin(n) {\n  spin(n)\n}\nfn main() {\n  spin(0)\n}\n"


class GenDriverTest(unittest.TestCase):

    def test_structure(self):
        src = ms.gen_driver(42, 3, 5)
        self.assertIn("spawn('collector)", src)
        self.assertIn("spawn('worker)", src)
        self.assertIn("let cfg = recv()", src)      # first-message config
        self.assertIn("let ref = monitor(cp)", src)  # DOWN barrier
        self.assertIn("import str", src)
        self.assertIn("cons(15, nil)", src)          # collector total K*M
        self.assertIn("// multiset harness seed=42 K=3 M=5", src)

    def test_deterministic(self):
        self.assertEqual(ms.gen_driver(7, 4, 10), ms.gen_driver(7, 4, 10))

    def test_seed_sensitive(self):
        # spawn order and/or plens vary with seed
        self.assertNotEqual(ms.gen_driver(1, 4, 10), ms.gen_driver(2, 4, 10))

    def test_km_validation(self):
        with self.assertRaises(ValueError):
            ms.gen_driver(1, 0, 10)
        with self.assertRaises(ValueError):
            ms.gen_driver(1, 4, 0)
        with self.assertRaises(ValueError):
            ms.gen_driver(1, 2000, 1)


class ExpectedTest(unittest.TestCase):

    def test_cartesian(self):
        self.assertEqual(ms.expected_multiset(2, 3),
                         ["0 0", "0 1", "0 2", "1 0", "1 1", "1 2"])

    def test_size(self):
        self.assertEqual(len(ms.expected_multiset(4, 25)), 100)
        self.assertEqual(ms.expected_multiset(4, 25),
                         sorted(ms.expected_multiset(4, 25)))


class JudgeTest(unittest.TestCase):
    """Acceptance #3: judge fed fabricated stdout directly (no VM)."""

    EXP = ms.expected_multiset(2, 2)  # ["0 0","0 1","1 0","1 1"]

    def test_conserved(self):
        v = judge(["1 0", "0 1", "1 1", "0 0"])
        self.assertTrue(v["conserved"])
        self.assertEqual((v["dropped"], v["extra"], v["malformed"]),
                         ([], [], []))

    def test_dropped(self):
        v = judge(["0 0", "0 1", "1 0"])          # "1 1" lost
        self.assertFalse(v["conserved"])
        self.assertEqual(v["dropped"], ["1 1"])
        self.assertEqual((v["extra"], v["malformed"]), ([], []))

    def test_extra(self):
        v = judge(["0 0", "0 1", "1 0", "1 1", "7 7"])
        self.assertFalse(v["conserved"])
        self.assertEqual(v["extra"], ["7 7"])
        self.assertEqual((v["dropped"], v["malformed"]), ([], []))

    def test_duplicate_is_extra(self):
        v = judge(["0 0", "0 0", "0 1", "1 0", "1 1"])
        self.assertFalse(v["conserved"])
        self.assertEqual(v["extra"], ["0 0"])
        self.assertEqual(v["dropped"], [])

    def test_malformed(self):
        v = judge(["0 0", "garbage", "x 9", "1 0", "1 1"])
        self.assertFalse(v["conserved"])
        self.assertEqual(v["malformed"], ["garbage", "x 9"])
        self.assertEqual(v["dropped"], ["0 1"])
        self.assertEqual(v["extra"], [])

    def test_combined(self):
        v = judge(["0 1", "0 1", "oops", "9 9"])  # dup + malformed + dropped
        self.assertFalse(v["conserved"])
        self.assertEqual(v["malformed"], ["oops"])
        self.assertEqual(v["dropped"], ["0 0", "1 0", "1 1"])
        self.assertEqual(v["extra"], ["0 1", "9 9"])

    def test_empty(self):
        v = judge([])
        self.assertFalse(v["conserved"])
        self.assertEqual(v["dropped"], self.EXP)

    def test_order_independent(self):
        a = judge(["0 1", "1 0", "0 0", "1 1"])
        b = judge(["1 1", "0 0", "1 0", "0 1"])
        self.assertTrue(a["conserved"] and b["conserved"])


def judge(lines):
    return ms.judge(lines, JudgeTest.EXP)


@unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
class RunTest(unittest.TestCase):

    def test_live_conserved(self):
        case = ms.run_case(42, 2, 10)
        self.assertTrue(case["conserved"],
                        msg="categories=%s rc=%s stdout=%r"
                            % (case["categories"], case["rc"],
                               case["stdout"][-200:]))
        self.assertEqual(case["n_lines"], 20)
        self.assertFalse(case["timed_out"])

    def test_timeout_path(self):
        """Acceptance #4: spinning program hits the timeout -> finding."""
        case = ms.run_program(spin_source(), ["0 0"], timeout=1)
        self.assertFalse(case["conserved"])
        self.assertTrue(case["timed_out"])
        self.assertIn("timeout", case["categories"])
        self.assertIn("dropped", case["categories"])  # 丢消息与超时均报

    def test_empty_stdout_binary(self):
        """Under a stdout-less 'binary', every expected line is dropped."""
        case = ms.run_program(ms.gen_driver(1, 2, 2),
                              ms.expected_multiset(2, 2),
                              tinyactor=CAT)
        self.assertFalse(case["conserved"])
        self.assertEqual(case["verdict"]["dropped"],
                         ms.expected_multiset(2, 2))


@unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
class CliTest(unittest.TestCase):

    def _cli(self, *extra):
        return subprocess.run(
            [sys.executable, os.path.join(_HERE, "multiset.py")] + list(extra),
            capture_output=True)

    def test_conserved_matrix_exit0(self):
        with tempfile.TemporaryDirectory() as td:
            proc = self._cli("--seeds", "0:3", "--k", "2", "--m", "5",
                             "--findings-dir", td)
            self.assertEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"cases=3 conserved=3 findings=0 known=0",
                          proc.stderr)

    @unittest.skipUnless(TRUE, "no `true` binary on PATH")
    def test_finding_exit1(self):
        with tempfile.TemporaryDirectory() as td:
            proc = self._cli("--seeds", "0:2", "--k", "2", "--m", "5",
                             "--tinyactor", TRUE, "--findings-dir", td)
            self.assertEqual(proc.returncode, 1)
            self.assertIn(b"findings=1", proc.stderr)
            jsons = [f for f in os.listdir(td) if f.endswith(".json")]
            self.assertEqual(len(jsons), 1)
            with open(os.path.join(td, jsons[0])) as fh:
                doc = json.load(fh)
            self.assertEqual(doc["categories"], ["dropped"])
            self.assertEqual(doc["n_dropped"], 10)
            self.assertTrue(os.path.exists(
                os.path.join(td, doc["signature"] + ".ta")))

    def test_known_signature_not_rewritten(self):
        with tempfile.TemporaryDirectory() as td:
            for _ in range(2):
                proc = self._cli("--seeds", "0:1", "--k", "2", "--m", "5",
                                 "--tinyactor", TRUE, "--findings-dir", td)
                self.assertEqual(proc.returncode, 1)
            self.assertIn(b"findings=0 known=1", proc.stderr)
            jsons = [f for f in os.listdir(td) if f.endswith(".json")]
            self.assertEqual(len(jsons), 1)   # dedup: one file, not two

    def test_bad_args_rejected(self):
        for extra in (["--seeds", "5:5"],
                      ["--seeds", "bogus"],
                      ["--seeds", "0:2", "--k", "0"],
                      ["--seeds", "0:2", "--m", "x"]):
            proc = self._cli(*extra)
            self.assertNotEqual(proc.returncode, 0, extra)


if __name__ == "__main__":
    unittest.main(verbosity=2)