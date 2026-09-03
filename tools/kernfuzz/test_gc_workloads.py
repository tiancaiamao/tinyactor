# -*- coding: utf-8 -*-
r"""
test_gc_workloads.py — unit tests for tools/kernfuzz/gc_workloads.py
(task-workloads, docs/kernel-fuzzing-design.md §7.0-7.4).

Stdlib-only unittest, style follows tools/kernfuzz/test_gcstress.py.
Run:
    python3 tools/kernfuzz/test_gc_workloads.py
Exit 0 = all pass.  Requires ./tinyactor for the smoke tests (skipped
gracefully on a bare checkout).

Covers (task acceptance points):
  * W-pure strictly messaging-free: word-level text gate over >= 3 seeds,
    plus the gate function itself (raises on any actor primitive).
  * W-pure runtime determinism: two runs of the same seed's program are
    byte-identical; normal vs TA_GC_STRESS=1 byte-identical (the §7.2
    oracle premise, spot-checked on one seed).
  * W-msg first-message configuration pattern present in source
    (spawn('worker) zero-arg + worker `let cfg = recv()`); run output is
    exactly the K*M-line multiset {<wid> <seq>} (sorted comparison).
  * W-chaos completes (exit 0) and ends with the "done <R>" line.
  * seed reproducibility: generate() twice == byte-identical for all
    kinds; different seeds give different programs.
  * CLI: --kind/--seed/--scale stdout == generate(); bad kind/seed/scale
    rejected; --smoke exit code propagated.
"""

import os
import re
import subprocess
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))
sys.path.insert(0, _HERE)

import gc_workloads as gw          # noqa: E402

TINYACTOR = gw.TINYACTOR
SEEDS = (1, 42, 12345)

# actor primitives banned in W-pure (§7.0) — mirrors gw.PURE_BANNED_RE
BANNED_RE = re.compile(r"\b(send|spawn|monitor|recv|receive|self)\b")


def gen(kind, seed, scale=1):
    return gw.generate(kind, seed, scale)


def run_ta(text, timeout=300, env_extra=None):
    """Run `text` via ./tinyactor; return (rc, stdout, stderr) or None."""
    return gw.run_smoke(text, timeout=timeout) if env_extra is None else (
        _run_ta_env(text, timeout, env_extra))


def _run_ta_env(text, timeout, env_extra):
    import tempfile
    if not os.path.exists(TINYACTOR):
        return None
    with tempfile.NamedTemporaryFile("w", suffix=".ta",
                                     delete=False) as f:
        f.write(text)
        path = f.name
    try:
        env = dict(os.environ)
        env.update(env_extra)
        proc = subprocess.run([TINYACTOR, "run", path],
                              capture_output=True, timeout=timeout, env=env)
        return proc.returncode, proc.stdout, proc.stderr
    finally:
        os.unlink(path)


class PureGateTest(unittest.TestCase):
    """W-pure must be strictly messaging-free (§7.0 sequential premise)."""

    def test_no_actor_primitives_multi_seed(self):
        for seed in SEEDS + (7, 999983):
            text = gen("pure", seed)
            m = BANNED_RE.search(text)
            self.assertIsNone(m, "seed=%d contains %r" % (seed, m and m.group(0)))

    def test_gate_raises_on_primitives(self):
        for frag in ("send(pid, 1)", "spawn('w)", "spawn(fn { f() })",
                                          "monitor(pid)", "recv()", "receive { 'a -> 0 }",
                     "self()", "let r = recv ()"):
            with self.subTest(frag=frag):
                with self.assertRaises(ValueError):
                    gw.assert_no_messaging("fn main() { 0 } " + frag)

    def test_gate_accepts_clean_text(self):
        gw.assert_no_messaging("fn main() { print(1) }\n"
                               "// resend? no — the word sandwich is fine\n"
                               "fn append(xs, ys) { xs }")
        # note: "append" contains no banned word; "resend" is word-bounded

    def test_pure_shape_single_main_and_checks(self):
        text = gen("pure", 42)
        self.assertIn("fn main() {", text)
        self.assertGreaterEqual(text.count("print(str.concat("), 5,
                                "expected >= 5 labeled checksum phases")
        self.assertIn("import str", text)


class PureDeterminismTest(unittest.TestCase):
    """Same seed -> same program -> same output (§7.2 oracle premise)."""

    def test_generate_byte_identical(self):
        for seed in SEEDS:
            self.assertEqual(gen("pure", seed), gen("pure", seed))

    @unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
    def test_two_runs_byte_identical(self):
        text = gen("pure", 42)
        r1 = run_ta(text)
        r2 = run_ta(text)
        self.assertIsNotNone(r1)
        self.assertEqual(r1[0], 0)
        self.assertEqual(r1[1], r2[1])

    @unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
    def test_stress_equals_normal(self):
        """TA_GC_STRESS=1 output == normal output (§7.2, one seed)."""
        text = gen("pure", 42)
        base = run_ta(text)
        stress = _run_ta_env(text, 300, {"TA_GC_STRESS": "1"})
        self.assertEqual(base[0], 0)
        self.assertEqual(base[1], stress[1])


class MsgTest(unittest.TestCase):
    """W-msg: first-message config pattern + multiset output (§7.3)."""

    def test_generate_byte_identical(self):
        for seed in SEEDS:
            self.assertEqual(gen("msg", seed), gen("msg", seed))

    def test_first_message_config_pattern(self):
        for seed in SEEDS:
            text = gen("msg", seed)
            # spawn is zero-arg by name (R3); params travel via send
            self.assertIn("spawn('collector)", text)
            self.assertIn("spawn('worker)", text)
            # worker + collector take their parameters from recv() first
            self.assertIn("fn worker() {\n  let cfg = recv()", text)
            self.assertIn("fn collector() {\n  let cfg = recv()", text)
            # main wires config to each worker with its pid + m + plen
            self.assertRegex(text, r"send\(w0, cons\(cp, cons\(\d+, .*\)\)\)")
            # main waits for collector termination via monitor + recv
            self.assertIn("let ref = monitor(cp)", text)

    def test_multiset_output_matches_KxM(self):
        """Sorted stdout == exactly the K*M lines "<wid> <seq>"."""
        seed = 42
        text = gen("msg", seed)
        res = run_ta(text)
        self.assertIsNotNone(res)
        rc, out, _ = res
        self.assertEqual(rc, 0)
        lines = out.decode().strip().splitlines()
        self.assertGreater(len(lines), 0)
        # recover K, M from the generated main (send(cp, cons(TOTAL, nil)))
        # and count distinct workers; expected pairs derived independently
        m = re.search(r"send\(cp, cons\((\d+), nil\)\)", text)
        total = int(m.group(1))
        k = len(re.findall(r"spawn\('worker\)", text))
        m_per = total // k
        self.assertEqual(k * m_per, total)
        expected = sorted("%d %d" % (w, s)
                          for w in range(k) for s in range(m_per))
        self.assertEqual(sorted(lines), expected)
        self.assertEqual(len(lines), total)


class ChaosTest(unittest.TestCase):
    """W-chaos: high spawn/death churn, deterministic round lines (§7.4)."""

    def test_generate_byte_identical(self):
        for seed in SEEDS:
            self.assertEqual(gen("chaos", seed), gen("chaos", seed))

    def test_first_message_config_pattern(self):
        text = gen("chaos", 7)
        self.assertIn("spawn('leaf)", text)
        # leaf takes [alloc_n, sub, die] from recv() first; death branch
        # uses a runtime divzero, never a literal 1/0
        self.assertIn("let cfg = recv()", text)
        self.assertIn("let boom = n / z", text)
        self.assertNotIn("/ 0", text)

    @unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
    def test_completes_with_done_line(self):
        text = gen("chaos", 42)
        res = run_ta(text)
        self.assertIsNotNone(res)
        rc, out, _ = res
        self.assertEqual(rc, 0)
        rounds = int(re.search(r"rounds_loop\(0, (\d+),", text).group(1))
        lines = out.decode().strip().splitlines()
        self.assertEqual(lines, ["round %d" % r for r in range(rounds)]
                         + ["done %d" % rounds])


class ReproTest(unittest.TestCase):
    """Seeded generation: reproducibility + seed sensitivity."""

    def test_all_kinds_deterministic(self):
        for kind in gw.KINDS:
            self.assertEqual(gen(kind, 5), gen(kind, 5), kind)

    def test_different_seeds_differ(self):
        for kind in gw.KINDS:
            self.assertNotEqual(gen(kind, 1), gen(kind, 2), kind)

    def test_scale_changes_output(self):
        self.assertNotEqual(gen("pure", 5, 1), gen("pure", 5, 2))

    def test_seed_domain(self):
        with self.assertRaises(ValueError):
            gen("pure", -1)
        with self.assertRaises(ValueError):
            gen("pure", 1 << 64)


class CliTest(unittest.TestCase):

    def test_stdout_matches_generate(self):
        for kind in gw.KINDS:
            proc = subprocess.run(
                [sys.executable, os.path.join(_HERE, "gc_workloads.py"),
                 "--kind", kind, "--seed", "42", "--scale", "1"],
                capture_output=True)
            self.assertEqual(proc.returncode, 0, kind)
            self.assertEqual(proc.stdout.decode(), gen(kind, 42), kind)

    def test_bad_args_rejected(self):
        for extra in (["--kind", "bogus", "--seed", "1"],
                      ["--kind", "pure", "--seed", "1", "--scale", "0"],
                      ["--kind", "pure", "--seed", "-5"]):
            proc = subprocess.run(
                [sys.executable, os.path.join(_HERE, "gc_workloads.py")]
                + extra, capture_output=True)
            self.assertNotEqual(proc.returncode, 0, extra)

    @unittest.skipUnless(os.path.exists(TINYACTOR), "./tinyactor not built")
    def test_smoke_flag_ok(self):
        proc = subprocess.run(
            [sys.executable, os.path.join(_HERE, "gc_workloads.py"),
             "--kind", "pure", "--seed", "42", "--smoke"],
            capture_output=True)
        self.assertEqual(proc.returncode, 0)
        self.assertIn(b"gc-workloads-SMOKE-OK", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)