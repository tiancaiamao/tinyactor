#!/bin/bash
# benchmark/vs-go/fair.sh — mixed-load fairness: TinyActor vs Go.
#
# run.sh measures raw throughput on a homogeneous light load. This measures
# the axis where the runtimes actually differ: one CPU-heavy request must not
# stall a stream of light requests.
#
#   * tinyactor side : server.ta  (NWORKERS=1) — /heavy is compute.burn, a
#                      lib/compute.c kernel driven from a TA loop. The C keeps
#                      raw speed at compiled-C level; the TA loop around it
#                      pays a VM reduction every ~500 chunks, so the burn
#                      actor yields roughly every 2ms and keeps serving.
#   * go side        : server.go  (GOMAXPROCS=1) — /heavy is the same integer
#                      mix in pure Go, same round count, same call-per-chunk
#                      shape, so the runtime has the same boundaries to
#                      preempt at.
#   * client         : fair_bench.go (one binary against both) — buckets light
#                      latency into "no heavy in flight" vs "heavy in flight".
#
# Both sides are pinned to ONE worker so the comparison is scheduling policy,
# not core count. Same client, same load, sequential runs.
#
# Env: CONC (default 16), DUR (default 6s), WARM (default 2s), GO.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
TAVM="$REPO_ROOT/tavm"
GO="${GO:-go}"

CONC="${CONC:-16}"
DUR="${DUR:-6s}"
WARM="${WARM:-2s}"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "error: curl not found (used to wait for server readiness)" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }
[ -x "$TAVM" ]      || { echo "error: $TAVM missing — run 'make tavm' first" >&2; exit 1; }
[ -f "$REPO_ROOT/lib/compute.dylib" ] || [ -f "$REPO_ROOT/lib/compute.so" ] || {
    echo "error: lib/compute.{dylib,so} missing — run 'make' in the repo root first" >&2; exit 1; }

WORK="$(mktemp -d /tmp/ta-vs-go-fair-XXXXXX)"
PIDS=()
cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- build once ----------------------------------------------------------
( cd "$REPO_ROOT" && "$TINYACTOR" build "$SCRIPT_DIR/server.ta" "$WORK/server.tabc" >/dev/null )
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/go-server"  server.go )
( cd "$SCRIPT_DIR" && "$GO" build -o "$WORK/fair_bench" fair_bench.go )

free_port() {
    python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# --- fairness probe against one server ------------------------------------
# fair_one <label> <server-cmd...>   (port is appended to the command)
fair_one() {
    local label="$1"; shift
    local port url pid i=0
    port="$(free_port)"
    url="http://127.0.0.1:$port"

    "$@" "$port" >"$WORK/$label.server.log" 2>&1 &
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
    "$WORK/fair_bench" -url "$url" -c "$CONC" -d "$DUR" -warm "$WARM" \
        >"$WORK/$label.client.log" 2>&1

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== tinyactor vs Go — mixed-load fairness (1 worker each) ==="
if [ "$(uname)" = Darwin ]; then
    echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown), macOS $(sw_vers -productVersion 2>/dev/null)"
else
    echo "machine: $(uname -srmo)"
fi
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "concurrency=$CONC  duration=$DUR  warmup=$WARM  heavy=400M rounds chunk=2000 (both sides)"
echo

fair_one tinyactor env NWORKERS=1 "$TAVM" -q "$WORK/server.tabc"
fair_one go        env GOMAXPROCS=1 "$WORK/go-server"

echo "--- tinyactor (server.ta, NWORKERS=1) ---"
cat "$WORK/tinyactor.client.log"
echo
echo "--- go (server.go, GOMAXPROCS=1) ---"
cat "$WORK/go.client.log"
echo

# light latency inflation caused by the heavy request: during / baseline.
python3 - "$WORK/tinyactor.client.log" "$WORK/go.client.log" <<'PY'
import re, sys

def parse(path):
    out = {}
    for line in open(path):
        m = re.match(r"(baseline \(no heavy\)|during heavy)\s+n=\d+\s+p50=([0-9.]+)\s+p90=([0-9.]+)\s+p99=([0-9.]+)\s+max=([0-9.]+)", line)
        if m:
            out["base" if m.group(1).startswith("baseline") else "during"] = [float(m.group(i)) for i in (2, 3, 4, 5)]
    return out

print("--- summary: light latency inflation (during-heavy / baseline) ---")
print("%-10s %9s %10s %10s %10s" % ("side", "p50x", "p90x", "p99x", "maxx"))
for name, path in (("tinyactor", sys.argv[1]), ("go", sys.argv[2])):
    d = parse(path)
    if "base" not in d or "during" not in d:
        print("%-10s (incomplete)" % name)
        continue
    b, u = d["base"], d["during"]
    print("%-10s %9.2f %10.2f %10.2f %10.2f" % (name, u[0]/b[0] if b[0] else 0, u[1]/b[1] if b[1] else 0,
                                                u[2]/b[2] if b[2] else 0, u[3]/b[3] if b[3] else 0))
print()
print("(1.00x = the heavy request did not disturb light latency; the smaller the better.)")
PY