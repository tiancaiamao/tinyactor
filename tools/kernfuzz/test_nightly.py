#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_nightly.py — unit tests for the nightly ring orchestrator.

Covers (task-nightly 验收 #7): composition orchestration logic (plan
computation + seed block partitioning + snapshot→source mapping), M-7
rolling-seed reproducibility + counter-file independence from fast,
--with-cps gate semantics, and exit semantics (toolchain missing → 1,
finding → 1, all green → 0 with counter advance).  All re-executing
parts (the actual rings) are stubbed out — no subprocess, no toolchain.

Run:  python3 -m unittest test_nightly -v
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import nightly                                 # noqa: E402
import fast                                    # noqa: E402


def _args(scale=1.0, with_cps=False, no_cps=False):
    """A cmd_run-ready Namespace mirroring what main() builds."""
    a = argparse.Namespace(scale=scale, with_cps=with_cps, no_cps=no_cps,
                           cmd=None)
    a.plan = nightly.compute_plan(scale)
    return a


class TestRollingSeeds(unittest.TestCase):
    """M-7: same formula as fast, independent counter file."""

    def test_roll_seed_matches_m7_formula(self):
        # nightly.roll_seed must be byte-for-byte the fast/M-7 formula
        self.assertEqual(nightly.roll_seed("abc123", "2026-09-04", 0),
                         fast.roll_seed("abc123", "2026-09-04", 0))
        # determinism + monotone-ish spread across counters
        s0 = nightly.roll_seed("abc123", "2026-09-04", 0)
        s1 = nightly.roll_seed("abc123", "2026-09-04", 1)
        self.assertEqual(s0, nightly.roll_seed("abc123", "2026-09-04", 0))
        self.assertNotEqual(s0, s1)
        for s in (s0, s1):
            self.assertTrue(0 <= s < 140737488355327)   # int48 正值域

    def test_counter_file_independent_from_fast(self):
        # 验收 #4: nightly counter 与 fast counter 独立（不同文件）
        self.assertNotEqual(nightly.COUNTER_FILE, fast.COUNTER_FILE)
        self.assertTrue(nightly.COUNTER_FILE.endswith(
            "rolling-counter-nightly"))
        self.assertTrue(fast.COUNTER_FILE.endswith("rolling-counter"))
        self.assertTrue(os.path.dirname(nightly.COUNTER_FILE) ==
                        os.path.dirname(fast.COUNTER_FILE))

    def test_counter_roundtrip_and_default(self):
        tmp = tempfile.mkdtemp(prefix="nightly-test-counter-")
        old = nightly.COUNTER_FILE
        try:
            nightly.COUNTER_FILE = os.path.join(tmp, "rolling-counter-n")
            self.assertEqual(nightly.read_counter(), 0)   # 缺席从 0 起
            nightly.write_counter(41)
            self.assertEqual(nightly.read_counter(), 41)
        finally:
            nightly.COUNTER_FILE = old
            shutil.rmtree(tmp, ignore_errors=True)

    def test_seed_block_reproducible(self):
        a = nightly.rolling_seeds("sha", "2026-09-04", 7, 10)
        b = nightly.rolling_seeds("sha", "2026-09-04", 7, 10)
        c = nightly.rolling_seeds("sha", "2026-09-04", 8, 10)
        self.assertEqual(a, b)
        self.assertNotEqual(a[0], c[0])


class TestPlanAndPartition(unittest.TestCase):
    """--scale 计算 + seed 块按环切分（纯函数）。"""

    def test_plan_full_scale_matches_design_constants(self):
        p = nightly.compute_plan(1.0)
        self.assertEqual(p["n_morph"], 1000)     # §9: 1000 生成程序
        self.assertEqual(p["n_tc"], 2000)        # §9: typecheck 双向 2000 例
        self.assertEqual(p["n_seqdiff"], 100)    # 窗口 100 seed
        self.assertEqual(p["seqdiff_stress"], "1")  # N=1
        self.assertEqual(p["ks"], [4, 16])       # 矩阵含 K=16
        self.assertEqual(p["ms"], [25, 100])     # 矩阵含 M=100
        self.assertEqual(p["tsan_cap_s"], 1800.0)   # §7.4 30min 上限

    def test_plan_scales_down(self):
        p = nightly.compute_plan(0.1)
        self.assertEqual(p["n_morph"], 100)
        self.assertEqual(p["n_tc"], 200)
        self.assertEqual(p["n_seqdiff"], 10)
        self.assertEqual(p["n_multiset"], 2)
        self.assertAlmostEqual(p["tsan_cap_s"], 180.0)
        # tsan cap 下限 60s
        self.assertEqual(nightly.compute_plan(0.01)["tsan_cap_s"], 60.0)
        # 非法 scale
        with self.assertRaises(ValueError):
            nightly.compute_plan(0.0)

    def test_partition_disjoint_and_complete(self):
        seeds = list(range(100, 100 + 1000 + 2000 + 1 + 20))
        m, t, sd, ms = nightly.partition_seeds(seeds, 1000, 2000, 20)
        self.assertEqual(len(m), 1000)
        self.assertEqual(len(t), 2000)
        self.assertEqual(len(ms), 20)
        self.assertEqual(m + t + ms, [s for s in seeds if s != sd])
        # seqdiff 窗口起点来自块的专属槽位
        self.assertEqual(sd, seeds[3000])
        # 确定性：同块必同切分
        m2, t2, sd2, ms2 = nightly.partition_seeds(seeds, 1000, 2000, 20)
        self.assertEqual((m, t, sd, ms), (m2, t2, sd2, ms2))


class TestSnapshotMapping(unittest.TestCase):
    """golden 子集语料：冻结快照名 → 源码路径。"""

    def test_mapping_basic_and_compiler(self):
        self.assertEqual(
            nightly.snapshot_source_path("/x/basic-closure.sexp"),
            os.path.join(nightly.REPO_ROOT, "test", "basic", "closure.ta"))
        self.assertEqual(
            nightly.snapshot_source_path("/x/compiler-parser-ast.sexp"),
            os.path.join(nightly.REPO_ROOT, "test", "compiler",
                         "parser-ast.ta"))
        # 多 '-' 名字只切第一段
        self.assertEqual(
            nightly.snapshot_source_path("/x/basic-tail-call-deep.sexp"),
            os.path.join(nightly.REPO_ROOT, "test", "basic",
                         "tail-call-deep.ta"))

    def test_mapping_missing_source_is_none(self):
        self.assertIsNone(
            nightly.snapshot_source_path("/x/basic-no-such-file.sexp"))
        self.assertIsNone(nightly.snapshot_source_path("/x/not-a-snap.txt"))

    def test_frozen_corpus_present(self):
        snaps = nightly.frozen_snapshots()
        self.assertTrue(len(snaps) >= 40, "frozen corpus missing?")
        for s in snaps:
            self.assertTrue(os.path.basename(s).endswith(".sexp"))


class _StubRing(object):
    """Reusable stub: records calls, returns configurable summary."""

    def __init__(self, ret, novel=0):
        self.ret = ret
        self.novel = novel
        self.calls = []

    def __call__(self, *a, **kw):
        self.calls.append(a)
        if self.novel:
            # mimic the novel-counter out-param contract (tsan ring)
            counter = a[-1]
            if isinstance(counter, dict) and "novel" in counter:
                counter["novel"] += self.novel
        return self.ret


class TestExitSemantics(unittest.TestCase):
    """§9 退出语义：工具链缺席=1 / finding=1 / 全绿=0（与 fast 相反）。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nightly-test-run-")
        self._saved = {k: getattr(nightly, k) for k in
                       ("OUT_DIR", "COUNTER_FILE", "toolchain_missing",
                        "ring_morph", "ring_tc", "ring_seqdiff",
                        "ring_multiset", "ring_tsan", "ring_golden_corpus",
                        "ring_cps", "CPS_GATE_PASSED",
                        "compute_plan", "rolling_seeds")}
        nightly.OUT_DIR = os.path.join(self.tmp, "out")
        nightly.COUNTER_FILE = os.path.join(self.tmp, "counter")

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(nightly, k, v)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _stub_green_rings(self):
        nightly.ring_golden_corpus = _StubRing(
            {"snapshots": 3, "checks": {"dump-cmp": 3, "anchor-cmp": 3},
             "tally": {}, "skips": []})
        nightly.ring_morph = _StubRing(
            {"seeds": 5, "ran": 5, "skips": [], "dedup_hits": 0,
             "findings": {"mismatch": 0}})
        nightly.ring_tc = _StubRing(
            {"seeds": 5, "positives": 5, "skips": [],
             "findings": {"sound-fail": 0}, "classes": {}})
        nightly.ring_seqdiff = _StubRing(
            {"rc": 0, "window": "0:5", "stress_n": "1",
             "counts": {"pass": 5, "total": 5}, "new_findings": 0,
             "summary": {"counts": {"pass": 5, "total": 5}}})
        nightly.ring_multiset = _StubRing(
            {"n_cases": 4, "conserved": 4, "n_findings": 0, "n_known": 0,
             "elapsed": 0.1})
        nightly.ring_tsan = _StubRing(
            {"status": "ok", "races": 0, "elapsed": 0.1})
        nightly.ring_cps = _StubRing(
            {"seeds": 2, "consistent": 2, "judged": 2, "unsupported": 0,
             "anchor-diverge": 0, "findings": 0, "counts": {}})

    def test_toolchain_missing_exit_1(self):
        # §9: nightly 工具链缺席 → exit 1（fast 是 exit 0——语义相反）
        nightly.toolchain_missing = lambda: ["/no/tinyactor"]
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)

    def test_cps_refused_while_gate_not_passed(self):
        # gate 常量被翻转为 True 后（task-cps-gate 第 2 轮），拒绝语义仍须
        # 保留：临时压回 False，CPS 环默认开 → 直接 exit 1，不进任何环
        nightly.toolchain_missing = lambda: []
        nightly.CPS_GATE_PASSED = False
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)

    def test_no_cps_excludes_cps_ring(self):
        # --no-cps 逃生口：不进 CPS 环，report.cps_ring=False
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        rc = nightly.cmd_run(_args(scale=0.01, no_cps=True))
        self.assertEqual(rc, 0)
        self.assertEqual(nightly.ring_cps.calls, [])
        with open(os.path.join(nightly.OUT_DIR, "report.json")) as f:
            self.assertFalse(json.load(f)["cps_ring"])

    def test_cps_finding_exit_1(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.ring_cps = _StubRing(
            {"seeds": 2, "consistent": 1, "judged": 2, "unsupported": 0,
             "anchor-diverge": 0, "findings": 1, "counts": {}})
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)

    def test_all_green_exit_0_and_counter_advances(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.write_counter(100)
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 0)
                # counter 推进 = 块大小（scale=0.01 → morph 10 + tc 20 + seqdiff
        # 槽位 1 + multiset 1 = 32；n_seqdiff=2 是窗口长度，从槽位起延伸，
        # 不占额外槽位）
        self.assertEqual(nightly.read_counter(), 100 + 32)
        # report.json 落盘，含全部 7 环（gate 全绿后 CPS 默认开）
        with open(os.path.join(nightly.OUT_DIR, "report.json")) as f:
            report = json.load(f)
        self.assertEqual(
            set(report["rings"]),
            {"golden-corpus", "morph", "tc-bidirectional", "gc-seqdiff",
             "multiset", "tsan", "cps"})
        self.assertTrue(report["cps_ring"])
        self.assertEqual(report["novel_findings"], 0)

    def test_morph_finding_exit_1_no_counter_advance(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.ring_morph = _StubRing(
            {"seeds": 5, "ran": 4, "skips": [], "dedup_hits": 0,
             "findings": {"mismatch": 1}})
        nightly.write_counter(100)
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)
        self.assertEqual(nightly.read_counter(), 100)   # 失败不推进
        with open(os.path.join(nightly.OUT_DIR, "report.json")) as f:
            self.assertEqual(json.load(f)["novel_findings"], 1)

    def test_seqdiff_finding_exit_1(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.ring_seqdiff = _StubRing(
            {"rc": 1, "window": "0:5", "stress_n": "1",
             "counts": {"pass": 4, "mismatch": 1, "total": 5},
             "new_findings": 1, "summary": {}})
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)

    def test_multiset_novel_finding_exit_1(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.ring_multiset = _StubRing(
            {"n_cases": 4, "conserved": 3, "n_findings": 1, "n_known": 0,
             "elapsed": 0.1})
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)

    def test_tsan_skip_is_not_a_failure(self):
        # M-13: 平台不可用 = 显式 SKIP 观测项，不算失败（否则静默跳过=1
        # 只针对工具链缺席；TSan SKIP 不改变全绿结论）
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        nightly.ring_tsan = _StubRing(
            {"status": "SKIP", "detail": "probe build failed", "races": 0})
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 0)

    def test_tsan_finding_exit_1(self):
        nightly.toolchain_missing = lambda: []
        self._stub_green_rings()
        # tsan ring 通过 novel-counter out-param 上报（最后一参为 dict）
        nightly.ring_tsan = _StubRing(
            {"status": "FAIL", "races": 1}, novel=1)
        rc = nightly.cmd_run(_args(scale=0.01))
        self.assertEqual(rc, 1)


class TestRecordFinding(unittest.TestCase):
    """nightly 本地 evidence 落盘：签名去重语义。"""

    def test_novel_then_dedup(self):
        tmp = tempfile.mkdtemp(prefix="nightly-test-rec-")
        try:
            dedup = set()
            counter = {"novel": 0}
            ok1 = nightly.record_nightly_finding(
                tmp, "golden", "anchor-mismatch", "prog A", {"x": 1},
                dedup, counter)
            ok2 = nightly.record_nightly_finding(
                tmp, "golden", "anchor-mismatch", "prog A", {"x": 2},
                dedup, counter)
            self.assertTrue(ok1)
            self.assertFalse(ok2)            # 同签名去重
            self.assertEqual(counter["novel"], 1)
            dirs = [d for d in os.listdir(tmp) if
                    d.startswith("golden.")]
            self.assertEqual(len(dirs), 1)
            with open(os.path.join(tmp, dirs[0], "finding.json")) as f:
                ev = json.load(f)
            self.assertEqual(ev["ring"], "golden")
            self.assertEqual(ev["category"], "anchor-mismatch")
            self.assertIn("src.ta", os.listdir(os.path.join(tmp, dirs[0])))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class TestCliSurface(unittest.TestCase):
    """验收 #5: CPS 环默认开（--no-cps 逃生口）；rolling-seeds 纯函数入口。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nightly-test-cli-")
        self._saved = {k: getattr(nightly, k) for k in
                       ("toolchain_missing", "ring_morph",
                        "ring_golden_corpus", "ring_tc", "ring_seqdiff",
                        "ring_multiset", "ring_tsan", "ring_cps",
                        "CPS_GATE_PASSED", "OUT_DIR", "COUNTER_FILE")}
        nightly.OUT_DIR = os.path.join(self.tmp, "out")
        nightly.COUNTER_FILE = os.path.join(self.tmp, "counter")

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(nightly, k, v)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_cps_refused_before_any_ring(self):
        # gate 压回 False：CPS 环默认开 → 在任何环执行前即拒绝
        nightly.toolchain_missing = lambda: []
        nightly.CPS_GATE_PASSED = False
        nightly.ring_morph = _StubRing(None)
        rc = nightly.main([])
        self.assertEqual(rc, 1)
        self.assertEqual(nightly.ring_morph.calls, [])   # 未进任何环

    def test_default_includes_cps_and_runs_green(self):
        # 默认（gate 已过）：Tier C CPS 环默认开，全绿走完
        nightly.toolchain_missing = lambda: []
        nightly.ring_golden_corpus = _StubRing(
            {"snapshots": 1, "checks": {"dump-cmp": 1, "anchor-cmp": 1},
             "tally": {}, "skips": []})
        nightly.ring_morph = _StubRing(
            {"seeds": 1, "ran": 1, "skips": [], "dedup_hits": 0,
             "findings": {"mismatch": 0}})
        nightly.ring_tc = _StubRing(
            {"seeds": 1, "positives": 1, "skips": [],
             "findings": {"sound-fail": 0}, "classes": {}})
        nightly.ring_seqdiff = _StubRing(
            {"rc": 0, "window": "0:2", "stress_n": "1",
             "counts": {"pass": 2, "total": 2}, "new_findings": 0,
             "summary": {}})
        nightly.ring_multiset = _StubRing(
            {"n_cases": 1, "conserved": 1, "n_findings": 0, "n_known": 0,
             "elapsed": 0.1})
        nightly.ring_tsan = _StubRing(
            {"status": "ok", "races": 0, "elapsed": 0.1})
        nightly.ring_cps = _StubRing(
            {"seeds": 3, "consistent": 3, "judged": 3, "unsupported": 0,
             "anchor-diverge": 0, "findings": 0, "counts": {}})
        rc = nightly.main([])
        self.assertEqual(rc, 0)
        self.assertTrue(nightly.CPS_GATE_PASSED)
        self.assertEqual(len(nightly.ring_cps.calls), 1)  # CPS 环确已执行

    def test_rolling_seeds_subcommand(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = nightly.main(["rolling-seeds", "--date", "2026-09-04",
                               "--counter", "3", "--count", "4",
                               "--git-sha", "deadbeefcafe"])
        self.assertEqual(rc, 0)
        seeds = [int(x) for x in buf.getvalue().split()]
        self.assertEqual(seeds, nightly.rolling_seeds(
            "deadbeefcafe", "2026-09-04", 3, 4))


@unittest.skipUnless(not nightly.toolchain_missing(),
                     "toolchain binaries not present")
class TestRingCpsLive(unittest.TestCase):
    """环 7 实现层 smoke：直接实跑 1 个 seed——stub 测不到的实现级
    缺陷（import 缺失、落盘路径错误）在此暴露。"""

    def test_ring_cps_one_seed_green(self):
        tmp = tempfile.mkdtemp(prefix="nightly-test-ringcps-")
        try:
            r = nightly.ring_cps(0, 1, tmp)   # seed 0: gate round-2 consistent
            self.assertEqual(r["seeds"], 1)
            self.assertEqual(r["consistent"], 1)
            self.assertEqual(r["judged"], 1)
            self.assertEqual(r["findings"], 0)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()