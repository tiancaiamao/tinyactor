#!/bin/bash
# Run the comparison end to end: start the TA server and the Go server one
# after the other, drive both with the same client, print the table.
#
# The same thing by hand:
#   cd benchmark/vs-go && make
#   tinyactor run server.ta 8099
#   ./go-server 8099
#   ./bench -url http://127.0.0.1:8099
#
# Env: CONC (16), DUR (12s), HEAVYPCT (20), LIGHTN (65536), HEAVYN (16777216),
#      DELAYC (0) — latency-sensitive probes against POST /delay1ms, each on
#      its own connection. 0 = off; 4 is the tiancaiamao/sched case: the
#      same mixed load plus a request whose answer depends on how fast the
#      runtime wakes a parked actor back up.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"
GO="${GO:-go}"

CONC="${CONC:-16}"
DUR="${DUR:-12s}"
HEAVYPCT="${HEAVYPCT:-20}"
LIGHTN="${LIGHTN:-65536}"
HEAVYN="${HEAVYN:-16777216}"
DELAYC="${DELAYC:-0}"

command -v "$GO" >/dev/null 2>&1 || { echo "error: go toolchain not found" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 1; }
[ -x "$TINYACTOR" ] || { echo "error: $TINYACTOR missing — run 'make' in the repo root first" >&2; exit 1; }

make -C "$SCRIPT_DIR" >/dev/null
# The stdlib module the TA side dlopens (lib/sha256.dylib — OpenSSL EVP).
make -C "$REPO_ROOT" lib/sha256.dylib >/dev/null

WORK="$(mktemp -d /tmp/ta-vs-go-XXXXXX)"
PIDS=()
cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

free_port() {
    python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# bench_one <label> <server-cmd...>   (port is appended to the command)
bench_one() {
    local label="$1" port pid i=0; shift
    port="$(free_port)"
    ( cd "$SCRIPT_DIR" && exec "$@" "$port" ) >"$WORK/$label.server.log" 2>&1 &
    pid=$!
    PIDS+=("$pid")
    until curl -fsS -o /dev/null -X POST -d '{"bytes":1024}' \
        "http://127.0.0.1:$port/digest" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge 300 ]; then
            echo "error: $label server did not come up on port $port; log:" >&2
            cat "$WORK/$label.server.log" >&2
            return 1
        fi
        sleep 0.1
    done

    # Same fixed sizes to both servers: the responses carry the digest of the
    # work, so identical bodies mean both sides did identical work.
    : > "$WORK/$label.work"
    for n in 1000 65536 1048576 16777216; do
        curl -s -X POST -d "{\"bytes\":$n}" "http://127.0.0.1:$port/digest" \
            >>"$WORK/$label.work"
        echo >>"$WORK/$label.work"
    done

    "$SCRIPT_DIR/bench" -url "http://127.0.0.1:$port" -c "$CONC" -d "$DUR" \
        -heavypct "$HEAVYPCT" -lightn "$LIGHTN" -heavyn "$HEAVYN" \
        -delayc "$DELAYC" \
        >"$WORK/$label.log" 2>&1
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== tinyactor vs Go — POST /digest, ${HEAVYPCT}% heavy, 1 worker each ==="
echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -srmo)"
echo "commit : $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "c=$CONC  duration=$DUR  heavy=$HEAVYPCT% ($HEAVYN bytes)  light=$((100 - HEAVYPCT))% ($LIGHTN bytes)  probes=$DELAYC"
echo "tinyactor NWORKERS=1   go GOMAXPROCS=1"
echo

bench_one tinyactor env NWORKERS=1 "$TINYACTOR" run server.ta
bench_one go        env GOMAXPROCS=1 "$SCRIPT_DIR/go-server"

cat "$WORK/tinyactor.log"; echo; cat "$WORK/go.log"; echo

python3 - "$WORK/tinyactor.log" "$WORK/go.log" "$WORK/tinyactor.work" "$WORK/go.work" <<'PY'
import re, sys

def parse(path):
    out = {}
    for line in open(path):
        m = re.match(r"(light|heavy|delay)\s+n=(\d+)\s+p50=([0-9.]+)\s+p90=([0-9.]+)\s+p99=([0-9.]+)", line)
        if m:
            out[m.group(1)] = (int(m.group(2)), float(m.group(3)), float(m.group(5)))
        m = re.match(r"throughput\s+light ([0-9.]+)/s\s+heavy ([0-9.]+)/s\s+delay ([0-9.]+)/s\s+total ([0-9.]+)/s", line)
        if m:
            out["tps"] = tuple(float(m.group(i)) for i in (1, 2, 3, 4))
    return out

ta, go_ = parse(sys.argv[1]), parse(sys.argv[2])

def ratio(a, b):
    # A row with no samples on one side (the delay probes are off by
    # default) has nothing to compare — say so instead of dividing by zero.
    if b == 0:
        return "n/a"
    return "%.2fx" % (a / b)

print("--- summary (ms; latency ratio < 1 = tinyactor faster, throughput > 1 = tinyactor ahead) ---")
print("%-22s %12s %12s %9s" % ("", "tinyactor", "go", "ta/go"))
for label, key, i in (("light p50", "light", 1), ("light p99", "light", 2),
                      ("heavy p50", "heavy", 1), ("heavy p99", "heavy", 2),
                      ("delay p50", "delay", 1), ("delay p99", "delay", 2)):
    a, b = ta.get(key), go_.get(key)
    if a and b:
        print("%-22s %12.3f %12.3f %8s" % (label, a[i], b[i], ratio(a[i], b[i])))
for i, label in enumerate(("light req/s", "heavy req/s", "delay req/s", "total req/s")):
    a, b = ta.get("tps"), go_.get("tps")
    if a and b:
        print("%-22s %12.0f %12.0f %8s" % (label, a[i], b[i], ratio(a[i], b[i])))
same = open(sys.argv[3]).read() == open(sys.argv[4]).read()
print("%-22s %s" % ("same work", "yes" if same else "NO — the two sides did different work"))
if not same:
    print(open(sys.argv[3]).read(), open(sys.argv[4]).read())
PY