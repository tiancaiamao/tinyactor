# -*- coding: utf-8 -*-
"""
test_tc_meta.py — tests for tools/kernfuzz/tc_meta.py (§6.3 元性质).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_tc_meta.py
Exit 0 = all pass, non-zero = failure.

Coverage per .pge/tasks/task-tc-meta.md:
  * determinism directed positive: same file double build → diagnostics
    (stdout+stderr) byte-identical AND artifacts byte-identical,
  * order-independence directed positive: a shufflable gen program
    (≥2 top-level defs) reorders to a DIFFERENT source that still
    parses/builds and keeps the same accept/reject conclusion,
  * fmt idempotence directed positive: cp→fmt→double build on a real gen
    program → artifacts byte-equal (byte premise); structural downgrade
    path also covered (forced mode="structural", tavm behavioral cmp),
  * bite tests (assertions must be able to fail):
      - fake build emitting a timestamped diagnostic → meta-determinism,
      - fake build writing nondeterministic artifacts → premise downgrade
        + artifact finding,
      - fake order-dependent build (rejects the shuffled file only) →
        meta-order,
      - fake fmt that scrambles semantics → meta-fmt (conclusion flip in
        byte mode, structural mismatch in structural mode),
  * shuffle effectiveness: shuffled source differs and still parses
    (anti-green-light for "didn't actually shuffle"),
  * smoke: 30 real seeds, exit 0, all three properties counted,
    zero findings expected,
  * determinism of the tool: same args twice → identical stats (minus
    elapsed) and identical findings signature sets,
  * premise downgrade recording: reason string surfaced in stats.

Toolchain-dependent tests skip cleanly when ./tinyactor (or ./tavm_asan
for the structural path) is absent — same convention as test_morph.py.
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
import morph                                  # noqa: E402
import tc_meta as tcm                         # noqa: E402


def _rr(out=b"", err=b"", rc=0, timed_out=False):
    return morph.RunResult(out, err, rc, timed_out)


REJECT = (b"typecheck: 1 type error(s) found\n"
          b"compile aborted: 1 type error(s)\n")


def _toolchain_ready():
    return os.path.exists(morph.TINYACTOR)


def _tavm_ready():
    return os.path.exists(morph.TAVM_ASAN)


def _shufflable_seed(start=0, limit=300):
    """First seed in [start, start+limit) whose program has ≥2 top-level
    defs (helper fns) — i.e. a real reorder site for property 2."""
    for s in range(start, start + limit):
        plan = gen.build_program(s)
        if len(plan.fns) >= 2:
            return s
    return None


class FakeRunner(object):
    """Scripted stand-in for tc_meta.MetaRunner (fully offline).

    build:  returns `build` (RunResult or f(call_index, src_path) ->
            RunResult) and writes `artifact` (bytes or
            f(call_index) -> bytes) to the artifact path so the
            byte-level cmp has files to read.
    fmt:    returns `fmt_result`, optionally rewriting the file via
            `fmt_fn(path)`.
    run_artifact: returns `run_out` (structural mode comparisons).
    """

    def __init__(self, build=None, artifact=b"fake-artifact",
                 fmt_result=None, fmt_fn=None, run_out=b"1\n"):
        self.workdir = tempfile.mkdtemp(prefix="tcmeta-fake-")
        self._build = build if build is not None else _rr()
        self._artifact = artifact
        self._fmt_result = fmt_result if fmt_result is not None else _rr()
        self._fmt_fn = fmt_fn
        self._run_out = run_out
        self._i = 0
        self.build_calls = []

    def build(self, src_path, artifact_path):
        i = self._i
        self._i += 1
        self.build_calls.append(src_path)
        res = (self._build(i, src_path) if callable(self._build)
               else self._build)
        art = (self._artifact(i) if callable(self._artifact)
               else self._artifact)
        with open(artifact_path, "wb") as f:
            f.write(art)
        return res

    def fmt(self, src_path):
        if self._fmt_fn is not None:
            self._fmt_fn(src_path)
        return self._fmt_result

    def run_artifact(self, artifact_path):
        return _rr(out=self._run_out)

    def cleanup(self):
        shutil.rmtree(self.workdir, ignore_errors=True)


class World(object):
    """run_seed plumbing (fresh out dir + dedup per test)."""

    def __init__(self, runner, mode="byte"):
        self.out = tempfile.mkdtemp(prefix="tcmeta-find-")
        self.dedup = set()
        self.skips = []
        self.findings = dict((c, 0) for c in tcm.FINDING_CATEGORIES)
        self.checks = {"determinism": 0, "order": 0, "fmt": 0,
                       "fmt-byte": 0, "fmt-structural": 0}
        self.runner = runner
        self.mode = mode

    def run(self, seed):
        return tcm.run_seed(self.runner, seed, self.mode, self.out,
                            self.dedup, self.skips, self.findings,
                            self.checks, log=lambda m: None)

    def sigs(self):
        s = set()
        for n in os.listdir(self.out):
            if n == "skips.log":
                continue
            with open(os.path.join(self.out, n, "meta.json")) as f:
                s.add(json.load(f)["signature"])
        return s

    def cleanup(self):
        shutil.rmtree(self.out, ignore_errors=True)
        if hasattr(self.runner, "cleanup"):
            self.runner.cleanup()
        elif os.path.isdir(self.runner.workdir):
            shutil.rmtree(self.runner.workdir, ignore_errors=True)


def _finding_dirs(out_dir):
    return [n for n in sorted(os.listdir(out_dir)) if n != "skips.log"]


# ---------------------------------------------------------------------------
# shuffle algorithm (property 2 mechanics, offline)
# ---------------------------------------------------------------------------

class ShuffleTreeTest(unittest.TestCase):
    def test_shuffle_changes_source_when_shufflable(self):
        seed = _shufflable_seed()
        self.assertIsNotNone(seed, "no shufflable seed found in 300")
        plan = gen.build_program(seed)
        src0 = gen.render_tree(plan)
        rng = prng.make_prng(prng.derive_seed(seed, tcm.DERIVE_ORDER))
        srcS = gen.render_tree(tcm.shuffle_tree(plan, rng))
        self.assertNotEqual(srcS, src0, "shuffle did not change the source")

    def test_shuffle_is_deterministic(self):
        seed = _shufflable_seed()
        outs = []
        for _ in (0, 1):
            plan = gen.build_program(seed)
            rng = prng.make_prng(prng.derive_seed(seed, tcm.DERIVE_ORDER))
            outs.append(gen.render_tree(tcm.shuffle_tree(plan, rng)))
        self.assertEqual(outs[0], outs[1])

    def test_no_shuffle_site_renders_identical(self):
        for s in range(500):
            plan = gen.build_program(s)
            if len(plan.fns) < 2 and len(plan.type_decls) < 2:
                src0 = gen.render_tree(plan)
                rng = prng.make_prng(prng.derive_seed(s, tcm.DERIVE_ORDER))
                self.assertEqual(
                    gen.render_tree(tcm.shuffle_tree(plan, rng)), src0)
                return
        self.fail("no degenerate program found in 500 seeds")


# ---------------------------------------------------------------------------
# codegen determinism premise probe (offline, fakes)
# ---------------------------------------------------------------------------

class PremiseProbeTest(unittest.TestCase):
    def test_deterministic_artifacts_hold_the_premise(self):
        runner = FakeRunner(artifact=b"same-bytes")
        try:
            ok, reason, mism = tcm.probe_codegen_determinism(runner, [1, 2])
            self.assertTrue(ok)
            self.assertEqual(reason, "")
            self.assertEqual(mism, [])
        finally:
            runner.cleanup()

    def test_nondeterministic_artifacts_break_and_record(self):
        # artifact payload embeds the build call counter → every probe
        # seed mismatches (timestamp/address randomness analog).
        runner = FakeRunner(artifact=lambda i: b"artifact-build-%d" % i)
        try:
            ok, reason, mism = tcm.probe_codegen_determinism(
                runner, [11, 12, 13])
            self.assertFalse(ok)
            self.assertEqual(len(mism), 3)
            self.assertIn("artifact mismatch", reason)
        finally:
            runner.cleanup()


# ---------------------------------------------------------------------------
# property 1: determinism — directed positive + bites
# ---------------------------------------------------------------------------

class DeterminismTest(unittest.TestCase):
    @unittest.skipUnless(_toolchain_ready(), "./tinyactor not built")
    def test_double_build_diagnostics_byte_identical(self):
        """定向正例：同文件双 build，诊断 stdout+stderr diff 为空。"""
        src = gen.gen_program(0)
        wd = tempfile.mkdtemp(prefix="tcmeta-det-")
        try:
            runner = tcm.MetaRunner(wd)
            sp = os.path.join(wd, "P.ta")
            with open(sp, "wb") as f:
                f.write(src.encode("latin-1"))
            r1 = runner.build(sp, os.path.join(wd, "a1.tabc"))
            r2 = runner.build(sp, os.path.join(wd, "a2.tabc"))
            self.assertEqual(r1.out, r2.out, "stdout diff")
            self.assertEqual(r1.err, r2.err, "stderr diff")
            self.assertEqual(r1.rc, r2.rc)
        finally:
            shutil.rmtree(wd, ignore_errors=True)

    def test_jittered_diagnostics_bite(self):
        """反例有效性：诊断含时间戳的假 build → 断言必须失败。"""
        runner = FakeRunner(
            build=lambda i, p: _rr(out=b"diag-jitter %.9f\n" % (0.1 * i)))
        w = World(runner)
        try:
            outcome = w.run(0)
            self.assertTrue(outcome.startswith("finding:meta-determinism"))
            self.assertEqual(w.findings["meta-determinism"], 1)
            self.assertIn("meta-determinism-", str(_finding_dirs(w.out)))
        finally:
            w.cleanup()

    def test_nondeterministic_artifact_bite(self):
        """反例有效性：产物每次不同 → artifact 级 finding。"""
        runner = FakeRunner(artifact=lambda i: b"blob-%d" % i)
        w = World(runner)
        try:
            outcome = w.run(0)
            self.assertTrue(outcome.startswith("finding:"))
            self.assertGreaterEqual(w.findings["meta-determinism"], 1)
        finally:
            w.cleanup()


# ---------------------------------------------------------------------------
# property 2: order independence — directed positive + bite
# ---------------------------------------------------------------------------

class OrderTest(unittest.TestCase):
    @unittest.skipUnless(_toolchain_ready(), "./tinyactor not built")
    def test_shuffled_program_keeps_conclusion_and_still_parses(self):
        """定向正例：可打乱程序重排后源码确实变化、仍可 parse、结论一致。"""
        seed = _shufflable_seed()
        wd = tempfile.mkdtemp(prefix="tcmeta-ord-")
        try:
            runner = tcm.MetaRunner(wd)
            plan = gen.build_program(seed)
            src0 = gen.render_tree(plan)
            rng = prng.make_prng(prng.derive_seed(seed, tcm.DERIVE_ORDER))
            srcS = gen.render_tree(tcm.shuffle_tree(plan, rng))
            self.assertNotEqual(srcS, src0)     # 确实打乱了
            sp = os.path.join(wd, "P.ta")
            ss = os.path.join(wd, "PS.ta")
            with open(sp, "wb") as f:
                f.write(src0.encode("latin-1"))
            with open(ss, "wb") as f:
                f.write(srcS.encode("latin-1"))
            r0 = runner.build(sp, os.path.join(wd, "p0.tabc"))
            rS = runner.build(ss, os.path.join(wd, "p1.tabc"))
            v0 = tcm.tco.classify_build(r0)
            vS = tcm.tco.classify_build(rS)
            self.assertEqual(v0, "accept")      # gen 对照必 accept
            self.assertEqual(vS, v0)            # 结论不变（accept ⇒ 可 parse）
        finally:
            shutil.rmtree(wd, ignore_errors=True)

    def test_fake_order_dependent_build_bites(self):
        """反例有效性：只对重排文件 reject 的假 build → 断言必须失败。"""
        seed = _shufflable_seed()
        fake = os.path.join(tempfile.mkdtemp(prefix="tcmeta-bite-"),
                            "fake-ta.py")
        with open(fake, "w") as f:
            f.write(
                "#!/usr/bin/env python3\n"
                "import subprocess, sys\n"
                "real = %r\n"
                "if any('src_PS.ta' in a for a in sys.argv[2:]):\n"
                "    sys.stdout.write(%r)\n"
                "    sys.exit(1)\n"
                "p = subprocess.run([real] + sys.argv[1:],\n"
                "                   capture_output=True)\n"
                "sys.stdout.buffer.write(p.stdout)\n"
                "sys.stderr.buffer.write(p.stderr)\n"
                "sys.exit(p.returncode)\n"
                % (morph.TINYACTOR, REJECT.decode("latin-1")))
        os.chmod(fake, 0o755)
        wd = tempfile.mkdtemp(prefix="tcmeta-bite-run-")
        try:
            runner = tcm.MetaRunner(wd, build_bin=fake)
            w = World(runner)
            outcome = w.run(seed)
            self.assertTrue(outcome.startswith("finding:meta-order"),
                            outcome)
            self.assertEqual(w.findings["meta-order"], 1)
            metas = _finding_dirs(w.out)
            self.assertTrue(any(n.startswith("meta-order-") for n in metas))
        finally:
            w.cleanup()
            shutil.rmtree(os.path.dirname(fake), ignore_errors=True)


# ---------------------------------------------------------------------------
# property 3: fmt idempotence — directed positives + bites
# ---------------------------------------------------------------------------

class FmtTest(unittest.TestCase):
    @unittest.skipUnless(_toolchain_ready(), "./tinyactor not built")
    def test_fmt_workflow_byte_identical(self):
        """定向正例（§6.3 :704-708 原文工作流，fmt 就地改写先拷贝）：
        cp P.ta P.fmt.ta && tinyactor fmt P.fmt.ta; 双 build; byte 相等。"""
        src = gen.gen_program(7)
        wd = tempfile.mkdtemp(prefix="tcmeta-fmt-")
        try:
            runner = tcm.MetaRunner(wd)
            p = os.path.join(wd, "P.ta")
            fp = os.path.join(wd, "P.fmt.ta")
            with open(p, "wb") as f:
                f.write(src.encode("latin-1"))
            with open(fp, "wb") as f:
                f.write(src.encode("latin-1"))   # cp（原件不动）
            self.assertEqual(runner.fmt(fp).rc, 0)
            runner.build(p, os.path.join(wd, "a.tabc"))
            runner.build(fp, os.path.join(wd, "b.tabc"))
            with open(os.path.join(wd, "a.tabc"), "rb") as f:
                a = f.read()
            with open(os.path.join(wd, "b.tabc"), "rb") as f:
                b = f.read()
            self.assertEqual(a, b)
            with open(p, "rb") as f:
                self.assertEqual(f.read(),
                                 src.encode("latin-1"))  # 原件未被改写
        finally:
            shutil.rmtree(wd, ignore_errors=True)

    @unittest.skipUnless(_toolchain_ready(), "./tinyactor not built")
    def test_seed_run_counts_fmt_byte(self):
        wd = tempfile.mkdtemp(prefix="tcmeta-fmtrun-")
        try:
            runner = tcm.MetaRunner(wd)
            w = World(runner)
            outcome = w.run(7)
            self.assertEqual(outcome, "ok")
            self.assertEqual(w.checks["fmt-byte"], 1)
            self.assertEqual(sum(w.findings.values()), 0)
        finally:
            w.cleanup()

    def test_fake_scrambling_fmt_bites_byte_mode(self):
        """反例有效性：打乱语义的假 fmt → 幂等断言必须失败（结论翻转）。
        fake build 是内容感知的：含 zz 的源（被假 fmt 改写后的产物）
        会被 reject —— 模拟真实编译器对语义破坏的反应。"""
        MARK = b"kernfuzz-fmt-bite"   # NOT "zz": gen emits that literal

        def content_aware_build(_i, src_path):
            with open(src_path, "rb") as f:
                if MARK in f.read():
                    return _rr(REJECT, rc=1)
            return _rr()

        def scramble(path):
            with open(path, "w") as f:
                f.write('fn main() { let qz: int = 1 + "%s"; print(qz); }'
                        "\n" % MARK.decode("latin-1"))
        runner = FakeRunner(build=content_aware_build, fmt_fn=scramble)
        w = World(runner, mode="byte")
        try:
            outcome = w.run(0)
            self.assertTrue(outcome.startswith("finding:meta-fmt"), outcome)
            self.assertEqual(w.findings["meta-fmt"], 1)
        finally:
            w.cleanup()

    @unittest.skipUnless(_toolchain_ready() and _tavm_ready(),
                         "./tinyactor / ./tavm_asan not built")
    def test_structural_downgrade_path_holds_on_real_toolchain(self):
        """降级正例：强制 structural 模式 → tavm 行为对比通过。"""
        wd = tempfile.mkdtemp(prefix="tcmeta-struct-")
        try:
            runner = tcm.MetaRunner(wd)
            w = World(runner, mode="structural")
            outcome = w.run(7)
            self.assertEqual(outcome, "ok")
            self.assertEqual(w.checks["fmt-structural"], 1)
        finally:
            w.cleanup()

    @unittest.skipUnless(_toolchain_ready() and _tavm_ready(),
                         "./tinyactor / ./tavm_asan not built")
    def test_structural_mode_catches_semantics_scramble(self):
        """降级路径的反例有效性：合法但语义不同的 fmt → structural 违例。
        fmt_bin 换 fake（在 main 前注入 print(999)），build 仍走真编译器。"""
        wd = tempfile.mkdtemp(prefix="tcmeta-structbite-")
        try:
            fake = os.path.join(wd, "fake_fmt.py")
            with open(fake, "w") as f:
                f.write(
                    "#!/usr/bin/env python3\n"
                    "import sys\n"
                    "path = sys.argv[2]\n"
                    "text = open(path).read().replace(\n"
                    "    'fn main() {', 'fn main() { print(999);', 1)\n"
                    "open(path, 'w').write(text)\n")
            os.chmod(fake, 0o755)
            runner = tcm.MetaRunner(wd, fmt_bin=fake)
            w = World(runner, mode="structural")
            outcome = w.run(7)
            self.assertTrue(outcome.startswith("finding:meta-fmt"), outcome)
            self.assertEqual(w.findings["meta-fmt"], 1)
        finally:
            w.cleanup()


# ---------------------------------------------------------------------------
# smoke + determinism of the tool (real toolchain)
# ---------------------------------------------------------------------------

@unittest.skipUnless(_toolchain_ready(), "./tinyactor not built")
class RealToolchainSmokeTest(unittest.TestCase):
    def test_smoke_30_seeds(self):
        out = tempfile.mkdtemp(prefix="tcmeta-smoke-")
        wd = tempfile.mkdtemp(prefix="tcmeta-smoke-run-")
        try:
            runner = tcm.MetaRunner(wd)
            seeds = list(range(301, 331))        # 30 fresh seeds
            stats = tcm.fuzz_batch(runner, seeds, out)
            self.assertEqual(stats["seeds"], 30)
            self.assertEqual(stats["premise"]["mode"], "byte")
            self.assertEqual(stats["premise"]["probe_mismatches"], 0)
            self.assertEqual(stats["checks"]["determinism"], 30)
            self.assertEqual(stats["checks"]["fmt"], 30)
            self.assertEqual(stats["checks"]["fmt-byte"], 30)
            self.assertGreater(stats["checks"]["order"], 0)
            self.assertEqual(sum(stats["findings"].values()), 0)
        finally:
            shutil.rmtree(out, ignore_errors=True)
            shutil.rmtree(wd, ignore_errors=True)

    def test_two_runs_identical(self):
        """同参数两跑：stats（除 elapsed）与 findings 签名集完全一致。"""
        stats_list = []
        outs = []
        for i in (0, 1):
            out = tempfile.mkdtemp(prefix="tcmeta-det2-%d-" % i)
            wd = tempfile.mkdtemp(prefix="tcmeta-det2-run-%d-" % i)
            outs.append(out)
            runner = tcm.MetaRunner(wd)
            stats_list.append(tcm.fuzz_batch(runner, list(range(401, 409)),
                                             out))
            shutil.rmtree(wd, ignore_errors=True)
        try:
            a, b = stats_list
            a.pop("elapsed"), b.pop("elapsed")
            self.assertEqual(a, b)
            sig = []
            for d in outs:
                s = set()
                for n in os.listdir(d):
                    if n == "skips.log":
                        continue
                    with open(os.path.join(d, n, "meta.json")) as f:
                        s.add(json.load(f)["signature"])
                sig.append(s)
            self.assertEqual(sig[0], sig[1])
        finally:
            for d in outs:
                shutil.rmtree(d, ignore_errors=True)


# ---------------------------------------------------------------------------
# CLI surface
# ---------------------------------------------------------------------------

class CliTest(unittest.TestCase):
    def test_parse_seeds_delegates_to_morph(self):
        self.assertEqual(tcm.parse_seeds("3"), [3])
        self.assertEqual(tcm.parse_seeds("1..3"), [1, 2, 3])


if __name__ == "__main__":
    unittest.main(verbosity=2)
