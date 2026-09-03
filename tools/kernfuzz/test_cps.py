#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_cps.py — tests for tools/kernfuzz/cps.py (DELIV-11, §5.2 Tier C).

Layers:
  * TestDirectional   — ≥10 hand-built dump-AST cases, each pinning ONE
                        transformation behaviour (continuation threading,
                        let/if/match/begin lowering, argument ordering,
                        fn-value calls, negative literals, ...).  Pure
                        Python: embedded s-expr, no binaries needed.
  * TestUnsupported   — every subset-exit construct is explicitly rejected
                        (Unsupported with the construct named).
  * TestHygiene       — fresh continuation/value names never collide with
                        program identifiers (gen uses k_N for lambda
                        params).
  * TestDeterminism   — transform is a pure function of the input tree.
  * TestOutputForm    — annotation iron rule: no `: type` params, no
                        type-sig in output; main stays 0-arity.
  * TestLive          — runner-backed: 4-way identity check (A/G/R/GC) on
                        hand programs + 3 gen corpus seeds + the 10^5-deep
                        tail-recursion TCO rider.  Skipped when the
                        toolchain binaries are absent.

Run: python3 tools/kernfuzz/test_cps.py
"""

import os
import re
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import sexp
from sexp import sexp_read_string
import cps
from cps import Unsupported, cps_transform, CPSTransformer

_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))


def tr(sexp_text):
    """Transform an embedded dump s-expr (helper for directional tests).
    The whole-file dump is ONE list of forms — wrap accordingly."""
    return cps_transform(sexp_read_string("(" + sexp_text + ")"))


def fn_def(name, params, body_sexp):
    """Dump shape of `(define (name params...) body)` as s-expr text."""
    return "(define (%s %s) %s)" % (name, params, body_sexp)


# ---------------------------------------------------------------------------
# directional transform cases
# ---------------------------------------------------------------------------

class TestDirectional(unittest.TestCase):
    """One behaviour per case; assertions on the rendered TA source."""

    def test_01_atom_thunk(self):
        # literal value flows into the continuation
        out = tr(fn_def("main", "", "(print 42)"))
        self.assertIn("(print(42))", out)

    def test_02_recursive_tail_call_gets_k(self):
        # down(n) tail-calls itself: the k parameter is threaded through
        out = tr("(define (down n) (if (<= n 0) 0 (down (- n 1))))")
        self.assertRegex(out, r"fn down\(n, k_\d+\)")
        self.assertRegex(out, r"down\(\(n - 1\), k_\d+\)")

    def test_03_non_tail_call_gets_fresh_k_closure(self):
        # (print (down (- n 1))) — non-tail: result bound to v_N
        out = tr("(define (down n) (if (<= n 0) 0 "
                 "(print (down (- n 1)))))")
        self.assertRegex(out, r"down\(\(n - 1\), fn\(v_\d+\)")

    def test_04_if_branches_share_hoisted_k(self):
        # non-atom k (a closure) used in two branches must be bound ONCE
        # via a kk_N let, not duplicated into both branches.  Inside a
        # plain fn the continuation is a variable, so use main's terminal
        # closure continuation to exercise the hoist.
        out = tr("(define (h x) x) (define (main) (if (h 0) (print 1) (print 2)))")
        m = re.search(r"let (kk_\d+) =", out)
        self.assertIsNotNone(m)
        # the hoisted variable is referenced in a branch body
        self.assertGreaterEqual(out.count(m.group(1) + "("), 1)

    def test_05_impure_condition_binds_c(self):
        # if with a call in the condition: condition evaluated first into
        # c_N, then branched
        out = tr("(define (h x) x) (define (f) (if (h 1) (print 1) (print 2)))")
        self.assertRegex(out, r"h\(1, fn\(c_\d+\)")
        self.assertIn("if (c_", out)

    def test_06_impure_match_scrutinee_binds_s(self):
        out = tr("(define (h x) x) (define (f) (match (h 1) (0 (print 1)) "
                 "(_ (print 2))))")
        self.assertRegex(out, r"h\(1, fn\(s_\d+\)")
        self.assertIn("match s_", out)

    def test_07_match_patterns(self):
        out = tr("(define (f x) (match x (0 (print 100)) "
                 "((cons h t) (print h)) (n (print n))))")
        self.assertIn("match x {", out)
        self.assertIn("0 ->", out)
        self.assertIn("cons(h, t) ->", out)
        self.assertIn("n ->", out)

    def test_08_match_guard_when(self):
        # (pat guard body) arm shape with a PURE guard
        out = tr("(define (f x) (match x (n (> n 0) (print n)) "
                 "(_ (print 0))))")
        self.assertIn("n when ((n > 0)) ->", out)
        self.assertIn("_ ->", out)

    def test_09_begin_statement_order(self):
        # begin: pure prints stay as statements, in order
        out = tr("(define (main) (begin (print 1) (print 2) (print 3)))")
        self.assertIn("begin", out)
        self.assertLess(out.index("print(1)"), out.index("print(2)"))
        self.assertLess(out.index("print(2)"), out.index("print(3)"))

    def test_10_let_threaded_and_stmt_let(self):
        # threaded let (let x v body) and 2-arg statement let both lower
        # to `let x = v;` statement form
        out = tr("(define (main) (let a 1 (begin (print a) "
                 "(let b 2) (print b))))")
        self.assertIn("let a = 1;", out)
        self.assertIn("let b = 2;", out)

    def test_11_impure_let_value_binds_v(self):
        out = tr("(define (h x) x) (define (main) (begin (let x (h 1)) (print x)))")
        self.assertRegex(out, r"h\(1, fn\(v_\d+\)")
        self.assertIn("let x = v_", out)

    def test_12_left_to_right_argument_order(self):
        # h(a, b): the LEFT operand's continuation is OUTERMOST — g(1)'s
        # result is bound to v_N (inside g(1)'s closure) BEFORE g(2) runs
        out = tr("(define (g x) x) (define (h a b) a) (define (main) (print (h (g 1) (g 2))))")
        i_g1 = out.index("g(1,")
        i_g2 = out.index("g(2,")
        self.assertLess(i_g1, i_g2)
        self.assertIn("fn(v_", out[i_g1:i_g2])

    def test_13_fn_value_param_call_appends_k(self):
        # gen's apply1 shape: unannotated param f called as f(x)
        out = tr("(define (apply1 f x) (f x))")
        self.assertRegex(out, r"fn apply1\(f, x, k_\d+\)")
        self.assertRegex(out, r"f\(x, k_\d+\)")

    def test_14_let_bound_lambda_called(self):
        out = tr("(define (main) (let f (lambda (z) (+ z 1) nil) "
                 "(begin (print (f 41)))))")
        # the lambda gains a k param and the call appends a continuation
        self.assertRegex(out, r"let f = fn\(z, k_\d+\)")
        self.assertRegex(out, r"f\(41, fn\(v_\d+\)")

    def test_15_chained_call(self):
        out = tr("(define (g x) x) (define (main) (print ((g 1) 2)))")
        # callee bound to h_N, then applied with the continuation
        self.assertRegex(out, r"g\(1, fn\(h_\d+\)")
        self.assertRegex(out, r"h_\d+\(2, ")

    def test_16_negative_literal_r1(self):
        out = tr("(define (main) (print (- 0 5)))")
        self.assertIn("(0 - 5)", out)

    def test_17_ctor_call_and_pattern(self):
        src = ("(type Shape nil (Circle int int)) "
               + fn_def("area", "s", "(match s ((Circle r m) (* r m)))")
               + " " + fn_def("main", "",
                              "(print (area (cons (quote Circle) "
                              "(cons 3 (cons 4 nil)))))"))
        out = tr(src)
        self.assertIn("type Shape { Circle(int, int) }", out)
        self.assertIn("Circle(r, m) ->", out)
        self.assertRegex(out, r"area\(Circle\(3, 4\), fn\(v_\d+\)")

    def test_18_string_and_escape(self):
        out = tr('(define (main) (print "a\\"b"))')
        self.assertIn('print("a\\"b")', out)

    def test_19_lambda_as_argument_gains_k(self):
        out = tr("(define (apply1 f x) (f x)) (define (main) "
                 "(print (apply1 (lambda (y) (* y 2) nil) 5)))")
        self.assertIn("apply1(fn(y, k_", out)

    def test_20_bare_top_level_fn_as_argument(self):
        out = tr("(define (apply1 f x) (f x)) (define (g x) x) "
                 "(define (main) (print (apply1 g 9)))")
        self.assertIn("apply1(g, 9,", out)


# ---------------------------------------------------------------------------
# unsupported constructs: explicit rejection
# ---------------------------------------------------------------------------

class TestUnsupported(unittest.TestCase):
    def rejects(self, sexp_text, what):
        with self.assertRaises(Unsupported) as cm:
            tr(sexp_text)
        self.assertIn(what, str(cm.exception))

    def test_float(self):
        self.rejects(fn_def("main", "", "(print 1.5)"), "1.5")

    def test_receive(self):
        self.rejects(fn_def("main", "", "(receive (msg) 0)"), "receive")

    def test_spawn(self):
        self.rejects(fn_def("main", "", "(spawn f)"), "spawn")

    def test_send(self):
        self.rejects(fn_def("main", "", "(send p 1)"), "send")

    def test_use(self):
        self.rejects("(use str)" + fn_def("main", "", "0"), "use")

    def test_const(self):
        self.rejects("(const X 1)" + fn_def("main", "", "0"), "const")

    def test_import(self):
        self.rejects("(import str)" + fn_def("main", "", "0"), "import")

    def test_external_fn(self):
        self.rejects("(external_fn f)" + fn_def("main", "", "0"),
                     "external_fn")

    def test_define_pub(self):
        self.rejects("(define_pub (f) 0)", "define_pub")

    def test_dotted_module_call(self):
        self.rejects(fn_def("main", "", '(print (str.concat "a" "b"))'),
                     "str.concat")

    def test_unknown_head(self):
        self.rejects(fn_def("main", "", "(mystery 1)"), "mystery")

    def test_bool_literal_pattern(self):
        self.rejects(fn_def("f", "x", "(match x (true (print 1)) "
                            "(_ (print 0)))"), "bool literal pattern")

    def test_quoted_symbol_pattern(self):
        self.rejects(fn_def("f", "x",
                            "(match x ((quote red) (print 1)) "
                            "(_ (print 0)))"), "quoted-symbol pattern")

    def test_impure_and_operand(self):
        # && short-circuit cannot be preserved without a protocol
        self.rejects(fn_def("main", "",
                            "(print (and (g 1) true))"), "call to g")

    def test_impure_guard(self):
        self.rejects(fn_def("f", "x",
                            "(match x (n (print n) (g n)) "
                            "(_ (print 0)))"), "impure match guard")

    def test_let_in_expression_position(self):
        self.rejects(fn_def("main", "",
                            "(print (+ 1 (let x 2 x)))"),
                     "let in strict expression position")


# ---------------------------------------------------------------------------
# hygiene / determinism / output form
# ---------------------------------------------------------------------------

class TestHygiene(unittest.TestCase):
    def test_fresh_names_avoid_program_identifiers(self):
        # gen names its lambda params k_N — the transformer's k param must
        # not collide
        out = tr("(define (f k_1) (print k_1))")
        params = out.split("fn f(")[1].split(")")[0].split(", ")
        self.assertEqual(len(set(params)), 2)

    def test_value_prefixes_avoided(self):
        out = tr("(define (f v_1 d_2 kk_3 s_4) "
                 "(begin (print v_1) (print d_2) (print kk_3) "
                 "(print s_4)))")
        # every generated binding name must be fresh: no `let X =` rebinding
        # of the user's names
        for name in ("v_1", "d_2", "kk_3", "s_4"):
            self.assertNotIn("let %s =" % name, out)


class TestDeterminism(unittest.TestCase):
    def test_same_input_same_output(self):
        src = ("(type T nil (A int)) "
               + fn_def("f", "x", "(match x ((A v) (print v)))")
               + " " + fn_def("main", "",
                              "(print (f (cons (quote A) "
                              "(cons 1 nil))))"))
        tree = sexp_read_string("(" + src + ")")
        self.assertEqual(cps_transform(tree), cps_transform(tree))


class TestOutputForm(unittest.TestCase):
    def test_no_annotations_no_type_sig(self):
        src = ("(type-sig f (int) int) "
               + fn_def("f", "x", "x")
               + " " + fn_def("main", "", "(print (f 1))"))
        out = tr(src)
        self.assertNotIn("type-sig", out)
        self.assertNotIn(": int", out)
        self.assertNotIn("-> int", out)
        # params are bare names + the continuation
        self.assertRegex(out, r"fn f\(x, k_\d+\)")

    def test_main_stays_0_arity(self):
        out = tr(fn_def("main", "", "(print 1)"))
        self.assertRegex(out, r"fn main\(\)")


# ---------------------------------------------------------------------------
# runner-backed live tests (skipped without the toolchain)
# ---------------------------------------------------------------------------

def _toolchain_ready():
    return all(os.path.exists(p) for p in (
        os.path.join(_REPO, "tinyactor"),
        os.path.join(_REPO, "tavm_asan"),
        os.path.join(_HERE, "ast-dump.ta"),
        os.path.join(_HERE, "golden", "golden.py"),
    ))


LIVE_HAND_PROGRAMS = {
    "order": """\
fn g(n: int) -> int { print(n); n }
fn h(a: int, b: int) -> int { (a * 10 + b) }
fn main() {
  print(h(g(1), g(2)));
  print(g(3) + g(4))
}
""",
    "adt": """\
type Shape { Circle(int, int); Null }
fn area(s: Shape) -> int {
  match s {
    Circle(r, m) when (r > 0) -> (r * m),
    Null -> 0
  }
}
fn down(n: int) -> int {
  if (n <= 0) { 0 } else { down((n - 1)) }
}
fn apply1(f, x: int) -> int { f(x) }
fn main() {
  print(area(Circle(3, 4)));
  print(area(Null));
  let f = fn(z) { (z + 1) };
  print(apply1(f, 41));
  print(apply1(down, 100000));
  print(down(100000))
}
""",
}


@unittest.skipUnless(_toolchain_ready(), "toolchain binaries not present")
class TestLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import morph
        cls.morph = morph
        cls.runner = morph.Runner(
            tempfile.mkdtemp(prefix="kernfuzz-test-cps-"))

    def _check(self, src, tag):
        verdict, detail = cps.check_program(self.runner, src, tag)
        self.assertEqual(
            verdict, "consistent",
            "%s: verdict=%s detail=%s" % (
                tag, verdict,
                {k: v for k, v in detail.items() if k != "cps_src"}))

    def test_hand_programs_4way_consistent(self):
        for tag, src in LIVE_HAND_PROGRAMS.items():
            self._check(src, tag)

    def test_corpus_seeds_0_to_2(self):
        import gen
        for seed in (0, 1, 2):
            self._check(gen.gen_program(seed), "seed%d" % seed)

    def test_tco_100k(self):
        ok, detail = cps.tco_check(self.runner, levels=100000)
        self.assertTrue(ok, detail)

    def test_unsupported_is_explicit_rejection_not_crash(self):
        src = 'fn main() { print(str.len("ab")) }\n'
        verdict, detail = cps.check_program(self.runner, src, "unsup")
        self.assertEqual(verdict, "unsupported")
        self.assertIn("str.len", detail.get("error", ""))


if __name__ == "__main__":
    unittest.main(verbosity=2)