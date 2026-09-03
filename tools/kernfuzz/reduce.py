# -*- coding: utf-8 -*-
"""reduce.py — failure-case reducer (kernel-fuzzing DELIV-4, §5.5).

Shrinks a morph finding to a minimal program that still reproduces the
SAME failure.  The reproduction criterion is the §5.5 root-cause match,
NOT the full §5.4 signature (reduction necessarily changes the source
hash):

  * category must not change, and
  * mismatch        : golden(P) vs VM(P) diverge at the SAME first
                      differing output-line index as the original;
  * anchor-crash    : golden(P) vs VM(P) divergence still exists (the
                      diverging values themselves MAY change — reduction
                      changes the program, hence its outputs; same rule
                      as morph's anchor assertion);
  * tavm-crash      : normalized stderr first line identical
                      (M-12: `0x[0-9a-fA-F]+` -> `0xADDR`);
  * unexpected-divzero: VM exit 1 + DIVZERO protocol line present;
  * hang            : run still hits the §5.4 5s timeout;
  * dump-fail       : ast-dump still fails;
  * build-fail      : build still fails.

Every candidate is checked through the SAME §5.4 two-step call card as
morph — build/run/classify are IMPORTED from morph.py (Runner, norm_*,
classify_run), never re-implemented here, so runner logic cannot drift.

Three-phase strategy (§5.5), each step kept only if the criterion still
holds, rolled back otherwise:
  1. whole-line deletion (ddmin over lines; generated multi-line layout
     guarantees most line deletions stay parseable);
  2. expression subtree -> literal `0` (balanced-paren expression spans
     in the full-parens generated source, largest span first; the AST
     Pair tree from the ast-dump — read with the shared sexp.py reader —
     is reported alongside as the structural reference);
  3. delta-debugging token deletion (last resort).

Line count is best-effort (≤15 is a goal, not a gate); acceptance only
requires the reduced program to satisfy the criterion.

Runner logic is reused from morph.py.  The Pair tree comes from the
toolchain-wide sexp.py reader (tools/kernfuzz/sexp.py — per its
docstring it is THE reader/writer for transforms/reduce/runner;
golden/sexp.py is golden-only and stays untouched).

CLI:
    python3 reduce.py <finding_dir | src.ta> --category <cat>
            [--out DIR] [--max-evals N] [--budget-s S]

`--category` may be omitted for a finding-dir input (taken from its
meta.json).  For a finding dir, artifacts land in <dir>/reduced/;
for a bare source file, in <file's dir>/reduced/ (or --out).  Exit 0
when the final program reproduces, 1 when no reduction stuck (original
returned), 2 on infrastructure errors.

Stdlib-only, host Python 3.
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import morph                                  # noqa: E402  (runner reuse)
import sexp                                   # noqa: E402  (Pair tree)

REPO_ROOT = morph._REPO_ROOT

# §5.5 M-12: crash feature string = stderr first line, addresses masked.
_ADDR_RE = re.compile(r"0x[0-9a-fA-F]+")

# categories whose reproduction check needs the golden side channel
_NEEDS_GOLDEN = ("mismatch", "anchor-crash")


class ReduceError(Exception):
    """Infrastructure / usage error — aborts with exit 2."""


class BudgetExhausted(Exception):
    """Internal: evaluation cap or wall-clock budget ran out."""


class Budget(object):
    """Shared cap on oracle evaluations + wall-clock deadline."""

    def __init__(self, max_evals, budget_s):
        self.max_evals = max_evals
        self.deadline = time.time() + budget_s
        self.evals = 0

    def tick(self):
        self.evals += 1
        if self.evals > self.max_evals:
            raise BudgetExhausted("eval cap %d reached" % self.max_evals)
        if time.time() > self.deadline:
            raise BudgetExhausted("time budget reached")


# ---------------------------------------------------------------------------
# Features (§5.5 root-cause characterization)
# ---------------------------------------------------------------------------

def norm_crash_feature(err_bytes):
    """M-12: stderr first line with `0x...` masked to `0xADDR`."""
    first = err_bytes.split(b"\n", 1)[0].decode("latin-1", "replace")
    return _ADDR_RE.sub("0xADDR", first).strip()


def first_diff_index(a, b):
    """First index where the two norm line lists differ, or None."""
    n = max(len(a), len(b))
    for i in range(n):
        if i >= len(a) or i >= len(b) or a[i] != b[i]:
            return i
    return None


# ---------------------------------------------------------------------------
# Observation + oracle
# ---------------------------------------------------------------------------

class Observation(object):
    """Everything one candidate run tells us (single call card pass)."""

    __slots__ = ("build_ok", "build_err", "vm", "golden")

    def __init__(self, build_ok, build_err, vm, golden):
        self.build_ok = build_ok
        self.build_err = build_err
        self.vm = vm              # morph.RunResult or None (build-fail)
        self.golden = golden      # norm_golden line list or None


class Reducer(object):
    """§5.5 oracle for one (source, category) pair."""

    def __init__(self, category, runner, budget):
        if category not in morph.CATEGORIES:
            raise ReduceError("unknown category: %s" % category)
        self.category = category
        self.runner = runner
        self.budget = budget
        self.feature = None       # baseline root-cause feature

    # -- one pass of the §5.4 two-step call card --------------------------
    def observe(self, src_text):
        r = self.runner
        src_path = os.path.join(r.workdir, "cand.ta")
        with open(src_path, "wb") as f:
            f.write(src_text.encode("latin-1"))
        artifact = os.path.join(r.workdir, "cand.tabc")
        bp = r.build(src_path, artifact)
        if bp.rc != 0:
            return Observation(False, (bp.out + bp.err), None, None)
        res = r.run(artifact)
        golden = None
        if self.category in _NEEDS_GOLDEN \
                and morph.classify_run(res) is None:
            d = r.dump(src_path)
            dump_ok = (not d.timed_out and d.rc == 0 and bool(d.out)
                       and b"AST-DUMP-ERROR" not in d.out)
            if dump_ok:
                spath = os.path.join(r.workdir, "cand.sexp")
                with open(spath, "wb") as f:
                    f.write(d.out)
                g = r.golden_eval(spath)
                if g.rc == 0 or b"DIVZERO:" in g.out:
                    golden = morph.norm_golden(g.out)
                # g.rc != 0 without DIVZERO = golden itself blew up:
                # an anchor-class problem → candidate does not count as
                # a reproduced differential failure (golden stays None).
        return Observation(True, b"", res, golden)

    # -- root-cause feature of an observation (None = feature absent) -----
    def feature_of(self, obs):
        cat = self.category
        if cat == "build-fail":
            return None if obs.build_ok else "build-fail"
        if not obs.build_ok:
            return None
        if cat == "hang":
            return "timeout" if (obs.vm and obs.vm.timed_out) else None
        if cat == "tavm-crash":
            if obs.vm and morph.classify_run(obs.vm) == "tavm-crash":
                return norm_crash_feature(obs.vm.err)
            return None
        if cat == "unexpected-divzero":
            if obs.vm and obs.vm.rc == 1:
                return "divzero"
            return None
        if cat == "dump-fail":
            return "dump-fail" if obs.golden is None and obs.vm is not None \
                else None
        if cat == "mismatch":
            if obs.vm is None or obs.golden is None:
                return None
            v = morph.norm_tavm(obs.vm.out, obs.vm.rc)
            idx = first_diff_index(v, obs.golden)
            return None if idx is None else idx
        if cat == "anchor-crash":
            if obs.vm is None or obs.golden is None:
                return None
            v = morph.norm_tavm(obs.vm.out, obs.vm.rc)
            return "divergence" if v != obs.golden else None
        raise ReduceError("unhandled category %s" % cat)

    # -- establish the baseline feature from the ORIGINAL program ---------
    def establish_baseline(self, src_text):
        obs = self.observe(src_text)
        feat = self.feature_of(obs)
        if feat is None:
            raise ReduceError(
                "original program does not reproduce a %s root-cause "
                "feature — refusing to reduce a stale/non-reproducing "
                "input" % self.category)
        self.feature = feat
        return obs, feat

    # -- §5.5 criterion ----------------------------------------------------
    def reproduces(self, obs):
        feat = self.feature_of(obs)
        if feat is None:
            return False
        if self.category == "anchor-crash":
            return True                # any divergence, values may change
        return feat == self.feature    # mismatch: same first-diff index


# ---------------------------------------------------------------------------
# Text plumbing: lines / paren-group expressions / tokens
# ---------------------------------------------------------------------------

def find_paren_groups(text):
    """Spans (start, end_exclusive) of every `(...)` group in TA source,
    skipping string literals and // comments.  Generated code is
    full-parens (DEC-3), so these spans are exactly the printed
    expression subtrees (plus fn param lists / cons patterns, which the
    oracle rejects via build-fail when replaced)."""
    groups = []
    depth = 0
    start = 0
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':                      # string literal
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                groups.append((start, i + 1))
        i += 1
    return groups


def tokenize_keep_layout(text):
    """Token list for phase 3: atoms, single punct chars, string
    literals, comments and whitespace runs (whitespace keeps the line
    layout intact when untouched)."""
    toks = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            j = i
            while j < n and text[j].isspace():
                j += 1
            toks.append(text[i:j])
            i = j
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            toks.append(text[i:j])
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = i
            while j < n and text[j] != "\n":
                j += 1
            toks.append(text[i:j])
            i = j
        elif c.isalnum() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            toks.append(text[i:j])
            i = j
        else:
            toks.append(c)
            i += 1
    return toks


def tree_cons_count(dump_bytes):
    """Structural reference: number of cons nodes in the ast-dump Pair
    tree (shared sexp.py reader).  None when the dump does not parse."""
    try:
        tree = sexp.sexp_read_string(dump_bytes.decode("latin-1"))
    except (ValueError, IndexError):
        return None

    def walk(v):
        if isinstance(v, sexp.Pair):
            return 1 + walk(v.car) + walk(v.cdr)
        return 0
    return walk(tree)


# ---------------------------------------------------------------------------
# ddmin core (generic over item lists; only accepting steps survive)
# ---------------------------------------------------------------------------

def ddmin(items, test, log):
    """Classic delta-debugging over `items`; `test(candidate_items)`
    returns True when the reduced candidate still satisfies the
    criterion.  Returns the (possibly unchanged) reduced list."""
    n = 2
    while len(items) >= 2:
        chunk = max(1, len(items) // n)
        reduced = False
        i = 0
        while i < len(items):
            cand = items[:i] + items[i + chunk:]
            if cand and test(cand):
                items = cand
                log("  ddmin: kept -%d items (now %d)"
                    % (chunk, len(items)))
                n = max(n - 1, 2)
                reduced = True
            else:
                i += chunk
        if not reduced:
            if n >= len(items):
                break
            n = min(len(items), n * 2)
    return items


# ---------------------------------------------------------------------------
# The three §5.5 strategies
# ---------------------------------------------------------------------------

class Reduction(object):
    """Carries state shared by the strategies for one reduction run."""

    def __init__(self, reducer, log):
        self.reducer = reducer
        self.log = log
        self.steps = []           # trajectory entries
        self.kept = {"lines": 0, "exprs": 0, "tokens": 0}
        self.best = None          # last text that VERIFIED the criterion

    def pred(self, cand_text, phase, desc):
        """Criterion-checked candidate; True = keep."""
        self.reducer.budget.tick()
        obs = self.reducer.observe(cand_text)
        ok = self.reducer.reproduces(obs)
        if ok:
            self.best = cand_text
        self.steps.append({"phase": phase, "op": desc, "kept": ok,
                           "eval": self.reducer.budget.evals})
        if not ok:
            self.log("  rollback: %s (criterion broken)" % desc)
        return ok

    # -- phase 1: whole-line deletion --------------------------------------
    def phase_lines(self, text):
        self.log("phase 1: whole-line deletion (ddmin)")
        lines = text.split("\n")

        def test(cand):
            return self.pred("\n".join(cand), "lines",
                             "delete %d line(s)" % (len(lines) - len(cand)))

        out = ddmin(lines, test, self.log)
        self.kept["lines"] = len(lines) - len(out)
        return "\n".join(out)

    # -- phase 2: expression subtree -> literal ----------------------------
    def phase_exprs(self, text):
        self.log("phase 2: expression subtree -> literal `0`")
        groups = find_paren_groups(text)
        if not groups:
            self.log("  no paren groups; skipping")
            return text
        # structural reference from the Pair tree of the ORIGINAL source
        stats = None
        try:
            obs0 = self.reducer.observe(text)
        except BudgetExhausted:
            raise
        if obs0.build_ok and obs0.vm is not None:
            d = self.reducer.runner.dump(
                os.path.join(self.reducer.runner.workdir, "cand.ta"))
            if not d.timed_out and d.rc == 0 and d.out:
                cnt = tree_cons_count(d.out)
                if cnt is not None:
                    stats = cnt
        self.log("  candidates: %d paren groups%s"
                 % (len(groups), ("; Pair-tree cons nodes=%d" % stats)
                    if stats is not None else ""))
        # largest span first: biggest subtree collapse per eval.  Spans
        # are RECOMPUTED on the current text after every accepted edit —
        # reusing spans from a stale text would splice `0` into the
        # middle of unrelated tokens (corruption, not reduction).
        cur = text
        tried = set()
        while True:
            groups = find_paren_groups(cur)
            if not groups:
                break
            progressed = False
            for (s, e) in sorted(groups, key=lambda g: g[0] - g[1]):
                cand = cur[:s] + "0" + cur[e:]
                if cand in tried:
                    continue
                tried.add(cand)
                if self.pred(cand, "exprs",
                             "expr->0 %r" % cur[s:e][:48]):
                    cur = cand
                    progressed = True
                break     # spans are stale after any text change
            if not progressed:
                break     # every current candidate tried and rejected
        self.kept["exprs"] = sum(1 for st in self.steps
                                 if st["phase"] == "exprs" and st["kept"])
        return cur

        # -- phase 3: token deletion -------------------------------------------
    def phase_tokens(self, text):
        self.log("phase 3: token deletion (ddmin)")
        toks = tokenize_keep_layout(text)
        # whitespace tokens are never deleted: removing the separator
        # between two atoms merges them into a NEW identifier
        # (`match n_1` -> `matchn_1`) — junk the permissive parser may
        # still accept, faking a reproduction instead of reducing.
        deletable = [i for i, t in enumerate(toks) if not t.isspace()]

        def test(cand_idx):
            # ddmin hands back the KEPT indices; the kill set is the rest
            kill = set(deletable) - set(cand_idx)
            return self.pred(render_tokens(toks, kill), "tokens",
                             "delete %d token(s)" % len(kill))

        kept_idx = ddmin(deletable, test, self.log)
        self.kept["tokens"] = len(deletable) - len(kept_idx)
        return render_tokens(toks, set(deletable) - set(kept_idx))


def render_tokens(toks, kill):
    """Join tokens, dropping the killed ones; insert a single space
    wherever a deletion would otherwise merge two non-whitespace tokens
    (a newline already separating them is left alone)."""
    out = []
    for i, t in enumerate(toks):
        if i in kill:
            continue
        if out and not out[-1].isspace() and not t.isspace() \
                and (out[-1][-1].isalnum() or out[-1][-1] == "_"
                     or t[0].isalnum() or t[0] == "_"):
            out.append(" ")
        out.append(t)
    return "".join(out)


def reduce_source(reducer, src_text, log):
    """Run the three phases; returns (reduced_text, Reduction).
    Always terminates: every strategy is bounded by the shared Budget,
    and on budget exhaustion the best-so-far text is returned."""
    red = Reduction(reducer, log)
    cur = src_text
    for phase in (red.phase_lines, red.phase_exprs, red.phase_tokens):
        try:
            phase(cur)
        except BudgetExhausted as e:
            log("budget stop: %s" % e)
            break
        # trust only what the criterion verified: red.best is the last
        # candidate that actually reproduced, so `cur` can never get
        # worse across phases (idempotent + safe on budget exhaustion)
        if red.best is not None:
            cur = red.best
    return cur, red


# ---------------------------------------------------------------------------
# Finding-dir plumbing
# ---------------------------------------------------------------------------

def _read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def pick_target_src(fdir, category, log):
    """Choose which finding artifact to reduce and return its text.

    The failing artifact, not the signature base, is what shrinks:
    mismatch → the first variant that diverged from E₀; crash / divzero
    / hang → the program that actually died; everything else → src_E0."""
    names = os.listdir(fdir)
    if "meta.json" in names:
        with open(os.path.join(fdir, "meta.json"), "r",
                  encoding="latin-1") as f:
            meta = json.load(f)
    else:
        meta = {"programs": []}
    progs = meta.get("programs", [])

    def src_of(tag):
        p = os.path.join(fdir, "src_%s.ta" % tag)
        if not os.path.isfile(p):
            raise ReduceError("missing %s" % p)
        return _read_bytes(p).decode("latin-1")

    if category == "mismatch":
        e0 = morph.norm_tavm(_read_bytes(os.path.join(fdir, "stdout_E0.txt")),
                             None)
        for prog in progs:
            tag = prog["tag"]
            if tag == "E0":
                continue
            p = os.path.join(fdir, "stdout_%s.txt" % tag)
            if not os.path.isfile(p):
                continue
            if e0 != morph.norm_tavm(_read_bytes(p), None):
                log("target: %s (first variant diverging from E0)" % tag)
                return src_of(tag)
    elif category in ("tavm-crash", "unexpected-divzero", "hang"):
        for prog in progs:
            if prog.get("timed_out"):
                if category == "hang":
                    log("target: %s (timed out)" % prog["tag"])
                    return src_of(prog["tag"])
                continue
            rc = prog.get("exit")
            if category == "tavm-crash" and rc is not None \
                    and rc not in (0, 1):
                log("target: %s (crash exit %s)" % (prog["tag"], rc))
                return src_of(prog["tag"])
            if category == "unexpected-divzero" and rc == 1:
                log("target: %s (DIVZERO exit 1)" % prog["tag"])
                return src_of(prog["tag"])
    elif category == "build-fail":
        for name in sorted(names):
            if name.startswith("build_stderr_") and name.endswith(".txt"):
                tag = name[len("build_stderr_"):-len(".txt")]
                log("target: %s (build failed)" % tag)
                return src_of(tag)
    log("target: E0")
    return src_of("E0")


def stored_feature_hint(fdir, category):
    """Best-effort root-cause feature from the stored artifacts (for the
    trajectory cross-check only — the live baseline is authoritative)."""
    try:
        if category == "tavm-crash":
            for name in sorted(os.listdir(fdir)):
                if name.startswith("stderr_") and name.endswith(".txt"):
                    err = _read_bytes(os.path.join(fdir, name))
                    if err.strip():
                        return norm_crash_feature(err)
        if category == "mismatch":
            e0 = morph.norm_tavm(
                _read_bytes(os.path.join(fdir, "stdout_E0.txt")), None)
            for name in sorted(os.listdir(fdir)):
                if name.startswith("stdout_k") and name.endswith(".txt"):
                    v = morph.norm_tavm(
                        _read_bytes(os.path.join(fdir, name)), None)
                    idx = first_diff_index(e0, v)
                    if idx is not None:
                        return idx
    except (OSError, IOError):
        pass
    return None


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="归约器 (kernel-fuzzing §5.5, DELIV-4)")
    ap.add_argument("target", help="morph finding dir or a .ta source file")
    ap.add_argument("--category", default=None,
                    help="failure category (default: meta.json of a "
                         "finding dir)")
    ap.add_argument("--out", default=None, help="output dir")
    ap.add_argument("--max-evals", type=int, default=300,
                    help="oracle evaluation cap (default 300)")
    ap.add_argument("--budget-s", type=int, default=900,
                    help="wall-clock budget in seconds (default 900)")
    args = ap.parse_args(argv)

    target = args.target
    is_finding = os.path.isdir(target)
    if is_finding:
        meta_path = os.path.join(target, "meta.json")
        if not os.path.isfile(meta_path):
            raise ReduceError("%s has no meta.json — not a finding dir"
                              % target)
        with open(meta_path, "r", encoding="latin-1") as f:
            category = args.category or json.load(f).get("category")
        outdir = args.out or os.path.join(target, "reduced")
    else:
        if not os.path.isfile(target):
            raise ReduceError("no such file: %s" % target)
        category = args.category
        if category is None:
            raise ReduceError("--category is required for a source file "
                              "input")
        outdir = args.out or os.path.join(
            os.path.dirname(os.path.abspath(target)), "reduced")

    log_lines = []

    def log(msg):
        log_lines.append(msg)
        sys.stdout.write(msg + "\n")

    log("reduce: target=%s category=%s" % (target, category))

    workdir = tempfile.mkdtemp(prefix="reduce-run-")
    try:
        runner = morph.Runner(workdir)
        budget = Budget(args.max_evals, args.budget_s)
        reducer = Reducer(category, runner, budget)
        src_text = pick_target_src(target, category, log) \
            if is_finding else \
            _read_bytes(target).decode("latin-1")
        n0 = src_text.count("\n") + 1
        log("original: %d lines" % n0)
        hint = stored_feature_hint(target, category) if is_finding else None

        base_obs, feature = reducer.establish_baseline(src_text)
        if hint is not None and hint != feature:
            log("note: stored-artifact feature %r != live baseline %r "
                "(live wins)" % (hint, feature))
        log("baseline feature: %r" % (feature,))

        reduced, red = reduce_source(reducer, src_text, log)
        n1 = reduced.count("\n") + 1
        final_ok = reducer.reproduces(reducer.observe(reduced))
        log("reduced: %d lines (was %d) reproduces=%s evals=%d"
            % (n1, n0, final_ok, budget.evals))

        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "reduced.ta"), "wb") as f:
            f.write(reduced.encode("latin-1"))
        with open(os.path.join(outdir, "trajectory.log"), "w",
                  encoding="latin-1") as f:
            f.write("\n".join(log_lines) + "\n")
        with open(os.path.join(outdir, "reduced.meta.json"), "w",
                  encoding="latin-1") as f:
            json.dump({
                "category": category,
                "feature": repr(feature),
                "lines_before": n0,
                "lines_after": n1,
                "reproduces": final_ok,
                "evals": budget.evals,
                "kept": red.kept,
                "steps": red.steps,
            }, f, indent=2, sort_keys=True)
        log("artifacts: %s" % outdir)
        return 0 if final_ok else 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ReduceError, morph.MorphError) as e:
        sys.stderr.write("reduce: error: %s\n" % e)
        sys.exit(2)