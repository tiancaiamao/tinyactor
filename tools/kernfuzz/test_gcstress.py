# -*- coding: utf-8 -*-
r"""
test_gcstress.py — unit tests for the VM GC stress knob (task-gcstress,
docs/kernel-fuzzing-design.md §7.1 / DEC-5 / DELIV-6).

Stdlib-only unittest, style follows tools/kernfuzz/test_prng.py.
Run:
    python3 tools/kernfuzz/test_gcstress.py
Exit 0 = all pass.
Requires ./tinyactor (and optionally ./tavm_asan) to be built.

Covers (task acceptance points 2/4/5 + zero-change):
  * TA_GC_STRESS unset vs 0: byte-identical output on an allocation-dense
    probe (knob off = zero behavior change).
  * TA_GC_STRESS=1 / 2 / 97: byte-identical output on the probe — the
    forced per-N gc_collect must not change observable behavior.
  * env robustness: TA_GC_STRESS=abc / -3 / 99999999999999999999 must not
    crash and must behave like off (byte-identical to the unset run).
"""

import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))

TINYACTOR = os.path.join(_REPO, "tinyactor")

# Allocation-dense deterministic probe (>= 10^5 heap allocations:
# ~30k conses + ~50k closures + ~500 string concats).
PROBE_TA = r"""
fn build(n) {
  if n == 0 {
    nil
  } else {
    cons(n, build(n - 1))
  }
}

fn sum_list(lst, acc) {
  if null?(lst) {
    acc
  } else {
    sum_list(cdr(lst), acc + car(lst))
  }
}

fn make_adder(n) {
  fn(x) { x + n }
}

fn churn(n, acc) {
  if n == 0 {
    acc
  } else {
    let f = make_adder(n)
    churn(n - 1, acc + f(1))
  }
}

fn strchurn(n, acc) {
  if n == 0 {
    str.length(acc)
  } else {
    strchurn(n - 1, str.concat(acc, "x"))
  }
}

fn main() {
  print(sum_list(build(30000), 0))
  print(churn(50000, 0))
  print(strchurn(500, ""))
}
"""

PROBE_EXPECTED = b"450015000\n1250075000\n500\n"


def run_probe(env_extra=None):
    """Build+run PROBE_TA with ./tinyactor; return (returncode, stdout)."""
    with tempfile.NamedTemporaryFile(
            "w", suffix=".ta", delete=False) as f:
        f.write(PROBE_TA)
        path = f.name
    try:
        env = dict(os.environ)
        env.pop("TA_GC_STRESS", None)
        if env_extra:
            env.update(env_extra)
        proc = subprocess.run(
            [TINYACTOR, "run", path],
            capture_output=True, timeout=120, env=env)
        return proc.returncode, proc.stdout
    finally:
        os.unlink(path)


@unittest.skipUnless(os.path.exists(TINYACTOR),
                     "./tinyactor not built (run `make tinyactor`)")
class TestGcStress(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        rc, out = run_probe()
        cls.baseline_rc = rc
        cls.baseline_out = out

    def test_baseline_is_healthy(self):
        """Probe runs green without the knob and produces the fixed golden."""
        self.assertEqual(self.baseline_rc, 0)
        self.assertEqual(self.baseline_out, PROBE_EXPECTED)

    def test_unset_equals_stress0(self):
        """TA_GC_STRESS=0 behaves exactly like unset (zero change)."""
        rc, out = run_probe({"TA_GC_STRESS": "0"})
        self.assertEqual((rc, out), (self.baseline_rc, self.baseline_out))

    def test_stress_modes_identical(self):
        """N=1/2/97: forced GC every N allocs, output byte-identical."""
        for n in ("1", "2", "97"):
            with self.subTest(n=n):
                rc, out = run_probe({"TA_GC_STRESS": n})
                self.assertEqual(rc, 0)
                self.assertEqual(out, self.baseline_out)

    def test_invalid_env_behaves_like_off(self):
        """Non-numeric / negative / overflow: no crash, same as off."""
        for bad in ("abc", "-3", "99999999999999999999", "", "1x"):
            with self.subTest(bad=bad):
                rc, out = run_probe({"TA_GC_STRESS": bad})
                self.assertEqual(rc, 0)
                self.assertEqual(out, self.baseline_out)


if __name__ == "__main__":
    unittest.main(verbosity=2)