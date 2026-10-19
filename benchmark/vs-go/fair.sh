#!/bin/bash
# benchmark/vs-go/fair.sh — mixed load: TinyActor vs Go.
#
# One service, one kind of request, and how much work each request asks for is
# a parameter of that request: GET /work/<n> runs n passes of work.c. The
# client draws those sizes 80/20 — mostly light, one in five heavy — from a
# per-worker seeded PRNG, so BOTH servers are handed the same sizes, in the
# same order, by the same worker. That is the whole scenario; nothing about it
# is tailored to either side.
#
#   * tinyactor side : server.ta (NWORKERS=1) — work.ta run() splits n into
#                      chunk-sized native calls; the VM charges a reduction on
#                      those call boundaries, so a heavy request hands its
#                      worker back about every 2ms and light requests keep
#                      being served.
#   * go side        : server.go (GOMAXPROCS=1) — the identical loop in Go:
#                      same chunk, same seed per chunk, same kernel source.
#   * client         : fair_bench.go (one binary, both servers) — buckets each
#                      light request by whether a heavy request was in flight
#                      for its whole lifetime.
#
# Resources are aligned on purpose: NWORKERS=1 against GOMAXPROCS=1, so what
# differs is scheduling policy and the kernel, not core count. Runs are
# sequential so the two never fight for CPU.
#
# Env: CONC (default 16), DUR (default 12s), HEAVYPCT (default 20),
#      LIGHTN (default 20000), HEAVYN (default 2000000), GO.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
TAVM="$REPO_ROOT/tavm"
GO="${GO:-go}"

CONC="${CONC:-16}"
DUR="${DUR:-12s}"
HEAVYPCT="${HEAVYPCT:-20}"
LIGHTN="${LIGHTN:-20000}"
HEAVYN="${HEAVYN:-2000000}"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "error: curl not found (used to wait for server readiness)" >&2; exit 1; }
command -v cc   >/dev/null 2>&1 || { echo "error: C compiler not found (the workload module is built here)" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }
[ -x "$TAVM" ]      || { echo "error: $TAVM missing — run 'make tavm' first" >&2; exit 1; }

WORK="$(mktemp -d /tmp/ta-vs-go-fair-XXXXXX)"
PIDS=()
cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- build once ----------------------------------------------------------
# shellcheck source=workenv.sh
. "$SCRIPT_DIR/workenv.sh"
build_work_module "$WORK"
run_build "$WORK" "$SCRIPT_DIR/server.ta" "$WORK/server.tabc"
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/go-server"  server.go )
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/fair_bench" fair_bench.go )

free_port() {
    python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# --- mixed load against one server ---------------------------------------
# fair_one <label> <cwd> <server-cmd...>   (port is appended to the command)
fair_one() {
    local label="$1" cwd="$2"; shift 2
    local port url pid i=0
    port="$(free_port)"
    url="http://127.0.0.1:$port"

    ( cd "$cwd" && exec "$@" "$port" ) >"$WORK/$label.server.log" 2>&1 &
    pid=$!
    PIDS+=("$pid")

    until curl -fsS -o /dev/null "$url/" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge 300 ]; then
            echo "error: $label server did not come up on port $port; log:" >&2
            cat "$WORK/$label.server.log" >&2
            return 1
        fi
        sleep 0.1
    done

    printf '%s\n' "$url" > "$WORK/$label.url"
    "$WORK/fair_bench" -url "$url" -c "$CONC" -d "$DUR" \
        -heavypct "$HEAVYPCT" -lightn "$LIGHTN" -heavyn "$HEAVYN" \
        >"$WORK/$label.client.log" 2>&1

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== tinyactor vs Go — mixed load (1 worker each) ==="
if [ "$(uname)" = Darwin ]; then
    echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown), macOS $(sw_vers -productVersion 2>/dev/null)"
else
    echo "machine: $(uname -srmo)"
fi
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "concurrency=$CONC  duration=$DUR  mix: ${HEAVYPCT}% heavy (${HEAVYN} passes) / $((100 - HEAVYPCT))% light (${LIGHTN} passes), chunk=50 (both sides)"
echo "workers: tinyactor NWORKERS=1   go GOMAXPROCS=1"
echo

# The TA server runs with $WORK as its CWD: that is where lib/work.dylib is
# (the VM resolves a C module relative to CWD, see workenv.sh).
fair_one tinyactor "$WORK" env NWORKERS=1 "$TAVM" -q "$WORK/server.tabc"
fair_one go        "$WORK" env GOMAXPROCS=1 "$WORK/go-server"

echo "--- tinyactor (server.ta, NWORKERS=1) ---"
cat "$WORK/tinyactor.client.log"
echo
echo "--- go (server.go, GOMAXPROCS=1) ---"
cat "$WORK/go.client.log"
echo

python3 - "$WORK/tinyactor.client.log" "$WORK/go.client.log" <<'PY'
import re, sys

def parse(path):
    out = {}
    for line in open(path):
        m = re.match(r"(light \(all\)|light, no heavy|light, heavy running|heavy request)\s+n=(\d+)\s+p50=([0-9.]+)\s+p90=([0-9.]+)\s+p99=([0-9.]+)\s+max=([0-9.]+)", line)
        if m:
            out[m.group(1)] = [int(m.group(2))] + [float(m.group(i)) for i in (3, 4, 5, 6)]
        m = re.match(r"throughput\s+light ([0-9.]+)/s\s+heavy ([0-9.]+)/s\s+total ([0-9.]+)/s", line)
        if m:
            out["tps"] = [float(m.group(i)) for i in (1, 2, 3)]
    return out

ta, go_ = parse(sys.argv[1]), parse(sys.argv[2])

print("--- summary (identical window, identical per-worker request sequences) ---")
print("%-38s %12s %12s %10s" % ("", "tinyactor", "go", "ta/go"))
rows = [
    ("light p50, all light requests", "light (all)", 1),
    ("light p99, all light requests", "light (all)", 3),
    ("light p50, heavy in flight",    "light, heavy running", 1),
    ("heavy request p50",             "heavy request", 1),
]
for label, key, i in rows:
    if key in ta and key in go_ and ta[key][0] and go_[key][0]:
        a, b = ta[key][i], go_[key][i]
        print("%-38s %12.3f %12.3f %9.2fx" % (label + " (ms)", a, b, a / b if b else 0))
for i, label in enumerate(("light req/s", "heavy req/s", "total req/s")):
    if "tps" in ta and "tps" in go_:
        a, b = ta["tps"][i], go_["tps"][i]
        print("%-38s %12.0f %12.0f %9.2fx" % (label, a, b, a / b if b else 0))
print()
print("(latency ratio < 1.00 = tinyactor faster; throughput ratio > 1.00 = tinyactor ahead.)")
PY