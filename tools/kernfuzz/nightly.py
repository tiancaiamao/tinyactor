# -*- coding: utf-8 -*-
"""nightly.py — kernfuzz 慢速环编排器 (kernel-fuzzing §9, DELIV-10).

`make kernfuzz-nightly`（nightly 触发的慢速环）的全部组成，§9 原文
（"慢速环：golden 子集语料全量 + 1000 生成程序、typecheck 双向 2000 例
（现生成）、GC 顺序差分、多重集 harness、TSan 长跑、CPS 4-way 250
seed"；Tier 覆盖矩阵：nightly=Tier A+B+golden+GC+CPS（§5.2 gate 于
task-cps-gate 第 2 轮全绿后上线；--no-cps 为逃生口）：

  环 1  golden 子集语料全量: test/kernfuzz-frozen/snapshots/ 全部快照
        （M-10 入库件）——逐条 (a) ast-dump 现dump 与冻结快照 byte 级比对
        （DELIV-1 防漂移），(b) 源码 build+run(tavm_asan) vs golden 冻结
        快照求值，norm 后比对（DELIV-2 锚点断言）。
  环 2  morph 差分 1000 滚动 seed（M-7，nightly 自己的 counter 文件），
        全部变换规则 = Tier A+B（--tier 语义: rules=None）；Tier C 全程序
        变换不在 morph transforms 内（由环 7 专管）。
  环 3  typecheck 双向 2000 例（现生成，非固化重放——tc_oracle.fuzz_batch）。
  环 4  GC 顺序差分（gc_seqdiff，窗口参数化默认 100 seed × N=1）。
  环 5  多重集 harness（multiset.run_matrix，矩阵参数化 K/M）。
  环 6  TSan 长跑（W-chaos，§7.4: 30min 上限，超时=疑似死锁 finding）。
        M-13: TSan 战线以 Linux x86_64 CI runner 为准，macOS 上不承诺——
        探测 TSAN=1 构建可用性，不可用时记显式 SKIP 观测项（慢速环唯一
        允许的跳过形态：平台不可用留痕，而非静默绿）。

退出语义（§9，与 fast 相反）:
  - 工具链缺席（tinyactor / tavm_asan / ast-dump.ta / golden.py 任一不存在）
    → 打印 `KERNFUZZ-NIGHTLY-TOOLCHAIN-MISSING: ...` 且 **exit 1**
    （慢速环不允许静默跳过）。
  - 有工具链但任一组成出现 novel finding → exit 1。
  - 全绿 → exit 0，并推进 nightly 滚动 counter（失败保留同批 seed 便于复现，
    与 fast.py 同惯例）。

滚动 seed（M-7，与 fast.py 逐字同公式）:
    sha256(<git_sha>:<YYYY-MM-DD>:<counter>) 折 int48 正值域；counter 持久化
    于 build/kernfuzz/rolling-counter-nightly（**独立于 fast 的
    rolling-counter**）。一个 seed 块按环切分: [morph | tc | seqdiff起点 |
    multiset]，同 commit+date+counter 复现一致
    （`nightly.py rolling-seeds --date D --counter N --count K` 纯函数可验）。

预算（--scale，默认 1.0 = 全量）: 各组成规模按比例缩减供本地干跑；
TSan 长跑上限同比例缩减（下限 60s）。落盘纪律: build/kernfuzz/nightly/
report.json 每环结束即写；progress.json 由后台心跳每小时刷新。

用法:
    python3 nightly.py                    # 全量慢速环（make kernfuzz-nightly）
    python3 nightly.py --scale 0.2       # 缩减干跑
    python3 nightly.py rolling-seeds --date 2026-09-04 --counter 0 --count 5

Stdlib-only, host Python 3.  确定性：除日志时间戳外，同参数 → 同结果。
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import prng                                    # noqa: E402
import morph                                   # noqa: E402
import fast                                    # noqa: E402  (roll_seed + 冻结已知签名)
import cps                                     # noqa: E402  (环 7: Tier C 4-way)
import tc_oracle as tco                        # noqa: E402
import gc_workloads                            # noqa: E402
import gc_seqdiff                              # noqa: E402
import multiset                                # noqa: E402
import sexp as sexp_mod                        # noqa: E402  (§5.0 子集判定)

REPO_ROOT = morph._REPO_ROOT
SNAPSHOT_DIR = os.path.join(fast.FROZEN_DIR, "snapshots")
COUNTER_FILE = os.path.join(fast.STATE_DIR, "rolling-counter-nightly")
OUT_DIR = os.path.join(fast.STATE_DIR, "nightly")
KNOWN_SIGS_FILE = fast.KNOWN_SIGS_FILE
TAVM_TSAN = os.path.join(REPO_ROOT, "tavm_tsan")

# §9 慢速环组成常量（原文行）
N_MORPH = 1000                 # morph 生成程序（Tier A+B）
N_TC = 2000                    # typecheck 双向（现生成）
N_SEQDIFF = 100                # GC 顺序差分窗口（N=1 stress）
N_MULTISET_SEEDS = 20          # 多重集 seed 数（× K × M 矩阵）
MULTISET_KS = (4, 16)          # K 矩阵（P1-DELIV-8#2: K=16,M=100 在内）
MULTISET_MS = (25, 100)        # M 矩阵
SEQDIFF_STRESS = "1"           # gc_seqdiff --stress-n（N=1）
TSAN_CAP_S = 1800.0            # §7.4: W-chaos 长跑 30min 上限
TSAN_BUILD_TIMEOUT = 900.0     # TSAN=1 探测构建的上限

# CPS（Tier C）上线门槛（§5.2 语料自检）。gate 未过 → 拒绝运行
# （exit 1），绝不静默跳过。task-cps-gate 第 2 轮（2026-09-04）修 cps.py
# trans() impure &&/|| 拒绝缺失后全绿（生成语料 232/232 + 快照 19/19
# judged 全 consistent，findings 0），翻转此常量并接线环 7。
CPS_GATE_PASSED = True

# 环 7: Tier C CPS 4-way 自检 seed 数（= §5.2 gate 生成语料窗口）
N_CPS = 250

HEARTBEAT_S = 3600.0           # 落盘纪律: 每小时刷新 progress.json


# ---------------------------------------------------------------------------
# 滚动 seed（M-7，复用 fast 的纯函数）+ nightly 独立 counter 持久化
# ---------------------------------------------------------------------------

def roll_seed(git_sha, date, counter):
    """M-7 原文公式（与 fast.roll_seed 逐字一致，独立 counter 文件）。"""
    return fast.roll_seed(git_sha, date, counter)


def rolling_seeds(git_sha, date, counter, count):
    return fast.rolling_seeds(git_sha, date, counter, count)


def read_counter():
    """nightly 自己的 counter（build/kernfuzz/rolling-counter-nightly）。"""
    try:
        with open(COUNTER_FILE) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return 0


def write_counter(value):
    os.makedirs(os.path.dirname(COUNTER_FILE), exist_ok=True)
    with open(COUNTER_FILE, "w") as f:
        f.write("%d\n" % value)


def partition_seeds(seeds, n_morph, n_tc, n_multiset):
    """一个滚动 seed 块按环切分。

    Returns (morph_seeds, tc_seeds, seqdiff_start, multiset_seeds)；
    seqdiff 窗口 = [seqdiff_start, seqdiff_start + n_seqdiff) 连续区间
    （gc_seqdiff --seeds A:B 语义），切分确定性：同块必同切分。
    """
    a = n_morph
    b = a + n_tc
    morph_seeds = seeds[:a]
    tc_seeds = seeds[a:b]
    seqdiff_start = seeds[b]
    multiset_seeds = seeds[b + 1:b + 1 + n_multiset]
    return morph_seeds, tc_seeds, seqdiff_start, multiset_seeds


# ---------------------------------------------------------------------------
# 计划计算（--scale 纯函数，可单测）
# ---------------------------------------------------------------------------

def compute_plan(scale, n_corpus_total=None):
    """全量组成 × scale 的实例化规模（全部为纯算术）。"""
    if scale <= 0:
        raise ValueError("scale must be > 0, got %r" % scale)
    if n_corpus_total is None:
        n_corpus_total = len(frozen_snapshots())
    return {
        "scale": scale,
        "n_corpus": max(1, int(round(n_corpus_total * min(scale, 1.0)))),
        "n_morph": max(1, int(round(N_MORPH * scale))),
        "n_tc": max(1, int(round(N_TC * scale))),
        "n_seqdiff": max(2, int(round(N_SEQDIFF * scale))),
        "n_multiset": max(1, int(round(N_MULTISET_SEEDS * scale))),
        "n_cps": max(1, int(round(N_CPS * min(scale, 1.0)))),
        "ks": list(MULTISET_KS),
        "ms": list(MULTISET_MS),
        "seqdiff_stress": SEQDIFF_STRESS,
        "tsan_cap_s": max(60.0, TSAN_CAP_S * scale),
    }


# ---------------------------------------------------------------------------
# 工具链探测（§9 退出语义：nightly 缺席 = exit 1）
# ---------------------------------------------------------------------------

def toolchain_missing():
    need = (morph.TINYACTOR, morph.TAVM_ASAN, morph.AST_DUMP, morph.GOLDEN)
    return [p for p in need if not os.path.exists(p)]


# ---------------------------------------------------------------------------
# golden 子集语料映射 + finding 证据落盘（nightly 本地 evidence 树）
# ---------------------------------------------------------------------------

def snapshot_source_path(snap_path):
    """快照文件名 → 语料源码路径。

    命名（snapshot.scm 冻结）: `<dir>-<name>.sexp`（flat，首个 '-' 前是
    test/ 下目录名，其后是文件名去 .ta），如 basic-closure.sexp →
    test/basic/closure.ta、compiler-parser-ast.sexp →
    test/compiler/parser-ast.ta。源码缺席返回 None（调用方计 skip）。
    """
    base = os.path.basename(snap_path)
    if not base.endswith(".sexp"):
        return None
    stem = base[:-len(".sexp")]
    dir_name, _, name = stem.partition("-")
    if not dir_name or not name:
        return None
    src = os.path.join(REPO_ROOT, "test", dir_name, name + ".ta")
    return src if os.path.exists(src) else None


def frozen_snapshots():
    if not os.path.isdir(SNAPSHOT_DIR):
        return []
    return sorted(os.path.join(SNAPSHOT_DIR, n)
                  for n in os.listdir(SNAPSHOT_DIR)
                  if n.endswith(".sexp"))


def load_node_table():
    """冻结 AST 节点表（test/kernfuzz-frozen/ast-nodes.txt 结构节点段），
    即 DELIV-2 「§5.0 子集」判定的参照集合。"""
    path = os.path.join(fast.FROZEN_DIR, "ast-nodes.txt")
    table = set()
    in_atoms = False
    with open(path, encoding="latin-1") as f:
        for line in f:
            s = line.strip()
            if s.startswith("## Atoms"):
                in_atoms = True
            if not s or s.startswith("#") or s.startswith(";") or in_atoms:
                continue
            table.add(s.split()[0])
    return table


_NODE_TABLE = None


def golden_subset_reason(snap_path):
    """DELIV-2 §5.0 子集判定：snapshot sexp 全树 list 节点的 car（symbol）
    与冻结节点表求差 = 空 → 在子集内（返回 None）；否则返回含未知头样例的
    原因串。golden 锚点只对子集内程序有意义——golden 解释器只实现纯核心
    语义，语料里的 net./str./用户函数调用不在其覆盖范围（非编译器缺陷）。"""
    global _NODE_TABLE
    if _NODE_TABLE is None:
        _NODE_TABLE = load_node_table()
    tree = sexp_mod.sexp_read(snap_path)
    unknown = sorted(set(str(c.name) for c in sexp_mod.sexp_collect_cars(tree))
                     - _NODE_TABLE)
    if not unknown:
        return None
    return "outside-subset:" + ",".join(unknown[:3])


def load_known_signatures():
    """M-10 入库件（triaged 已知 finding 签名）+ out_dir 自动累积。"""
    return fast.load_known_signatures()


def load_ring_signatures(out_dir):
    """重载本环 evidence 目录里已有的 finding 签名（目录名
    `<ring>.<category>:<sig16>`），同 morph 「同类已知自动跳过」惯例。"""
    known = set()
    if not os.path.isdir(out_dir):
        return known
    for name in os.listdir(out_dir):
        if "." in name and ":" in name:
            known.add(name.split(".", 1)[1])
    return known


def record_nightly_finding(out_dir, ring, category, src_text, evidence,
                           dedup, counter):
    """落盘一条 finding 证据（签名去重，同 morph 惯例）。

    Returns True = novel（计入失败），False = 已知签名去重命中。
    签名/落盘前先做 latin-1 投影（morph.signature 假定 latin-1 可编码；
    gc_workloads 生成头含 em-dash 等非 latin-1 字符）。
    """
    src_text = src_text.encode("latin-1", "replace").decode("latin-1")
    sig = morph.signature(category, src_text)
    if sig in dedup:
        counter["dedup"] = counter.get("dedup", 0) + 1
        return False
    d = os.path.join(out_dir, "%s.%s" % (ring, sig))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "src.ta"), "wb") as f:
        f.write(src_text.encode("latin-1", "replace"))
    with open(os.path.join(d, "finding.json"), "w") as f:
        json.dump({"ring": ring, "category": category, "signature": sig,
                   "evidence": evidence}, f, indent=1, sort_keys=True,
                  default=str)
    dedup.add(sig)
    counter["novel"] = counter.get("novel", 0) + 1
    return True


# ---------------------------------------------------------------------------
# 环 1: golden 子集语料全量（冻结快照防漂移 + DELIV-2 锚点断言）
# ---------------------------------------------------------------------------

def ring_golden_corpus(runner, out_dir, n_corpus, counter):
    snaps = frozen_snapshots()[:n_corpus]
    dedup = load_known_signatures() | load_ring_signatures(out_dir)
    tally = {"snapshot-drift": 0, "build-fail": 0, "hang": 0,
             "tavm-crash": 0, "anchor-mismatch": 0, "anchor-crash": 0,
             "anchor-skip": 0}
    checks = {"dump-cmp": 0, "anchor-cmp": 0}
    skips = []
    for snap in snaps:
        name = os.path.basename(snap)
        src = snapshot_source_path(snap)
        if src is None:
            skips.append((name, "source-missing"))
            continue
        with open(snap, "rb") as f:
            frozen_sexp = f.read()
        # (a) 现dump vs 冻结快照 byte 级（DELIV-1 快照防漂移）
        d = runner.dump(src)
        if d.timed_out or d.rc != 0 or not d.out:
            if record_nightly_finding(out_dir, "golden", "snapshot-drift",
                                      frozen_sexp.decode("latin-1"),
                                      {"snapshot": name,
                                       "why": "dump-fail",
                                       "stderr": d.err}, dedup, counter):
                tally["snapshot-drift"] += 1
            continue
        if d.out != frozen_sexp:
            if record_nightly_finding(out_dir, "golden", "snapshot-drift",
                                      frozen_sexp.decode("latin-1"),
                                      {"snapshot": name,
                                       "why": "dump-bytes-differ"},
                                      dedup, counter):
                tally["snapshot-drift"] += 1
            continue
        checks["dump-cmp"] += 1
                # (b) build + run(tavm_asan) vs golden(冻结快照)（DELIV-2 锚点）
        with open(src) as f:
            # morph.build_and_run 按 latin-1 落盘；签入测试源含 em-dash 等
            # 非 latin-1 注释字符 → 投影为 '?'（仅注释字节，语义不变）。
            src_text = f.read().encode("latin-1", "replace").decode("latin-1")
        res, _paths, bp = runner.build_and_run(src_text, "corpus")
        if bp.rc != 0:
            if record_nightly_finding(out_dir, "golden", "build-fail",
                                      src_text,
                                      {"snapshot": name,
                                       "stderr": (bp.out + bp.err)},
                                      dedup, counter):
                tally["build-fail"] += 1
            continue
        cat = morph.classify_run(res)
        if cat is not None:
            key = "hang" if cat == "hang" else "tavm-crash"
            if record_nightly_finding(out_dir, "golden", key, src_text,
                                      {"snapshot": name, "rc": res.rc,
                                       "stderr": res.err}, dedup, counter):
                tally[key] += 1
            continue
        # (c) golden 锚点仅对 §5.0 子集（DELIV-2：节点表求差 = 空）。
        # 子集外语料（net./str./用户函数）不是编译器缺陷，显式留痕跳过。
        reason = golden_subset_reason(snap)
        if reason is not None:
            skips.append((name, reason))
            tally["anchor-skip"] += 1
            continue
        g = runner.golden_eval(snap)
        base = morph.norm_tavm(res.out, res.rc)
        if g.rc != 0 and b"DIVZERO:" not in g.out:
            if record_nightly_finding(out_dir, "golden", "anchor-crash",
                                      src_text,
                                      {"snapshot": name,
                                       "golden_err": g.err}, dedup, counter):
                tally["anchor-crash"] += 1
            continue
        if morph.norm_golden(g.out) != base:
            if record_nightly_finding(out_dir, "golden", "anchor-mismatch",
                                      src_text,
                                      {"snapshot": name,
                                       "tavm_norm": base,
                                       "golden_norm":
                                       morph.norm_golden(g.out)},
                                      dedup, counter):
                tally["anchor-mismatch"] += 1
            continue
        checks["anchor-cmp"] += 1
    return {"snapshots": len(snaps), "checks": checks, "tally": tally,
            "skips": skips}


# ---------------------------------------------------------------------------
# 环 2: morph 差分 1000 滚动 seed（Tier A+B，即全部规则 rules=None）
# ---------------------------------------------------------------------------

def ring_morph(seeds, out_dir, log):
    runner = morph.Runner(tempfile.mkdtemp(prefix="kernfuzz-nightly-morph-"))
    try:
        dedup = morph.load_known_signatures(out_dir)
        dedup |= load_known_signatures()
        stats = morph.fuzz_batch(runner, seeds, out_dir, log=log,
                                 known=dedup, rules=None)
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)
    return stats


# ---------------------------------------------------------------------------
# 环 3: typecheck 双向（现生成，非固化重放）
# ---------------------------------------------------------------------------

def ring_tc(seeds, out_dir, log):
    runner = morph.Runner(tempfile.mkdtemp(prefix="kernfuzz-nightly-tc-"))
    try:
        stats = tco.fuzz_batch(runner, seeds, out_dir, log=log)
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)
    return stats


# ---------------------------------------------------------------------------
# 环 4: GC 顺序差分 — ring_seqdiff（编排复用 gc_seqdiff CLI）
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 环 5: 多重集 harness（multiset.run_matrix，矩阵参数化）
# ---------------------------------------------------------------------------

def ring_multiset(seeds, ks, ms, out_dir):
    summary = multiset.run_matrix(list(seeds), ks, ms,
                                  findings_dir=out_dir)
    multiset.print_summary(summary, stream=sys.stderr)
    return summary


# ---------------------------------------------------------------------------
# 环 6: TSan 长跑（W-chaos，§7.4；M-13 macOS 显式 SKIP 观测项）
# ---------------------------------------------------------------------------

def _try_build_tsan(log):
    """探测 TSAN=1 构建可用性。Returns (ok, detail)。"""
    if os.path.exists(TAVM_TSAN):
        return True, "tavm_tsan exists"
    log("ring tsan: tavm_tsan missing, probing TSAN=1 build (M-13)")
    try:
                p = subprocess.run(["make", "--no-print-directory", "TSAN=1"],
                           cwd=REPO_ROOT, capture_output=True,
                           timeout=TSAN_BUILD_TIMEOUT)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, "probe build error: %s" % e
    if p.returncode == 0 and os.path.exists(TAVM_TSAN):
        return True, "TSAN=1 build ok"
    return False, ("TSAN=1 build failed (rc=%d): %s"
                   % (p.returncode,
                      (p.stderr or p.stdout).decode("utf-8", "replace")
                      [-800:]))


def ring_tsan(seed, cap_s, out_dir, counter, log):
    """W-chaos 长跑。平台不可用 → 显式 SKIP 观测项（M-13，不计失败）。"""
    ok, detail = _try_build_tsan(log)
    if not ok:
        obs = {"component": "tsan-long-run", "status": "SKIP",
               "why": "M-13 platform-unavailable", "detail": detail}
        with open(os.path.join(out_dir, "tsan-skip.json"), "w") as f:
            json.dump(obs, f, indent=1, sort_keys=True)
        log("ring tsan: SKIP (explicit observation): %s" % detail)
        return {"status": "SKIP", "detail": detail, "races": 0}
    dedup = load_known_signatures() | load_ring_signatures(out_dir)

    src_text = gc_workloads.generate("chaos", seed, 1)
    workdir = tempfile.mkdtemp(prefix="kernfuzz-nightly-tsan-")
    try:
        sp = os.path.join(workdir, "chaos.ta")
        ap = os.path.join(workdir, "chaos.tabc")
        with open(sp, "w") as f:
            f.write(src_text)
        bp = morph._run([morph.TINYACTOR, "build", sp, ap], 120.0)
        if bp.rc != 0:
            record_nightly_finding(out_dir, "tsan", "tsan-build-fail",
                                   src_text, {"stderr": bp.out + bp.err},
                                   dedup, counter)
            return {"status": "FAIL", "races": 1}
        env = {"TSAN_OPTIONS": "halt_on_error=1"}
        res = morph._run([TAVM_TSAN, ap], cap_s, env)
        if res.timed_out:
            # §7.4: 30min 上限，超时检测死锁
            novel = record_nightly_finding(
                out_dir, "tsan", "tsan-hang", src_text,
                {"cap_s": cap_s, "stderr": res.err[-4000:]},
                dedup, counter)
            return {"status": "FAIL" if novel else "DEDUP", "races": 1}
        if res.rc != 0 or b"WARNING: ThreadSanitizer" in (res.out + res.err):
            novel = record_nightly_finding(
                out_dir, "tsan", "tsan-race", src_text,
                {"rc": res.rc, "stderr": (res.out + res.err)[-8000:]},
                dedup, counter)
            return {"status": "FAIL" if novel else "DEDUP", "races": 1}
        return {"status": "ok", "races": 0}
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# 每小时落盘心跳（progress.json）
# ---------------------------------------------------------------------------

class Heartbeat(object):
    """后台线程：每小时 + 每环边界刷新 build/kernfuzz/nightly/progress.json。"""

    def __init__(self, path):
        self.path = path
        self.state = {"started": time.strftime("%Y-%m-%dT%H:%M:%S"),
                      "updated": None, "current_ring": None,
                      "completed": []}
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop)
        self._thread.daemon = True
        self._thread.start()

    def ring_start(self, name):
        self.state["current_ring"] = name
        self.flush()

    def ring_done(self, name):
        self.state["completed"].append(name)
        self.state["current_ring"] = None
        self.flush()

    def _loop(self):
        while not self._stop.wait(HEARTBEAT_S):
            self.flush()

    def flush(self):
        self.state["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        try:
            with open(self.path, "w") as f:
                json.dump(self.state, f, indent=1)
        except OSError:
            pass

    def close(self):
        self._stop.set()
        self.flush()


def ring_cps(start, n_seeds, out_dir):
    """环 7 实现：Tier C CPS 全程序变换 4-way 自检（§5.2）。

    进程内复用 cps.check_program（与 cps.py --corpus 主循环同语义：
    A/G/R/GC 四方比对；unsupported / anchor-diverge / infra 按
    cps.EXCLUDED_VERDICTS 出分母），finding 三件套经 cps._write_finding
    落盘 <out_dir>/findings/。不占滚动 counter 槽位，seed 由 seqdiff
    槽位派生（derive_seed v=37，与 tsan 的 31 错开）。
    """
    import gen
    runner = morph.Runner(
        tempfile.mkdtemp(prefix="kernfuzz-nightly-cps-"))
    try:
        counts = {}
        n_findings = 0
        for i in range(n_seeds):
            seed = start + i
            src_text = gen.gen_program(seed)
            verdict, detail = cps.check_program(runner, src_text,
                                                "p%d" % seed)
            counts[verdict] = counts.get(verdict, 0) + 1
            if verdict in cps.FINDING_VERDICTS:
                n_findings += 1
                cps._write_finding(
                    os.path.join(out_dir, "findings"), verdict,
                    "p%d" % seed, src_text, detail)
    finally:
        shutil.rmtree(runner.workdir, ignore_errors=True)
    consistent = counts.get("consistent", 0)
    judged = sum(v for k, v in counts.items()
                 if k not in cps.EXCLUDED_VERDICTS)
    return {"seeds": n_seeds, "consistent": consistent, "judged": judged,
            "unsupported": counts.get("unsupported", 0),
            "anchor-diverge": counts.get("anchor-diverge", 0),
            "findings": n_findings, "counts": counts}


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def cmd_run(args):
    missing = toolchain_missing()
    if missing:
        # §9 退出语义（与 fast 相反）: 慢速环不允许静默跳过 → exit 1
        print("KERNFUZZ-NIGHTLY-TOOLCHAIN-MISSING: %s" %
              ", ".join(missing))
        return 1
    # Tier C CPS 环默认开（gate 已过）；--no-cps 为显式逃生口。
    cps_on = not getattr(args, "no_cps", False)
    if cps_on and not CPS_GATE_PASSED:
        print("KERNFUZZ-NIGHTLY: Tier C CPS ring is enabled but the §5.2 "
              "gate has not passed — refusing to run (use --no-cps to "
              "exclude CPS explicitly).")
        return 1

    scale = args.scale
    plan = args.plan
    os.makedirs(OUT_DIR, exist_ok=True)
    hb = Heartbeat(os.path.join(OUT_DIR, "progress.json"))

    date = time.strftime("%Y-%m-%d")
    counter = read_counter()
    total_needed = (plan["n_morph"] + plan["n_tc"] + 1
                    + plan["n_multiset"])
    block = rolling_seeds(fast.git_sha(), date, counter, total_needed)
    morph_seeds, tc_seeds, seqdiff_start, ms_seeds = partition_seeds(
        block, plan["n_morph"], plan["n_tc"], plan["n_multiset"])
    hb.state["seeds"] = {"date": date, "counter": counter,
                         "sha": fast.git_sha(), "block_size": total_needed}

    novel = {"novel": 0}
    ring_out = lambda name: os.path.join(OUT_DIR, name)  # noqa: E731
    n_rings = 7 if cps_on else 6
    report = {"date": date, "counter": counter, "scale": scale,
              "cps_ring": cps_on,
              "rings": {}, "started": hb.state["started"]}

    try:
        # ---- 环 1: golden 子集语料 ---------------------------------------
        hb.ring_start("golden-corpus")
        t0 = time.time()
        runner = morph.Runner(tempfile.mkdtemp(
            prefix="kernfuzz-nightly-golden-"))
        try:
            g = ring_golden_corpus(runner, ring_out("golden"),
                                   plan["n_corpus"], novel)
        finally:
            shutil.rmtree(runner.workdir, ignore_errors=True)
        g["elapsed"] = round(time.time() - t0, 1)
        report["rings"]["golden-corpus"] = g
        print(f"== ring 1/{n_rings} golden corpus: snapshots=%d dump-cmp=%d "
              "anchor-cmp=%d tally={%s} skip=%d findings=%d elapsed=%.1fs"
              % (g["snapshots"], g["checks"]["dump-cmp"],
                 g["checks"]["anchor-cmp"],
                 ", ".join("%s=%d" % kv for kv in sorted(g["tally"].items())),
                 len(g["skips"]),
                 sum(v for k, v in g["tally"].items()
                     if k != "anchor-skip"), g["elapsed"]))
        hb.ring_done("golden-corpus")

        # ---- 环 2: morph 差分 (Tier A+B) ---------------------------------
        hb.ring_start("morph")
        t0 = time.time()
        m = ring_morph(morph_seeds, ring_out("morph"),
                       log=lambda msg: None)
        m["elapsed"] = round(time.time() - t0, 1)
        report["rings"]["morph"] = {k: m[k] for k in
                                    ("seeds", "ran", "dedup_hits",
                                     "elapsed")}
        report["rings"]["morph"]["findings"] = dict(m["findings"])
        report["rings"]["morph"]["n_skips"] = len(m["skips"])
        for c, n in m["findings"].items():
            novel["novel"] += n
        print(f"== ring 2/{n_rings} morph (Tier A+B): programs=%d ran=%d skip=%d "
              "dedup=%d findings=%d elapsed=%.1fs"
              % (m["seeds"], m["ran"], len(m["skips"]), m["dedup_hits"],
                 sum(m["findings"].values()), m["elapsed"]))
        hb.ring_done("morph")

        # ---- 环 3: typecheck 双向（现生成） ------------------------------
        hb.ring_start("tc-bidirectional")
        t0 = time.time()
        tc = ring_tc(tc_seeds, ring_out("tc"), log=lambda msg: None)
        tc["elapsed"] = round(time.time() - t0, 1)
        report["rings"]["tc-bidirectional"] = {
            "seeds": tc["seeds"], "positives": tc["positives"],
            "n_skips": len(tc["skips"]), "elapsed": tc["elapsed"],
            "findings": dict(tc["findings"]),
            "classes": tc["classes"]}
        for c, n in tc["findings"].items():
            novel["novel"] += n
        print(f"== ring 3/{n_rings} typecheck bidirectional: seeds=%d positives=%d "
              "skip=%d findings=%d elapsed=%.1fs"
              % (tc["seeds"], tc["positives"], len(tc["skips"]),
                 sum(tc["findings"].values()), tc["elapsed"]))
        hb.ring_done("tc-bidirectional")

        # ---- 环 4: GC 顺序差分 -------------------------------------------
        hb.ring_start("gc-seqdiff")
        t0 = time.time()
        sd = ring_seqdiff(seqdiff_start, plan["n_seqdiff"],
                                plan["seqdiff_stress"],
                                ring_out("seqdiff"))
        sd["elapsed"] = round(time.time() - t0, 1)
        counts = sd["counts"]
        novel["novel"] += (counts.get("mismatch", 0) + counts.get("crash", 0)
                           + counts.get("timeout", 0))
        report["rings"]["gc-seqdiff"] = sd
        print(f"== ring 4/{n_rings} gc seqdiff: window=%s stress_n=%s total=%d "
              "pass=%d mismatch=%d crash=%d timeout=%d dedup=%d "
              "new_findings=%d elapsed=%.1fs"
              % (sd["window"], plan["seqdiff_stress"], counts.get("total", 0),
                 counts.get("pass", 0), counts.get("mismatch", 0),
                 counts.get("crash", 0), counts.get("timeout", 0),
                 counts.get("dedup-skip", 0),
                 sd.get("new_findings", 0), sd["elapsed"]))
        hb.ring_done("gc-seqdiff")

        # ---- 环 5: 多重集 harness ----------------------------------------
        hb.ring_start("multiset")
        t0 = time.time()
        ms = ring_multiset(ms_seeds, plan["ks"], plan["ms"],
                           ring_out("multiset"))
        ms["elapsed"] = round(time.time() - t0, 1)
        novel["novel"] += ms["n_findings"]
        report["rings"]["multiset"] = {
            "n_cases": ms["n_cases"], "conserved": ms["conserved"],
            "n_findings": ms["n_findings"], "n_known": ms["n_known"],
            "ks": plan["ks"], "ms": plan["ms"], "elapsed": ms["elapsed"]}
        print(f"== ring 5/{n_rings} multiset: cases=%d conserved=%d findings=%d "
              "known=%d elapsed=%.1fs"
              % (ms["n_cases"], ms["conserved"], ms["n_findings"],
                 ms["n_known"], ms["elapsed"]))
        hb.ring_done("multiset")

        # ---- 环 6: TSan 长跑（W-chaos） -----------------------------------
        hb.ring_start("tsan")
        t0 = time.time()
        # tsan seed 用专属派生（不与 multiset 槽位复用同一 seed）
        tsan_seed = prng.derive_seed(seqdiff_start, 31)
        ts = ring_tsan(tsan_seed, plan["tsan_cap_s"], ring_out("tsan"),
                       novel, log=lambda msg: sys.stderr.write(
                           "nightly: %s\n" % msg))
        ts["elapsed"] = round(time.time() - t0, 1)
        report["rings"]["tsan"] = ts
        print(f"== ring 6/{n_rings} tsan long-run (W-chaos): status=%s races=%d "
              "cap=%.0fs elapsed=%.1fs"
              % (ts["status"], ts["races"], plan["tsan_cap_s"],
                 ts["elapsed"]))
        hb.ring_done("tsan")

        # ---- 环 7: Tier C CPS 4-way 自检（gate 全绿后默认开） ------------
        if cps_on:
            hb.ring_start("cps")
            t0 = time.time()
            cps_start = prng.derive_seed(seqdiff_start, 37)
            cp = ring_cps(cps_start, plan["n_cps"], ring_out("cps"))
            cp["elapsed"] = round(time.time() - t0, 1)
            novel["novel"] += cp["findings"]
            report["rings"]["cps"] = cp
            print("== ring 7/%d cps 4-way (Tier C): seeds=%d "
                  "consistent=%d/%d judged (unsupported=%d, "
                  "anchor-diverge=%d) findings=%d elapsed=%.1fs"
                  % (n_rings, cp["seeds"], cp["consistent"], cp["judged"],
                     cp["unsupported"], cp["anchor-diverge"],
                     cp["findings"], cp["elapsed"]))
            hb.ring_done("cps")
    finally:
        report["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        report["novel_findings"] = novel["novel"]
        with open(os.path.join(OUT_DIR, "report.json"), "w") as f:
            json.dump(report, f, indent=1, sort_keys=True, default=str)
        hb.close()

    # ---- 退出语义 ---------------------------------------------------------
    if novel["novel"]:
        print("KERNFUZZ-NIGHTLY: %d novel finding(s) — evidence under "
              "build/kernfuzz/nightly/<ring>/ + report.json"
              % novel["novel"])
        return 1
    # 全绿才推进 nightly counter（失败保留同批 seed 便于复现）
    write_counter(read_counter() + total_needed)
    print("KERNFUZZ-NIGHTLY: all rings green (%d/%d, scale=%s)"
          % (n_rings, n_rings, scale))
    return 0


def ring_seqdiff(seqdiff_start, n_seeds, stress_n, out_dir):
    """环 4 实现：进程内复用 gc_seqdiff 的 CLI（--json 捕获汇总）。

    用 redirect_stdout 捕获 gc_seqdiff.main 打印的 JSON 汇总，避免任何
    主循环重写（本任务=编排不重写）。
    """
    import io
    from contextlib import redirect_stdout
    window = "%d:%d" % (seqdiff_start, seqdiff_start + n_seeds)
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = gc_seqdiff.main(["--seeds", window, "--stress-n", stress_n,
                              "--out", out_dir, "--json"])
    try:
        summary = json.loads(buf.getvalue())
    except ValueError:
        summary = {}
    counts = dict(summary.get("counts", {}))
    counts["total"] = summary.get("total", 0)
    return {"rc": rc, "window": window, "stress_n": stress_n,
            "counts": counts,
            "new_findings": len(summary.get("new_findings", [])),
            "summary": summary}


# ---------------------------------------------------------------------------
# rolling-seeds 子命令（M-7 可复现性验证入口，纯函数）
# ---------------------------------------------------------------------------

def cmd_rolling_seeds(args):
    sha = args.git_sha if args.git_sha else fast.git_sha()
    for s in rolling_seeds(sha, args.date, args.counter, args.count):
        print(s)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="kernfuzz nightly ring (kernel-fuzzing §9, DELIV-10)")
    ap.add_argument("--scale", type=float, default=float(
        os.environ.get("KERNFUZZ_NIGHTLY_SCALE", "1.0")),
        help="scale factor for all ring sizes (default 1.0 = full)")
    ap.add_argument("--no-cps", action="store_true",
                    help="exclude the Tier C CPS ring (escape hatch; CPS "
                         "is on by default since the §5.2 gate passed)")
    ap.add_argument("--with-cps", action="store_true",
                    help="(legacy, no-op) the CPS ring is default-on "
                         "since the §5.2 gate passed")
    ap.add_argument("--multiset-k", default=",".join(str(k)
                                                     for k in MULTISET_KS),
                    help="multiset K matrix (comma list, default %(default)s)")
    ap.add_argument("--multiset-m", default=",".join(str(m)
                                                     for m in MULTISET_MS),
                    help="multiset M matrix (comma list, default %(default)s)")
    ap.add_argument("--seqdiff-stress", default=SEQDIFF_STRESS,
                    help="gc_seqdiff stress level(s) (default %(default)s)")
    sub = ap.add_subparsers(dest="cmd")
    sp_rs = sub.add_parser(
        "rolling-seeds",
        help="print M-7 rolling seeds (pure; reproducibility check)")
    sp_rs.add_argument("--date", required=True, help="YYYY-MM-DD")
    sp_rs.add_argument("--counter", type=int, required=True)
    sp_rs.add_argument("--count", type=int, default=10)
    sp_rs.add_argument("--git-sha", default=None)
    args = ap.parse_args(argv)
    if args.cmd == "rolling-seeds":
        return cmd_rolling_seeds(args)
    args.plan = compute_plan(args.scale)
    args.plan["ks"] = [int(x) for x in args.multiset_k.split(",")]
    args.plan["ms"] = [int(x) for x in args.multiset_m.split(",")]
    args.plan["seqdiff_stress"] = args.seqdiff_stress
    return cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())