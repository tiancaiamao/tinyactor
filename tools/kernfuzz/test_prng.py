# -*- coding: utf-8 -*-
r"""
test_prng.py — unit tests for tools/kernfuzz/{prng.py, sexp.py}.

Stdlib-only unittest, style follows tools/kernfuzz/golden/test_golden.py.
Run:
    python3 tools/kernfuzz/test_prng.py
Exit 0 = all pass.

Covers:
  * PRNG (M-2): same-seed double run of 1000 draws is identical; different
    seeds give different streams; prng_next_range(p, 1) == 0; rejection
    sampling distribution sanity; int48 domain bounds.
  * derive_seed (M-6, docs/kernel-fuzzing-design.md §9): fixed hand-computed
    vectors for (42,0), (42,1), (0,0), (42,99) — sha256("42:0")[:8] etc.,
    big-endian, folded into [0, 2^47).
  * s-expr: round-trip read -> write byte-identical for ALL snapshots in
    test/kernfuzz-frozen/snapshots/ (N/N ok summary); hand-written dotted /
    escape / RAW-high-byte / \xNN cases; sexp_collect_cars.
"""

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import prng                      # noqa: E402
import sexp                      # noqa: E402
from sexp import NIL, TRUE, FALSE, Symbol, Pair  # noqa: E402

SNAPSHOT_DIR = os.path.join(
    _HERE, "..", "..", "test", "kernfuzz-frozen", "snapshots")
SNAPSHOT_DIR = os.path.normpath(SNAPSHOT_DIR)


# ---------------------------------------------------------------------------
# PRNG
# ---------------------------------------------------------------------------

class PrngTest(unittest.TestCase):
    """Counter-based PRNG (M-2) reproducibility and contract."""

    def test_same_seed_double_run_identical(self):
        for seed in (0, 1, 42, 2**64 - 1):
            p1 = prng.make_prng(seed)
            p2 = prng.make_prng(seed)
            a = [prng.prng_next_u64(p1) for _ in range(1000)]
            b = [prng.prng_next_u64(p2) for _ in range(1000)]
            self.assertEqual(a, b)

    def test_draws_in_u64_domain(self):
        p = prng.make_prng(42)
        for _ in range(200):
            u = prng.prng_next_u64(p)
            self.assertTrue(0 <= u < 2**64)

    def test_different_seeds_differ(self):
        a = [prng.prng_next_u64(prng.make_prng(1)) for _ in range(10)]
        b = [prng.prng_next_u64(prng.make_prng(2)) for _ in range(10)]
        self.assertNotEqual(a, b)

    def test_next_int48_domain_and_reproducible(self):
        p1 = prng.make_prng(7)
        p2 = prng.make_prng(7)
        for _ in range(100):
            v1 = prng.prng_next_int48(p1)
            v2 = prng.prng_next_int48(p2)
            self.assertEqual(v1, v2)
            self.assertTrue(0 <= v1 < 2**47)

    def test_next_range_contract(self):
        p = prng.make_prng(3)
        for _ in range(100):
            self.assertEqual(0, prng.prng_next_range(p, 1))  # n=1 always 0
        for n in (2, 3, 10, 1000, 2**20 + 1):
            q = prng.make_prng(n)
            for _ in range(200):
                v = prng.prng_next_range(q, n)
                self.assertTrue(0 <= v < n)

    def test_next_range_reproducible(self):
        p1 = prng.make_prng(9)
        p2 = prng.make_prng(9)
        a = [prng.prng_next_range(p1, 100) for _ in range(200)]
        b = [prng.prng_next_range(p2, 100) for _ in range(200)]
        self.assertEqual(a, b)

    def test_next_range_unbiased_sanity(self):
        # Rejection sampling must be uniform: chi-square-ish check on n=4,
        # 4000 draws, each bucket expected 1000, allow generous margin.
        p = prng.make_prng(11)
        buckets = [0] * 4
        for _ in range(4000):
            buckets[prng.prng_next_range(p, 4)] += 1
        for c in buckets:
            self.assertTrue(900 <= c <= 1100, buckets)

    def test_seed_domain_check(self):
        with self.assertRaises(ValueError):
            prng.make_prng(-1)
        with self.assertRaises(ValueError):
            prng.make_prng(2**64)
        with self.assertRaises(ValueError):
            p = prng.make_prng(0)
            prng.prng_next_range(p, 0)


class DeriveSeedTest(unittest.TestCase):
    """M-6 fixed vectors: seed_i = int(sha256(b\"<base>:<i>\")[:8]) % 2^47.

    Hand-computed (big-endian first 8 bytes folded into positive int48):
      sha256("42:0")[:8] = 5476... -> 6085284259181818738 % 2^47
    The four vectors below were computed independently with
    hashlib.sha256(b"%d:%d") and are frozen here as the authority.
    """

    VECTORS = [
        ((42, 0), 76737674146674),
        ((42, 1), 132289597893886),
        ((0, 0), 59967816734913),
        ((42, 99), 120215793540624),
    ]

    def test_fixed_vectors(self):
        for (base, i), want in self.VECTORS:
            got = prng.derive_seed(base, i)
            self.assertEqual(want, got, "derive_seed(%d, %d)" % (base, i))
            self.assertTrue(0 <= got < 2**47)

    def test_derived_seed_is_valid_prng_seed(self):
        s = prng.derive_seed(42, 0)
        p = prng.make_prng(s)
        p2 = prng.make_prng(s)
        self.assertEqual(
            [prng.prng_next_u64(p) for _ in range(10)],
            [prng.prng_next_u64(p2) for _ in range(10)])


# ---------------------------------------------------------------------------
# s-expr
# ---------------------------------------------------------------------------

def _pairlist(items, tail=None):
    """Build a cons chain from a Python list (+ optional dotted tail)."""
    node = NIL if tail is None else tail
    for item in reversed(items):
        node = Pair(item, node)
    return node


class SexpRoundTripSnapshotTest(unittest.TestCase):
    """All frozen snapshots: read -> write == original bytes."""

    def test_all_snapshots_round_trip(self):
        names = sorted(
            n for n in os.listdir(SNAPSHOT_DIR) if n.endswith(".sexp"))
        self.assertTrue(len(names) > 0, "no snapshots found")
        ok = 0
        for name in names:
            path = os.path.join(SNAPSHOT_DIR, name)
            with open(path, "rb") as f:
                original = f.read().rstrip(b"\n")
            tree = sexp.sexp_read_string(original.decode("latin-1"))
            rendered = sexp.sexp_write(tree).encode("latin-1")
            if rendered == original:
                ok += 1
            else:
                self.fail("%s round-trip mismatch\norig=%r\nnew =%r" % (
                    name, original[:200], rendered[:200]))
        print("snapshot round-trip: %d/%d ok" % (ok, len(names)))


def _tree_eq(a, b):
    """Structural equality (Pair has no __eq__; sentinels are interned)."""
    if type(a) is not type(b):
        return False
    if isinstance(a, Pair):
        return _tree_eq(a.car, b.car) and _tree_eq(a.cdr, b.cdr)
    return a == b


class SexpReaderTest(unittest.TestCase):
    def test_atoms(self):
        self.assertEqual(42, sexp.sexp_read_string("42"))
        self.assertEqual(-7, sexp.sexp_read_string("-7"))
        self.assertIs(NIL, sexp.sexp_read_string("nil"))
        self.assertIs(TRUE, sexp.sexp_read_string("true"))
        self.assertIs(FALSE, sexp.sexp_read_string("false"))
        self.assertEqual(Symbol("foo"), sexp.sexp_read_string("foo"))

    def test_symbol_vs_string(self):
        tree = sexp.sexp_read_string('(foo "bar")')
        self.assertEqual(Symbol("foo"), tree.car)
        self.assertEqual("bar", tree.cdr.car)

    def test_dotted_pair(self):
        tree = sexp.sexp_read_string("(a . b)")
        self.assertEqual(Symbol("a"), tree.car)
        self.assertEqual(Symbol("b"), tree.cdr)
        self.assertEqual("(a . b)", sexp.sexp_write(tree))

    def test_escapes_decoded(self):
        tree = sexp.sexp_read_string('"a\\nb\\tc\\\\d\\"e"')
        self.assertEqual('a\nb\tc\\d"e', tree)

    def test_hex_escape(self):
        tree = sexp.sexp_read_string('"\\x41\\xff"')
        self.assertEqual("A\xff", tree)

    def test_raw_high_byte_latin1(self):
        # A raw 0xff byte in the file (non-UTF-8) is byte-transparent.
        tree = sexp.sexp_read_string('"a\xffb"')
        self.assertEqual("a\xffb", tree)

    def test_string_with_parens(self):
        tree = sexp.sexp_read_string('(")(")')
        self.assertEqual(")(", tree.car)

    def test_malformed_raises(self):
        for bad in ["(a", "a)", '"abc', "(a . )", ""]:
            with self.assertRaises(ValueError):
                sexp.sexp_read_string(bad)

    def test_sexp_read_file(self):
        path = os.path.join(SNAPSHOT_DIR, "basic-arith.sexp")
        tree = sexp.sexp_read(path)
        self.assertEqual(Symbol("define"), tree.car.car)


class SexpWriterTest(unittest.TestCase):
    def test_write_atoms(self):
        self.assertEqual("42", sexp.sexp_write(42))
        self.assertEqual("nil", sexp.sexp_write(NIL))
        self.assertEqual("true", sexp.sexp_write(TRUE))
        self.assertEqual("false", sexp.sexp_write(FALSE))
        self.assertEqual("foo", sexp.sexp_write(Symbol("foo")))

    def test_write_list_and_dotted(self):
        self.assertEqual("(1 2 3)",
                         sexp.sexp_write(_pairlist([1, 2, 3])))
        self.assertEqual("(1 . 2)", sexp.sexp_write(_pairlist([1], 2)))
        self.assertEqual("((a b) nil)",
                         sexp.sexp_write(_pairlist(
                             [_pairlist([Symbol("a"), Symbol("b")]), NIL])))

    def test_write_escapes_match_ast_dump(self):
        # escape_str() in tools/kernfuzz/ast-dump.ta: \n \r \t \\ \" escaped,
        # everything else (RAW high bytes, other control bytes) literal.
        self.assertEqual('"a\\nb"', sexp.sexp_write("a\nb"))
        self.assertEqual('"a\\tb"', sexp.sexp_write("a\tb"))
        self.assertEqual('"a\\rb"', sexp.sexp_write("a\rb"))
        self.assertEqual('"a\\\\b"', sexp.sexp_write("a\\b"))
        self.assertEqual('"a\\"b"', sexp.sexp_write('a"b'))
        self.assertEqual('"A\xff"', sexp.sexp_write("A\xff"))  # RAW byte
        # \x01 has no escape: ast-dump.ta writes it as the RAW literal byte
        self.assertEqual('"\x01x"', sexp.sexp_write("\x01x"))

    def test_round_trip_handwritten_cases(self):
        cases = [
            "(1 . 2)",
            "(a b . c)",      # dotted tail
            "(a b c)",        # proper list
            "(a b c)",       # (a . (b . (c . nil)))
            '"a\\nb\\tc\\\\d\\"e"',
            '"raw\xffbyte"',  # RAW high byte stays RAW (ast-dump behavior)
            '"A\x7f"',  # writer emits \xNN bytes literally, no \x re-encode
            '(print "x\\"y")',
            "(a b . c)",
        ]
        for text in cases:
            tree = sexp.sexp_read_string(text)
            self.assertEqual(
                text, sexp.sexp_write(tree), "round-trip of %r" % text)

    def test_write_file_round_trip(self):
        import tempfile
        tree = sexp.sexp_read_string('(define (main) (print "hi\\n\xff"))')
        fd, path = tempfile.mkstemp(suffix=".sexp")
        os.close(fd)
        try:
            sexp.sexp_write_file(tree, path)
            with open(path, "rb") as f:
                data = f.read()
            self.assertEqual(
                b'(define (main) (print "hi\\n\xff"))', data)
            self.assertTrue(_tree_eq(tree, sexp.sexp_read(path)))
        finally:
            os.unlink(path)


class CollectCarsTest(unittest.TestCase):
    def test_collect_all_cars(self):
        tree = sexp.sexp_read_string(
            "((define (main) (begin (print x) (if y (let z 1 z) nil))) 42)")
        names = [s.name for s in sexp.sexp_collect_cars(tree)]
        self.assertEqual(
            ["define", "main", "begin", "print", "x", "if", "y",
             "let", "z", "z", "nil"], names)

    def test_dotted_cdr_traversed(self):
        tree = sexp.sexp_read_string("((a . (b . nil)) . (c . nil))")
        names = [s.name for s in sexp.sexp_collect_cars(tree)]
        self.assertEqual(["a", "b", "c"], names)

    def test_empty(self):
        self.assertEqual([], sexp.sexp_collect_cars(42))
        self.assertEqual([], sexp.sexp_collect_cars(NIL))


if __name__ == "__main__":
    unittest.main(verbosity=2)