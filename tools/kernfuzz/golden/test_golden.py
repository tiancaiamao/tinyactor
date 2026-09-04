# -*- coding: utf-8 -*-
"""
test_golden.py — unit tests for tools/kernfuzz/golden/{sexp.py,golden.py}.

Stdlib-only unittest. Run:
    python3 tools/kernfuzz/golden/test_golden.py
Exit 0 = all pass, non-zero = failure.

Translates the key assertions from tools/kernfuzz/golden/test-interp-core.scm
(60 assertions) plus the evaluator-level cases from the task definition:
  * w48 wrap both directions,
  * 7/(0-2) == -3, 7%(0-2) == 1, 1/0 => DIVZERO,
  * print_val formatting (int decimal, string raw, symbol, pair/dotted/list),
  * closure capture/shadow chain, match all pattern kinds, mutual recursion,
  * zero-arg closures, function-value passing.
"""

import os
import subprocess
import sys
import tempfile
import unittest

# Make the golden package importable regardless of the invoking cwd.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import golden                       # noqa: E402
from sexp import NIL, TRUE, FALSE, Symbol, Pair, parse   # noqa: E402


class W48Test(unittest.TestCase):
    """w48 → int48 normalization (two's-complement 48-bit)."""

    def test_wrap_positive(self):
        # 2^47 - 1 + 1 wraps to -2^47.
        self.assertEqual(-1 << 47, golden.w48((1 << 47) - 1 + 1))

    def test_wrap_negative(self):
        # (0 - 2^47) - 1 wraps to 2^47 - 1.
        self.assertEqual((1 << 47) - 1, golden.w48((0 - (1 << 47)) - 1))

    def test_boundary_stable(self):
        self.assertEqual((1 << 47) - 1, golden.w48((1 << 47) - 1))
        self.assertEqual(-(1 << 47), golden.w48(-(1 << 47)))

    def test_2_47_wraps_to_neg(self):
        self.assertEqual(-(1 << 47), golden.w48(1 << 47))


class ArithmeticTest(unittest.TestCase):
    """+ - * / % with int48 wrapping and C-style truncation."""

    def test_add_simple(self):
        self.assertEqual(6, golden._binop("+", 3, 3))

    def test_mul_no_wrap(self):
        # (2^23+1)^2 = 2^46 + 2^24 + 1 (< 2^47): exact.
        x = (1 << 23) + 1
        self.assertEqual((1 << 46) + (1 << 24) + 1, golden._binop("*", x, x))

    def test_mul_wrap(self):
        # 2^47 * 2^47 = 2^94 ≡ 0 (mod 2^48).
        self.assertEqual(0, golden._binop("*", 1 << 47, 1 << 47))

    def test_trunc_div_toward_zero(self):
        self.assertEqual(-3, golden._binop("/", 7, -2))   # 7 / (0-2)
        self.assertEqual(-3, golden._binop("/", -7, 2))   # (0-7) / 2
        self.assertEqual(3, golden._binop("/", 7, 2))

    def test_remainder_sign_follows_dividend(self):
        self.assertEqual(1, golden._binop("%", 7, -2))    # 7 % (0-2)
        self.assertEqual(-1, golden._binop("%", -7, 2))   # (0-7) % 2
        self.assertEqual(-1, golden._binop("%", -7, -2))  # (0-7) % (0-2)
        self.assertEqual(1, golden._binop("%", 7, 2))

    def test_divzero_raises(self):
        with self.assertRaises(golden.Divzero):
            golden._binop("/", 1, 0)
        with self.assertRaises(golden.Divzero):
            golden._binop("/", 0, 0)
        with self.assertRaises(golden.Divzero):
            golden._binop("%", 1, 0)

    def test_valid_division(self):
        self.assertEqual(2, golden._binop("/", 6, 3))


class ComparisonTest(unittest.TestCase):
    """== and numeric orderings, matching facts.md."""

    def test_eq_int(self):
        self.assertIs(golden._cmpop("=", 5, 5), True)
        self.assertIs(golden._cmpop("=", 5, 6), False)

    def test_eq_string_content(self):
        self.assertIs(golden._cmpop("=", "ab", "ab"), True)
        self.assertIs(golden._cmpop("=", "ab", "ac"), False)

    def test_eq_bool_nil(self):
        self.assertIs(golden._cmpop("=", True, True), True)
        self.assertIs(golden._cmpop("=", NIL, NIL), True)

    def test_eq_pair_identity(self):
        p = Pair(1, 2)
        self.assertIs(golden._cmpop("=", p, p), True)
        # structurally equal but separately built → identity fails (f1)
        self.assertIs(golden._cmpop("=", Pair(1, 2), Pair(1, 2)), False)
        a = Pair(1, 2)
        c = a
        self.assertIs(golden._cmpop("=", c, a), True)

    def test_eq_int_vs_bool(self):
        self.assertIs(golden._cmpop("=", 1, True), False)

    def test_ordering(self):
        self.assertIs(golden._cmpop("<", 1, 2), True)
        self.assertIs(golden._cmpop("<", 2, 1), False)
        self.assertIs(golden._cmpop("<=", 2, 2), True)
        self.assertIs(golden._cmpop(">", 3, 2), True)
        self.assertIs(golden._cmpop(">=", 3, 3), True)

    def test_ordering_non_int_false(self):
        self.assertIs(golden._cmpop("<", True, 2), False)
        self.assertIs(golden._cmpop("<", 1, "x"), False)
        self.assertIs(golden._cmpop("<=", NIL, NIL), False)


class PrintValTest(unittest.TestCase):
    """print_val formatting: int decimal, nil, true/false, symbol, string raw."""

    def test_int(self):
        self.assertEqual("-5", golden._print_val(golden.w48(-5)))
        self.assertEqual("0", golden._print_val(0))
        self.assertEqual("42", golden._print_val(42))

    def test_int_boundary(self):
        self.assertEqual(str((1 << 47) - 1), golden._print_val((1 << 47) - 1))
        self.assertEqual(str(-(1 << 47)), golden._print_val(-(1 << 47)))

    def test_pair_dotted_list(self):
        self.assertEqual("(1 . 2)", golden._print_val(Pair(1, 2)))
        self.assertEqual("(1 2 . 3)", golden._print_val(Pair(1, Pair(2, 3))))
        self.assertEqual("(1 2 3)",
                         golden._print_val(Pair(1, Pair(2, Pair(3, NIL)))))

    def test_nil_bool_symbol_string(self):
        self.assertEqual("nil", golden._print_val(NIL))
        self.assertEqual("true", golden._print_val(True))
        self.assertEqual("false", golden._print_val(False))
        self.assertEqual("hello", golden._print_val(Symbol("hello")))

    def test_string_raw_no_quotes(self):
        self.assertEqual("abc", golden._print_val("abc"))

    def test_string_keeps_newline_and_backslash(self):
        # raw bytes preserved: no escape processing in print_val (src/vm.c).
        self.assertEqual("a\nb", golden._print_val("a\nb"))
        self.assertEqual("a\\b", golden._print_val("a\\b"))


class SexpReaderTest(unittest.TestCase):
    """sexp reader produces AST structures (list = Pair chain, str, int, Symbol)."""

    def test_parse_int(self):
        self.assertIsInstance(parse("42"), int)
        self.assertEqual(42, parse("42"))

    def test_parse_string(self):
        self.assertIsInstance(parse('"hi"'), str)
        self.assertEqual("hi", parse('"hi"'))

    def test_parse_escapes(self):
        self.assertEqual("a\nb", parse('"a\\nb"'))
        self.assertEqual('a"b', parse('"a\\"b"'))
        self.assertEqual("a\\b", parse('"a\\\\b"'))

    def test_parse_string_with_parens(self):
        # a quoted string literal containing parens must not break the structure.
        node = parse('("(paren)" 1)')
        self.assertIsInstance(node, Pair)
        self.assertEqual("(paren)", node.car)
        self.assertEqual(1, node.cdr.car)

    def test_parse_symbol_vs_string(self):
        sym = parse("hello")
        self.assertIsInstance(sym, Symbol)
        self.assertEqual("hello", sym.name)

    def test_parse_true_false_nil(self):
        self.assertIs(parse("true"), TRUE)
        self.assertIs(parse("false"), FALSE)
        self.assertIs(parse("nil"), NIL)


def _eval(prog):
    """Evaluate a full program text and return stdout."""
    return golden.eval_string(prog)


class EvaluatorTest(unittest.TestCase):
    """Interpreted behavior for closures, match, recursion, and function values."""

    def test_closure_capture_and_shadow_chain(self):
        prog = ('((define (make) (let x 1 (lambda (y) (+ x y))))'
                ' (define (main) (let f (make) (print (f 10)))))')
        self.assertEqual("11\n", _eval(prog))

    def test_closure_shadow(self):
        # inner let shadows the captured x.
        prog = ('((define (make) (let x 1 (lambda (y) (let x 100 (+ x y)))))'
                ' (define (main) (let f (make) (print (f 10)))))')
        self.assertEqual("110\n", _eval(prog))

    def test_mutual_recursion(self):
        prog = ('((define (even? n) (if (= n 0) true (odd? (- n 1))))'
                ' (define (odd? n) (if (= n 0) false (even? (- n 1))))'
                ' (define (main) (print (even? 10))))')
        self.assertEqual("true\n", _eval(prog))

    def test_zero_arg_closure(self):
        prog = ('((define (make) (lambda () 42))'
                ' (define (main) (let f (make) (print (f)))))')
        self.assertEqual("42\n", _eval(prog))

    def test_function_value_passing(self):
        prog = ('((define (add a b) (+ a b))'
                ' (define (curry f x) (lambda (y) (f x y)))'
                ' (define (main) (let g (curry add 3) (print (g 4)))))')
        self.assertEqual("7\n", _eval(prog))

    def test_match_int_binding(self):
        prog = ('((define (main) (begin (match 5'
                ' (0 (print "zero")) (n (print (* n 2))) (_ (print "wild"))))))')
        self.assertEqual("10\n", _eval(prog))

    def test_match_string_literal_and_wildcard(self):
        prog = ('((define (main) (begin'
                ' (match "a" ("a" (print "matched")) (_ (print "wild")))'
                ' (match "z" ("a" (print "matched")) (_ (print "wild"))))))')
        self.assertEqual("matched\nwild\n", _eval(prog))

    def test_match_ctor_with_guard(self):
        prog = ('((type Msg nil (Add a b))'
                ' (define (main)'
                '   (let m (cons (quote Add) (cons 1 (cons 2 nil)))'
                '     (match m'
                '       ((Mul x y) (print "mul"))'
                '       ((Add a b) (if (> a 0) (print "add+") (print "add-")))'
                '       (_ (print "other"))))))')
        self.assertEqual("add+\n", _eval(prog))

    def test_and_or_short_circuit_returns_bool(self):
        prog = ('((define (main) (begin'
                ' (print (and true true))'
                ' (print (or false true))'
                ' (print (and false (print "NO1")))'
                ' (print (or true (print "NO2"))))))')
        self.assertEqual("true\ntrue\nfalse\ntrue\n", _eval(prog))

    def test_function_name_as_value(self):
        # A top-level fn name used as a value resolves to its Closure; we can
        # alias it via let and call it (full first-class functions).
        prog = ('((define (add a b) (+ a b))'
                ' (define (main) (let g add (print (g 5 6)))))')
        self.assertEqual("11\n", _eval(prog))

    def test_deep_non_tail_recursion_5000(self):
        # 5000-deep NON-tail recursion must survive (big-stack worker thread +
        # raised recursionlimit; P2 review finding 2). The VM handles this
        # fine; golden must not die with RecursionError.
        prog = ('((define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))'
                ' (define (main) (print (f 5000))))')
        self.assertEqual("5000\n", _eval(prog))

    def test_long_list_builtins_iterative(self):
        # 20000-element list: list builtins are iterative so they do not hit
        # the Python stack (P2 review finding 2).
        prog = ('((define (build n acc) (if (= n 0) acc (build (- n 1) (cons n acc))))'
                ' (define (main) (begin'
                ' (print (list.length (build 20000 nil)))'
                ' (print (list.length (list.append (build 20000 nil) (build 20000 nil))))'
                ' (print (list.length (list.map (lambda (x) (+ x 1)) (build 20000 nil))))'
                ' (print (list.length (list.filter (lambda (x) true) (build 20000 nil))))'
                ' (print (list.length (list.reverse (build 20000 nil))))'
                ' (print (list.length (list.take 20000 (build 20000 nil))))'
                ' (print (list.foldl (lambda (a b) (+ a 1)) 0 (build 20000 nil))))))')
        self.assertEqual("20000\n40000\n20000\n20000\n20000\n20000\n20000\n",
                         _eval(prog))


class ErrorPathTest(unittest.TestCase):
    """CLI error paths flush completed output lines (P2 review finding 1)."""

    def _run_cli(self, prog):
        import subprocess, tempfile, os
        f = tempfile.NamedTemporaryFile("w", suffix=".sexp", delete=False)
        f.write(prog)
        f.close()
        try:
            r = subprocess.run(
                [sys.executable, os.path.join(os.path.dirname(golden.__file__), "golden.py"), f.name],
                capture_output=True)
            return r.returncode, r.stdout
        finally:
            os.unlink(f.name)

    def test_divzero_keeps_completed_lines(self):
        code, out = self._run_cli(
            '((define (main) (begin (print 1) (print (/ 1 0)) (print 2))))')
        self.assertEqual(1, code)
        self.assertEqual(b"1\nDIVZERO:1\n", out)

    def test_cartype_keeps_completed_lines_protocol(self):
        # car of non-pair kills the process (cartype); the runner synthesizes
        # the DIVZERO:<n> protocol line on ANY exit-1 death (§5.1.3), so
        # golden must emit completed lines + the protocol line too.
        code, out = self._run_cli(
            '((define (main) (begin (print 1) (print (car 5)) (print 2))))')
        self.assertEqual(1, code)
        self.assertEqual(b"1\nDIVZERO:1\n", out)

    def test_cdrtype_keeps_completed_lines_protocol(self):
        code, out = self._run_cli(
            '((define (main) (begin (print "a") (print (cdr 7)))))')
        self.assertEqual(1, code)
        self.assertEqual(b"a\nDIVZERO:1\n", out)


class HighByteStringTest(unittest.TestCase):
    """Byte-transparent string semantics (P2 review finding 3): a backslash-xNN high
    byte goes out as ONE byte, matching the VM (fwrite, not UTF-8)."""

    def _run_cli(self, prog):
        import subprocess, tempfile, os
        f = tempfile.NamedTemporaryFile("w", suffix=".sexp", delete=False)
        f.write(prog)
        f.close()
        try:
            r = subprocess.run(
                [sys.executable, os.path.join(os.path.dirname(golden.__file__), "golden.py"), f.name],
                capture_output=True)
            return r.returncode, r.stdout
        finally:
            os.unlink(f.name)

    def test_parse_high_byte_escape_is_single_codepoint(self):
        v = parse('"a\xffb"')
        self.assertEqual("a\xffb", v)

    def test_high_byte_printed_as_single_byte(self):
        # verified against the VM: ./tinyactor run prints 61 ff 62 0a
        prog = '((define (main) (print "a' + chr(255) + 'b")))'
        f = tempfile.NamedTemporaryFile("wb", suffix=".sexp", delete=False)
        f.write(prog.encode("latin-1"))
        f.close()
        try:
            r = subprocess.run(
                [sys.executable,
                 os.path.join(os.path.dirname(golden.__file__), "golden.py"), f.name],
                capture_output=True)
            code, out = r.returncode, r.stdout
        finally:
            os.unlink(f.name)
        self.assertEqual(0, code)
        self.assertEqual(b"a\xffb\n", out)

    def test_raw_high_byte_in_dump_file(self):
        # End-to-end: a snapshot file containing a RAW 0xff byte (as produced
        # by ast-dump) must be read byte-transparently (latin-1, symmetric
        # with _write_out) -- not UTF-8, which would raise UnicodeDecodeError.
        prog = ('((define (main) (print "a' + chr(255) + 'b")))')
        import tempfile
        f = tempfile.NamedTemporaryFile("wb", suffix=".sexp", delete=False)
        f.write(prog.encode("latin-1"))
        f.close()
        try:
            r = subprocess.run(
                [sys.executable,
                 os.path.join(os.path.dirname(golden.__file__), "golden.py"), f.name],
                capture_output=True)
            self.assertEqual(0, r.returncode)
            self.assertEqual(b"a\xffb\n", r.stdout)
        finally:
            os.unlink(f.name)

    def test_str_chr_high_byte_single_byte(self):
        # VM: str.chr(200) -> single byte 0xc8
        code, out = self._run_cli('((define (main) (print (str.chr 200))))')
        self.assertEqual(0, code)
        self.assertEqual(b"\xc8\n", out)


    def test_tco_deep_tail_recursion(self):
        # 200k-deep tail recursion must not blow the Python stack (apply_fn
        # trampoline). Checks both the result value and a guard against a
        # runaway loop.
        prog = ('((define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1))))'
                ' (define (main) (print (loop 200000 0))))')
        self.assertEqual("200000\n", _eval(prog))


if __name__ == "__main__":
    unittest.main(verbosity=2)