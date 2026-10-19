#!/bin/bash
# benchmark/vs-go/run.sh — end-to-end HTTP service comparison: TinyActor vs Go.
#
# The SAME client load-tests two servers that serve the identical JSON
# document, and we compare the client-observed throughput and latency
# distribution:
#
#   * TinyActor side : server.ta  (lib/http.ta — listen_routes + serve_loop,
#                      one handler actor per connection, keep-alive)
#   * Go side        : server.go  (net/http)
#   * client         : serve_bench.go (the same binary against both)
#
# Runs are sequential (never concurrent) so the two never fight for CPU.
# A 1s warmup precedes the measured window; the tail ratio p99/p50 in the
# summary is a fairness/latency-spread indicator (lower = tighter tail).
#
# Env: CONC (default 32), DUR (default 5s), NWORKERS (default ncpu), GO.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
TAVM="$REPO_ROOT/tavm"
GO="${GO:-go}"

NCPU="$(python3 -c 'import os; print(os.cpu_count() or 1)')"
CONC="${CONC:-32}"
DUR="${DUR:-5s}"
NWORKERS="${NWORKERS:-$NCPU}"

command -v "$GO"     >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
command -v curl      >/dev/null 2>&1 || { echo "error: curl not found (used to wait for server readiness)" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }
[ -x "$TAVM" ]      || { echo "error: $TAVM missing — run 'make tavm' first" >&2; exit 1; }

WORK="$(mktemp -d /tmp/ta-vs-go-XXXXXX)"
PIDS=()
cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- build once ----------------------------------------------------------
( cd "$REPO_ROOT" && "$TINYACTOR" build "$SCRIPT_DIR/server.ta" "$WORK/server.tabc" >/dev/null )
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/go-server"     server.go )
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/serve_bench"   serve_bench.go )

free_port() {
    python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# --- load-test one server -------------------------------------------------
# bench_one <label> <server-cmd...>   (port is appended to the command)
bench_one() {
    local label="$1"; shift
    local port url pid i=0
    port="$(free_port)"
    url="http://127.0.0.1:$port/"

    "$@" "$port" >"$WORK/$label.server.log" 2>&1 &
    pid=$!
    PIDS+=("$pid")

    until curl -fsS -o /dev/null "$url" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge 300 ]; then
            echo "error: $label server did not come up on port $port; log:" >&2
            cat "$WORK/$label.server.log" >&2
            return 1
        fi
        sleep 0.1
    done

    echo "$port" > "$WORK/$label.port"
    "$WORK/serve_bench" -url "$url" -c "$CONC" -d 1s >/dev/null 2>&1 || true
    "$WORK/serve_bench" -url "$url" -c "$CONC" -d "$DUR" >"$WORK/$label.client.log" 2>&1

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== tinyactor vs Go — HTTP service ==="
if [ "$(uname)" = Darwin ]; then
    echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown), macOS $(sw_vers -productVersion 2>/dev/null)"
else
    echo "machine: $(uname -srmo)"
fi
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "ncpu=$NCPU  concurrency=$CONC  duration=$DUR  (1s warmup per side)"

bench_one tinyactor env NWORKERS="$NWORKERS" GOMAXPROCS="$NCPU" "$TAVM" -q "$WORK/server.tabc"
bench_one go        env GOMAXPROCS="$NCPU" "$WORK/go-server"

# --- report ---------------------------------------------------------------
tp()  { sed -n 's/^throughput[^:]*: *\([0-9][0-9]*\) .*/\1/p' "$1"; }
lat() { sed -n 's/^latency[^:]*: *//p' "$1"; }

# p99/p50 ratio from a client log (lower = tighter tail).
tailratio() {
    python3 -c '
import re, sys
log = open(sys.argv[1]).read()
def us(s):
    m = re.fullmatch(r"([0-9.]+)(ns|µs|us|ms|s)", s)
    return float(m.group(1)) * {"ns": 1e-3, "µs": 1.0, "us": 1.0, "ms": 1e3, "s": 1e6}[m.group(2)]
def f(name):
    return re.search(r"\b" + name + r" ([0-9.]+(?:ns|µs|us|ms|s))", log).group(1)
print("%.2f" % (us(f("p99")) / us(f("p50"))))
' "$1"
}

echo
echo "--- tinyactor (server.ta, NWORKERS=$NWORKERS, port $(cat "$WORK/tinyactor.port")) ---"
cat "$WORK/tinyactor.client.log"
echo
echo "--- go (server.go, GOMAXPROCS=$NCPU, port $(cat "$WORK/go.port")) ---"
cat "$WORK/go.client.log"

echo
echo "--- summary (client-observed) ---"
printf '%-10s %10s req/s   %s\n' "tinyactor" "$(tp "$WORK/tinyactor.client.log")" "$(lat "$WORK/tinyactor.client.log")"
printf '%-10s %10s req/s   %s\n' "go"        "$(tp "$WORK/go.client.log")"        "$(lat "$WORK/go.client.log")"

ta_tp="$(tp "$WORK/tinyactor.client.log")"; go_tp="$(tp "$WORK/go.client.log")"
if [ -n "$ta_tp" ] && [ "$ta_tp" -gt 0 ] 2>/dev/null; then
    printf 'throughput : go / tinyactor = %.2fx\n' "$(awk -v a="$go_tp" -v b="$ta_tp" 'BEGIN{printf a/b}')"
fi
printf 'tail p99/p50 : tinyactor %s   go %s   (lower = tighter tail)\n' \
    "$(tailratio "$WORK/tinyactor.client.log")" "$(tailratio "$WORK/go.client.log")"