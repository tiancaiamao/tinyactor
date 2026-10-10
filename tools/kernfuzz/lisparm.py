# -*- coding: utf-8 -*-
"""lisparm.py — the kernfuzz lisp arm (kernel-fuzzing §5.4).

Cross-VM differential half of the morph runner: every gen program that
survives the tavm star comparison AND the golden anchor is also run
through the lisp pipeline, and its normalized output must equal the
tavm arm's norm for the same unit.

Two-step call card, mirroring morph.Runner's build/run split (build and
run failures must never share an exit code):

  compile half:  tavm_asan lib/bootstrap.tabc \\
                      <src> <bc> <cache_dir>     (cwd = repo root)
                 The bootstrap driver (TA sources, self-hosting
                 compiler) lowers the source to bytecode.  ASan base so
                 a compiler-pipeline memory bug under a mutated input
                 is a recorded finding, not a silent crash.
  run half:      tavm_asan -q <bc>             (ASAN exitcode=42)

Deliberately dependency-free (stdlib only, no morph import): methods
return plain (out, err, rc, timed_out) tuples and the constructor
raises LispToolchainError; morph converts both to its own types at the
boundary.  Keeps the module graph acyclic and this arm testable alone.

Notes:
  * gen's v0 subset emits no `import`, so no expand_imports step here.
  * norm protocol lives in morph (norm_tavm = test_gen._norm_vm):
    the norm derives from tavm exit semantics (0 clean, 1 = main crash ->
    DIVZERO:n synthesized by the norm function), so the SAME norm
    function applies to both arms' outputs.
  * compile cwd = repo root, like tinyactor's run_tavm (the driver
    links lib/bootstrap relative to the repo root).
"""

import os
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))

# compile-half base, standalone default — morph.Runner always passes its
# own TAVM_ASAN pin explicitly (mirrors morph.TAVM_ASAN on purpose; this
# module must not import morph).
TAVM_ASAN = os.path.join(_REPO_ROOT, "tavm_asan")
DRIVER_TABC = os.path.join(_REPO_ROOT, "lib", "bootstrap.tabc")
CACHE_DIR = os.path.join(_REPO_ROOT, ".build", "modules")

ASAN_EXIT = 42               # same R3 C-1 convention as morph.Runner
ASAN_ENV = {"ASAN_OPTIONS": "exitcode=%d" % ASAN_EXIT}

_TOOLCHAIN_HINT = (
    "build it first with:\n"
    "  ASAN=1 make tavm_asan && "
    "make lib/bootstrap.tabc")


class LispToolchainError(Exception):
    """Missing lisp-arm toolchain file — morph converts it to MorphError."""


def _spawn(argv, timeout):
    """Run argv under the ASan exit protocol -> (out, err, rc, timed_out);
    kill after timeout (rc=None, timed_out=True)."""
    env = dict(os.environ)
    env.update(ASAN_ENV)
    try:
        p = subprocess.run(argv, capture_output=True, timeout=timeout,
                           env=env, cwd=_REPO_ROOT)
        return (p.stdout, p.stderr, p.returncode, False)
    except subprocess.TimeoutExpired as ex:
        return (ex.stdout or b"", ex.stderr or b"", None, True)


class LispArm(object):
    """Executes the lisp half of the §5.4 call card."""

    def __init__(self, workdir, timeout,
                 tavm_asan=TAVM_ASAN, driver_tabc=DRIVER_TABC,
                 cache_dir=CACHE_DIR):
        missing = [p for p in (tavm_asan, driver_tabc)
                   if not os.path.exists(p)]
        if missing:
            raise LispToolchainError(
                "lisp-arm toolchain missing file(s): %s\n%s"
                % (", ".join(missing), _TOOLCHAIN_HINT))
        self.workdir = workdir
        self.timeout = timeout
        self.tavm_asan = tavm_asan
        self.driver_tabc = driver_tabc
        self.cache_dir = cache_dir
        os.makedirs(cache_dir, exist_ok=True)

    def build_and_run(self, src_text, tag):
        """Full two-step card for one program.  Returns (res, paths, bp)
        where res/bp are (out, err, rc, timed_out) tuples; bp is the
        compile half, res the run half.  On compile failure the run half
        never executes and res mirrors bp (same shape as
        morph.Runner.build_and_run)."""
        src_path = os.path.join(self.workdir, "lsrc_%s.ta" % tag)
        bc_path = os.path.join(self.workdir, "lsrc_%s.tabc" % tag)
        with open(src_path, "wb") as f:
            f.write(src_text.encode("latin-1"))
        # compile half: the driver runs on the ASan tavm base (run_tavm
        # precedent — $TAVM drives the driver; the run half only
        # executes .tabc)
        bp = _spawn([self.tavm_asan, self.driver_tabc, src_path,
                     bc_path, self.cache_dir], self.timeout * 4)
        if bp[2] != 0:                      # rc != 0, or None (timeout)
            return (b"", bp[0] + bp[1], bp[2], bp[3]), \
                (src_path, bc_path), bp
        rp = _spawn([self.tavm_asan, "-q", bc_path], self.timeout)
        return rp, (src_path, bc_path), bp