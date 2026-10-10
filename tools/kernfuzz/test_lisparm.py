# -*- coding: utf-8 -*-
"""
test_lisparm.py — tests for tools/kernfuzz/lisparm.py (§5.4 lisp arm).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_lisparm.py
Exit 0 = all pass, non-zero = failure.

The lisp arm is the cross-VM differential half of the morph runner:
compile = bootstrap driver on the ASan tavm base, run = tavm_asan.
Contract:
  * toolchain validation — missing tavm_asan / driver.tabc raises
    LispToolchainError with an actionable make hint (never a bare
    FileNotFoundError deep inside a batch),
  * happy path — type-correct source compiles to a .tabc and runs clean
    with the program's output on stdout,
  * compile-fail — the driver rejects a broken source with rc != 0 and
    a non-empty stderr; the run half never executes (res mirrors the
    compile failure, same shape as morph.Runner.build_and_run).

The happy-path tests need the real toolchain, same contract as
test_morph.py: build it first (ASAN=1 make tavm_asan tavm_asan &&
make lib/bootstrap.tabc).
"""

import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lisparm                                # noqa: E402


class ToolchainValidationTest(unittest.TestCase):
    """缺工具链 → LispToolchainError + 可执行的构建提示。"""

    def test_missing_tavm_asan_gives_build_hint(self):
        with self.assertRaises(lisparm.LispToolchainError) as ctx:
            lisparm.LispArm(tempfile.mkdtemp(), 2.0,
                            tavm_asan="/nonexistent/tavm_asan")
        msg = str(ctx.exception)
        self.assertIn("tavm_asan", msg)
        self.assertIn("ASAN=1 make", msg)

    def test_missing_driver_tabc_gives_build_hint(self):
        with self.assertRaises(lisparm.LispToolchainError) as ctx:
            lisparm.LispArm(tempfile.mkdtemp(), 2.0,
                            driver_tabc="/nonexistent/driver.tabc")
        msg = str(ctx.exception)
        self.assertIn("bootstrap.tabc", msg)
        self.assertIn("make", msg)


class RealArmTest(unittest.TestCase):
    """真实工具链端到端：happy path + driver 拒绝坏源。"""

    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.mkdtemp(prefix="lisparm-real-")
        cls.arm = lisparm.LispArm(cls.workdir, 5.0)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.workdir, ignore_errors=True)

    def test_happy_build_and_run(self):
        res, paths, bp = self.arm.build_and_run(
            'fn main() {\n  print("hi")\n}\n', "ok")
        _b_out, _b_err, b_rc, b_to = bp
        self.assertEqual((b_rc, b_to), (0, False))
        _src_path, bc_path = paths
        self.assertTrue(os.path.exists(bc_path))
        out, _err, rc, timed_out = res
        self.assertFalse(timed_out)
        self.assertEqual(rc, 0)
        self.assertIn(b"hi", out)

    def test_driver_rejects_broken_source(self):
        # typecheck-fail source: the driver rejects with rc=1 and a
        # non-empty stderr (verified against both plain and ASan bases).
        # NB: `print(` unclosed is NOT a valid fixture — the grammar
        # accepts it identically in both pipelines (prints `nil`).
        res, paths, bp = self.arm.build_and_run(
            'fn main() {\n  let x: string = 42\n}\n', "bf")
        _o, _e, b_rc, b_to = bp
        self.assertFalse(b_to)
        self.assertNotEqual(b_rc, 0)
        # run half never executed: res mirrors the compile failure
        out, err, rc, timed_out = res
        self.assertEqual(out, b"")
        self.assertEqual(rc, b_rc)
        self.assertTrue(err)


if __name__ == "__main__":
    unittest.main(verbosity=2)