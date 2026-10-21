#!/bin/bash
# The scheduler cases: the same protocol on both sides, TA and Go.
#
#   cd benchmark/vs-go && ./run-sched.sh
#
# Case 1 — fairness (benchmark/actor/fairness.ta vs fairness.go):
#   32 CPU-bound workers, each spinning until a 3s wall-clock deadline.
#   The worker that gets more CPU does more iterations, so the spread of
#   the counts is the unfairness: dev_x100000 = (max-min)/max * 100000.
#   Both sides run a plain compute loop that the runtime has to preempt —
#   neither side yields voluntarily.
#
# Case 2 — wake-up latency (latency.ta vs latency.go):
#   One worker asks for a 1ms sleep while N CPU-bound workers fill the
#   machine; the delay it actually sees is the scheduler's wake-up
#   latency. This is the case tiancaiamao/sched is about: Go has no
#   priority scheduler, so a latency-sensitive task waits behind
#   CPU-bound ones.
#
# Env: WORKERS (fairness sweep, default "1 2 4 8"), NOISE (latency sweep,
#      default "2 4 8 16"), LAT_WORKERS (latency case worker count,
#      default = ncpu).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
GO="${GO:-go}"

WORKERS="${WORKERS:-1 2 4 8}"
NOISE="${NOISE:-2 4 8 16}"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LAT_WORKERS="${LAT_WORKERS:-$NCPU}"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }

make -C "$SCRIPT_DIR" >/dev/null

WORK="$(mktemp -d /tmp/ta-sched-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "=== scheduler cases — tinyactor vs Go ==="
echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -srmo)  cores=$NCPU"
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo

# --- case 1: fairness -----------------------------------------------------
echo "--- case 1: fairness (32 workers, 3s window, dev = (max-min)/max*100000) ---"
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

# --- case 2: wake-up latency ---------------------------------------------
echo "--- case 2: wake-up latency of a 1ms sleep (workers=$LAT_WORKERS each side) ---"
printf "%-8s %8s %8s %7s | %8s %8s %7s\n" \
    "noise" "ta_p50" "ta_p99" "ta_n" "go_p50" "go_p99" "go_n"
for n in $NOISE; do
    TA_NOISE=$n TA_WINDOW=3000 NWORKERS=$LAT_WORKERS \
        "$TINYACTOR" run "$SCRIPT_DIR/latency.ta" >"$WORK/ta.txt" 2>&1
    (cd "$SCRIPT_DIR" && GOMAXPROCS=$LAT_WORKERS ./latency "$n" 3000) >"$WORK/go.txt" 2>&1
    read -r ta_p50 ta_p99 ta_n < <(python3 - "$WORK/ta.txt" <<'PY'
import re, sys
d = dict(re.findall(r"(\w+)=(\d+)", open(sys.argv[1]).read()))
print(d["p50_us"], d["p99_us"], d["samples"])
PY
)
    read -r go_p50 go_p99 go_n < <(python3 - "$WORK/go.txt" <<'PY'
import re, sys
d = dict(re.findall(r"(\w+)=(\d+)", open(sys.argv[1]).read()))
print(d["p50_us"], d["p99_us"], d["samples"])
PY
)
    printf "%-8s %8s %8s %7s | %8s %8s %7s\n" \
        "$n" "$ta_p50" "$ta_p99" "$ta_n" "$go_p50" "$go_p99" "$go_n"
done
echo
echo "noise = CPU-bound workers besides the probe; both sides run the same"
echo "protocol with the same worker count. Below saturation (noise < cores)"
echo "the two sides are identical; at saturation Go's wake-up delay explodes."