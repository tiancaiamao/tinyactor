#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM="$ROOT/test/actor/million-actors-ready.ta"
TIMEOUT_SECONDS=${MEMORY_REGRESSION_TIMEOUT:-180}
STABLE_SECONDS=${MEMORY_REGRESSION_STABLE_SECONDS:-30}
RSS_LIMIT_KB=$((1536 * 1024))
CPU_LIMIT=3.0

if [ "$(uname -s)" != Linux ]; then
    echo "1M actor regression test requires Linux /proc" >&2
    exit 2
fi

log=$(mktemp)
pid=
vm_pid=
cleanup() {
    if [ -n "$pid" ]; then
        # 先只 TERM tavm：外壳 tinyactor 会 wait 到、走完
        # rm -rf /tmp/tinyactor-tavm-XXX 再退出，不留临时目录。
        if [ -n "$vm_pid" ]; then
            kill -TERM "$vm_pid" 2>/dev/null || true
        fi
        # 等外壳自然退出收尾；最多等 10s，不退再按组杀兜底
        # （setsid 让整棵树独占进程组 —— 不杀组会把 5.3GB / 96% CPU
        # 的 tavm 孤儿化扔给 init，泄漏实测）。
        waited=0
        while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 10 ]; do
            sleep 1
            waited=$((waited + 1))
        done
        kill -TERM -- "-$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$log"
}
trap cleanup EXIT INT TERM

cd "$ROOT"
# setsid：让 run 的整棵进程树成为独立进程组的组长（$! 即组长 pid），
# 清理时按组杀。不 setsid 时后台任务与本脚本同组，按组杀会自杀。
setsid ./tinyactor run "$PROGRAM" >"$log" 2>&1 &
pid=$!

deadline=$(( $(date +%s) + TIMEOUT_SECONDS ))
while ! grep -q '^READY$' "$log"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        cat "$log" >&2
        echo "1M actor regression: process exited before READY" >&2
        exit 1
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "1M actor regression: timeout waiting for READY" >&2
        exit 1
    fi
    sleep 1
done

# ./tinyactor run 是个 bash 编排外壳：tavm 编译完，它 fork 子进程
# `tavm -q run.tabc` 真正执行程序。RSS/CPU 必须量这个 tavm ——
# 量 $pid 外壳等于量一个 4MB 的空壳，护栏全绿但 VM 在 5.3GB / 96% CPU
# 烧着，测试照样 PASS（假阴性实测）。READY 出现时 tavm 必已是外壳的子进程。
i=0
while [ -z "$vm_pid" ] && [ "$i" -lt 10 ]; do
    vm_pid=$(pgrep -P "$pid" -x tavm | head -n1) || true
    if [ -z "$vm_pid" ]; then
        sleep 1
        i=$((i + 1))
    fi
done
if [ -z "$vm_pid" ]; then
    echo "1M actor regression: tavm child not found under pid $pid" >&2
    cat "$log" >&2
    exit 1
fi

echo "READY: observe with: pid=$vm_pid; watch -n 1 \"grep -E 'VmRSS|VmSize' /proc/$vm_pid/status\""

start_ticks=$(awk '{print $14 + $15}' "/proc/$vm_pid/stat")
start_time=$(date +%s)
sleep "$STABLE_SECONDS"

if ! kill -0 "$vm_pid" 2>/dev/null; then
    echo "1M actor regression: process exited during observation" >&2
    cat "$log" >&2
    exit 1
fi

rss_kb=$(awk '/^VmRSS:/ {print $2}' "/proc/$vm_pid/status")
end_ticks=$(awk '{print $14 + $15}' "/proc/$vm_pid/stat")
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