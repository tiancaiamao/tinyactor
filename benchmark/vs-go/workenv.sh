#!/bin/bash
# Builds the benchmark's own module tree into a workdir. Sourced by run.sh.
#
#   $WORK/lib/work.dylib                  from work.c
#   $WORK/vendor/bench.vs-go/lib/work.ta  from work.ta
#
# The compiler resolves `import "bench.vs-go:lib/work"` through the vendor
# root; the VM resolves the C module as lib/<mod>.<ext> relative to CWD. So both
# servers run with $WORK as their CWD. The compile half has to be invoked FROM
# the workdir too: tinyactor sets TA_PROJECT_ROOT from its own $PWD.

# build_work_module <workdir>
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

# run_build <workdir> <server.ta> <out.tabc>
run_build() {
    ( cd "$1" && "$TINYACTOR" build "$2" "$3" ) >/dev/null
}