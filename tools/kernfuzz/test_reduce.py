# -*- coding: utf-8 -*-
"""test_reduce.py — tests for the §5.5 reducer (kernel-fuzzing DELIV-4).

Core case: the REAL anchor-crash material (morph seed 1000051, the
match-guard / string+int VM-vs-golden divergence) must reduce to a
program that still trips the same-class divergence, with the line count
recorded before/after (≤15 lines is best-effort, NOT a hard gate).

Also covered: the mismatch root-cause criterion (same first differing
output-line index) — both at unit level (fabricated observations) and
end-to-end on a hand-built minimal VM/golden divergence program — plus
per-strategy "criterion preserved" assertions, budget/termination
safety, and the pure text helpers.

Runner logic is imported from morph.py; the reducer is imported from
reduce.py — no duplicated call card anywhere.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import morph                                  # noqa: E402
import reduce as reduce_mod                   # noqa: E402

ANCHOR_DIR = "/tmp/morph-200/anchor-crash-n"
ANCHOR_SEED = 1000051

# hand-built minimal VM/golden divergence (string element summed by a
# list-recursion match — same heterogenous-add root as the anchor seed).
MINIMAL_DIVERGENT = """fn sum(l_1) -> int {
match l_1 {
  nil -> 0,
  cons(h_1, t_1) -> (h_1 + sum(t_1))
}
}

fn main() {
  print(sum(["x"]));
}
"""


def anchor_material():
    """Path to the anchor-crash finding dir; regenerates it via the
    morph runner (never a copy of its logic) when /tmp was wiped."""
    if os.path.isdir(ANCHOR_DIR):
        return ANCHOR_DIR
    out = "/tmp/morph-reduce-regen"
    if not os.path.isdir(os.path.join(out, "anchor-crash-n")):
        env = dict(os.environ)
        env["PYTHONPATH"] = _HERE + os.pathsep + env.get("PYTHONPATH", "")
        p = subprocess.run(
            [sys.executable, os.path.join(_HERE, "morph.py"),
             "--seeds", "%d..%d" % (ANCHOR_SEED, ANCHOR_SEED),
             "--out", out],
            capture_output=True, timeout=300, env=env)
        if p.returncode != 0:
            sys.stderr.write(p.stderr.decode("latin-1"))
            return None
    d = os.path.join(out, "anchor-crash-n")
    return d if os.path.isdir(d) else None


class ReduceTestBase(unittest.TestCase):
    def setUp(self):
        self._tmpdirs = []
        self.runner = morph.Runner(self._mktmp())
        self._reducers = []

    def tearDown(self):
        for d in self._tmpdirs:
            shutil.rmtree(d, ignore_errors=True)

    def _mktmp(self):
        d = tempfile.mkdtemp(prefix="reduce-test-")
        self._tmpdirs.append(d)
        return d

    def make_reducer(self, category, max_evals=300, budget_s=240):
        red = reduce_mod.Reducer(
            category, self.runner,
            reduce_mod.Budget(max_evals, budget_s))
        self._reducers.append(red)
        return red


class TestAnchorCrashRealMaterial(ReduceTestBase):
    """DELIV-3#4 / DELIV-4 acceptance: the real finding shrinks while
    the same-class divergence keeps firing."""

    def test_full_reduction(self):
        fdir = anchor_material()
        if fdir is None:
            self.skipTest("anchor material unavailable and regen failed")
        with open(os.path.join(fdir, "src_E0.ta"), "r",
                  encoding="latin-1") as f:
            src = f.read()
        n0 = src.count("\n") + 1

        red = self.make_reducer("anchor-crash")
        base_obs, feature = red.establish_baseline(src)
        self.assertEqual(feature, "divergence")

        reduced, traj = reduce_mod.reduce_source(red, src,
                                                 lambda m: None)
        n1 = reduced.count("\n") + 1
        # criterion preserved on the final program (re-checked fresh)
        final_ok = red.reproduces(red.observe(reduced))
        self.assertTrue(final_ok,
                        "reduced program lost the anchor divergence")
        self.assertLess(n1, n0)
        # significant shrink: the record for progress.md
        self.assertLessEqual(n1, max(15, n0 // 2),
                             "expected a substantial reduction")
        self.assertTrue(traj.steps, "empty reduction trajectory")
        sys.stderr.write(
            "anchor-crash reduction: %d lines -> %d lines, %d evals\n"
            % (n0, n1, red.budget.evals))


class TestCriterionPreservedPerStrategy(ReduceTestBase):
    """整行删除 / 子树替换 each get a 'criterion holds before AND after'
    assertion, plus a negative control showing the criterion can tell."""

    def setUp(self):
        ReduceTestBase.setUp(self)
        fdir = anchor_material()
        if fdir is None:
            self.skipTest("anchor material unavailable")
        with open(os.path.join(fdir, "src_E0.ta"), "r",
                  encoding="latin-1") as f:
            self.src = f.read()
        self.red = self.make_reducer("anchor-crash")
        self.red.establish_baseline(self.src)

    def _reproduces(self, text):
        return self.red.reproduces(self.red.observe(text))

    def test_line_deletion_keeps_criterion(self):
        lines = self.src.split("\n")
        # drop the two trailing prints: outputs after the diverging line
        # vanish, the divergence itself stays
        gone = [ln for ln in lines
                if "print(140737488355327);" in ln
                or "print((0 - 140737488355327) - 1);" in ln]
        self.assertEqual(len(gone), 2)
        cand = "\n".join(ln for ln in lines if ln not in gone)
        self.assertTrue(self._reproduces(cand))
        # negative control: removing the diverging print kills it
        kill = [ln for ln in lines if "print(helper_2(l_2))" in ln]
        self.assertEqual(len(kill), 1)
        cand2 = "\n".join(ln for ln in lines
                          if ln != kill[0])
        self.assertFalse(self._reproduces(cand2))

    def test_expr_to_literal_keeps_criterion(self):
        groups = reduce_mod.find_paren_groups(self.src)
        self.assertTrue(groups)
        s, e = max(groups, key=lambda g: g[1] - g[0])   # longest subtree
        cand = self.src[:s] + "0" + self.src[e:]
        self.assertNotEqual(cand, self.src)
        self.assertTrue(self._reproduces(cand),
                        "largest-expr->0 unexpectedly broke criterion")


class TestMismatchCriterion(ReduceTestBase):
    """mismatch 复现判据：差异行位置相同 (§5.5)."""

    def _obs(self, vm_lines, golden_lines):
        vm = morph.RunResult(
            "\n".join(vm_lines).encode("latin-1") + b"\n", b"", 0, False)
        gold = [ln.encode("latin-1") for ln in golden_lines]
        return reduce_mod.Observation(True, b"", vm, gold)

    def test_unit_level(self):
        red = self.make_reducer("mismatch")
        red.feature = 0        # baseline: outputs diverge at line 0
        self.assertTrue(red.reproduces(
            self._obs(["999"], ["0"])))            # same position
        self.assertFalse(red.reproduces(
            self._obs(["ok", "999"], ["ok", "0"])))  # position moved
        self.assertFalse(red.reproduces(
            self._obs(["0"], ["0"])))              # no divergence
        self.assertFalse(red.reproduces(
            reduce_mod.Observation(True, b"",
                                   morph.RunResult(b"999\n", b"", 0,
                                                   False),
                                   None)))          # golden side dead

    def test_end_to_end_synthetic(self):
        red = self.make_reducer("mismatch")
        base_obs, feature = red.establish_baseline(MINIMAL_DIVERGENT)
        self.assertEqual(feature, 0)   # diverges at output line 0

        reduced, traj = reduce_mod.reduce_source(red, MINIMAL_DIVERGENT,
                                                 lambda m: None)
        self.assertTrue(red.reproduces(red.observe(reduced)),
                        "mismatch criterion lost after reduction")
        obs = red.observe(reduced)
        idx = reduce_mod.first_diff_index(
            morph.norm_tavm(obs.vm.out, obs.vm.rc), obs.golden)
        self.assertEqual(idx, 0)       # same root-cause position
        sys.stderr.write(
            "mismatch synthetic reduction: %d -> %d lines\n"
            % (MINIMAL_DIVERGENT.count("\n") + 1,
               reduced.count("\n") + 1))


class TestTerminationAndSafety(ReduceTestBase):
    def test_zero_budget_returns_original(self):
        red = self.make_reducer("anchor-crash", max_evals=0, budget_s=60)
        out, traj = reduce_mod.reduce_source(red, MINIMAL_DIVERGENT,
                                             lambda m: None)
        self.assertEqual(out, MINIMAL_DIVERGENT)   # nothing stuck: original

    def test_stale_input_refused(self):
        red = self.make_reducer("anchor-crash")
        with self.assertRaises(reduce_mod.ReduceError):
            # a clean program has no divergence to establish
            red.establish_baseline("fn main() {\n  print(1);\n}\n")

    def test_unknown_category_rejected(self):
        with self.assertRaises(reduce_mod.ReduceError):
            self.make_reducer("no-such-category")


class TestTextHelpers(unittest.TestCase):
    def test_find_paren_groups_skips_strings_and_comments(self):
        src = ('let a = (f (x) 1); // ( comment ( paren\n'
               'let s = "str ( with ) parens";\n'
               'let b = (g 2);\n')
        spans = reduce_mod.find_paren_groups(src)
        texts = [src[s:e] for s, e in spans]
        # top-level groups only (nested ones are covered by their
        # parent's replacement); strings and comments are skipped
        self.assertEqual(texts, ["(f (x) 1)", "(g 2)"])

    def test_first_diff_index(self):
        self.assertEqual(reduce_mod.first_diff_index(
            [b"a", b"b"], [b"a", b"c"]), 1)
        self.assertEqual(reduce_mod.first_diff_index(
            [b"a"], [b"a", b"b"]), 1)
        self.assertIsNone(reduce_mod.first_diff_index(
            [b"a"], [b"a"]))
    def test_crash_feature_normalization_m12(self):
        err = b"AddressSanitizer: SEGV on unknown address 0x00010a3f0018"
        self.assertEqual(reduce_mod.norm_crash_feature(err),
                         "AddressSanitizer: SEGV on unknown address 0xADDR")


class TestCLI(unittest.TestCase):
    def test_category_required_for_file_input(self):
        with tempfile.NamedTemporaryFile(suffix=".ta", delete=False) as f:
            f.write(b"fn main() {\n}\n")
            path = f.name
        try:
            p = subprocess.run(
                [sys.executable, os.path.join(_HERE, "reduce.py"), path],
                capture_output=True, timeout=60)
            self.assertEqual(p.returncode, 2)
            self.assertIn(b"--category", p.stderr)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()