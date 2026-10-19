#!/bin/bash
# benchmark/vs-go/workenv.sh — the benchmark's own module tree, built into a
# workdir. Sourced by run.sh and fair.sh; not runnable on its own.
#
# The workload lives HERE, next to the two servers, and not in the repo's lib/:
# it is benchmark code — no other program has a use for it — so it belongs with
# the benchmark. Nothing in the compiler or the VM had to change for that; the
# two resolution hooks that already exist are enough:
#
#   * the compiler resolves `import "bench.vs-go:lib/work"` through the vendor
#     root table: $TA_PROJECT_ROOT/vendor/bench.vs-go/lib/work.ta
#   * the VM resolves a C module as lib/<mod>.<ext>, relative to the CWD the
#     server runs with
#
# So a workdir holding both is a complete, self-contained module tree, and both
# servers run with that workdir as their CWD:
#
#   $WORK/lib/work.dylib                    built here from work.c
#   $WORK/vendor/bench.vs-go/lib/work.ta    copied here from work.ta
#
# The compile half must be invoked FROM the workdir: tinyactor sets
# TA_PROJECT_ROOT from its own $PWD (an exported value is overwritten), and that
# is what anchors the vendor root. See run_build().
#
#   build_work_module <workdir>

build_work_module() {
    local work="$1" ext undef
    if [ "$(uname)" = Darwin ]; then
        ext="dylib"
        undef="-undefined dynamic_lookup"
    else
        ext="so"
        undef=""
    fi
    mkdir -p "$work/lib" "$work/vendor/bench.vs-go/lib"
    cp "$SCRIPT_DIR/work.ta" "$work/vendor/bench.vs-go/lib/work.ta"
        # shellcheck disable=SC2086 -- $undef is intentionally word-split (or empty)
    cc -Wall -Wextra -std=c99 -O2 -D_DEFAULT_SOURCE -fPIC -shared $undef \
        -I"$REPO_ROOT" -o "$work/lib/work.$ext" "$SCRIPT_DIR/work.c"
}

# run_build <workdir> <server.ta> <out.tabc> — compile a server against the
# module tree above. The cd is load-bearing: the vendor root the CLI resolves
# against is its own $PWD, so the build has to happen inside the workdir.
run_build() {
    local work="$1" src="$2" out="$3"
    ( cd "$work" && "$TINYACTOR" build "$src" "$out" ) >/dev/null
}