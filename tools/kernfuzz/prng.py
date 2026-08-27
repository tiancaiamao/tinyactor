# -*- coding: utf-8 -*-
"""prng.py — deterministic counter-based PRNG for the kernel-fuzzing toolchain.

Implements the PRNG convention (M-2) from docs/kernel-fuzzing-design.md §5.4:
the Python stdlib `random` module is NEVER used (algorithm/seeding differs
across versions and is not auditable). Instead:

    state = seed (u64)
    each draw: h = sha256(state_be8 || counter_be8); take h[:8] as u64;
               counter += 1

Pure Python, ~stdlib-only, bit-identical across Python versions and platforms.

Seed derivation (M-6) from docs/kernel-fuzzing-design.md §9 (cited at line:
"batch seed 派生公式 `seed_i = int(sha256(\"42:\" + i) 前8字节)` 取正 int48 值
（M-6）"): the doc does not pin endianness for M-6; we use big-endian
(int.from_bytes default), consistent with the M-2 state encoding. The result
is folded into the positive int48 domain [0, 2^47).
"""

import hashlib

_U64 = 1 << 64
_I48 = 1 << 47  # exclusive upper bound of the positive int48 domain


def derive_seed(base, i):
    """M-6 batch seed derivation: seed_i from (base, i).

    seed_i = int.from_bytes(sha256(b"<base>:<i>")[:8]) % 2^47
    (positive int48 domain, so the value is directly usable as a PRNG seed).
    """
    msg = b"%d:%d" % (base, i)
    return int.from_bytes(hashlib.sha256(msg).digest()[:8], "big") % _I48


class Prng(object):
    """Counter-based PRNG. state = seed, counter increments per draw."""

    __slots__ = ("state", "counter")

    def __init__(self, seed):
        if not (0 <= seed < _U64):
            raise ValueError("seed must be in [0, 2^64)")
        self.state = seed
        self.counter = 0

    def next_u64(self):
        """One draw in [0, 2^64): sha256(state_be8 || counter_be8)[:8]."""
        block = self.state.to_bytes(8, "big") + self.counter.to_bytes(8, "big")
        self.counter += 1
        return int.from_bytes(hashlib.sha256(block).digest()[:8], "big")

    def next_int48(self):
        """One draw in [0, 2^47): the top 47 bits of a u64 draw.

        Taking high bits (>> 17) of the sha256 output is uniform and
        unbiased; the result is directly usable as a new seed.
        """
        return self.next_u64() >> 17

    def next_range(self, n):
        """One draw in [0, n) for n >= 1, no modulo bias.

        Rejection sampling: draw u64 values, accept while u >= 2^64 % n
        (re-draw otherwise), then return u % n. Expected acceptance is
        >= 1/2 per draw, so the loop terminates with probability 1.
        """
        if n <= 0:
            raise ValueError("n must be positive")
        limit = _U64 % n  # values in [limit, 2^64) are accepted
        while True:
            u = self.next_u64()
            if u >= limit:
                return u % n


def make_prng(seed):
    """Create a PRNG object from a seed (see M-2)."""
    return Prng(seed)


def prng_next_u64(p):
    return p.next_u64()


def prng_next_int48(p):
    return p.next_int48()


def prng_next_range(p, n):
    return p.next_range(n)