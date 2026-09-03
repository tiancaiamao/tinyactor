# -*- coding: utf-8 -*-
"""morph.py — metamorphic differential runner (kernel-fuzzing DELIV-3).

Implements the runner protocol of docs/kernel-fuzzing-design.md §5.4,
line by line:

  Pipeline notation: tree₀ = gen(seed) → treeₖ = apply(tₖ, tree₀) →
  srcₖ = render(treeₖ) → run(srcₖ).  Transforms operate on TREES only;
  rendering happens only right before a run.

  Two-step call card (build vs run split, so a build-fail exit 1 can
  never be confused with a crash exit 1):
    build: ./tinyactor build src.ta <artifact>   exit != 0 → build-fail,
                                                 no comparison is entered
    run:   <asan tavm> <artifact>                with
           ASAN_OPTIONS=exitcode=42 — otherwise an ASan report would die
           with the default exit 1 and the §5.1.3 death protocol would
           synthesize a bogus DIVZERO line (R3 C-1).  exit == 42 is
           ALWAYS tavm-crash, with the full ASan stderr attached.
    timeout 5s → kill → hang category (hang is a FINDING, not a skip).
    Collect stdout / stderr / exit triple for every run.

  ASan base: built once via `ASAN=1 make tavm` (Makefile: TARGET :=
  tavm_$(SAN)) and pinned below as TAVM_ASAN.  If the binary is missing,
  the runner aborts with a clear build hint.

  Star comparison topology (A-6): E₀ vs each variant, one pair each;
  variants are never compared to each other; variants never run golden.
  The anchor assertion (golden(dump(src₀)) vs norm(E₀)) applies to E₀
  only.

  Failure taxonomy (closed enum): mismatch | tavm-crash | anchor-crash |
  hang | unexpected-divzero | dump-fail | build-fail.
  Signature = (category, sha256(strip_ws(source))[:16]); a known
  signature is skipped automatically.

  Findings land in <out>/<category>-<hash8>/ with sources, seed,
  transform paths, E₀+variant stdout/stderr/exit, golden output, ASan
  report (if any) and run.sh repro commands.

  Determinism: no host `random` anywhere — all randomness comes from
  prng.py (M-2 counter-based sha256 stream).  Same CLI args → same
  findings.

Skips (not in the acceptance denominator):
  * applicable transforms < 3 after 5 re-rolls of the sub-seed,
  * P runs twice with different output — known heterogenous-list
    OP_ADD nondeterminism VM bug (progress.md, eval-task-gen); such a
    program says nothing about the transforms, so it is skipped.

Any VM/compiler bug this runner catches is NOT fixed here — it is
recorded under findings/ and progress.md.

CLI:
    python3 morph.py --seeds A..B [--count N] [--out DIR]

Stdlib-only, host Python 3.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                                    # noqa: E402
import prng                                   # noqa: E402
import transforms                             # noqa: E402
# §5.1.3 norm_tavm / norm_golden are reused verbatim from test_gen.py
# (single definition point, per task-runner "复用，不重写").
import test_gen as tg                         # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# ---------------------------------------------------------------------------
# Pinned paths / protocol constants
# ---------------------------------------------------------------------------

TINYACTOR = os.path.join(_REPO_ROOT, "tinyactor")
# ASan 底座（首日钉死）：`ASAN=1 make tavm` 产物为仓库根的 ./tavm_asan
# （Makefile: TARGET := tavm_$(SAN)，SAN=asan）。
TAVM_ASAN = os.path.join(_REPO_ROOT, "tavm_asan")
AST_DUMP = os.path.join(_HERE, "ast-dump.ta")
GOLDEN = os.path.join(_HERE, "golden", "golden.py")

RUN_TIMEOUT = 5.0        # §5.4 调用卡：超时 5s → hang
GOLDEN_TIMEOUT = 120.0   # golden 是纯 Python 求值器，给宽一点
ASAN_EXIT = 42           # R3 C-1: ASAN_OPTIONS=exitcode=42
ASAN_ENV = {"ASAN_OPTIONS": "exitcode=%d" % ASAN_EXIT}

N_VARIANTS = 3           # 每 seed 恰 3 个变体，每变体恰 1 条变换
MAX_ATTEMPTS = 5         # applicable < 3 时重 roll 上限

# 失败分类学（§5.4 封闭枚举）
CATEGORIES = ("mismatch", "tavm-crash", "anchor-crash", "hang",
              "unexpected-divzero", "dump-fail", "build-fail")


class MorphError(Exception):
    """Infrastructure error (missing binary etc.) — aborts the batch."""


class RunResult(object):
    """stdout / stderr / exit triple for one process run."""

    __slots__ = ("out", "err", "rc", "timed_out")

    def __init__(self, out, err, rc, timed_out):
        self.out = out
        self.err = err
        self.rc = rc
        self.timed_out = timed_out


def _run(argv, timeout, env_extra=None):
    """Run argv, capture stdout/stderr/exit; kill after timeout."""
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    try:
        p = subprocess.run(argv, capture_output=True, timeout=timeout,
                           env=env)
        return RunResult(p.stdout, p.stderr, p.returncode, False)
    except subprocess.TimeoutExpired as ex:
        return RunResult(ex.stdout or b"", ex.stderr or b"", None, True)


# ---------------------------------------------------------------------------
# §5.1.3 norm protocol — thin wrappers over the test_gen implementations
# ---------------------------------------------------------------------------

def norm_tavm(stdout_bytes, run_exit_code):
    """norm_tavm (§5.1.3): split, strip trailing blanks, synthesize the
    DIVZERO:n protocol line on run exit code 1 (build already succeeded)."""
    return tg._norm_vm(stdout_bytes, run_exit_code)


def norm_golden(golden_stdout):
    """norm_golden (§5.1.3): golden already prints DIVZERO:n itself."""
    return tg._norm_golden(golden_stdout)


# ---------------------------------------------------------------------------
# Runner: the two-step call card (build / run) + dump / golden side channels
# ---------------------------------------------------------------------------

class Runner(object):
    """Executes the §5.4 call card against pinned toolchain paths."""

    def __init__(self, workdir,
                 tinyactor=TINYACTOR, tavm_asan=TAVM_ASAN,
                 ast_dump=AST_DUMP, golden=GOLDEN,
                 timeout=RUN_TIMEOUT, golden_timeout=GOLDEN_TIMEOUT):
        missing = [p for p in (tinyactor, ast_dump, golden)
                   if not os.path.exists(p)]
        if missing:
            raise MorphError(
                "missing toolchain file(s): %s" % ", ".join(missing))
        if not os.path.exists(tavm_asan):
            raise MorphError(
                "ASan tavm base not found: %s\n"
                "build it first with:  ASAN=1 make tavm" % tavm_asan)
        self.tinyactor = tinyactor
        self.tavm_asan = tavm_asan
        self.ast_dump = ast_dump
        self.golden = golden
        self.timeout = timeout
        self.golden_timeout = golden_timeout
        self.workdir = workdir
        self._dump_artifact = None

    # -- step 1: build (exit != 0 → build-fail, never enters comparison) ---
    def build(self, src_path, artifact_path):
        p = _run([self.tinyactor, "build", src_path, artifact_path],
                 self.timeout * 4)
        return p

    # -- step 2: run on the ASan base, ASAN_OPTIONS=exitcode=42 ------------
    def run(self, artifact_path):
        return _run([self.tavm_asan, artifact_path], self.timeout, ASAN_ENV)

    # -- anchor side channels ------------------------------------------------
    def dump(self, src_path):
        """ast-dump.ta on the ASan base (same底座 as the fuzz targets).
        Returns RunResult; caller classifies dump-fail."""
        if self._dump_artifact is None:
            artifact = os.path.join(self.workdir, "ast-dump.tabc")
            p = self.build(self.ast_dump, artifact)
            if p.rc != 0:
                raise MorphError(
                    "ast-dump.ta failed to build (toolchain broken):\n%s"
                    % p.err.decode("latin-1", "replace"))
            self._dump_artifact = artifact
        return _run([self.tavm_asan, self._dump_artifact, src_path],
                    self.timeout, ASAN_ENV)

    def golden_eval(self, sexp_path):
        return _run([sys.executable, self.golden, sexp_path],
                    self.golden_timeout)

    # -- combined card -------------------------------------------------------
    def build_and_run(self, src_text, tag):
        """Full two-step card for one program.  Returns (RunResult, paths);
        on build-fail the RunResult carries rc != 0 and empty stdout."""
        src_path = os.path.join(self.workdir, "src_%s.ta" % tag)
        artifact = os.path.join(self.workdir, "src_%s.tabc" % tag)
        with open(src_path, "wb") as f:
            f.write(src_text.encode("latin-1"))
        bp = self.build(src_path, artifact)
        if bp.rc != 0:
            return RunResult(b"", bp.out + bp.err, bp.rc, False), \
                (src_path, artifact), bp
        rp = self.run(artifact)
        return rp, (src_path, artifact), bp


# ---------------------------------------------------------------------------
# Classification / signature / findings contract
# ---------------------------------------------------------------------------

def classify_run(res):
    """Per-run death classification (§5.1.3 + 调用卡).  Returns a category
    string or None when the run finished 'normally' (exit 0, or exit 1 =
    the DIVZERO death protocol — see unexpected-divzero note below)."""
    if res.timed_out:
        return "hang"
    if res.rc == ASAN_EXIT:
        return "tavm-crash"          # ASan report, full stderr attached
    if res.rc is None or res.rc < 0 or res.rc > 1:
        return "tavm-crash"          # signal death / unknown death code
    return None                      # 0 = clean, 1 = DIVZERO protocol


def strip_ws_sha16(text):
    """§5.4 signature hash: sha256 of the whitespace-stripped source,
    first 16 hex chars."""
    stripped = "".join(text.split()).encode("latin-1")
    return hashlib.sha256(stripped).hexdigest()[:16]


def signature(category, text):
    """§5.4 signature = (类别, sha256(strip_ws(源码)) 前 16 hex)."""
    return "%s:%s" % (category, strip_ws_sha16(text))


def load_known_signatures(out_dir):
    """Reload signatures from a previous run's findings/ tree so reusing
    an --out DIR keeps the 同类已知 finding 自动跳过 contract."""
    known = set()
    if not os.path.isdir(out_dir):
        return known
    for name in os.listdir(out_dir):
        meta = os.path.join(out_dir, name, "meta.json")
        if not os.path.isfile(meta):
            continue
        try:
            with open(meta, "r", encoding="latin-1") as f:
                m = json.load(f)
            known.add(m["signature"])
        except (ValueError, KeyError, OSError):
            continue
    return known


def record_finding(out_dir, category, src0_text, seed, effective_seed,
                   attempts, variants_meta, programs, anchor, dedup):
    """Write one finding per §5.4 落盘契约; dedup by signature.

    programs: list of dicts {tag, src_text, res, build_err}
    anchor:   dict {golden_stdout, dump_stdout, dump_err} (may be empty)
    Returns True if a new finding dir was written, False if the
    signature was already known (dedup skip).
    """
    sig = signature(category, src0_text)
    if sig in dedup:
        return False
    dedup.add(sig)
    fdir = os.path.join(out_dir, "%s-%s" % (category, sig[1][:8]))
    os.makedirs(fdir, exist_ok=True)

    def _w(name, data):
        with open(os.path.join(fdir, name), "wb") as f:
            f.write(data)

    for prog in programs:
        tag = prog["tag"]
        _w("src_%s.ta" % tag, prog["src_text"].encode("latin-1"))
        res = prog["res"]
        _w("stdout_%s.txt" % tag, res.out)
        _w("stderr_%s.txt" % tag, res.err)
        exit_repr = "TIMEOUT" if res.timed_out else str(res.rc)
        _w("exit_%s.txt" % tag, exit_repr.encode("latin-1"))
        if prog.get("build_err"):
            _w("build_stderr_%s.txt" % tag, prog["build_err"])
    if anchor.get("golden_stdout") is not None:
        _w("golden.txt", anchor["golden_stdout"])
    if anchor.get("dump_stdout") is not None:
        _w("dump.sexp", anchor["dump_stdout"])
    if anchor.get("dump_err"):
        _w("dump_stderr.txt", anchor["dump_err"])

    asan_reports = [p["res"].err for p in programs
                    if not p["res"].timed_out
                    and p["res"].rc == ASAN_EXIT and p["res"].err]
    if asan_reports:
        _w("asan.txt", b"\n=====\n".join(asan_reports))

    repo = _REPO_ROOT
    repro = ["#!/bin/sh",
                          "# morph finding repro - category=%s signature=%s" % (category, sig),
             "# seed=%d (effective_seed=%d, attempts=%d)"
             % (seed, effective_seed, attempts),
             "set -e",
             "cd %s" % repo]
    for prog in programs:
        repro.append("# --- %s ---" % prog["tag"])
        repro.append(
            "./tinyactor build %s /tmp/morph_repro_%s.tabc"
            % (os.path.join(fdir, "src_%s.ta" % prog["tag"]), prog["tag"]))
        repro.append(
            "ASAN_OPTIONS=exitcode=%d ./tavm_asan /tmp/morph_repro_%s.tabc"
            % (ASAN_EXIT, prog["tag"]))
    run_sh = os.path.join(fdir, "run.sh")
    with open(run_sh, "w", encoding="latin-1") as f:
        f.write("\n".join(repro) + "\n")
    os.chmod(run_sh, 0o755)

    meta = {
        "category": category,
        "signature": sig,
        "seed": seed,
        "effective_seed": effective_seed,
        "attempts": attempts,
        "variants": variants_meta,
        "programs": [{"tag": p["tag"], "exit": ("TIMEOUT" if
                      p["res"].timed_out else p["res"].rc),
                      "timed_out": p["res"].timed_out} for p in programs],
        "run_timeout_s": RUN_TIMEOUT,
    }
    with open(os.path.join(fdir, "meta.json"), "w",
              encoding="latin-1") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
    return True


# ---------------------------------------------------------------------------
# Main loop (§5.4 pseudocode, faithful)
# ---------------------------------------------------------------------------

def _make_variants(tree0, eff_seed):
    """3 variants, each exactly ONE transform from tree₀ (variants are
    independent rewrites of E₀, never stacked).  Returns
    ([(tree, info)], None) or ([], attempt_index_that_ran_dry)."""
    variants = []
    for k in range(1, N_VARIANTS + 1):
        rng_k = prng.make_prng(prng.derive_seed(eff_seed, 1000 + k))
        tree_k, info = transforms.apply_one(tree0, rng_k)
        if info is None:
            return [], k                # applicable pool ran dry
        variants.append((tree_k, info))
    return variants, None


def run_seed(runner, seed, out_dir, dedup, skips, findings, log):
    """Fuzz one seed.  Appends to skips/findings; returns the outcome
    string: 'ok' | 'skip:<reason>' | 'finding:<category>'."""
    # re-roll loop: applicable < 3 → new sub-seed, regenerate, ≤5 tries
    effective_seed = seed
    attempts = 0
    tree0 = variants = None
    while attempts < MAX_ATTEMPTS:
        attempts += 1
        tree0 = gen.build_program(effective_seed)
        variants, dry_k = _make_variants(tree0, effective_seed)
        if dry_k is None:
            break
        # re-roll: derive a fresh sub-seed, regenerate the program
        effective_seed = prng.derive_seed(seed, attempts * 97 + 13)
        tree0 = None
    if tree0 is None:
        skips.append((seed, "applicable<%d after %d attempts"
                      % (N_VARIANTS, MAX_ATTEMPTS)))
        return "skip:applicable"

    src0 = gen.render_tree(tree0)
    programs = [{"tag": "E0", "src_text": src0}]
    variants_meta = []
    for k, (tree_k, info) in enumerate(variants, 1):
        programs.append({"tag": "k%d" % k,
                         "src_text": gen.render_tree(tree_k)})
        variants_meta.append({
            "variant": k,
            "rule": info["rule_name"],
            "direction": info["direction"],
            "path": list(info["path"]) if info.get("path") else [],
            "before": info.get("before"),
            "after": info.get("after"),
        })

    # build + run everything (E₀ first; variants each built+run once —
    # variants never run golden, per A-6)
    for prog in programs:
        res, _paths, bp = runner.build_and_run(prog["src_text"], prog["tag"])
        prog["res"] = res
        prog["build_err"] = (bp.out + bp.err) if bp.rc != 0 else b""

    # consistency guard: P run twice must agree (known heterogenous-list
    # OP_ADD nondeterminism VM bug — see module docstring).  Re-run E₀.
    e0 = programs[0]["res"]
    res2, _p2, _b2 = runner.build_and_run(src0, "E0_guard")
    if (e0.out, e0.rc, e0.timed_out) != (res2.out, res2.rc, res2.timed_out):
        skips.append((seed, "nondeterministic: E0 output differs across "
                      "two runs (known heterogenous-list VM bug)"))
        return "skip:nondet"

    # --- classification, precedence order ---------------------------------
    # 1. build-fail: any target failed to build → no comparison entered
    for prog in programs:
        if prog["build_err"]:
            if record_finding(out_dir, "build-fail", src0, seed,
                              effective_seed, attempts, variants_meta,
                              programs, {}, dedup):
                findings["build-fail"] += 1
                return "finding:build-fail"
            return "dedup:build-fail"
    # 2-4. per-run death classes across all targets
    for cat in ("hang", "tavm-crash", "unexpected-divzero"):
        for prog in programs:
            if classify_run(prog["res"]) == cat:
                if record_finding(out_dir, cat, src0, seed, effective_seed,
                                  attempts, variants_meta, programs, {},
                                  dedup):
                    findings[cat] += 1
                    return "finding:" + cat
                return "dedup:" + cat
    # 5. metamorphic star comparison: E₀ vs each variant (variants are
    #    never compared to each other — A-6)
    base = norm_tavm(e0.out, e0.rc)
    for prog in programs[1:]:
        if base != norm_tavm(prog["res"].out, prog["res"].rc):
            if record_finding(out_dir, "mismatch", src0, seed,
                              effective_seed, attempts, variants_meta,
                              programs, {}, dedup):
                findings["mismatch"] += 1
                return "finding:mismatch"
            return "dedup:mismatch"
    # 6-7. anchor assertion, E₀ only: golden(dump(src₀)) vs norm(E₀)
        dump_res = runner.dump(
        os.path.join(runner.workdir, "src_E0.ta"))
    anchor = {"dump_stdout": dump_res.out, "dump_err": dump_res.err,
              "golden_stdout": None}
    if dump_res.timed_out or dump_res.rc != 0 \
            or b"AST-DUMP-ERROR" in dump_res.out or not dump_res.out:
        if record_finding(out_dir, "dump-fail", src0, seed, effective_seed,
                          attempts, variants_meta, programs, anchor, dedup):
            findings["dump-fail"] += 1
            return "finding:dump-fail"
        return "dedup:dump-fail"
    sexp_path = os.path.join(runner.workdir, "src_E0.sexp")
    with open(sexp_path, "wb") as f:
        f.write(dump_res.out)
    g = runner.golden_eval(sexp_path)
    anchor["golden_stdout"] = g.out
    # golden exits 1 with DIVZERO:n synthesized internally on a
    # protocol death; any OTHER golden error is an anchor-crash.
    if g.rc != 0 and b"DIVZERO:" not in g.out:
        if record_finding(out_dir, "anchor-crash", src0, seed,
                          effective_seed, attempts, variants_meta,
                          programs, anchor, dedup):
            findings["anchor-crash"] += 1
            return "finding:anchor-crash"
        return "dedup:anchor-crash"
    if norm_golden(g.out) != base:
        if record_finding(out_dir, "anchor-crash", src0, seed,
                          effective_seed, attempts, variants_meta,
                          programs, anchor, dedup):
            findings["anchor-crash"] += 1
            return "finding:anchor-crash"
        return "dedup:anchor-crash"
    return "ok"


def fuzz_batch(runner, seeds, out_dir, log=None, known=None):
    """Run the §5.4 main loop over `seeds`.  Returns (stats dict).

    `known` (optional): extra pre-seeded signatures (e.g. the frozen
    triaged list, M-10) merged into the dedup set on top of what is
    auto-reloaded from out_dir's previous findings."""
    if log is None:
        log = lambda msg: None                  # noqa: E731
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    dedup = load_known_signatures(out_dir)
    if known:
        dedup |= set(known)
    skips = []
    findings = dict((c, 0) for c in CATEGORIES)
    dedup_hits = [0]
    ran = 0
    t0 = time.time()
    for seed in seeds:
        outcome = run_seed(runner, seed, out_dir, dedup, skips,
                           findings, log)
        if outcome.startswith("dedup:"):
            dedup_hits[0] += 1
        elif outcome == "ok":
            ran += 1
        log("seed %d -> %s" % (seed, outcome))
    # skip log (append; deterministic content per seed set)
    with open(os.path.join(out_dir, "skips.log"), "a",
              encoding="latin-1") as f:
        for seed, reason in skips:
            f.write("seed=%d reason=%s\n" % (seed, reason))
    return {
        "seeds": len(seeds),
        "ran": ran,
        "skips": skips,
        "findings": findings,
        "dedup_hits": dedup_hits[0],
        "elapsed": time.time() - t0,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_seeds(spec):
    """'N' → [N];  'A..B' → list(range(A, B+1)) (inclusive)."""
    spec = spec.strip()
    if ".." in spec:
        a, b = spec.split("..", 1)
        a, b = int(a), int(b)
        if b < a:
            raise MorphError("bad --seeds range: %s" % spec)
        return list(range(a, b + 1))
    return [int(spec)]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="morph 对拍 runner (kernel-fuzzing §5.4, DELIV-3)")
    ap.add_argument("--seeds", required=True,
                    help="single seed N or inclusive range A..B")
    ap.add_argument("--count", type=int, default=None,
                    help="only run the first N seeds of the range")
    ap.add_argument("--out", default=os.path.join(_HERE, "build",
                                                  "findings"),
                    help="findings output dir (default: tools/kernfuzz/"
                         "build/findings — gitignored)")
    args = ap.parse_args(argv)

    seeds = parse_seeds(args.seeds)
    if args.count is not None:
        seeds = seeds[:args.count]

    workdir = tempfile.mkdtemp(prefix="morph-run-")
    try:
        runner = Runner(workdir)
        stats = fuzz_batch(runner, seeds, args.out,
                           log=lambda m: sys.stderr.write(m + "\n"))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    fk = stats["findings"]
    sys.stderr.write(
        "morph summary: seeds=%d ran=%d skip=%d (nondet=%d applicable=%d) "
        "dedup=%d findings=%d {%s} elapsed=%.1fs\n"
        % (stats["seeds"], stats["ran"], len(stats["skips"]),
           sum(1 for _s, r in stats["skips"] if r.startswith("nondet")),
           sum(1 for _s, r in stats["skips"] if r.startswith("applicable")),
           stats["dedup_hits"], sum(fk.values()),
           ", ".join("%s=%d" % (c, fk[c]) for c in CATEGORIES),
           stats["elapsed"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except MorphError as e:
        sys.stderr.write("morph: error: %s\n" % e)
        sys.exit(2)