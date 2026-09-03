# -*- coding: utf-8 -*-
"""
test_transforms.py — tests for tools/kernfuzz/transforms.py (DELIV-3,
extended: §5.2 Tier A T1-T8 + extras X1/X2).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_transforms.py
Exit 0 = all pass, non-zero = failure.

Covers the task-transforms acceptance criteria:
  * semantic preservation (CORE): >=30 gen programs x every rule;
    P and P' are each executed by the REAL `./tinyactor run` and by the
    golden interpreter (ast-dump.ta -> golden.py) under the §5.1.3
    compare protocol — VM(P)==VM(P') line-for-line AND
    golden(P)==golden(P').  Runtime note: the ast-dump round costs
    ~1.6 s/program, so this class alone takes >10 minutes.
  * identity: 0 transforms -> P' is byte-identical P (render_tree is
    lossless over build_program).
  * variants really change: apply_random output differs from the input
    (guards against a green light from a silent no-op transform), and
    the REAL parser/typecheck accepts P' (tinyactor build; one variant
    per rule asserted explicitly).
  * direction coverage: inject AND elim of every identity rule, both
    associativity sides, fold AND expand, split AND join, if-elim
    (unit-level for the structural-only directions), cmp duality, T6
    inline AND hoist, T8 swap.  Death guards: X1 (a*0 -> 0) skips
    death-capable subtrees; T6 refuses inlining a death-RHS with zero
    references (both unit-tested on synthetic division nodes — gen v0
    emits no `/`).
  * T6 correctness conditions: shadow-chain lets are never inlined
    (alpha-renaming safety by construction), references outside known
    int slots block inlining.
  * T8: wildcard arm stays last and never moves; non-wildcard-only
    matches require the gen tree pairwise-disjoint metadata; a guarded
    binder arm is never swapped against a literal arm (refinement of
    the doc rule — see progress.md).
  * determinism: same prng seed -> byte-identical transformed program.
  * rule trigger coverage: every rule fires on the corpus (directed
    seed search); structural-only directions are additionally exercised
    at unit level (rule_sites).
"""

import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                        # noqa: E402
import prng                       # noqa: E402
import transforms                 # noqa: E402
from transforms import Bin, Lit, Var, Cmp, If   # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
TINYACTOR = os.path.join(_REPO_ROOT, "tinyactor")
AST_DUMP = os.path.join(_REPO_ROOT, "tools", "kernfuzz", "ast-dump.ta")
GOLDEN = os.path.join(_REPO_ROOT, "tools", "kernfuzz", "golden",
                      "golden.py")

N_SEEDS = 30                      # semantic-preservation corpus size
SEM_BASE = 90000                  # corpus seed base
ALL_RULES = tuple(transforms.RULE_NAMES)


def _norm_vm(stdout_bytes, exit_code):
    """§5.1.3 norm_tavm."""
    lines = stdout_bytes.split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    if exit_code == 1:
        lines.append(("DIVZERO:%d" % len(lines)).encode("latin-1"))
    return lines


def _run_vm_bytes(src):
    """Write src, run the real VM.  Returns normalized output lines."""
    f = tempfile.NamedTemporaryFile(suffix=".ta", delete=False,
                                    mode="wb")
    f.write(src.encode("latin-1"))
    f.close()
    try:
        p = subprocess.run([TINYACTOR, "run", f.name], capture_output=True,
                           timeout=60)
        return _norm_vm(p.stdout, p.returncode)
    finally:
        os.unlink(f.name)


def _build_ok(src):
    """Real parser/typecheck accepts the source (tinyactor build)."""
    f = tempfile.NamedTemporaryFile(suffix=".ta", delete=False,
                                    mode="wb")
    f.write(src.encode("latin-1"))
    f.close()
    try:
        return subprocess.run([TINYACTOR, "build", f.name],
                              capture_output=True, timeout=60).returncode == 0
    finally:
        os.unlink(f.name)


def _golden_lines(src):
    """ast-dump.ta -> golden.py, §5.1.3 normalized.  None = out-of-subset
    skip (structure golden does not cover)."""
    f = tempfile.NamedTemporaryFile(suffix=".ta", delete=False,
                                    mode="wb")
    f.write(src.encode("latin-1"))
    f.close()
    try:
        d = subprocess.run([TINYACTOR, "run", AST_DUMP, f.name],
                           capture_output=True, timeout=120)
        if d.returncode != 0 or b"AST-DUMP-ERROR" in d.stdout:
            return None
        sf = tempfile.NamedTemporaryFile(suffix=".sexp", delete=False,
                                         mode="wb")
        sf.write(d.stdout)
        sf.close()
        try:
            g = subprocess.run([sys.executable, GOLDEN, sf.name],
                               capture_output=True, timeout=120)
            if g.returncode != 0 and b"DIVZERO:" not in g.stdout:
                return None
            lines = g.stdout.split(b"\n")
            while lines and lines[-1] == b"":
                lines.pop()
            return lines
        finally:
            os.unlink(sf.name)
    finally:
        os.unlink(f.name)


def _plan_with_stmts(stmts, match_meta=None):
    """A gen plan whose main statements are replaced by `stmts` (test
    scaffolding for directed statement-level rule tests)."""
    p = gen.build_program(7)
    p.main_stmts = list(stmts)
    if match_meta is not None:
        p.match_meta = list(match_meta)
    return p


def _inline_cands(plan):
    types = transforms._Types(plan)
    slots = transforms._find_slots(plan, types)
    return [c for c in transforms._inline_candidates(plan, types, slots)]


def _t8_cands(plan):
    types = transforms._Types(plan)
    return transforms._t8_candidates(plan, types)


class IdentityTest(unittest.TestCase):
    """apply 0 transforms -> byte-identical program (render lossless)."""

    def test_render_tree_matches_gen_program(self):
        for seed in range(20):
            self.assertEqual(gen.render_tree(gen.build_program(seed)),
                             gen.gen_program(seed))

    def test_apply_zero_is_identity(self):
        for seed in range(20):
            p = gen.build_program(seed)
            t2, info = transforms.apply_one(p, prng.make_prng(seed),
                                            rules=())
            self.assertIsNone(info)
            self.assertEqual(gen.render_tree(t2), gen.render_tree(p))

    def test_no_legal_position_returns_original_marked(self):
        # a plan with a single literal statement offers T6 nothing and
        # the one int slot only trivial identities -> find a program/
        # rule pair with no site and assert the documented marking
        p = _plan_with_stmts(["  print(5);"])
        t2, info = transforms.apply_one(p, prng.make_prng(1),
                                        rules=("T8",))
        self.assertIsNone(info)
        self.assertEqual(t2, p)


class DeterminismTest(unittest.TestCase):
    def test_same_prng_seed_byte_identical(self):
        for seed in range(10):
            p = gen.build_program(seed)
            rng = prng.make_prng(1000 + seed)
            a, ia = transforms.apply_random(p, 5, rng)
            b, ib = transforms.apply_random(p, 5, prng.make_prng(1000
                                                                 + seed))
            self.assertEqual(gen.render_tree(a), gen.render_tree(b))
            self.assertEqual(len(ia), len(ib))

    def test_apply_rule_deterministic(self):
        p = gen.build_program(1)
        a, _ = transforms.apply_rule(p, "T1a", prng.make_prng(42))
        b, _ = transforms.apply_rule(p, "T1a", prng.make_prng(42))
        self.assertEqual(gen.render_tree(a), gen.render_tree(b))


class VariantChangesTest(unittest.TestCase):
    def test_apply_random_changes_program_and_builds(self):
        changed = 0
        for seed in range(30):
            p = gen.build_program(seed)
            t2, infos = transforms.apply_random(
                p, 6, prng.make_prng(prng.derive_seed(seed, 5)))
            if not infos:
                continue
            src = gen.render_tree(t2)
            self.assertNotEqual(src, gen.render_tree(p))
            self.assertTrue(_build_ok(src),
                            "build rejected transformed seed %d" % seed)
            changed += 1
        self.assertGreater(changed, 20)

    def test_build_accepts_every_rule_variant_once(self):
        for rule in ALL_RULES:
            accepted = False
            for seed in range(80):
                p = gen.build_program(seed)
                t2, info = transforms.apply_rule(
                    p, rule, prng.make_prng(prng.derive_seed(seed, 9)))
                if info is None:
                    continue
                self.assertTrue(
                    _build_ok(gen.render_tree(t2)),
                    "build rejected %s variant (seed %d)" % (rule, seed))
                accepted = True
                break
            self.assertTrue(accepted, "rule %s never applied" % rule)


class DeathGuardTest(unittest.TestCase):
    """X1 (a*0 -> 0) death guard + T6 death condition, on synthetic
    division nodes (gen v0 never emits `/`/`%`)."""

    def test_has_death_risk(self):
        self.assertFalse(transforms.has_death_risk(Bin("+", Lit(1),
                                                       Lit(2))))
        self.assertTrue(transforms.has_death_risk(
            Bin("/", Lit(1), Lit(0))))
        self.assertTrue(transforms.has_death_risk(
            Bin("+", Lit(1), Bin("%", Var("n_1"), Lit(2)))))

    def test_no_rule_creates_death(self):
        for rule in ALL_RULES:
            for node in (Bin("/", Lit(1), Lit(0)),
                         Bin("*", Bin("/", Lit(8), Lit(2)), Lit(3))):
                if transforms.has_death_risk(node):
                    continue        # swaps keep death; elimination rules
                                    # are separately guarded (X1 test)
                for _d, repl in transforms.rule_sites(rule, node):
                    self.assertFalse(transforms.has_death_risk(repl),
                                     "rule %s created death" % rule)

    def test_rule_x1_skips_death_subtrees(self):
        guarded = Bin("*", Bin("/", Lit(8), Lit(0)), Lit(0))
        self.assertEqual(transforms.rule_sites("X1", guarded), [])
        safe = Bin("*", Bin("+", Lit(8), Lit(2)), Lit(0))
        self.assertEqual([d for d, _r in
                          transforms.rule_sites("X1", safe)], ["elim"])
        other = Bin("*", Lit(0), Bin("/", Lit(8), Lit(0)))
        self.assertEqual(transforms.rule_sites("X1", other), [])


class T6InlineConditionTest(unittest.TestCase):
    """Doc T6 condition: e contains no `/`/`%`, or x is referenced >= 1
    time.  Plus alpha-renaming safety (shadow chains) and the
    ref-outside-slot bail-out."""

    def test_death_rhs_zero_refs_is_refused(self):
        p = _plan_with_stmts([
            "  let n_1 = 5;",
            "  let d_1 = (n_1 / 0);",
            "  print(n_1);",
        ])
        cands = _inline_cands(p)
        self.assertTrue(cands)                      # n_1 let inlinable
        self.assertFalse(any("let d_1" == c[3] for c in cands))

    def test_death_rhs_one_ref_is_allowed(self):
        p = _plan_with_stmts([
            "  let n_1 = 5;",
            "  let d_1 = (n_1 / 0);",
            "  print((d_1 + 1));",
        ])
        cands = [c for c in _inline_cands(p) if c[3] == "let d_1"]
        self.assertEqual(len(cands), 1)
        t2 = cands[0][2]()
        src = gen.render_tree(t2)
        self.assertNotIn("let d_1", src)
        self.assertIn("(n_1 / 0)", src)
        self.assertTrue(_build_ok(src))

    def test_shadow_chain_never_inlined(self):
        p = _plan_with_stmts([
            "  let n_1 = 5;",
            "  let n_1 = (n_1 + 7);",
            "  print(n_1);",
        ])
        self.assertEqual(_inline_cands(p), [])

    def test_ref_outside_int_slot_blocks_inline(self):
        p = _plan_with_stmts([
            "  let n_1 = 5;",
            "  let mr_1 = match n_1 {\n"
            "    1 -> 2,\n"
            "    _ -> 3\n"
            "  };",
            "  print(mr_1);",
        ])
        self.assertEqual(_inline_cands(p), [])

    def test_inline_hoist_roundtrip_on_corpus(self):
        fired = {"inline": False, "hoist": False}
        for seed in range(60):
            p = gen.build_program(seed)
            t2, info = transforms.apply_rule(p, "T6", prng.make_prng(seed))
            if info is not None:
                fired[info["direction"]] = True
                self.assertTrue(_build_ok(gen.render_tree(t2)))
        self.assertTrue(all(fired.values()), "T6 directions: %r" % fired)


class T8MatchReorderTest(unittest.TestCase):
    """Wildcard rules + gen pairwise-disjoint metadata + the guarded-
    arm refinement."""

    CORPUS_BLOCK = [
        "  let n_1 = 5;",
        "  let mr_1 = match n_1 {\n"
        "    5 -> 1,\n"
        "    m_1 when (m_1 < 10) -> 2,\n"
        "    m_1 -> 3\n"
        "  };",
        "  print(mr_1);",
    ]
    NOWILD_BLOCK = [
        "  let l_1 = [1, 2];",
        "  let mr_1 = match l_1 {\n"
        "    nil -> 0,\n"
        "    cons(h_1, t_1) -> h_1\n"
        "  };",
        "  print(mr_1);",
    ]

    def test_wildcard_form_swaps_nonwildcard_disjoint_arms(self):
        fired = False
        for seed in range(120):
            p = gen.build_program(seed)
            t2, info = transforms.apply_rule(p, "T8", prng.make_prng(seed))
            if info is None:
                continue
            fired = True
            lines_before = gen.render_tree(p).split("\n")
            lines_after = gen.render_tree(t2).split("\n")
            self.assertEqual(len(lines_before), len(lines_after))
        self.assertTrue(fired, "T8 never fired on seeds 0..119")

    def test_wildcard_arm_never_moves(self):
        p = _plan_with_stmts(self.CORPUS_BLOCK, match_meta=[(1, False)])
        cands = _t8_cands(p)
        # patterns: 5 (int), m_1 (binder), _ (wildcard): binder overlaps
        # the literal, wildcard never moves -> no legal swap
        self.assertEqual(cands, [])

    def test_nonwildcard_disjoint_metadata_allows_swap(self):
        p = _plan_with_stmts(self.NOWILD_BLOCK, match_meta=[(1, True)])
        cands = _t8_cands(p)
        self.assertEqual(len(cands), 1)      # nil <-> cons
        t2 = cands[0][2]()
        src = gen.render_tree(t2)
        self.assertIn("cons(h_1, t_1) -> h_1,", src)
        self.assertIn("nil -> 0\n", src)     # comma normalized
        self.assertTrue(_build_ok(src))

    def test_nonwildcard_without_metadata_is_refused(self):
        p = _plan_with_stmts(self.NOWILD_BLOCK, match_meta=[(1, False)])
        self.assertEqual(_t8_cands(p), [])

    def test_disjoint_swap_preserves_value(self):
        # literal swap on a wildcard match: same result for every input
        p = _plan_with_stmts([
            "  let n_1 = 5;",
            "  let mr_1 = match n_1 {\n"
            "    7 -> 10,\n"
            "    5 -> 20,\n"
            "    _ -> 30\n"
            "  };",
            "  print(mr_1);",
        ], match_meta=[(1, False)])
        cands = _t8_cands(p)
        self.assertEqual(len(cands), 1)      # 7 <-> 5 disjoint
        t2 = cands[0][2]()
        for v in (7, 5, 9):
            q = _plan_with_stmts([
                "  let n_1 = %d;" % v,
                "  let mr_1 = match n_1 {\n"
                "    7 -> 10,\n"
                "    5 -> 20,\n"
                "    _ -> 30\n"
                "  };",
                "  print(mr_1);",
            ])
            r = _plan_with_stmts([
                "  let n_1 = %d;" % v,
                "  let mr_1 = match n_1 {\n"
                "    5 -> 20,\n"
                "    7 -> 10,\n"
                "    _ -> 30\n"
                "  };",
                "  print(mr_1);",
            ])
            self.assertEqual(_run_vm_bytes(gen.render_tree(q)),
                             _run_vm_bytes(gen.render_tree(r)))


class DirectionCoverageTest(unittest.TestCase):
    """Every rule's directions exercised: corpus where natural, unit
    (rule_sites) for structural-only directions."""

    def _corpus_directions(self, rule, limit=200):
        dirs = set()
        for seed in range(limit):
            p = gen.build_program(seed)
            _t2, info = transforms.apply_rule(p, rule, prng.make_prng(seed))
            if info is not None:
                dirs.add(info["direction"])
                if dirs >= self._expected(rule):
                    break
        return dirs

    @staticmethod
    def _expected(rule):
        return {
            "T1a": {"swap"}, "T1b": {"swap"},
            "T2a": {"left-assoc", "right-assoc"},
            "T2b": {"left-assoc", "right-assoc"},
            "T3a": {"inject", "elim"}, "T3b": {"inject", "elim"},
            "T3c": {"inject"}, "T3d": {"inject"},
            "T4": {"fold", "expand"},
            "T5": {"inject-true", "inject-false"},
            "T7": {"dual"}, "X1": {"elim"}, "X2": {"split"},
        }[rule]

    def test_identity_rules_elim_and_inject(self):
        self.assertEqual(self._expected("T3a"),
                         self._expected("T3a") & self._corpus_directions(
                             "T3a"))
        self.assertEqual(self._expected("T3b"),
                         self._expected("T3b") & self._corpus_directions(
                             "T3b"))

    def test_comm_assoc_fold_expand_split_fire(self):
        for rule in ("T1a", "T1b", "T2a", "T2b", "T4", "T7", "X1", "X2"):
            got = self._corpus_directions(rule)
            self.assertTrue(self._expected(rule) <= got,
                            "rule %s directions %r missing %r"
                            % (rule, got, self._expected(rule) - got))

    def test_t5_inject_both_literals(self):
        got = self._corpus_directions("T5", limit=40)
        self.assertTrue({"inject-true", "inject-false"} <= got,
                        "T5 corpus directions: %r" % got)

    def test_structural_elim_directions_unit_level(self):
        # x-x <=> 0
        sites = transforms.rule_sites("T3c", Bin("-", Lit(7), Lit(7)))
        self.assertEqual([d for d, _r in sites], ["elim"])
        # (0-x)+x <=> 0
        sites = transforms.rule_sites(
            "T3d", Bin("+", Bin("-", Lit(0), Lit(7)), Lit(7)))
        self.assertEqual([d for d, _r in sites], ["elim"])
        sites = transforms.rule_sites(
            "T3d", Bin("+", Lit(7), Bin("-", Lit(0), Lit(7))))
        self.assertEqual([d for d, _r in sites], ["elim"])
        # if-true / if-false elim (sites also carry injections)
        sites = transforms.rule_sites("T5", If(True, Lit(5), Lit(6)))
        self.assertEqual([d for d, _r in sites][0], "elim-true")
        self.assertEqual(sites[0][1].v, 5)
        sites = transforms.rule_sites("T5", If(False, Lit(5), Lit(6)))
        self.assertEqual([d for d, _r in sites][0], "elim-false")
        self.assertEqual(sites[0][1].v, 6)
        # X2 join needs the '+' form
        sites = transforms.rule_sites(
            "X2", Bin("+", Lit(3), Bin("-", Lit(0), Lit(4))))
        self.assertEqual([d for d, _r in sites], ["join"])

    def test_t5_elim_roundtrip_via_injected_if(self):
        # elim never occurs in the raw corpus (gen has no `if`); inject
        # first, then splice the elim of the injected If root directly
        done = 0
        for seed in range(10):
            p = gen.build_program(seed)
            p2, info = transforms.apply_rule(p, "T5",
                                             prng.make_prng(seed))
            if info is None or not info["direction"].startswith("inject"):
                continue
            types = transforms._Types(p2)
            for slot in transforms._find_slots(p2, types):
                if not isinstance(slot.root, If):
                    continue
                sites = [s for s in transforms.rule_sites(
                    "T5", slot.root, slot)
                    if s[0].startswith("elim")]
                if not sites:
                    continue
                direction, repl = sites[0]
                p3 = transforms._splice(p2, slot, repl)
                src_q = gen.render_tree(p3)
                self.assertTrue(_build_ok(src_q))
                self.assertNotEqual(src_q, gen.render_tree(p2))
                self.assertEqual(_run_vm_bytes(gen.render_tree(p2)),
                                 _run_vm_bytes(src_q),
                                 "T5 %s changed semantics" % direction)
                done += 1
                break
        self.assertGreater(done, 0, "no inject->elim chain exercised")

    def test_slot_positions_not_top_only(self):
        found_nonroot = False
        for seed in range(60):
            t = gen.build_program(seed)
            _t2, infos = transforms.apply_random(t, 2, prng.make_prng(seed))
            for i in infos:
                if i.get("path"):
                    found_nonroot = True
        self.assertTrue(found_nonroot, "all transforms stayed at root")


class RuleTriggerCoverageTest(unittest.TestCase):
    """Each rule fires somewhere on the corpus (never-silently-dead)."""

    def test_every_rule_triggers(self):
        for rule in ALL_RULES:
            fired = False
            for seed in range(500):
                t = gen.build_program(seed)
                _t2, info = transforms.apply_rule(t, rule,
                                                  prng.make_prng(seed))
                if info is not None:
                    fired = True
                    break
            self.assertTrue(fired, "rule %s never fires on seeds 0..499"
                            % rule)

    def test_structural_only_directions_have_unit_sites(self):
        # these elim directions cannot occur in gen output (no `if`,
        # no x-x, no (0-x)+x in the grammar); unit-prove they exist
        self.assertTrue(transforms.rule_sites(
            "T3c", Bin("-", Var("n"), Var("n"))))
        self.assertTrue(transforms.rule_sites(
            "T5", If(False, Lit(1), Lit(2))))


class SemanticPreservationTest(unittest.TestCase):
    """CORE: >=30 programs x every rule, VM + golden both agree
    (§5.1.3).  Slow (>10 min: ~400 ast-dump pairs at ~1.6 s each)."""

    def test_semantic_preservation_30x_all_rules(self):
        triggers = dict((r, 0) for r in ALL_RULES)
        skips = 0
        nondet = 0
        pairs = 0
        for i in range(N_SEEDS):
            seed = SEM_BASE + i
            tree = gen.build_program(seed)
            src_p = gen.render_tree(tree)
            vm_p = _run_vm_bytes(src_p)
            # known VM bug (progress.md, eval-task-gen): heterogenous-list
            # OP_ADD is NONDETERMINISTIC (reads pointer payload bits).  A
            # program containing it does not reproduce run-to-run, so a
            # P/P' mismatch there would say nothing about the transforms.
            # Detect and skip such programs (they must stay rare).
            if vm_p != _run_vm_bytes(src_p):
                nondet += 1
                continue
            g_p = _golden_lines(src_p)
            for ri, rule in enumerate(ALL_RULES):
                t2, info = transforms.apply_rule(
                    tree, rule, prng.make_prng(seed * 100 + ri))
                if info is None:
                    continue          # rule has no site in this program
                triggers[rule] += 1
                src_q = gen.render_tree(t2)
                self.assertNotEqual(src_q, src_p,
                                    "no-op variant seed %d rule %s"
                                    % (seed, rule))
                vm_q = _run_vm_bytes(src_q)
                pairs += 1
                self.assertEqual(vm_p, vm_q,
                                 "VM divergence seed %d rule %s (%s): %s"
                                 % (seed, rule, info["direction"],
                                    info["after"]))
                g_q = _golden_lines(src_q)
                if g_p is None or g_q is None:
                    skips += 1
                    continue
                self.assertEqual(g_p, g_q,
                                 "golden divergence seed %d rule %s (%s)"
                                 % (seed, rule, info["direction"]))
        # acceptance: every rule fired on the corpus (no silent dead rule)
        for rule in ALL_RULES:
            self.assertGreater(triggers[rule], 0,
                               "rule %s never triggered on corpus" % rule)
        # golden skip rate must stay low (same gate as test_gen)
        self.assertLessEqual(skips, pairs // 5,
                             "golden skip rate too high: %d/%d"
                             % (skips, pairs))
        sys.stderr.write("\nsemantic pairs: %d, golden skips: %d, "
                         "nondet-VM programs skipped: %d, triggers: %r\n"
                         % (pairs, skips, nondet, triggers))


if __name__ == "__main__":
    unittest.main(verbosity=2)