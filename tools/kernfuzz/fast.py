# -*- coding: utf-8 -*-
"""fast.py — kernfuzz 快速环编排器 (kernel-fuzzing §9, DELIV-9).

`make kernfuzz-fast`（push 触发的快速环）的三环组成，§9 原文：

  环 1  morph 差分: 300 固定 seed 基准集（test/kernfuzz-frozen/fixed-seeds.txt,
        仅 seed 清单入库，程序由 gen 确定性再生成 — M-10）+ 200 滚动新 seed
        （M-7 派生，见 roll_seed）= 500 基础程序 ×4 执行单元（E0 + 3 变体）。
        单程序超时降为 2s（R3 C-4）。
  环 2  fmt 幂等性扫语料: 复用 tc_meta.check_fmt（§6.3 :704-708 工作流，byte 级
        cmp 前提 = codegen 确定性，先探针后断言）。
  环 3  typecheck 固定 seed 负例回归: 固化快照（test/kernfuzz-frozen/tc-negative/，
        `freeze-tc` 子命令用 tc_oracle 变异器离线生成），fast 只重放断言 —
        用当前编译器逐个 build，断言 reject / accept-hole / accept-quiet 不漂移。

退出语义（§9）: 工具链缺席（tinyactor / tavm_asan / ast-dump.ta 任一不存在）
→ 打印 `KERNFUZZ-SKIPPED: 工具链缺失` 且 exit 0；有工具链但发现 finding → exit 1。

滚动 seed（M-7 原文，可直接抄）:
    sha256(<git_sha>:<YYYY-MM-DD>:<counter>)，':' 分隔、固定顺序；
    counter 为该环当日递增整数，持久化于 build/kernfuzz/rolling-counter；
    折 int48 正值域。可复现：同日同 commit 同 counter 同 seed 序列
    （`fast.py rolling-seeds --date D --counter N --count K` 纯函数可验证）。

预算（R3 C-4）: fast 环目标 ≤5min。首日实测 build/run 单次耗时的均值在
run() 结束时打印成 `build X ms / run Y ms / 500×4×(X+Y) = Z s`；若超预算，
用 KERNFUZZ_FAST_SCALE（默认 1.0）按比例缩减三环规模并在预算表记录
（docs/kernel-fuzzing-design.md §9）。

用法:
    python3 fast.py                 # 三环全跑（make kernfuzz-fast 调这个）
    python3 fast.py rolling-seeds --date 2026-09-04 --counter 0 --count 200
    python3 fast.py freeze-tc       # 再生成 tc 负例固化快照（需工具链）

Stdlib-only, host Python 3.  确定性：除日志时间戳外，同参数 → 同结果。
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
import morph                                  # noqa: E402
import tc_meta                                # noqa: E402
import tc_oracle as tco                       # noqa: E402

REPO_ROOT = morph._REPO_ROOT
FROZEN_DIR = os.path.join(REPO_ROOT, "test", "kernfuzz-frozen")
FIXED_SEEDS_FILE = os.path.join(FROZEN_DIR, "fixed-seeds.txt")
TC_NEG_DIR = os.path.join(FROZEN_DIR, "tc-negative")
TC_NEG_MANIFEST = os.path.join(TC_NEG_DIR, "manifest.json")
KNOWN_SIGS_FILE = os.path.join(FROZEN_DIR, "morph-known-signatures.txt")
STATE_DIR = os.path.join(REPO_ROOT, "build", "kernfuzz")
COUNTER_FILE = os.path.join(STATE_DIR, "rolling-counter")

# §9 组成常量（原文：300 固定 + 200 滚动 = 500 基础程序 ×4 执行单元）
N_FIXED = 300
N_ROLLING = 200
EXEC_UNITS = 4                # E0 + 3 variants（morph N_VARIANTS=3）

FAST_TIMEOUT = 2.0            # R3 C-4: fast 环单程序超时上限（秒）
BUDGET_SECONDS = 300.0        # fast 环预算 ≤5min
INT48_MOD = 140737488355327   # int48 正值域（M-7 原文）


# ---------------------------------------------------------------------------
# 滚动 seed 派生（M-7 原文）+ counter 持久化
# ---------------------------------------------------------------------------

def roll_seed(git_sha, date, counter):
    """M-7 原文：sha256(<git_sha>:<YYYY-MM-DD>:<counter>) 折 int48 正值域。"""
    h = hashlib.sha256(("%s:%s:%d" % (git_sha, date, counter)).encode()) \
        .digest()
    return int.from_bytes(h[:8], "little") % INT48_MOD


def rolling_seeds(git_sha, date, counter, count):
    """counter 起 count 个滚动 seed（counter+i 逐个派生，序列确定）。"""
    return [roll_seed(git_sha, date, counter + i) for i in range(count)]


def git_sha():
    """当前 HEAD 短哈希；git 缺席时退化为常量（保持可复现）。"""
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT,
                             capture_output=True, timeout=10)
        if out.returncode == 0:
            return out.stdout.decode().strip()[:12]
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "nogit"


def read_counter():
    """持久化 counter（build/kernfuzz/rolling-counter）；缺席从 0 起。"""
    try:
        with open(COUNTER_FILE) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return 0


def write_counter(value):
    if not os.path.isdir(STATE_DIR):
        os.makedirs(STATE_DIR)
    with open(COUNTER_FILE, "w") as f:
        f.write("%d\n" % value)


# ---------------------------------------------------------------------------
# 固化清单 / 工具链
# ---------------------------------------------------------------------------

def load_fixed_seeds(path=FIXED_SEEDS_FILE):
    """固定 seed 清单（M-10 入库件：仅 seed 列表，程序由 gen 再生成）。"""
    seeds = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                seeds.append(int(line))
    return seeds


def toolchain_missing():
    """§9 退出语义的判定：三件套任一缺席 → 缺席名单（空 = 齐备）。"""
    need = (morph.TINYACTOR, morph.TAVM_ASAN, morph.AST_DUMP)
    return [p for p in need if not os.path.exists(p)]


# ---------------------------------------------------------------------------
# 环 1: morph 差分（固定 + 滚动 seed，×4 执行单元）
# ---------------------------------------------------------------------------

def load_known_signatures():
    """M-10 入库件：triaged 已知 finding 签名（见文件头 triage 注释）。
    与 findings/ 目录内自动累积的签名合并使用。"""
    known = set()
    if os.path.exists(KNOWN_SIGS_FILE):
        with open(KNOWN_SIGS_FILE) as f:
            for line in f:
                sig = line.split("#", 1)[0].strip()
                if sig:
                    known.add(sig)
    return known


def ring_morph(seeds, out_dir, log):
    runner = morph.Runner(tempfile.mkdtemp(prefix="kernfuzz-fast-morph-"),
                          timeout=FAST_TIMEOUT)
    try:
        dedup = morph.load_known_signatures(out_dir)
        dedup |= load_known_signatures()
        stats = morph.fuzz_batch(runner, seeds, out_dir, log=log,
                                 known=dedup)
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)
    return stats


# ---------------------------------------------------------------------------
# 环 2: fmt 幂等性扫语料（复用 tc_meta.check_fmt，fmt-only）
# ---------------------------------------------------------------------------

def ring_fmt(seeds, out_dir, log):
    workdir = tempfile.mkdtemp(prefix="kernfuzz-fast-fmt-")
    runner = tc_meta.MetaRunner(workdir, timeout=FAST_TIMEOUT)
    try:
        # 前提先行（DELIV-9 首项）：codegen byte 级确定性探针，不过则降级
        byte_ok, reason, _mm = tc_meta.probe_codegen_determinism(
            runner, seeds[:tc_meta.PREMISE_PROBE_MAX])
        mode = "byte" if byte_ok else "structural"
        if not byte_ok:
            log("fmt ring: PREMISE DOWNGRADE -> structural (%s)" % reason)
        checks = {"fmt": 0, "fmt-byte": 0, "fmt-structural": 0}
        findings = dict((c, 0) for c in ("meta-fmt",))
        dedup = morph.load_known_signatures(out_dir)
        skips = []
        for seed in seeds:
            plan = gen.build_program(seed)
            src0 = gen.render_tree(plan)
            sp0, ap0 = tc_meta._write_src(runner, src0, "P")
            base = runner.build(sp0, ap0)
            v0 = tco.classify_build(base)
            if v0 not in ("accept", "reject"):
                skips.append((seed, "baseline:%s" % v0))
                continue
            checks["fmt"] += 1
            tc_meta.check_fmt(
                runner, seed, src0, sp0, ap0, base, mode,
                out_dir, dedup, skips, findings, checks, log)
            log("fmt seed %d done" % seed)
        return {"checks": checks, "findings": findings, "skips": skips,
                "premise": mode}
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# 环 3: typecheck 固化负例重放（当前编译器逐个 build，断言不漂移）
# ---------------------------------------------------------------------------

def replay_one(runner, entry, dedup, findings, out_dir):
    """重放一条固化负例。Returns "ok" | "missed-reject" | "tc-drift"
    | "tc-crash"。"""
    path = os.path.join(TC_NEG_DIR, entry["file"])
    with open(path, "rb") as f:
        src_text = f.read().decode("latin-1")
    sp, ap = tco._write_src(runner, src_text, "r")
    res = runner.build(sp, ap)
    cat = tco.classify_build(res)
    if cat == "anomaly":
        # anomaly（exit!=0 但无 type error 关键字，常见为 parse error）对
        # 固化快照只能是基础设施噪音（冻结时已过 parse_precheck）或编译器
        # parser 回归 —— 重放一次区分瞬时/持续：以第二次结果为准。
        res = runner.build(sp, ap)
        cat = tco.classify_build(res)
    expect = entry["expect"]
    prog = [{"tag": entry["file"], "src_text": src_text, "res": res,
             "build_err": (res.out + res.err) if res.rc != 0 else b""}]

    def record(category):
        tco._record(out_dir, category, prog, entry["seed"], dedup, findings)
        return category

    if expect == "reject":
        if cat == "reject":
            return "ok"
        if cat == "crash":
            return record("tc-crash")
        return record("missed-reject")   # 期望 reject 却 accept → 假阴性洞
    if expect == "accept-hole":
        if cat == "accept":
            return "ok"
        if cat == "crash":
            return record("tc-crash")
        return record("tc-drift")        # 冻结的洞突然被查 → 行为漂移
    # accept-quiet（exhaust 类）：必须仍 accept 且无 warning
    _cat, warned = tco.classify_exhaust(res)
    if _cat == "accept" and not warned:
        return "ok"
    if _cat == "crash":
        return record("tc-crash")
    return record("tc-drift")


def ring_tc_replay(out_dir, max_cases=None):
    """重放固化负例。`max_cases`（预算缩减用）按确定性等距采样取子集 —
    同一 manifest 同参数永远选中同一批（不引入任何随机源）。"""
    with open(TC_NEG_MANIFEST) as f:
        manifest = json.load(f)
    entries = manifest["entries"]
    if max_cases is not None and max_cases < len(entries):
        step = len(entries) / float(max_cases)
        picked = [entries[int(i * step)] for i in range(max_cases)]
        manifest = dict(manifest, entries=picked, sampled_from=len(entries))
    runner = morph.Runner(tempfile.mkdtemp(prefix="kernfuzz-fast-tc-"),
                          timeout=FAST_TIMEOUT)
    try:
        dedup = morph.load_known_signatures(out_dir)
        findings = dict((c, 0) for c in tco.FINDING_CATEGORIES)
        tally = {"ok": 0, "missed-reject": 0, "tc-drift": 0, "tc-crash": 0}
        for entry in manifest["entries"]:
            outcome = replay_one(runner, entry, dedup, findings, out_dir)
            tally[outcome] += 1
        return {"cases": len(manifest["entries"]), "tally": tally,
                "findings": findings, "manifest": manifest,
                "total_frozen": len(entries)}
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# freeze-tc: 用 tc_oracle 变异器对固定 seed 离线生成负例并固化（一次性）
# ---------------------------------------------------------------------------

def cmd_freeze(args):
    missing = toolchain_missing()
    if missing:
        sys.stderr.write("freeze-tc needs the full toolchain, missing: %s\n"
                         % ", ".join(missing))
        return 2
    seeds = load_fixed_seeds(args.seeds_file)
    out_dir = args.out
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    # 生成期用 §5.4 默认 5s 超时（fast 的 2s 只约束重放环）
    runner = morph.Runner(tempfile.mkdtemp(prefix="kernfuzz-freeze-"))
    entries = []
    skipped = []
    try:
        for seed in seeds:
            plan = gen.build_program(seed)
            src0 = gen.render_tree(plan)
            # A1 对照：基程序必须 accept，否则整个 seed 跳过（同 tc_oracle）
            sp0, ap0 = tco._write_src(runner, src0, "P")
            if tco.classify_build(runner.build(sp0, ap0)) != "accept":
                skipped.append({"seed": seed, "why": "control-not-accept"})
                continue
            for ci, cls in enumerate(tco.CLASSES):
                rng = prng.make_prng(prng.derive_seed(seed, 7000 + ci))
                # 类内重 roll：与 tc_oracle.run_seed 逐行同构（9xxx 派生、
                # ≤3 次重 roll、每个 P' 过自己的 A1 对照）
                mplan = minfo = None
                for k in range(4):
                    cand = plan if k == 0 else gen.build_program(
                        prng.derive_seed(seed, 9000 + ci * 10 + k))
                    mplan, minfo = tco.MUTATORS[cls](cand, rng)
                    if mplan is None:
                        continue
                    if k:
                        csrc = gen.render_tree(cand)
                        csp, cap = tco._write_src(runner, csrc,
                                                  "c%d_%d" % (ci, k))
                        if tco.classify_build(
                                runner.build(csp, cap)) != "accept":
                            skipped.append({"seed": seed, "class": cls,
                                            "why": "reroll-control-fail"})
                            mplan = None
                    break
                if mplan is None:
                    skipped.append({"seed": seed, "class": cls,
                                    "why": "no-site-after-reroll"})
                    continue
                msrc = gen.render_tree(mplan)
                ok, _dres = tco.parse_precheck(runner, msrc, "m%d" % ci)
                if not ok:
                    skipped.append({"seed": seed, "class": cls,
                                    "why": "parse-reject"})
                    continue
                fname = "s%d_%s.ta" % (seed, cls)
                with open(os.path.join(out_dir, fname), "wb") as f:
                    f.write(msrc.encode("latin-1"))
                entries.append({
                    "file": fname, "seed": seed, "class": cls,
                    "sub": minfo["sub"],
                    "expect": tco.SUB_EXPECT[(cls, minfo["sub"])],
                })
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)
    manifest = {
        "frozen-from": os.path.relpath(args.seeds_file, REPO_ROOT),
        "frozen-with": "tools/kernfuzz/fast.py freeze-tc "
                       "(tc_oracle mutators, derive_seed 7xxx/9xxx)",
        "note": "负例固化快照（M-10 入库件）。fast 环只重放断言，不现生成；"
                "用当前编译器逐个 build，期望结论不漂移。",
        "entries": entries,
        "skipped": skipped,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    sys.stderr.write("freeze-tc: %d negatives frozen (%d seed-class slots "
                     "skipped) -> %s\n" % (len(entries), len(skipped),
                                           out_dir))
    return 0


# ---------------------------------------------------------------------------
# rolling-seeds: 纯函数子命令（可复现性验证入口）
# ---------------------------------------------------------------------------

def cmd_rolling_seeds(args):
    sha = args.git_sha if args.git_sha else git_sha()
    for s in rolling_seeds(sha, args.date, args.counter, args.count):
        print(s)
    return 0


# ---------------------------------------------------------------------------
# 主流程: 三环 + 预算实测 + 退出语义
# ---------------------------------------------------------------------------

def probe_budget():
    """首日实测：单次 build + run 耗时（固定 seed 程序，各测 5 次取均值）。"""
    seed = load_fixed_seeds()[0]
    src = gen.gen_program(seed)
    workdir = tempfile.mkdtemp(prefix="kernfuzz-budget-")
    try:
        sp = os.path.join(workdir, "budget.ta")
        with open(sp, "wb") as f:
            f.write(src.encode("latin-1"))
        ap = os.path.join(workdir, "budget.tabc")
        builds, runs = [], []
        for _ in range(5):
            t0 = time.time()
            morph._run([morph.TINYACTOR, "build", sp, ap], 30.0)
            builds.append(time.time() - t0)
            t0 = time.time()
            morph._run([morph.TAVM_ASAN, ap], 30.0, morph.ASAN_ENV)
            runs.append(time.time() - t0)
        return (sum(builds) / len(builds) * 1000.0,
                sum(runs) / len(runs) * 1000.0)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def cmd_run(args):
    missing = toolchain_missing()
    if missing:
        print("KERNFUZZ-SKIPPED: 工具链缺失 (%s)" %
              ", ".join(os.path.basename(p) for p in missing))
        return 0

    t_start = time.time()
    scale = float(os.environ.get("KERNFUZZ_FAST_SCALE", "1.0"))
    fixed = load_fixed_seeds()
    n_fixed = max(1, int(round(len(fixed) * scale)))
    n_rolling = max(0, int(round(N_ROLLING * scale)))
    seeds = fixed[:n_fixed]
    if n_rolling:
        date = time.strftime("%Y-%m-%d")
        counter = read_counter()
        seeds = seeds + rolling_seeds(git_sha(), date, counter, n_rolling)
    out_dir = os.path.join(STATE_DIR, "fast")
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    log = lambda m: None                  # noqa: E731  (进度走各环 summary)

    # ---- 环 1: morph 差分 -------------------------------------------------
    t0 = time.time()
    m = ring_morph(seeds, os.path.join(out_dir, "morph"), log)
    t_morph = time.time() - t0
    fk = m["findings"]
    print("== ring 1/3 morph differential: programs=%d (fixed=%d rolling=%d) "
          "exec-units=x%d ran=%d skip=%d findings=%d {%s} elapsed=%.1fs"
          % (len(seeds), n_fixed, n_rolling, EXEC_UNITS, m["ran"],
             len(m["skips"]), sum(fk.values()),
             ", ".join("%s=%d" % (c, fk[c]) for c in morph.CATEGORIES),
             t_morph))

    # ---- 环 2: fmt 幂等性 -------------------------------------------------
    t0 = time.time()
    fm = ring_fmt(seeds, os.path.join(out_dir, "fmt"), log)
    t_fmt = time.time() - t0
    print("== ring 2/3 fmt idempotence: programs=%d premise=%s checks="
          "{fmt=%d byte=%d structural=%d} skip=%d findings=%d elapsed=%.1fs"
          % (len(seeds), fm["premise"], fm["checks"]["fmt"],
             fm["checks"]["fmt-byte"], fm["checks"]["fmt-structural"],
             len(fm["skips"]), fm["findings"]["meta-fmt"], t_fmt))

    # ---- 环 3: tc 固化负例重放（规模随 scale 等比缩减） -------------------
    t0 = time.time()
    with open(TC_NEG_MANIFEST) as _mf:
        n_frozen = len(json.load(_mf)["entries"])
    n_tc = max(1, int(round(n_frozen * scale)))
    tc = ring_tc_replay(os.path.join(out_dir, "tc-replay"), max_cases=n_tc)
    t_tc = time.time() - t0
    print("== ring 3/3 tc frozen-negative replay: cases=%d/%d frozen "
          "tally={%s} findings=%d elapsed=%.1fs"
          % (tc["cases"], tc["total_frozen"],
             ", ".join("%s=%d" % kv for kv in sorted(tc["tally"].items())),
             sum(tc["findings"].values()), t_tc))

    # ---- 预算实测（R3 C-4 首日回填格式） ----------------------------------
    t_probe0 = time.time()
    b_ms, r_ms = probe_budget()
    t_probe = time.time() - t_probe0
    total = time.time() - t_start
    print("== budget: build %d ms / run %d ms / %dx%d*(%d+%d) = %d s "
          "(ideal 500-prog projection; actual wall incl. all rings+probe "
          "%.1f s, scale=%s)"
          % (b_ms, r_ms, len(seeds), EXEC_UNITS, b_ms, r_ms,
             len(seeds) * EXEC_UNITS * (b_ms + r_ms) / 1000.0,
             total, scale))

    # ---- 退出语义 ---------------------------------------------------------
    findings_total = (sum(m["findings"].values())
                      + fm["findings"]["meta-fmt"]
                      + sum(tc["findings"].values()))
    if findings_total:
        print("KERNFUZZ-FAST: %d finding(s) — see %s/{morph,fmt,tc-replay}"
              % (findings_total, out_dir))
        return 1
    # 成功才推进滚动 counter（失败保留同批 seed 便于复现）
    if n_rolling:
        write_counter(read_counter() + n_rolling)
    print("KERNFUZZ-FAST: all rings green (total %.1f s, budget %d s)"
          % (total, BUDGET_SECONDS))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="kernfuzz fast ring (kernel-fuzzing §9, DELIV-9)")
    sub = ap.add_subparsers(dest="cmd")

    sp_rs = sub.add_parser(
        "rolling-seeds",
        help="print M-7 rolling seeds (pure; reproducibility check)")
    sp_rs.add_argument("--date", required=True, help="YYYY-MM-DD")
    sp_rs.add_argument("--counter", type=int, required=True)
    sp_rs.add_argument("--count", type=int, default=N_ROLLING)
    sp_rs.add_argument("--git-sha", default=None,
                       help="override HEAD (default: current)")

    sp_fz = sub.add_parser(
        "freeze-tc",
        help="regenerate frozen tc negatives from the fixed seed list")
    sp_fz.add_argument("--seeds-file", default=FIXED_SEEDS_FILE)
    sp_fz.add_argument("--out", default=TC_NEG_DIR)

    args = ap.parse_args(argv)
    if args.cmd == "rolling-seeds":
        return cmd_rolling_seeds(args)
    if args.cmd == "freeze-tc":
        return cmd_freeze(args)
    return cmd_run(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except morph.MorphError as e:
        sys.stderr.write("fast: error: %s\n" % e)
        sys.exit(2)