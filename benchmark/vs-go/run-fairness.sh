#!/bin/bash
# The fairness case: the same service, another endpoint.
#
#   cd benchmark/vs-go && ./run-fairness.sh
#
# POST /fairness runs the fairness protocol: 32 CPU-bound workers, each
# spinning until a 3s wall-clock deadline. The worker that gets more CPU
# does more iterations, so the spread of the counts is the unfairness:
# dev_x100000 = (max-min)/max * 100000. The service does the work and
# answers with the counts; the client (bench.go -fairness) computes the
# deviation.
#
# Both sides run a plain compute loop that the runtime has to preempt —
# neither side yields voluntarily — so the difference is the scheduler,
# not the work.
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

. "$SCRIPT_DIR/serverctl.sh"

WORKERS="${WORKERS:-1 2 4 8}"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }

make -C "$SCRIPT_DIR" >/dev/null
# The stdlib module the TA side dlopens (lib/sha256.dylib — OpenSSL SHA-256).
make -C "$REPO_ROOT" lib/sha256.dylib >/dev/null

WORK="$(mktemp -d /tmp/ta-vs-go-fair-XXXXXX)"
cleanup() {
    stop_server
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "=== fairness — tinyactor vs Go ==="
echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -srmo)  cores=$NCPU"
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "actors : 32 (POST /fairness)  window: 3s"
echo

# fairness_one <label> <server-cmd...>: start the service, ask it for the
# counts, print the deviation line the client produced.
fairness_one() {
    local label="$1"; shift
    start_server "$label" "$WORK/$label.server.log" "$@" || return 1
    "$SCRIPT_DIR/bench" -url "http://127.0.0.1:$SRV_PORT" -fairness 2>&1 | tail -1
    stop_server
}

printf "%-8s %14s %14s %10s\n" "workers" "ta_dev" "go_dev" "ta/go"
for w in $WORKERS; do
    ta="$(fairness_one tinyactor env NWORKERS="$w" "$TINYACTOR" run server.ta)"
    go_="$(fairness_one go env GOMAXPROCS="$w" "$SCRIPT_DIR/go-server")"
    ta_dev="$(echo "$ta" | sed -n 's/.*dev_x100000=\([0-9]*\).*/\1/p')"
    go_dev="$(echo "$go_" | sed -n 's/.*dev_x100000=\([0-9]*\).*/\1/p')"
    printf "%-8s %14s %14s %9sx\n" "$w" "$ta_dev" "$go_dev" \
        "$(python3 -c "print('%.1f' % ($go_dev / $ta_dev)) if $ta_dev else print('n/a')")"
done
echo
echo "dev_x100000 = (max-min)/max * 100000 over the per-worker iteration counts;"
echo "the smaller the better. Both sides run the same compute loop, so the"
echo "difference is the scheduler's fairness, not the work."