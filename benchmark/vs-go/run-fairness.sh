#!/bin/bash
# The fairness case: the same protocol on both sides, TA and Go.
#
#   cd benchmark/vs-go && ./run-fairness.sh
#
# 32 CPU-bound workers, each spinning until a 3s wall-clock deadline. The
# worker that gets more CPU does more iterations, so the spread of the
# counts is the unfairness: dev_x100000 = (max-min)/max * 100000.
#
# Both sides run a plain compute loop that the runtime has to preempt —
# neither side yields voluntarily — and the TA side is the repository's
# own benchmark/actor/fairness.ta, so the two sides run the same program.
#
# This is the axis tiancaiamao/sched is about: Go's runtime is "barely
# FIFO" — it hands a P to the longest-running goroutine, so one goroutine
# can take a large share of a core while the others wait.
#
# Env: WORKERS (fairness sweep, default "1 2 4 8").
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
GO="${GO:-go}"

WORKERS="${WORKERS:-1 2 4 8}"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }

make -C "$SCRIPT_DIR" >/dev/null

echo "=== fairness — tinyactor vs Go ==="
echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -srmo)  cores=$NCPU"
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "actors : 32 (benchmark/actor/fairness.ta)  window: 3s"
echo

printf "%-8s %14s %14s %10s\n" "workers" "ta_dev" "go_dev" "ta/go"
for w in $WORKERS; do
    ta="$(NWORKERS=$w "$TINYACTOR" run "$REPO_ROOT/benchmark/actor/fairness.ta" 2>&1 | tail -1)"
    go_="$(cd "$SCRIPT_DIR" && GOMAXPROCS=$w ./fairness 32 3000 | tail -1)"
    ta_dev="$(echo "$ta" | sed -n 's/.*dev_x100000=\([0-9]*\).*/\1/p')"
    go_dev="$(echo "$go_" | sed -n 's/.*dev_x100000=\([0-9]*\).*/\1/p')"
    printf "%-8s %14s %14s %9sx\n" "$w" "$ta_dev" "$go_dev" \
        "$(python3 -c "print('%.1f' % ($go_dev / $ta_dev)) if $ta_dev else print('n/a')")"
done
echo
echo "dev_x100000 = (max-min)/max * 100000 over the per-actor iteration counts;"
echo "the smaller the better. Both sides run the same compute loop, so the"
echo "difference is the scheduler's fairness, not the work."