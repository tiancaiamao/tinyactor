#!/bin/bash
# benchmark/compile/build.sh — one timed clean build (langcompilebench 口径)
#
#   default        rm -rf the modsig sig cache first, then `tinyactor build`
#                  (cache MISS — the canonical "go clean -cache, then build")
#   --keep-cache   keep the sig cache (cache HIT — measures the modsig
#                  module-signature cache's benefit vs the default)
#
# The sig cache lives in benchmark/compile/.build (TA_SIG_CACHE_DIR) so the
# benchmark never touches the repo-root shared cache. Generated sources are
# (re)created via gen.sh when missing. Timing is bash `time`; the `real
# 0mX.XXXs` line goes to stdout (run.sh parses it), build chatter to stderr.
#
# Usage: build.sh [--keep-cache]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
SRC="$SCRIPT_DIR/src/main.ta"
OUT_DIR="$SCRIPT_DIR/out"
OUT="$OUT_DIR/main.tabc"
CACHE_DIR="$SCRIPT_DIR/.build"

CLEAN=1
for arg in "$@"; do
    case "$arg" in
        --keep-cache) CLEAN=0 ;;
        *)
            echo "usage: build.sh [--keep-cache]" >&2
            exit 2
            ;;
    esac
done

[ -x "$TINYACTOR" ] || {
    echo "error: $TINYACTOR not found — run 'make' in the repo root first" >&2
    exit 1
}
[ -f "$SRC" ] || "$SCRIPT_DIR/gen.sh"

[ "$CLEAN" -eq 1 ] && rm -rf "$CACHE_DIR"
mkdir -p "$OUT_DIR"
rm -f "$OUT"

log="$OUT_DIR/last-build.log"
rc=0
# TA_SIG_CACHE_DIR isolates the modsig cache inside benchmark/compile/
# (the tinyactor script would otherwise default to the repo-root shared
# .build/modules); export, not prefix — the `time` keyword swallows a
# VAR=... prefix assignment in some shells.
export TA_SIG_CACHE_DIR="$CACHE_DIR"
{ time "$TINYACTOR" build "$SRC" "$OUT" ; } > "$log" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "error: tinyactor build failed (rc=$rc):" >&2
    cat "$log" >&2
    exit "$rc"
fi

grep '^real' "$log"