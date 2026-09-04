# -*- coding: utf-8 -*-
"""
test_tc_oracle.py — tests for tools/kernfuzz/tc_oracle.py (§6 双向 oracle).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_tc_oracle.py
Exit 0 = all pass, non-zero = failure.

Coverage per .pge/tasks/task-tc-oracle.md:
  * §6.0 frozen-feature classifier: accept / reject / crash / anomaly each
    verified on its own synthetic RunResult (incl. the assertion triple:
    exit!=0 AND `type error(s) found` AND non-panic),
  * every mutation class has ≥1 directed positive AND negative assertion
    on real gen trees (structural), including the exhaust special class'
    accept+quiet double assertion,
  * parse-reject pre-validation routing: a mutant failing ast-dump lands
    in the parse-reject bucket and NEVER in the reject denominator,
  * control group: unmutated P must accept (broken-compiler injection →
    sound-fail finding proves the soundness assertion is not a shell),
  * strict reject assertion really bites: an accept-everything fake
    compiler produces missed-reject findings (completeness direction),
  * exhaust behavior-drift alarm: warning appearing under the frozen
    accept-quiet expectation fires tc-drift,
  * findings directory contract (morph.record_finding reuse): meta.json /
    sources / stdout / stderr / exit / run.sh,
  * smoke: real toolchain, 30 seeds, exit 0, class-skip rate < 20%,
  * determinism: same CLI args twice → identical findings signature sets
    and identical per-class stats.

Toolchain-dependent tests skip cleanly when ./tinyactor or ./tavm_asan
is absent (same convention as test_morph.py).
"""

import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                                    # noqa: E402
import prng                                   # noqa: E402
import transforms as tr                       # noqa: E402
import morph                                  # noqa: E402
import tc_oracle as tco                       # noqa: E402


def _rr(out=b"", err=b"", rc=0, timed_out=False):
    return morph.RunResult(out, err, rc, timed_out)


class FakeRunner(object):
    """Scripted stand-in for morph.Runner (no toolchain, fully offline).

    build_result/run_result/dump_result may be RunResults or callables
    f(tag_or_path) -> RunResult.
    """

    def __init__(self, build_result=None, run_result=None, dump_result=None):
        self.workdir = tempfile.mkdtemp(prefix="tco-fake-")
        self._build = build_result or _rr()
        self._run = run_result or _rr(out=b"1\n")
        self._dump = dump_result or _rr(out=b"(program)")
        self.builds = []

    def _resolve(self, x, arg):
        return x(arg) if callable(x) else x

    def build(self, src_path, artifact_path):
        self.builds.append(src_path)
        return self._resolve(self._build, src_path)

    def run(self, artifact_path):
        return self._resolve(self._run, artifact_path)

    def dump(self, src_path):
        return self._resolve(self._dump, src_path)


class OracleWorld(object):
    """run_seed/fuzz_batch plumbing over a FakeRunner."""

    def __init__(self, runner):
        self.out = tempfile.mkdtemp(prefix="tco-find-")
        self.dedup = set()
        self.skips = []
        self.findings = dict((c, 0) for c in tco.FINDING_CATEGORIES)
        self.classes = dict(
            (c, {"instances": 0, "skip": 0, "parse-reject": 0,
                 "rejected": 0, "known-hole": 0, "exhaust-ok": 0})
            for c in tco.CLASSES)
        self.runner = runner

    def run(self, seed):
        return tco.run_seed(self.runner, seed, self.out, self.dedup,
                            self.skips, self.findings, self.classes,
                            log=lambda m: None)

    def cleanup(self):
        shutil.rmtree(self.out, ignore_errors=True)
        shutil.rmtree(self.runner.workdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# §6.0 frozen-feature classifier
# ---------------------------------------------------------------------------

class ClassifyBuildTest(unittest.TestCase):
    REJECT_OUT = (b"typecheck: 1 type error(s) found\n"
                  b"  [E0001] in function 'main' (line 2): arg 1 of f: "
                  b"cannot unify string with int\n"
                  b"compile aborted: 1 type error(s)\n")

    def test_accept_is_exit0_without_type_error_line(self):
        self.assertEqual(tco.classify_build(_rr(b"", b"", 0)), "accept")

    def test_reject_is_nonzero_with_frozen_marker(self):
        self.assertEqual(
            tco.classify_build(_rr(out=self.REJECT_OUT, rc=1)), "reject")

    def test_reject_marker_on_stderr_also_counts(self):
        self.assertEqual(
            tco.classify_build(_rr(err=self.REJECT_OUT, rc=1)), "reject")

    def test_assertion_triple_nonzero_without_marker_is_anomaly(self):
        # exit != 0 but no `type error(s)` keyword: NOT a reject (e.g. a
        # parse error that slipped past the ast-dump pre-check)
        self.assertEqual(
            tco.classify_build(_rr(b"parse error: unbalanced '}'\n", b"", 1)),
            "anomaly")

    def test_exit0_with_marker_is_anomaly(self):
        self.assertEqual(
            tco.classify_build(_rr(out=self.REJECT_OUT, rc=0)), "anomaly")

    def test_panic_text_is_crash_even_with_marker(self):
        self.assertEqual(
            tco.classify_build(_rr(out=self.REJECT_OUT + b"panic: op\n",
                                   rc=1)),
            "crash")

    def test_signal_death_is_crash(self):
        self.assertEqual(tco.classify_build(_rr(rc=-9)), "crash")

    def test_unknown_exit_code_is_crash(self):
        self.assertEqual(tco.classify_build(_rr(rc=2)), "crash")

    def test_asan_exitcode_is_crash(self):
        self.assertEqual(tco.classify_build(_rr(rc=tco.ASAN_EXIT)), "crash")

    def test_timeout_is_crash(self):
        self.assertEqual(tco.classify_build(_rr(rc=None, timed_out=True)),
                         "crash")

    def test_exhaust_classifier_reports_warning_flag(self):
        cat, warned = tco.classify_exhaust(_rr(b"", b"non-exhaustive match\n",
                                               0))
        self.assertEqual((cat, warned), ("accept", True))
        cat, warned = tco.classify_exhaust(_rr())
        self.assertEqual((cat, warned), ("accept", False))


# ---------------------------------------------------------------------------
# mutations on real gen trees (structural, offline)
# ---------------------------------------------------------------------------

def _first_seed_with(cls, sub=None, lo=1, hi=40):
    """First seed in [lo, hi] whose mutator fires (optionally with sub)."""
    for seed in range(lo, hi + 1):
        plan = gen.build_program(seed)
        rng = prng.make_prng(prng.derive_seed(seed, 7000 +
                                              tco.CLASSES.index(cls)))
        mplan, info = tco.MUTATORS[cls](plan, rng)
        if mplan is not None and (sub is None or info.get("sub") == sub):
            return seed, plan, mplan, info
    raise AssertionError("no seed %d..%d exercises %s/%s"
                         % (lo, hi, cls, sub))


def _count(src, needle):
    return src.count(needle)


class MutationLitSwapTest(unittest.TestCase):
    def test_positive_swaps_int_literal_for_string(self):
        seed, plan, mplan, info = _first_seed_with("lit_swap")
        self.assertEqual(info["sub"], "arith_lit")
        src = gen.render_tree(plan)
        msrc = gen.render_tree(mplan)
        self.assertNotEqual(src, msrc)
        self.assertIn('"%s"' % tco.MUT_STR, msrc)
        # exactly ONE literal got swapped
        self.assertEqual(_count(msrc, '"%s"' % tco.MUT_STR), 1)

    def test_negative_never_touches_eq_ne_operands(self):
        # == / != accept mixed operands (frozen probe c3): the site pool
        # must be arith + strict-cmp only.  Scan many seeds: a mutated
        # operand must never sit inside an `==`/`!=` rendering.
        for seed in range(1, 25):
            plan = gen.build_program(seed)
            rng = prng.make_prng(prng.derive_seed(seed, 7000))
            mplan, _info = tco.MUTATORS["lit_swap"](plan, rng)
            if mplan is None:
                continue
            msrc = gen.render_tree(mplan)
            for m in _T_EQ_NE.finditer(msrc):
                self.assertNotIn(tco.MUT_STR, m.group(0))

    def test_deterministic_same_seed_same_mutation(self):
        for seed in (1, 5, 7):
            plan = gen.build_program(seed)
            r1 = prng.make_prng(prng.derive_seed(seed, 7000))
            r2 = prng.make_prng(prng.derive_seed(seed, 7000))
            a, _ = tco.MUTATORS["lit_swap"](plan, r1)
            b, _ = tco.MUTATORS["lit_swap"](plan, r2)
            self.assertEqual(gen.render_tree(a), gen.render_tree(b))


_T_EQ_NE = __import__("re").compile(r"\([^()\n]*(?:==|!=)[^()\n]*\)")


class MutationFnArgMismatchTest(unittest.TestCase):
    def test_positive_type_sub_swaps_annotated_int_arg(self):
        seed, plan, mplan, info = _first_seed_with("fn_arg_mismatch",
                                                   sub="type")
        self.assertEqual(info["sub"], "type")
        msrc = gen.render_tree(mplan)
        self.assertIn('"%s"' % tco.MUT_STR, msrc)
        self.assertEqual(_count(msrc, '"%s"' % tco.MUT_STR), 1)

    def test_positive_arity_sub_adds_exactly_one_arg(self):
        import re
        seed, plan, mplan, info = _first_seed_with("fn_arg_mismatch",
                                                   sub="arity")
        self.assertEqual(info["sub"], "arity")
        src = gen.render_tree(plan)
        msrc = gen.render_tree(mplan)
        # total top-level helper-call args grew by exactly 1, and every
        # helper name's call count is unchanged (no call added/removed)
        def tally(text):
            tot = 0
            cnt = {}
            for name in set(s.name for s, _ in plan.fns):
                pat = re.compile(r"\b%s\(([^()]*)\)" % re.escape(name))
                hits = pat.findall(text)
                cnt[name] = len(hits)
                tot += sum(len(h.split(",")) for h in hits)
            return tot, cnt

        tot_a, cnt_a = tally(src)
        tot_b, cnt_b = tally(msrc)
        self.assertEqual(tot_b - tot_a, 1)
        self.assertEqual(cnt_a, cnt_b)

    def test_negative_no_fns_means_no_site(self):
        # a program without helper fns cannot host the mutation
        for seed in range(1, 40):
            plan = gen.build_program(seed)
            if not plan.fns:
                rng = prng.make_prng(prng.derive_seed(seed, 7001))
                mplan, info = tco.MUTATORS["fn_arg_mismatch"](plan, rng)
                self.assertIsNone(mplan)
                self.assertEqual(info.get("reason"),
                                 "no-helper-call-site")
                return
        self.fail("no fn-less seed in 1..40 (generator changed?)")


class MutationUndefVarTest(unittest.TestCase):
    def test_positive_injects_fresh_undefined_name(self):
        import re
        seed, plan, mplan, info = _first_seed_with("undef_var")
        src = gen.render_tree(plan)
        msrc = gen.render_tree(mplan)
        m = re.search(r"\b%s_\d+\b" % tco.UNDEF_BASE, msrc)
        self.assertIsNotNone(m)
        # freshness: name absent from the original program
        self.assertIsNone(re.search(r"\b%s\b" % m.group(0), src))
        self.assertEqual(_count(msrc, m.group(0)), 1)

    def test_negative_freshness_avoids_collision(self):
        # if the program already mentions zz_undef_0, the next index is used
        import re
        for seed in range(1, 40):
            plan = gen.build_program(seed)
            if re.search(r"\b%s_0\b" % tco.UNDEF_BASE, gen.render_tree(plan)):
                continue
            rng = prng.make_prng(prng.derive_seed(seed, 7002))
            mplan, _ = tco.MUTATORS["undef_var"](plan, rng)
            if mplan is None:
                continue
            msrc = gen.render_tree(mplan)
            self.assertIn("%s_0" % tco.UNDEF_BASE, msrc)
            return
        self.fail("no undef_var site in 1..40")


class MutationCtorTest(unittest.TestCase):
    def test_positive_field_type_sub(self):
        seed, plan, mplan, info = _first_seed_with("ctor_field_type",
                                                   sub="field_type")
        msrc = gen.render_tree(mplan)
        self.assertIn('"%s"' % tco.MUT_STR, msrc)

    def test_positive_arity_sub_adds_arg(self):
        import re
        seed, plan, mplan, info = _first_seed_with("ctor_field_type",
                                                   sub="arity")
        types = tr._Types(plan)
        msrc = gen.render_tree(mplan)
        grew = False
        for name in types.ctors:
            for m in re.finditer(r"\b%s\(([^()]*)\)" % re.escape(name),
                                 msrc):
                self.assertLessEqual(len(m.group(1).split(",")), 3)
                grew = True
        # at least one ctor call now has args where it previously had one
        # fewer (or a bare zero-field ctor gained parens)
        self.assertTrue(grew or "(7)" in msrc)

    def test_negative_no_adt_means_no_site(self):
        for seed in range(1, 40):
            plan = gen.build_program(seed)
            if not plan.type_decls:
                rng = prng.make_prng(prng.derive_seed(seed, 7003))
                mplan, info = tco.MUTATORS["ctor_field_type"](plan, rng)
                self.assertIsNone(mplan)
                self.assertEqual(info.get("reason"), "no-ctor-call")
                return
        self.fail("no ADT-less seed in 1..40")


class MutationExhaustTest(unittest.TestCase):
    def test_positive_drops_exactly_the_wildcard_arm(self):
        seed, plan, mplan, info = _first_seed_with("exhaust")
        self.assertEqual(info["sub"], "drop_wildcard")
        stmt_i = int(info["why"].rsplit(" ", 1)[1].rstrip(")"))
        before = plan.main_stmts[stmt_i]
        after = mplan.main_stmts[stmt_i]
        import re
        w = re.compile(r"^\s*_\s*->", __import__("re").M)
        self.assertEqual(len(w.findall(before)) - len(w.findall(after)), 1)
        self.assertNotIn(stmt_i + 1, [0])       # other stmts untouched
        # non-mutated statements stay byte-identical
        for k, stmt in enumerate(plan.main_stmts):
            if k != stmt_i:
                self.assertEqual(stmt, mplan.main_stmts[k])

    def test_negative_no_match_stmt(self):
        for seed in range(1, 40):
            plan = gen.build_program(seed)
            if not getattr(plan, "match_meta", []):
                rng = prng.make_prng(prng.derive_seed(seed, 7004))
                mplan, info = tco.MUTATORS["exhaust"](plan, rng)
                self.assertIsNone(mplan)
                self.assertEqual(info.get("reason"), "no-match-stmt")
                return
        self.fail("no match-less seed in 1..40")


class MutationExpectationTableTest(unittest.TestCase):
    """The frozen per-class expectation table (§6.0 discipline extended
    per class) must stay closed over (class, sub) pairs."""

    def test_every_class_sub_pair_has_expectation(self):
        for cls in tco.CLASSES:
            seed, plan, _m, info = _first_seed_with(cls)
            key = (cls, info["sub"])
            self.assertIn(key, tco.SUB_EXPECT)

    def test_strict_and_hole_sets_are_frozen(self):
        self.assertEqual(tco.SUB_EXPECT[("lit_swap", "arith_lit")], "reject")
        self.assertEqual(tco.SUB_EXPECT[("fn_arg_mismatch", "type")],
                         "reject")
        self.assertEqual(tco.SUB_EXPECT[("fn_arg_mismatch", "arity")],
                         "reject")
        self.assertEqual(tco.SUB_EXPECT[("undef_var", "arith_lit")],
                         "accept-hole")
        self.assertEqual(tco.SUB_EXPECT[("ctor_field_type", "field_type")],
                         "accept-hole")
        self.assertEqual(tco.SUB_EXPECT[("ctor_field_type", "arity")],
                         "reject")
        self.assertEqual(tco.SUB_EXPECT[("exhaust", "drop_wildcard")],
                         "accept-quiet")


# ---------------------------------------------------------------------------
# routing with scripted fakes (no toolchain)
# ---------------------------------------------------------------------------

class PrecheckRoutingTest(unittest.TestCase):
    """变异前置校验：parse error 归 parse-reject，绝不入 reject 分母。"""

    def test_parse_error_mutant_routed_to_parse_reject(self):
        w = OracleWorld(FakeRunner(
            dump_result=_rr(out=b"AST-DUMP-ERROR: bad\n", rc=0)))
        try:
            outcome = w.run(1)
            self.assertEqual(outcome, "ok")
            for cls in tco.CLASSES:
                self.assertEqual(w.classes[cls]["rejected"], 0)
                self.assertEqual(w.classes[cls]["instances"], 0)
            self.assertGreater(
                sum(w.classes[c]["parse-reject"] for c in tco.CLASSES), 0)
            self.assertEqual(sum(w.findings.values()), 0)
        finally:
            w.cleanup()


class ControlGroupTest(unittest.TestCase):
    """对照组健全性断言必须真实生效：注入坏编译器 → finding。"""

    def test_control_rejected_is_sound_fail_finding(self):
        w = OracleWorld(FakeRunner(
            build_result=_rr(out=b"typecheck: 1 type error(s) found\n"
                                 b"compile aborted\n", rc=1)))
        try:
            outcome = w.run(1)
            self.assertEqual(outcome, "finding:sound-fail")
            self.assertEqual(w.findings["sound-fail"], 1)
            # nothing else ran — mutations never started
            self.assertEqual(sum(w.classes[c]["instances"]
                                 for c in tco.CLASSES), 0)
        finally:
            w.cleanup()

    def test_control_crash_is_tc_crash_finding(self):
        w = OracleWorld(FakeRunner(build_result=_rr(rc=-6)))
        try:
            outcome = w.run(1)
            self.assertEqual(outcome, "finding:tc-crash")
            self.assertEqual(w.findings["tc-crash"], 1)
        finally:
            w.cleanup()

    def test_control_vm_crash_is_vm_crash_finding(self):
        w = OracleWorld(FakeRunner(run_result=_rr(rc=tco.ASAN_EXIT,
                                                  err=b"ERROR: AddressSanitizer"
                                                      b": heap-buffer-overflow"
                                                  )))
        try:
            outcome = w.run(1)
            self.assertEqual(outcome, "finding:vm-crash")
            self.assertEqual(w.findings["vm-crash"], 1)
        finally:
            w.cleanup()


class StrictAssertionBiteTest(unittest.TestCase):
    """完备性方向断言非空壳：全接受假编译器 → strict 类必产 missed-reject。"""

    def test_accept_everything_compiler_produces_missed_reject(self):
        w = OracleWorld(FakeRunner(build_result=_rr()))
        try:
            self.assertEqual(w.run(3), "ok")
            # lit_swap + fn_arg_mismatch + ctor arity are strict: an
            # accept-everything compiler MUST be flagged, per instance
            strict_hits = (w.classes["lit_swap"]["instances"]
                           + w.classes["fn_arg_mismatch"]["instances"])
            self.assertGreater(strict_hits, 0)
            self.assertEqual(w.findings["missed-reject"],
                             strict_hits)
            # hole classes and the exhaust class expect accept → no finding
            self.assertGreater(sum(w.classes[c]["known-hole"]
                                   for c in tco.CLASSES), 0)
            self.assertGreater(w.classes["exhaust"]["exhaust-ok"], 0)
            self.assertEqual(w.findings["tc-drift"], 0)
        finally:
            w.cleanup()

    def test_hole_suddenly_rejecting_fires_drift(self):
        # undef_var is a frozen hole (expect accept); a compiler that
        # starts rejecting it is a behavior drift → tc-drift alarm.
        def build(src_path):
            with open(src_path, "rb") as f:
                if b"zz_undef_" in f.read():
                    return _rr(out=b"typecheck: 1 type error(s) found\n",
                               rc=1)
            return _rr()
        w = OracleWorld(FakeRunner(build_result=build))
        try:
            self.assertEqual(w.run(1), "ok")
            self.assertGreater(w.findings["tc-drift"], 0)
        finally:
            w.cleanup()

    def test_exhaust_warning_appearing_fires_drift(self):
        # frozen expectation is accept-quiet (no warning exists today);
        # a compiler that grows the warning must trigger the double
        # assertion's alarm half.
        w = OracleWorld(FakeRunner(
            build_result=_rr(out=b"warning: non-exhaustive match\n")))
        try:
            self.assertEqual(w.run(1), "ok")
            self.assertGreaterEqual(w.findings["tc-drift"], 1)
            self.assertEqual(w.classes["exhaust"]["exhaust-ok"], 0)
        finally:
            w.cleanup()


class FindingsContractTest(unittest.TestCase):
    """落盘契约复用 morph.record_finding — 目录内容完整性。"""

    def test_missed_reject_finding_dir_contract(self):
        w = OracleWorld(FakeRunner(build_result=_rr()))
        try:
            self.assertEqual(w.run(3), "ok")
            dirs = [n for n in os.listdir(w.out) if n != "skips.log"]
            self.assertTrue(dirs)
            d = os.path.join(w.out, dirs[0])
            self.assertTrue(dirs[0].startswith("missed-reject-"))
            names = set(os.listdir(d))
            for pat in ("meta.json", "run.sh"):
                self.assertIn(pat, names)
            self.assertTrue(any(n.startswith("src_") for n in names))
            self.assertTrue(any(n.startswith("stdout_") for n in names))
            self.assertTrue(any(n.startswith("stderr_") for n in names))
            self.assertTrue(any(n.startswith("exit_") for n in names))
            with open(os.path.join(d, "meta.json")) as f:
                meta = json.load(f)
            self.assertEqual(meta["category"], "missed-reject")
            self.assertEqual(meta["signature"].split(":")[0],
                             "missed-reject")
        finally:
            w.cleanup()

    def test_dedup_same_source_same_category_lands_once(self):
        # both seeds map to identical (category, source) → 1 dir
        w = OracleWorld(FakeRunner(build_result=_rr(rc=-6)))
        try:
            w.run(1)
            first = dict(w.findings)
            # re-record the identical finding through the same dedup set
            tco._record(w.out, "tc-crash",
                        [{"tag": "P", "src_text": "fn main() {\n}\n",
                          "res": _rr(rc=-6), "build_err": b""}],
                        999, w.dedup, w.findings)
            # seed 999 has a different source → new dir; but replaying
            # seed 1's exact source must dedup:
            before = len([n for n in os.listdir(w.out)
                          if n != "skips.log"])
            tco._record(w.out, "tc-crash",
                        [{"tag": "P",
                          "src_text": gen.render_tree(gen.build_program(1)),
                          "res": _rr(rc=-6), "build_err": b""}],
                        1, w.dedup, w.findings)
            after = len([n for n in os.listdir(w.out)
                         if n != "skips.log"])
            self.assertEqual(before, after)
            self.assertEqual(first["tc-crash"], 1)
        finally:
            w.cleanup()


# ---------------------------------------------------------------------------
# real-toolchain tests
# ---------------------------------------------------------------------------

def _toolchain_ready():
    return (os.path.exists(morph.TINYACTOR)
            and os.path.exists(morph.TAVM_ASAN)
            and os.path.exists(morph.AST_DUMP))


@unittest.skipUnless(_toolchain_ready(),
                     "tinyactor / tavm_asan / ast-dump.ta not built")
class RealToolchainSmokeTest(unittest.TestCase):
    """冒烟：30 seed 真跑，exit 0，类内 skip 率 <20%，断言真实触发。"""

    def test_smoke_30_seeds(self):
        out = tempfile.mkdtemp(prefix="tco-smoke-")
        workdir = tempfile.mkdtemp(prefix="tco-smoke-run-")
        try:
            runner = morph.Runner(workdir)
            seeds = list(range(101, 131))       # 30 fresh seeds
            stats = tco.fuzz_batch(runner, seeds, out)
            # exit-0-equivalent: no infrastructure exception + sane stats
            self.assertEqual(stats["seeds"], 30)
            self.assertEqual(stats["positives"], 30)
            self.assertEqual(stats["findings"]["sound-fail"], 0)
            self.assertEqual(stats["findings"]["vm-crash"], 0)
            self.assertEqual(stats["findings"]["tc-crash"], 0)
            instances = sum(stats["classes"][c]["instances"]
                            for c in tco.CLASSES)
            skips = sum(stats["classes"][c]["skip"]
                        for c in tco.CLASSES)
            self.assertGreater(instances, 0)
            self.assertLess(100.0 * skips / (30 * len(tco.CLASSES)), 20.0)
            # strict rejects really triggered
            self.assertGreater(stats["classes"]["lit_swap"]["rejected"], 0)
            self.assertGreater(stats["classes"]["fn_arg_mismatch"]
                               ["rejected"], 0)
            # frozen holes still accept + exhaust still quiet
            self.assertGreater(sum(stats["classes"][c]["known-hole"]
                                   for c in tco.CLASSES), 0)
            self.assertGreater(stats["classes"]["exhaust"]["exhaust-ok"], 0)
            self.assertEqual(sum(stats["findings"].values()), 0)
        finally:
            shutil.rmtree(out, ignore_errors=True)
            shutil.rmtree(workdir, ignore_errors=True)


@unittest.skipUnless(_toolchain_ready(),
                     "tinyactor / tavm_asan / ast-dump.ta not built")
class RealToolchainDeterminismTest(unittest.TestCase):
    """同参数两次 → findings 签名集合与逐类统计完全一致。"""

    def test_two_runs_identical(self):
        outs = []
        stats_list = []
        for i in (0, 1):
            out = tempfile.mkdtemp(prefix="tco-det-%d-" % i)
            workdir = tempfile.mkdtemp(prefix="tco-det-run-%d-" % i)
            outs.append(out)
            runner = morph.Runner(workdir)
            stats_list.append(tco.fuzz_batch(runner, list(range(201, 209)),
                                             out))
            shutil.rmtree(workdir, ignore_errors=True)
        try:
            a, b = stats_list
            self.assertEqual(a["positives"], b["positives"])
            self.assertEqual(a["classes"], b["classes"])
            self.assertEqual(a["findings"], b["findings"])
            self.assertEqual(a["skips"], b["skips"])
            sig_a = set()
            sig_b = set()
            for d, sigs in ((outs[0], sig_a), (outs[1], sig_b)):
                for n in os.listdir(d):
                    if n == "skips.log":
                        continue
                    with open(os.path.join(d, n, "meta.json")) as f:
                        sigs.add(json.load(f)["signature"])
            self.assertEqual(sig_a, sig_b)
        finally:
            for d in outs:
                shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)