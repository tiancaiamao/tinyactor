#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM="$ROOT/test/actor/million-actors-steady.ta"
TIMEOUT_SECONDS=${MEMORY_REGRESSION_TIMEOUT:-180}
STABLE_SECONDS=${MEMORY_REGRESSION_STABLE_SECONDS:-30}
RSS_LIMIT_KB=$((1536 * 1024))
CPU_LIMIT=3.0

if [ "$(uname -s)" != Linux ]; then
    echo "memory regression test requires Linux /proc" >&2
    exit 2
fi

log=$(mktemp)
pid=
cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$log"
}
trap cleanup EXIT INT TERM

cd "$ROOT"
./tinyactor run "$PROGRAM" >"$log" 2>&1 &
pid=$!

deadline=$(( $(date +%s) + TIMEOUT_SECONDS ))
while ! grep -q '^READY$' "$log"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        cat "$log" >&2
        echo "memory regression: process exited before READY" >&2
        exit 1
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "memory regression: timeout waiting for READY" >&2
        exit 1
    fi
    sleep 1
done

echo "READY: observe with: pid=$pid; watch -n 1 \"grep -E 'VmRSS|VmSize' /proc/\\$pid/status\""

start_ticks=$(awk '{print $14 + $15}' "/proc/$pid/stat")
start_time=$(date +%s)
sleep "$STABLE_SECONDS"

if ! kill -0 "$pid" 2>/dev/null; then
    echo "memory regression: process exited during observation" >&2
    exit 1
fi

rss_kb=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status")
end_ticks=$(awk '{print $14 + $15}' "/proc/$pid/stat")
end_time=$(date +%s)
elapsed=$((end_time - start_time))
[ "$elapsed" -gt 0 ] || elapsed=1
hz=$(getconf CLK_TCK)
cpu=$(awk -v ticks="$((end_ticks - start_ticks))" -v hz="$hz" -v seconds="$elapsed" \
    'BEGIN { printf "%.2f", ticks * 100 / hz / seconds }')

printf 'RSS: %s KB (limit < %s KB)\n' "$rss_kb" "$RSS_LIMIT_KB"
printf 'CPU: %s%% (limit < %.2f%%)\n' "$cpu" "$CPU_LIMIT"

awk -v value="$rss_kb" -v limit="$RSS_LIMIT_KB" 'BEGIN { exit !(value < limit) }' || {
    echo "FAIL: RSS limit exceeded" >&2
    exit 1
}
awk -v value="$cpu" -v limit="$CPU_LIMIT" 'BEGIN { exit !(value < limit) }' || {
    echo "FAIL: CPU limit exceeded" >&2
    exit 1
}
echo "PASS: million-actor steady-state regression"