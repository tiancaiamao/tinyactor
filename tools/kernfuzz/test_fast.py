# -*- coding: utf-8 -*-
"""
test_fast.py — tests for tools/kernfuzz/fast.py (§9 exit semantics).

Stdlib-only unittest.  Run:
    python3 tools/kernfuzz/test_fast.py
Exit 0 = all pass, non-zero = failure.

Covers the toolchain-missing gate: the fast ring must degrade to a
graceful KERNFUZZ-SKIPPED (exit 0) instead of a MorphError traceback
when any pinned toolchain file is absent — including the lisp-arm
base (tavm_asan / bootstrap.tabc) added with the §5.4 step-8
lisp arm.
"""

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import fast                                    # noqa: E402
import lisparm                                # noqa: E402


class ToolchainMissingTest(unittest.TestCase):
    """toolchain_missing():缺席名单的闭合契约（空 = 齐备）。"""

    @staticmethod
    def _missing_with(attr, value):
        real = getattr(lisparm, attr)
        try:
            setattr(lisparm, attr, value)
            return fast.toolchain_missing()
        finally:
            setattr(lisparm, attr, real)

    def test_full_toolchain_present(self):
        # local contract: toolchain built by the kernfuzz-fast prereqs
        self.assertEqual(fast.toolchain_missing(), [])

    def test_missing_tavm_reported(self):
        self.assertEqual(
            self._missing_with("TAVM_ASAN",
                               "/nonexistent/tavm_asan"),
            ["/nonexistent/tavm_asan"])

    def test_missing_driver_reported(self):
        self.assertEqual(
            self._missing_with("DRIVER_TABC",
                               "/nonexistent/driver.tabc"),
            ["/nonexistent/driver.tabc"])


if __name__ == "__main__":
    unittest.main(verbosity=2)