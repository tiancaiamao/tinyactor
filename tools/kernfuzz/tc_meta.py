# -*- coding: utf-8 -*-
"""tc_meta.py — typecheck 推导元性质 (kernel-fuzzing §6.3, DELIV-5 后半).

Implements docs/kernel-fuzzing-design.md §6.3 — three met properties over
gen-produced programs (tree-level operations; rendering always through
gen.render_tree):

  1. determinism   同一源文件连续 build 两次，诊断输出 (stdout+stderr)
                   逐字节一致；且两次编译产物 (.tabc) byte 相等。
  2. order-independence  随机打乱顶层非 main 定义顺序（树级 Fisher-Yates
                   重排 fns / type_decls）→ 仅断言 accept/reject 结论不变
                   （诊断行序变化不算翻转，§6.3 原文粒度）。
  3. fmt idempotence (§6.3 :704-708 workflow, fmt 就地改写故先拷贝):
                       cp P.ta P.fmt.ta && tinyactor fmt P.fmt.ta
                       build P.ta -o a.tabc ; build P.fmt.ta -o b.tabc
                       assert a.tabc == b.tabc  (byte 级)

  Codegen determinism PREREQUISITE (DELIV-9 first item, verified up-front
  before the fmt assertion is relied upon): the same input compiled twice
  must produce byte-identical artifacts.  The probe runs over a derived
  sample of the requested seeds; if any timestamp/address randomness shows
  up, the fmt idempotence assertion DOWNGRADES to structural equality
  (tavm run of both artifacts, stdout+exit compared) and the downgrade
  reason is recorded in the summary and in stats["premise"]["reason"].

Violations are only RECORDED (morph.record_finding contract, closed
category enum below) — never fixed:

    meta-determinism | meta-order | meta-fmt

Determinism of this tool itself: all randomness comes from prng.py
(derive_seed counter stream), same CLI args → same findings & stats.

CLI:
    python3 tc_meta.py --seeds A..B [--count N] [--out DIR]

Stdlib-only, host Python 3.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gen                                    # noqa: E402
import prng                                   # noqa: E402
import transforms as tr                       # noqa: E402
import morph                                  # noqa: E402
import tc_oracle as tco                       # noqa: E402  (§6.0 classifier)

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
TINYACTOR = morph.TINYACTOR
TAVM_ASAN = morph.TAVM_ASAN

# findings 类别（封闭枚举；落盘契约复用 morph）
FINDING_CATEGORIES = ("meta-determinism", "meta-order", "meta-fmt")

# derive_seed counters (M-2 counter stream; disjoint from tc_oracle's 7xxx/9xxx)
DERIVE_ORDER = 4200       # per-seed shuffle rng
DERIVE_PROBE = 8100       # up-front codegen determinism probe seeds

PREMISE_PROBE_MAX = 10    # §6.3 前提先行验证：探针 seed 数上限


class MetaError(Exception):
    """Infrastructure error — aborts the batch (mirrors morph.MorphError)."""


# ---------------------------------------------------------------------------
# runner（build / fmt 只需 tinyactor；tavm 仅在 structural 降级路径用到）
# ---------------------------------------------------------------------------

class MetaRunner(object):
    """Executes the §6.3 call card (build / fmt / structural-compare).

    build_bin / fmt_bin default to the pinned ./tinyactor and are
    injectable so tests can bite-test the assertions with fakes (a build
    that embeds a timestamp in its diagnostics, a fmt that scrambles
    semantics).  tavm_asan is only needed when the premise fails and the
    fmt assertion downgrades to structural equality.
    """

    def __init__(self, workdir, tinyactor=TINYACTOR, tavm_asan=TAVM_ASAN,
                 build_bin=None, fmt_bin=None, timeout=morph.RUN_TIMEOUT):
        self.build_bin = build_bin or tinyactor
        self.fmt_bin = fmt_bin or tinyactor
        self.tavm_asan = tavm_asan
        for p in (self.build_bin, self.fmt_bin):
            if not os.path.exists(p):
                raise MetaError("missing toolchain file: %s" % p)
        self.workdir = workdir
        self.timeout = timeout

    def build(self, src_path, artifact_path):
        """`tinyactor build src -o artifact`; timeout*4 同 morph.Runner."""
        return morph._run([self.build_bin, "build", src_path, artifact_path],
                          self.timeout * 4)

    def fmt(self, src_path):
        """`tinyactor fmt <file>` — IN-PLACE rewrite (§6.3 :704)."""
        return morph._run([self.fmt_bin, "fmt", src_path], self.timeout * 4)

    def run_artifact(self, artifact_path):
        """Structural downgrade path: run one .tabc on the ASan base."""
        if not self.tavm_asan or not os.path.exists(self.tavm_asan):
            raise MetaError(
                "structural equality needs tavm_asan; build it first "
                "with:  ASAN=1 make tavm")
        return morph._run([self.tavm_asan, artifact_path], self.timeout,
                          morph.ASAN_ENV)


# ---------------------------------------------------------------------------
# tree-level top-definition shuffle (property 2)
# ---------------------------------------------------------------------------

def _fy_shuffle(seq, rng):
    """In-place Fisher-Yates over prng counters (M-2; no host random)."""
    for i in range(len(seq) - 1, 0, -1):
        j = prng.prng_next_range(rng, i + 1)
        seq[i], seq[j] = seq[j], seq[i]
    return seq


def shuffle_tree(plan, rng):
    """Return a copy of `plan` with top-level non-main definitions
    (helper fns and type decls) reordered.  main_stmts are untouched —
    §6.3 shuffles 顶层定义, not statement order inside main."""
    np = tr._copy_plan(plan)
    np.type_decls = list(plan.type_decls)
    _fy_shuffle(np.fns, rng)
    _fy_shuffle(np.type_decls, rng)
    return np


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _write_src(runner, text, tag):
    sp = os.path.join(runner.workdir, "src_%s.ta" % tag)
    ap = os.path.join(runner.workdir, "src_%s.tabc" % tag)
    with open(sp, "wb") as f:
        f.write(text.encode("latin-1"))
    return sp, ap


def _read_or_none(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def _read_text_or(path, fallback):
    """Formatted-source text as str (morph.record_finding contract)."""
    b = _read_or_none(path)
    return b.decode("latin-1") if b is not None else fallback


def _record(out_dir, category, programs, seed, dedup, findings, meta=None):
    """morph.record_finding 薄封装（同 tc_oracle._record + 附加 meta）。"""
    ok = morph.record_finding(out_dir, category,
                              programs[0]["src_text"], seed, seed, 1,
                              list(meta or []), programs, {}, dedup)
    if ok:
        findings[category] += 1
    return ok


def _prog(tag, src_text, res):
    return {"tag": tag, "src_text": src_text, "res": res,
            "build_err": (res.out + res.err) if res.rc != 0 else b""}


# ---------------------------------------------------------------------------
# codegen determinism premise (DELIV-9 first item, §6.3 前提先行验证)
# ---------------------------------------------------------------------------

def _artifact_digest(data):
    """Return a stable short digest for an artifact or ``None``."""
    if data is None:
        return None
    return hashlib.sha256(data).hexdigest()[:16]


def _premise_reason(seed, first, second):
    """Describe a codegen mismatch without confusing it with source hashes."""
    return ("codegen artifact mismatch on probe seed %d (sha256 %s vs %s) "
            "— timestamp/address randomness?" %
            (seed, _artifact_digest(first) or "none",
             _artifact_digest(second) or "none"))


def probe_codegen_determinism(runner, seeds):
    """Compile the probe sample twice per seed and cmp artifacts.

    Returns (byte_deterministic, reason, mismatches):
      byte_deterministic  True → fmt 断言走 byte 级 cmp
      False → fmt 断言降级为结构相等（tavm 行为对比），reason 记录在案
    """
    mismatches = []
    reason = ""
    for seed in seeds[:PREMISE_PROBE_MAX]:
        pseed = prng.derive_seed(seed, DERIVE_PROBE)
        src = gen.gen_program(pseed)
        sp, ap = _write_src(runner, src, "probe_%d" % seed)
        a = ap.replace(".tabc", ".a.tabc")
        b = ap.replace(".tabc", ".b.tabc")
        r1 = runner.build(sp, a)
        r2 = runner.build(sp, b)
        if r1.rc != 0 or r2.rc != 0:
            # 探针程序本身编译失败 = 生成器/工具链问题，不算前提失败，
            # 但也不提供前提证据 → 记 skip 语义（由调用方计数）。
            continue
        ba, bb = _read_or_none(a), _read_or_none(b)
        if ba is None or bb is None or ba != bb:
            mismatches.append(seed)
            reason = _premise_reason(seed, ba, bb)
    return (not mismatches, reason, mismatches)


# ---------------------------------------------------------------------------
# per-seed three-property check (§6.3)
# ---------------------------------------------------------------------------

def run_seed(runner, seed, mode, out_dir, dedup, skips, findings, checks,
             log):
    """One seed = determinism + order + fmt.  `mode` is the premise
    verdict from probe_codegen_determinism: "byte" or "structural".

    Returns "ok" | "skip:<reason>" | "finding:<cat>[,<cat>]"."
    """
    plan = gen.build_program(seed)
    src0 = gen.render_tree(plan)
    sp0, ap0 = _write_src(runner, src0, "P")

    # ---- property 1: determinism (诊断逐字节 + 产物逐字节) ---------------
    a1, a2 = ap0 + ".d1", ap0 + ".d2"
    r1 = runner.build(sp0, a1)
    r2 = runner.build(sp0, a2)
    checks["determinism"] += 1
    v0 = tco.classify_build(r1)
    violated = []
    if (r1.out, r1.err) != (r2.out, r2.err):
        _record(out_dir, "meta-determinism",
                [_prog("P", src0, r1), _prog("P", src0, r2)],
                seed, dedup, findings,
                meta=[{"property": "determinism", "what": "diagnostics"}])
        violated.append("meta-determinism")
    b1, b2 = _read_or_none(a1), _read_or_none(a2)
    if v0 == "accept" and (b1 is None or b2 is None or b1 != b2):
        _record(out_dir, "meta-determinism",
                [_prog("P", src0, r1), _prog("P", src0, r2)],
                seed, dedup, findings,
                meta=[{"property": "determinism", "what": "artifact"}])
        violated.append("meta-determinism")
    if v0 == "crash":
        # §6.0 crash：panic/信号/超时 —— 元性质 harness 同样落盘（不修）
        _record(out_dir, "meta-determinism", [_prog("P", src0, r1)],
                seed, dedup, findings,
                meta=[{"property": "determinism", "what": "build-crash"}])
        violated.append("meta-determinism")
        skips.append((seed, "baseline:crash"))
        return "finding:" + ",".join(violated)
    if v0 not in ("accept", "reject"):
        skips.append((seed, "baseline:%s" % v0))
        return ("finding:" + ",".join(violated)) if violated \
            else "skip:baseline:%s" % v0
    if b1 is None or b2 is None or b1 != b2:
        # 产物级前提在该 seed 实测翻车（诊断可能一致）→ 后续 fmt 只能降级
        mode = "structural"
        log("seed %d: codegen premise broken on this seed -> structural"
            % seed)

    # ---- property 2: order independence (树级重排，仅比结论) -------------
    rng = prng.make_prng(prng.derive_seed(seed, DERIVE_ORDER))
    splan = shuffle_tree(plan, rng)
    srcS = gen.render_tree(splan)
    if srcS == src0:
        checks["order"] += 0          # nothing to assert — no reorder site
        skips.append((seed, "order:no-shuffle-site"))
    else:
        checks["order"] += 1
        spS, apS = _write_src(runner, srcS, "PS")
        rS = runner.build(spS, apS)
        vS = tco.classify_build(rS)
        # §6.3 原文粒度：只比 accept/reject 结论；诊断行序变化不算翻转。
        # crash / anomaly 也按结论不同处理（P 已被证为 accept/reject）。
        if vS != v0:
            _record(out_dir, "meta-order",
                    [_prog("P", src0, r1), _prog("PS", srcS, rS)],
                    seed, dedup, findings,
                    meta=[{"property": "order",
                           "conclusion_P": v0, "conclusion_PS": vS}])
            violated.append("meta-order")

    # ---- property 3: fmt idempotence (§6.3 :704-708 workflow) ------------
    checks["fmt"] += 1
    fmt_path = os.path.join(runner.workdir, "src_Pfmt.ta")
    with open(sp0, "rb") as f:
        raw = f.read()
    with open(fmt_path, "wb") as f:
        f.write(raw)                       # cp（原件永不被 fmt 触碰）
    fsp = fmt_path
    fap = os.path.join(runner.workdir, "src_Pfmt.tabc")
    fres = runner.fmt(fsp)
    if fres.rc != 0 or fres.timed_out:
        _record(out_dir, "meta-fmt",
                [_prog("P", src0, r1),
                                  {"tag": "Pfmt", "src_text": _read_text_or(fsp, src0),
                  "res": fres, "build_err": b""}],
                seed, dedup, findings,
                meta=[{"property": "fmt", "what": "fmt-command-failed"}])
        violated.append("meta-fmt")
    else:
        fb = runner.build(fsp, fap)
        vF = tco.classify_build(fb)
        if vF != v0:
            # fmt 改变了程序语义（结论翻转）→ 幂等性违例
            _record(out_dir, "meta-fmt",
                    [_prog("P", src0, r1),
                     {"tag": "Pfmt",
                      "src_text": _read_text_or(fsp, src0),
                      "res": fb,
                      "build_err": (fb.out + fb.err) if fb.rc != 0
                      else b""}],
                    seed, dedup, findings,
                    meta=[{"property": "fmt", "what": "conclusion-flip",
                           "conclusion_P": v0, "conclusion_Pfmt": vF}])
            violated.append("meta-fmt")
        elif mode == "byte":
            ba, bb = _read_or_none(a1), _read_or_none(fap)
            if ba != bb:
                _record(out_dir, "meta-fmt",
                        [_prog("P", src0, r1),
                         {"tag": "Pfmt",
                          "src_text": _read_text_or(fsp, src0),
                          "res": fb, "build_err": b""}],
                        seed, dedup, findings,
                        meta=[{"property": "fmt", "what": "artifact"}])
                violated.append("meta-fmt")
            else:
                checks["fmt-byte"] += 1
        else:
            # 降级路径：结构（行为）相等 —— tavm 跑两个产物比 stdout+exit
            try:
                ra = runner.run_artifact(a1)
                rb = runner.run_artifact(fap)
            except MetaError as e:
                skips.append((seed, "fmt:structural-unavailable:%s" % e))
                if violated:
                    return "finding:" + ",".join(violated)
                return "skip:fmt:structural-unavailable"
            if (ra.out, ra.rc) != (rb.out, rb.rc):
                _record(out_dir, "meta-fmt",
                        [_prog("P", src0, r1),
                         {"tag": "Pfmt",
                          "src_text": _read_text_or(fsp, src0),
                          "res": fb, "build_err": b""}],
                        seed, dedup, findings,
                        meta=[{"property": "fmt",
                               "what": "structural-mismatch"}])
                violated.append("meta-fmt")
            else:
                checks["fmt-structural"] += 1
    return ("finding:" + ",".join(violated)) if violated else "ok"


def fuzz_batch(runner, seeds, out_dir, log=None):
    """§6.3 main loop: up-front codegen premise probe, then the three
    properties per seed.  Returns stats dict (same args → same dict,
    except elapsed)."""
    if log is None:
        log = lambda msg: None                  # noqa: E731
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    dedup = morph.load_known_signatures(out_dir)
    findings = dict((c, 0) for c in FINDING_CATEGORIES)
    checks = {"determinism": 0, "order": 0, "fmt": 0,
              "fmt-byte": 0, "fmt-structural": 0}
    skips = []
    ok_seeds = 0

    # ---- 前提先行验证 (DELIV-9 首项) -------------------------------------
    t0 = time.time()
    byte_ok, reason, mismatches = probe_codegen_determinism(runner, seeds)
    mode = "byte" if byte_ok else "structural"
    if not byte_ok:
        log("PREMISE DOWNGRADE: codegen not byte-deterministic (%s); "
            "fmt assertion downgraded to structural equality" % reason)
        for seed in mismatches:
            pseed = prng.derive_seed(seed, DERIVE_PROBE)
            psrc = gen.gen_program(pseed)
            _record(out_dir, "meta-determinism",
                    [{"tag": "probe", "src_text": psrc,
                      "res": morph.RunResult(b"", reason.encode("latin-1"),
                                             1, False), "build_err": b""}],
                    seed, dedup, findings,
                    meta=[{"property": "determinism",
                           "what": "premise-artifact"}])

    for seed in seeds:
        outcome = run_seed(runner, seed, mode, out_dir, dedup, skips,
                           findings, checks, log)
        if outcome == "ok":
            ok_seeds += 1
        log("seed %d -> %s" % (seed, outcome))
    with open(os.path.join(out_dir, "skips.log"), "a",
              encoding="latin-1") as f:
        for seed, why in skips:
            f.write("seed=%d reason=%s\n" % (seed, why))
    return {
        "seeds": len(seeds),
        "ok": ok_seeds,
        "premise": {"mode": mode, "reason": reason,
                    "probe_seeds": min(len(seeds), PREMISE_PROBE_MAX),
                    "probe_mismatches": len(mismatches)},
        "checks": checks,
        "skips": skips,
        "findings": findings,
        "elapsed": time.time() - t0,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

parse_seeds = morph.parse_seeds


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="typecheck 推导元性质 (kernel-fuzzing §6.3, DELIV-5 后半)")
    ap.add_argument("--seeds", required=True,
                    help="single seed N or inclusive range A..B")
    ap.add_argument("--count", type=int, default=None,
                    help="only run the first N seeds of the range")
    ap.add_argument("--out", default=os.path.join(_HERE, "build",
                                                  "meta-findings"),
                    help="findings output dir (default gitignored)")
    args = ap.parse_args(argv)

    seeds = parse_seeds(args.seeds)
    if args.count is not None:
        seeds = seeds[:args.count]

    workdir = tempfile.mkdtemp(prefix="tc-meta-run-")
    try:
        runner = MetaRunner(workdir)
        stats = fuzz_batch(runner, seeds, args.out,
                           log=lambda m: sys.stderr.write(m + "\n"))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    fk = stats["findings"]
    ck = stats["checks"]
    prem = stats["premise"]
    sys.stderr.write(
        "tc_meta summary: seeds=%d ok=%d premise=%s (%d/%d probe "
        "mismatches) checks={det=%d order=%d fmt=%d (byte=%d struct=%d)} "
        "skips=%d findings=%d {%s} elapsed=%.1fs\n"
        % (stats["seeds"], stats["ok"], prem["mode"],
           prem["probe_mismatches"], prem["probe_seeds"],
           ck["determinism"], ck["order"], ck["fmt"],
           ck["fmt-byte"], ck["fmt-structural"], len(stats["skips"]),
           sum(fk.values()),
           ", ".join("%s=%d" % (c, fk[c]) for c in FINDING_CATEGORIES),
           stats["elapsed"]))
    if prem["reason"]:
        sys.stderr.write("  premise downgrade reason: %s\n" % prem["reason"])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except MetaError as e:
        sys.stderr.write("tc_meta: error: %s\n" % e)
        sys.exit(2)