# -*- coding: utf-8 -*-
"""
test_morph.py — tests for tools/kernfuzz/morph.py (§5.4 runner).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_morph.py
Exit 0 = all pass, non-zero = failure.

Coverage per .pge/tasks/task-runner.md:
  * consistency guard: a program whose two runs disagree (known
    heterogenous-list OP_ADD nondeterminism VM bug) is SKIPPED with an
    explicit note, never reported as a metamorphic mismatch,
  * unit: signature dedup (same source + same category lands once),
  * unit: failure-classification routing (build-fail / timeout=hang /
    divzero protocol / ASan exit 42 each verified on its own case),
  * findings directory contract completeness (sources, seed, transform
    paths, stdout/stderr/exit per program, golden output, run.sh, meta),
  * star topology: E₀-vs-variant divergence is caught as mismatch
    (verified by injection — the assertion really bites),
  * smoke: small real batch (20 seeds) through the full pipeline,
    exit 0, skip rate < 20%,
  * determinism: same CLI args twice → identical findings signature
    sets and identical skip logs,
  * anchor assertion effectiveness: a deliberately broken tavm base
    (wrong output) is caught as anchor-crash,
  * missing ASan base → clear error with the ASAN=1 make tavm hint.
"""

import json
import os
import shutil
import stat
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import morph                                # noqa: E402
import prng                                 # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def _write(path, text):
    with open(path, "w", encoding="latin-1") as f:
        f.write(text)
    return path


class NormProtocolTest(unittest.TestCase):
    """§5.1.3 wrappers are the test_gen implementations (reused, single
    definition point) — pin the protocol behavior here anyway."""

    def test_norm_tavm_synthesizes_divzero_on_exit1(self):
        lines = morph.norm_tavm(b"a\nb\n", 1)
        self.assertEqual(lines, [b"a", b"b", b"DIVZERO:2"])

    def test_norm_tavm_clean_exit_no_protocol_line(self):
        self.assertEqual(morph.norm_tavm(b"a\n\n", 0), [b"a"])

    def test_norm_golden_keeps_goldens_own_protocol_line(self):
        self.assertEqual(morph.norm_golden(b"a\nDIVZERO:1\n"), [b"a",
                                                                b"DIVZERO:1"])


class SignatureDedupTest(unittest.TestCase):
    """Same (category, source) → exactly one findings dir, ever."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="morph-dedup-")
        self.dedup = set()
        self.programs = [{"tag": "E0",
                          "src_text": "fn main() {\n  print(1)\n}",
                          "res": morph.RunResult(b"1\n", b"", 0, False),
                          "build_err": b""}]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _record(self, category="mismatch"):
        return morph.record_finding(
            self.tmp, category, "fn main() {\n  print(1)\n}",
            seed=1, effective_seed=1, attempts=1, variants_meta=[],
            programs=self.programs, anchor={}, dedup=self.dedup)

    def test_same_signature_recorded_once(self):
        self.assertTrue(self._record())
        self.assertFalse(self._record())
        dirs = [n for n in os.listdir(self.tmp) if n != "skips.log"]
        self.assertEqual(len(dirs), 1)

    def test_different_category_same_source_is_separate(self):
        self.assertTrue(self._record("mismatch"))
        self.assertTrue(self._record("hang"))
        dirs = sorted(n for n in os.listdir(self.tmp) if n != "skips.log")
        self.assertEqual(len(dirs), 2)
        self.assertTrue(dirs[0].startswith("hang-"))
        self.assertTrue(dirs[1].startswith("mismatch-"))

    def test_signature_format(self):
        self.assertTrue(self._record())
        with open(os.path.join(self.tmp, sorted(os.listdir(self.tmp))[0],
                               "meta.json")) as f:
            meta = json.load(f)
        # (类别, sha256 前 16 hex) 二元组字符串
        self.assertRegex(meta["signature"], r"^[a-z-]+:[0-9a-f]{16}$")

    def test_known_signatures_reload_from_out_dir(self):
        self.assertTrue(self._record())
        known = morph.load_known_signatures(self.tmp)
        self.assertIn("mismatch:" + morph.strip_ws_sha16(
            "fn main() {\n  print(1)\n}"), known)


class FindingsContractTest(unittest.TestCase):
    """落盘契约: sources, seed, transform paths, per-program
    stdout/stderr/exit, golden output, ASan report (if any), run.sh."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="morph-contract-")
        self.dedup = set()
        res = morph.RunResult(b"1\n", b"", 0, False)
        self.programs = [{"tag": "E0",
                          "src_text": "fn main() {\n  print(1)\n}",
                          "res": res, "build_err": b""}]
        for k in (1, 2, 3):
            self.programs.append(
                {"tag": "k%d" % k, "src_text": "fn main() {\n  print(%d)\n}"
                 % k, "res": res, "build_err": b""})
        self.variants_meta = [
            {"variant": k, "rule": "T1", "direction": "commut",
             "path": [3], "before": "a+b", "after": "b+a"}
            for k in (1, 2, 3)]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_contract_complete(self):
        self.assertTrue(morph.record_finding(
            self.tmp, "mismatch", "fn main() {\n  print(1)\n}",
            seed=42, effective_seed=42, attempts=1,
            variants_meta=self.variants_meta, programs=self.programs,
            anchor={"golden_stdout": b"GOLDEN-OUT\n", "dump_stdout":
                    b"(begin)", "dump_err": b""},
            dedup=self.dedup))
        fdir = os.path.join(self.tmp, sorted(
            n for n in os.listdir(self.tmp) if n != "skips.log")[0])
        for name in ("src_E0.ta", "src_k1.ta", "src_k2.ta", "src_k3.ta",
                     "stdout_E0.txt", "stderr_E0.txt", "exit_E0.txt",
                     "stdout_k1.txt", "stderr_k1.txt", "exit_k1.txt",
                     "stdout_k3.txt", "exit_k3.txt",
                     "golden.txt", "dump.sexp", "meta.json", "run.sh"):
            self.assertTrue(os.path.exists(os.path.join(fdir, name)),
                            "missing %s" % name)
        # run.sh must be executable and reference the pinned ASan call
        run_sh = os.path.join(fdir, "run.sh")
        self.assertTrue(os.stat(run_sh).st_mode & stat.S_IXUSR)
        with open(run_sh, encoding="latin-1") as f:
            body = f.read()
        self.assertIn("ASAN_OPTIONS=exitcode=42", body)
        self.assertIn("./tavm_asan", body)
        # meta carries seed + transform paths
        with open(os.path.join(fdir, "meta.json"), encoding="latin-1") as f:
            meta = json.load(f)
        self.assertEqual(meta["seed"], 42)
        self.assertEqual(len(meta["variants"]), 3)
        self.assertEqual(meta["variants"][0]["rule"], "T1")
        self.assertEqual(meta["category"], "mismatch")
        # asan.txt only when there is an ASan report
        self.assertFalse(os.path.exists(os.path.join(fdir, "asan.txt")))

    def test_asan_report_attached_on_exit42(self):
        res = morph.RunResult(b"", b"ERROR: AddressSanitizer: SEGV\n",
                              morph.ASAN_EXIT, False)
        progs = [{"tag": "E0", "src_text": "fn main() {\n}",
                  "res": res, "build_err": b""}]
        self.assertTrue(morph.record_finding(
            self.tmp, "tavm-crash", "fn main() {\n}", seed=1,
            effective_seed=1, attempts=1, variants_meta=[], programs=progs,
            anchor={}, dedup=self.dedup))
        fdir = os.path.join(self.tmp, sorted(
            n for n in os.listdir(self.tmp) if n != "skips.log")[0])
        with open(os.path.join(fdir, "asan.txt")) as f:
            self.assertIn("AddressSanitizer", f.read())


class ClassificationRoutingTest(unittest.TestCase):
    """失败分类路由：build-fail / 超时(hang) / divzero / ASan exit 42
    各一例定向验证（真实工具链 + 注入）。"""

    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.mkdtemp(prefix="morph-route-")
        cls.runner = morph.Runner(cls.workdir, timeout=2.0)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.workdir, ignore_errors=True)

    def test_build_fail_routed(self):
        # syntax error → build exit != 0, run never entered
        res, _paths, bp = self.runner.build_and_run(
            "fn main() {\n  print(\n}", "bf")
        self.assertNotEqual(bp.rc, 0)
        self.assertEqual(res.out, b"")
        self.assertEqual(res.rc, bp.rc)
        self.assertIsNone(morph.classify_run(res))  # not a run death at all

    def test_timeout_routed_to_hang(self):
        # infinite tail self-recursion: typechecks, never terminates
        res, _paths, bp = self.runner.build_and_run(
            "fn loop() {\n  loop()\n}\nfn main() {\n  loop()\n}", "hg")
        self.assertEqual(bp.rc, 0)
        self.assertTrue(res.timed_out)
        self.assertEqual(morph.classify_run(res), "hang")

    def test_divzero_is_protocol_exit1_not_crash(self):
        # death protocol: exit 1 → DIVZERO line synthesized by norm_tavm;
        # gen never emits division, so this is the unexpected-divzero
        # category at the seed level (checked via classify, not norm).
        res, _paths, bp = self.runner.build_and_run(
            "fn main() {\n  print(1 % 0)\n}", "dz")
        self.assertEqual(bp.rc, 0)
        self.assertEqual(res.rc, 1)
        self.assertEqual(morph.norm_tavm(res.out, res.rc)[-1],
                         b"DIVZERO:0")

    def test_asan_exit42_routed_to_tavm_crash(self):
        res = morph.RunResult(b"", b"AddressSanitizer: heap-buffer-overflow",
                              morph.ASAN_EXIT, False)
        self.assertEqual(morph.classify_run(res), "tavm-crash")

    def test_signal_death_routed_to_tavm_crash(self):
        res = morph.RunResult(b"", b"", -11, False)
        self.assertEqual(morph.classify_run(res), "tavm-crash")


class _FakeBuild(object):
    rc = 0
    out = b""
    err = b""


class _FakeRunner(object):
    """Duck-typed Runner for guard/star-topology tests.  E₀ outputs
    `e0_out`; variants `variant_out`; the mandatory second E₀ run (the
    consistency guard re-run) outputs `guard_out`."""

    def __init__(self, e0_out=b"1\n", variant_out=b"1\n",
                 guard_out=None):
        self.workdir = tempfile.mkdtemp(prefix="morph-fake-")
        self.e0_out = e0_out
        self.variant_out = variant_out
        self.guard_out = guard_out
        self.n_runs = 0

    def build_and_run(self, src_text, tag):
        self.n_runs += 1
        if tag == "E0_guard":
            out = self.guard_out if self.guard_out is not None \
                else self.e0_out
        elif tag == "E0":
            out = self.e0_out
        else:
            out = self.variant_out
        return morph.RunResult(out, b"", 0, False), (None, None), \
            _FakeBuild()

    def dump(self, src_path):
        return morph.RunResult(b"(begin (print 1))\n", b"", 0, False)

    def golden_eval(self, sexp_path):
        # golden agrees with E0 (norm-wise) -- anchor passes
        return morph.RunResult(self.e0_out, b"", 0, False)

    def cleanup(self):
        shutil.rmtree(self.workdir, ignore_errors=True)


class ConsistencyGuardTest(unittest.TestCase):
    """P 跑两遍不一致（已知异质 list VM bug 非确定性）→ 记 skip 注明。"""

    def tearDown(self):
        shutil.rmtree(self.out, ignore_errors=True)
        self.runner.cleanup()

    def test_nondet_run_skipped_with_note(self):
        self.out = tempfile.mkdtemp(prefix="morph-guard-")
        self.runner = _FakeRunner(e0_out=b"1\n", guard_out=b"2\n")
        stats = morph.fuzz_batch(self.runner, [12345], self.out)
        self.assertEqual(stats["ran"], 0)
        self.assertEqual(len(stats["skips"]), 1)
        seed, reason = stats["skips"][0]
        self.assertEqual(seed, 12345)
        self.assertIn("nondeterministic", reason)
        self.assertIn("heterogenous-list", reason)
        # skip is NOT a finding
        self.assertEqual(sum(stats["findings"].values()), 0)
        with open(os.path.join(self.out, "skips.log")) as f:
            self.assertIn("seed=12345", f.read())

    def test_consistent_passes(self):
        self.out = tempfile.mkdtemp(prefix="morph-guard2-")
        self.runner = _FakeRunner()
        stats = morph.fuzz_batch(self.runner, [12345], self.out)
        self.assertEqual(stats["ran"], 1)
        self.assertEqual(stats["skips"], [])


class StarTopologyTest(unittest.TestCase):
    """星形拓扑：E₀ 与各变体逐一比；注入坏变体输出 → mismatch 必被抓。"""

    def setUp(self):
        self.out = tempfile.mkdtemp(prefix="morph-star-")

    def tearDown(self):
        shutil.rmtree(self.out, ignore_errors=True)
        self.runner.cleanup()

    def test_variant_divergence_is_mismatch_finding(self):
        self.runner = _FakeRunner(e0_out=b"1\n", variant_out=b"2\n")
        stats = morph.fuzz_batch(self.runner, [777], self.out)
        self.assertEqual(stats["findings"]["mismatch"], 1)
        fdirs = [n for n in os.listdir(self.out) if n != "skips.log"]
        self.assertEqual(len(fdirs), 1)
        self.assertTrue(fdirs[0].startswith("mismatch-"))

    def test_agreement_is_ok(self):
        self.runner = _FakeRunner()
        stats = morph.fuzz_batch(self.runner, [778], self.out)
        self.assertEqual(stats["findings"]["mismatch"], 0)
        self.assertEqual(stats["ran"], 1)


class AnchorBitesTest(unittest.TestCase):
    """故意坏 tavm 底座（固定输出错误行）→ 锚点断言必报 anchor-crash。
    证明断言真的会抓，而不是永远绿灯。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="morph-anchor-")
        # fake ASan base: always prints a WRONG fixed line, exit 0
        self.fake_tavm = _write(os.path.join(self.tmp, "fake_tavm.sh"),
                                "#!/bin/sh\necho WRONG-OUTPUT\n")
        os.chmod(self.fake_tavm, 0o755)
        self.workdir = tempfile.mkdtemp(prefix="morph-anchor-wd-")
        self.out = tempfile.mkdtemp(prefix="morph-anchor-out-")
        self.runner = morph.Runner(self.workdir,
                                   tavm_asan=self.fake_tavm,
                                   timeout=10.0)

    def tearDown(self):
        for d in (self.tmp, self.workdir, self.out):
            shutil.rmtree(d, ignore_errors=True)

    def test_broken_base_caught_as_anchor_crash(self):
        stats = morph.fuzz_batch(self.runner, [999], self.out)
        self.assertEqual(stats["findings"]["anchor-crash"], 1)
        fdirs = [n for n in os.listdir(self.out) if n != "skips.log"]
        self.assertEqual(len(fdirs), 1)
        fdir = os.path.join(self.out, fdirs[0])
        # golden output + dump kept per contract; the wrong tavm line is
        # in the recorded stdout
        self.assertTrue(os.path.exists(os.path.join(fdir, "golden.txt")))
        self.assertTrue(os.path.exists(os.path.join(fdir, "dump.sexp")))
        with open(os.path.join(fdir, "stdout_E0.txt")) as f:
            self.assertIn("WRONG-OUTPUT", f.read())


class MissingBaseErrorTest(unittest.TestCase):
    """ASan 底座缺失 → 清晰报错 + 构建提示。"""

    def test_missing_tavm_asan_gives_build_hint(self):
        with self.assertRaises(morph.MorphError) as ctx:
            morph.Runner(tempfile.mkdtemp(), tavm_asan="/nonexistent/tavm")
        self.assertIn("ASAN=1 make tavm", str(ctx.exception))

    def test_missing_ast_dump(self):
        with self.assertRaises(morph.MorphError) as ctx:
            morph.Runner(tempfile.mkdtemp(), ast_dump="/nonexistent/dump")
        self.assertIn("missing toolchain", str(ctx.exception))


class CliTest(unittest.TestCase):
    """CLI: --seeds parsing, --count slicing."""

    def test_parse_seeds_single_and_range(self):
        self.assertEqual(morph.parse_seeds("5"), [5])
        self.assertEqual(morph.parse_seeds("3..6"), [3, 4, 5, 6])
        with self.assertRaises(morph.MorphError):
            morph.parse_seeds("6..3")

    def test_missing_arg_exits_nonzero(self):
        import subprocess
        p = subprocess.run(
            [sys.executable, os.path.join(_HERE, "morph.py")],
            capture_output=True)
        self.assertNotEqual(p.returncode, 0)


class SmokeRealPipelineTest(unittest.TestCase):
    """冒烟：20 seed 真跑全管线 exit 0；skip 率 < 20%。"""

    def test_20_seed_real_batch(self):
        out = tempfile.mkdtemp(prefix="morph-smoke-")
        try:
            rc = morph.main(["--seeds", "910000..910019", "--out", out])
            self.assertEqual(rc, 0)
            # nothing crashed the pipeline; skips stay rare
            skips = 0
            log = os.path.join(out, "skips.log")
            if os.path.exists(log):
                with open(log) as f:
                    skips = len(f.readlines())
            self.assertLess(skips, 20 * 0.2, "skip rate >= 20%%: %d" % skips)
        finally:
            shutil.rmtree(out, ignore_errors=True)


class DeterminismTest(unittest.TestCase):
    """同参数两次跑：findings 签名集合 + skip log 完全一致。"""

    def test_same_args_same_findings(self):
        outs = []
        for i in range(2):
            out = tempfile.mkdtemp(prefix="morph-det%d-" % i)
            outs.append(out)
            rc = morph.main(["--seeds", "920000..920004", "--out", out])
            self.assertEqual(rc, 0)
        try:
            sig_sets = []
            skip_logs = []
            for out in outs:
                sigs = sorted(n for n in os.listdir(out)
                              if n not in ("skips.log",))
                sig_sets.append(sigs)
                with open(os.path.join(out, "skips.log")) as f:
                    skip_logs.append(f.read())
            self.assertEqual(sig_sets[0], sig_sets[1])
            self.assertEqual(skip_logs[0], skip_logs[1])
        finally:
            for out in outs:
                shutil.rmtree(out, ignore_errors=True)


class PrngDisciplineTest(unittest.TestCase):
    """PRNG 约定（M-2）：morph 全程只经 prng.py 取随机，禁宿主 random。"""

    def test_morph_does_not_import_host_random(self):
        src = open(os.path.join(_HERE, "morph.py"),
                   encoding="latin-1").read()
        self.assertNotIn("import random", src)
        self.assertNotIn("from random", src)
        self.assertNotIn("random.", src.replace("prng.", ""))

    def test_variant_rng_streams_deterministic(self):
        # the transform picks for a seed are a pure function of the seed
        def picks(seed):
            tree = gen_tree = __import__("gen").build_program(seed)
            out = []
            for k in (1, 2, 3):
                rng = prng.make_prng(prng.derive_seed(seed, 1000 + k))
                _t, info = __import__("transforms").apply_one(tree, rng)
                out.append(None if info is None
                           else (info["rule"], info["direction"],
                                 tuple(info["path"] or ())))
            return out
        self.assertEqual(picks(31337), picks(31337))
        # different seeds pick differently (pool is non-degenerate)
        allp = [picks(s) for s in range(50)]
        self.assertGreater(len(set(map(str, allp))), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)