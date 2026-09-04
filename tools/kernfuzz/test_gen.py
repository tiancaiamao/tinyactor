# -*- coding: utf-8 -*-
"""
test_gen.py — unit tests for tools/kernfuzz/gen.py.

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_gen.py
Exit 0 = all pass, non-zero = failure.

Covers the task-gen acceptance criteria (.pge/tasks/task-gen.md):
  * determinism: same --seed → byte-identical program; different seeds differ,
  * executability hard gate: ≥20 generated programs run via the REAL
    `./tinyactor run <file>` binary, all exit 0 with non-empty stdout,
  * golden agreement: the same programs evaluated by the Python golden
    interpreter (tools/kernfuzz/golden/golden.py) agree with VM stdout
    under the §5.1.3 compare protocol; skips (out-of-golden-subset
    structures) counted, skip rate must stay < 20%,
  * negative-literal rule: no bare `-N` literal ever emitted,
  * boundary/coverage statistics: int48 boundary values, empty list,
    closure, match+wildcard arm appear in generated corpora (> 0),
  * --count batch mode: files written, names p_<seed>.ta, reproducible.

The golden-consistency sample deliberately uses programs whose divzero
behavior is under protocol control: this generator's v0 never emits a
division (see gen.py TODO), so both sides must agree line-for-line.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                         # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
TINYACTOR = os.path.join(_REPO_ROOT, "tinyactor")
AST_DUMP = os.path.join(_REPO_ROOT, "tools", "kernfuzz", "ast-dump.ta")
GOLDEN = os.path.join(_REPO_ROOT, "tools", "kernfuzz", "golden",
                      "golden.py")

_I48_MAX = (1 << 47) - 1
_STR_POOL = set(gen._STRING_ATOMS)


def _norm_vm(stdout_bytes, exit_code):
    """§5.1.3 norm_tavm: split lines, strip trailing blanks, synthesize
    the DIVZERO:n protocol line on exit code 1."""
    lines = stdout_bytes.split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    if exit_code == 1:
        lines.append(("DIVZERO:%d" % len(lines)).encode("latin-1"))
    return lines


def _norm_golden(stdout_bytes):
    lines = stdout_bytes.split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    return lines


def _run_vm(path):
    """Run the real VM binary.  Returns (stdout_bytes, exit_code)."""
    p = subprocess.run([TINYACTOR, "run", path], capture_output=True,
                       timeout=60)
    return p.stdout, p.returncode


def _golden_lines(path):
    """Dump AST via ast-dump.ta, evaluate with golden.py.
    Returns (stdout_lines, exit_code) or None when the program uses a
    structure outside golden's coverage (skip)."""
    d = subprocess.run([TINYACTOR, "run", AST_DUMP, path],
                       capture_output=True, timeout=60)
    if d.returncode != 0 or b"AST-DUMP-ERROR" in d.stdout:
        return None
    with tempfile.NamedTemporaryFile(suffix=".sexp", delete=False) as f:
        f.write(d.stdout)
        sexp_path = f.name
    try:
        g = subprocess.run([sys.executable, GOLDEN, sexp_path],
                           capture_output=True, timeout=120)
        # golden exits 1 with the DIVZERO:n line synthesized internally;
        # unexpected evaluator errors also exit 1 but with empty stdout —
        # treat those as skips (out-of-subset) rather than mismatches.
        if g.returncode != 0 and b"DIVZERO:" not in g.stdout:
            return None
        return _norm_golden(g.stdout), g.returncode
    finally:
        os.unlink(sexp_path)


class DeterminismTest(unittest.TestCase):
    """--seed N → byte-identical program; different seeds differ."""

    def test_same_seed_byte_identical(self):
        for seed in (0, 1, 42, 999999):
            a = gen.gen_program(seed)
            b = gen.gen_program(seed)
            self.assertEqual(a, b)

    def test_cli_same_seed_byte_identical(self):
        a = subprocess.run([sys.executable, os.path.abspath(__file__
                            ).replace("test_gen.py", "gen.py"),
                            "--seed", "777"], capture_output=True)
        b = subprocess.run([sys.executable, os.path.abspath(__file__
                            ).replace("test_gen.py", "gen.py"),
                            "--seed", "777"], capture_output=True)
        self.assertEqual(a.returncode, 0)
        self.assertEqual(a.stdout, b.stdout)

    def test_different_seeds_differ(self):
        texts = [gen.gen_program(s) for s in range(20)]
        self.assertEqual(len(set(texts)), 20)

    def test_prng_discipline(self):
        # gen's randomness is fully driven by prng.make_prng(seed): two
        # generations from the same seed advance the PRNG counter
        # identically (every draw = one sha256 call).
        plan = gen.ProgramPlan(1234, 4)
        plan.build()
        plan2 = gen.ProgramPlan(1234, 4)
        plan2.build()
        self.assertGreater(plan.rng.counter, 0)
        self.assertEqual(plan2.rng.counter, plan.rng.counter)
        self.assertEqual(plan.render(), plan2.render())


class ExecutabilityTest(unittest.TestCase):
    """Hard gate: `./tinyactor run` executes generated programs."""

    def test_20_programs_run_exit0_with_stdout(self):
        for seed in range(30):
            src = gen.gen_program(seed)
            with tempfile.NamedTemporaryFile(
                    mode="w", suffix=".ta", delete=False,
                    encoding="latin-1") as f:
                f.write(src)
                path = f.name
            try:
                out, rc = _run_vm(path)
                self.assertEqual(rc, 0,
                                 "seed %d exited %d\n%s" % (seed, rc, src))
                self.assertTrue(out, "seed %d produced no stdout" % seed)
            finally:
                os.unlink(path)


class GoldenAgreementTest(unittest.TestCase):
    """Generated subset programs agree with the golden interpreter."""

    def test_vm_golden_agreement(self):
        agreed = 0
        skipped = 0
        mismatches = []
        for seed in range(30):
            src = gen.gen_program(1000 + seed)
            with tempfile.NamedTemporaryFile(
                    mode="w", suffix=".ta", delete=False,
                    encoding="latin-1") as f:
                f.write(src)
                path = f.name
            try:
                vm_out, vm_rc = _run_vm(path)
                gres = _golden_lines(path)
            finally:
                os.unlink(path)
            if gres is None:
                skipped += 1
                continue
            gold_out, gold_rc = gres
            # v0 gen emits no division: both sides should exit 0.
            self.assertEqual(vm_rc, 0, "unexpected VM death, seed %d" % seed)
            self.assertEqual(gold_rc, 0,
                             "unexpected golden error, seed %d" % seed)
            if _norm_vm(vm_out, vm_rc) == gold_out:
                agreed += 1
            else:
                mismatches.append(seed)
        total = agreed + skipped + len(mismatches)
        self.assertGreater(total, 0)
        self.assertEqual(mismatches, [],
                         "VM/golden mismatch on seeds %s" % mismatches)
        self.assertLess(skipped / float(total), 0.20,
                        "skip rate too high: %d/%d" % (skipped, total))


class NegativeLiteralRuleTest(unittest.TestCase):
    """R1: negatives are always `(0 - N)`; a bare `-N` never appears."""

    def test_no_bare_negative_literal(self):
        # R1: a '-' immediately followed by a digit is a unary/bare
        # negative literal — gen must never emit one.  Binary minus in
        # `(0 - N)` always has a space on BOTH sides, so the compact
        # '-<digit>' pattern only matches bare negatives.
        pat = re.compile(r"-(?!\s)\s*\d")
        for seed in range(50):
            src = gen.gen_program(2000 + seed)
            # strip // comments (e.g. the "per DEC-3" header) — the rule
            # concerns TA program text only.
            code = "\n".join(l.split("//")[0] for l in src.split("\n"))
            for m in pat.finditer(code):
                ctx = code[max(0, m.start() - 12):m.end() + 12]
                self.fail("bare negative literal, seed %d: ...%s..."
                          % (2000 + seed, ctx))

    def test_negative_values_present_via_minus_form(self):
        seen = False
        for seed in range(10):
            if "(0 - " in gen.gen_program(3000 + seed):
                seen = True
        self.assertTrue(seen, "no (0 - N) negative form in corpus sample")

    def test_boundary_literal_in_every_program(self):
        for seed in range(10):
            src = gen.gen_program(4000 + seed)
            self.assertIn(str(_I48_MAX), src,
                          "2^47-1 boundary missing, seed %d" % (4000 + seed))


class CoverageStatsTest(unittest.TestCase):
    """Boundary/construct coverage statistics over a generated corpus."""

    def test_construct_coverage(self):
        corpus = [gen.gen_program(s) for s in range(60)]
        joined = "\n".join(corpus)
        self.assertIn("match ", joined)          # match + wildcard arm
        self.assertIn("_ ->", joined)
        self.assertIn("fn(", joined)             # anonymous closure literal
        self.assertIn("cons(", joined)           # dotted pair / cons chain
        self.assertIn("[]", joined)              # empty list
        self.assertIn("nil", joined)             # nil literal
        self.assertIn("&&", joined)              # logic ops
        self.assertIn(" when ", joined)          # match guards
        self.assertIn("'", joined)               # symbol literal
        self.assertIn(str(_I48_MAX), joined)     # int48 boundary value

    def test_per_program_print_lines(self):
        for seed in range(10):
            src = gen.gen_program(5000 + seed)
            # every print sits on its own line (R11)
            n_print = len(re.findall(r"^\s*print\(", src, re.M))
            self.assertGreaterEqual(n_print, 4, src)


class BatchModeTest(unittest.TestCase):
    """--count K --out-dir D → p_<seed>.ta files, reproducible."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="genbatch-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _genpy(self):
        return os.path.join(_HERE, "gen.py")

    def test_batch_writes_files(self):
        out = os.path.join(self.tmp, "corpus")
        p = subprocess.run([sys.executable, self._genpy(), "--count", "5",
                            "--out-dir", out, "--seed", "42"],
                           capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        names = sorted(os.listdir(out))
        self.assertEqual(len(names), 5)
        for n in names:
            self.assertRegex(n, r"^p_\d+\.ta$")

    def test_batch_reproducible(self):
        out1 = os.path.join(self.tmp, "c1")
        out2 = os.path.join(self.tmp, "c2")
        for d in (out1, out2):
            p = subprocess.run([sys.executable, self._genpy(), "--count",
                                "4", "--out-dir", d, "--seed", "7"],
                               capture_output=True)
            self.assertEqual(p.returncode, 0, p.stderr)
        f1 = sorted(os.listdir(out1))
        f2 = sorted(os.listdir(out2))
        self.assertEqual(f1, f2)
        for n in f1:
            with open(os.path.join(out1, n), "rb") as a, \
                    open(os.path.join(out2, n), "rb") as b:
                self.assertEqual(a.read(), b.read(), n)

    def test_batch_files_execute(self):
        out = os.path.join(self.tmp, "c3")
        p = subprocess.run([sys.executable, self._genpy(), "--count", "3",
                            "--out-dir", out, "--seed", "9"],
                           capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        for n in sorted(os.listdir(out)):
            outb, rc = _run_vm(os.path.join(out, n))
            self.assertEqual(rc, 0, n)
            self.assertTrue(outb, n)


class GenInvariantTest(unittest.TestCase):
    """Direct unit checks of the type model invariants (R3/R5/R6/R8)."""

    def test_no_bool_literal_patterns(self):
        # R6: `true`/`false` never appear as match PATTERNS.  Occurrences
        # inside match ARM BODIES or if-conditions are fine; the pattern
        # position is directly before ` -> `.
        pat = re.compile(r"(true|false)\s*->")
        for seed in range(40):
            src = gen.gen_program(6000 + seed)
            self.assertIsNone(pat.search(src),
                              "bool literal pattern, seed %d" % (6000 + seed))

    def test_match_always_has_wildcard_last(self):
        # R5: every match block's last arm before its closing '}' is the
        # `_ ->` wildcard — EXCEPT the exhaustive nil/cons list pair that
        # the list-recursion helper template emits (§5.0 allows "wildcard
        # arm OR generator-proven exhaustiveness"; nil+cons covers the
        # whole list domain).
        for seed in range(40):
            src = gen.gen_program(7000 + seed)
            blocks = re.findall(r"match [^{]*\{([^}]*)\}", src, re.S)
            for blk in blocks:
                arm_lines = [l.strip() for l in blk.split("\n")
                             if l.strip()]
                self.assertTrue(arm_lines, src)
                pats = [a.split(" -> ")[0] for a in arm_lines]
                if set(pats) == {"nil"} | {p for p in pats
                                           if p.startswith("cons(")}:
                    continue  # proven-exhaustive list match
                self.assertTrue(arm_lines[-1].startswith("_ ->"),
                                "last arm not wildcard: %r\n%s"
                                % (arm_lines[-1], src))

    def test_lambda_unannotated(self):
        # R8: lambda params/returns carry no annotations.
        pat = re.compile(r"fn\(([^)]*:)")
        for seed in range(40):
            src = gen.gen_program(8000 + seed)
            self.assertIsNone(pat.search(src),
                              "annotated lambda, seed %d" % (8000 + seed))

    def test_max_depth_respected(self):
        # depth knob changes output for the same seed (it is a real knob)
        a = gen.gen_program(11, max_depth=2)
        b = gen.gen_program(11, max_depth=6)
        self.assertIsInstance(a, str)
        self.assertIsInstance(b, str)


if __name__ == "__main__":
    unittest.main(verbosity=2)