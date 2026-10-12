#!/bin/bash
# benchmark_compile/run.sh — median-of-11 compile-time benchmark
# (照抄 langcompilebench: build 11 times, 2s between runs, median = 6th
# smallest of the sorted samples; parse the `real 0mX.XXXs` line).
#
#   ./run.sh            cold builds (default: sig cache wiped each run)
#   ./run.sh cold       same as above (cache MISS 口径)
#   ./run.sh warm       sig cache kept across runs (cache HIT 口径; one
#                       untimed priming build first) — the modsig-cache
#                       benefit is the cold/warm median ratio
#   ./run.sh ratio      11 interleaved cold-then-warm PAIRS; report both
#                       medians + median of per-pair cold/warm ratios
#                       (robust against machine-load drift between
#                       standalone runs)
#
# Results are only comparable on the SAME machine (printout includes the
# model + commit); cross-machine numbers are meaningless.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE="${1:-cold}"
case "$MODE" in
    cold) BUILD_ARGS=() ;;
    warm) BUILD_ARGS=(--keep-cache) ;;
    ratio) BUILD_ARGS=() ;;   # interleaved cold-then-warm pairs, see below
    *)
        echo "usage: run.sh [cold|warm|ratio]" >&2
        exit 2
        ;;
esac

ITER=11

# "real 0m12.345s" -> "12.345" (BSD awk has no match(s, re, arr); sed + awk)
parse_real() {
    local mm ss
    mm=$(sed -n 's/^real[[:space:]]*\([0-9][0-9]*\)m\([0-9.][0-9.]*\)s$/\1/p' <<< "$1")
    ss=$(sed -n 's/^real[[:space:]]*\([0-9][0-9]*\)m\([0-9.][0-9.]*\)s$/\2/p' <<< "$1")
    [ -n "$mm" ] || return 1
    awk -v m="$mm" -v s="$ss" 'BEGIN { printf "%.3f", m * 60 + s }'
}

# machine info header (记录机器型号, per issue: 结果只跟自己比)
echo "=== tinyactor compile-time benchmark ($MODE) ==="
if [ "$(uname)" = "Darwin" ]; then
    echo "machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown), macOS $(sw_vers -productVersion 2>/dev/null)"
else
    echo "machine: $(uname -srmo)"
fi
echo "commit:  $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [ "$MODE" = ratio ]; then
        echo "runs:    $ITER cold->warm pairs (median = 6th smallest of each)"
else
    echo "runs:    $ITER (median = 6th smallest), $ITER builds, 2s between runs"
fi
echo

# median of 11 sorted-smallest samples
median6() {
    printf '%s\n' "$@" | sort -n | sed -n 6p
}

if [ "$MODE" = ratio ]; then
    # Interleaved pairs: each iteration runs a cold build (wipes the cache,
    # typechecks every module, stores their sigs on success) immediately
    # followed by a warm build (all sigs hit) — both in the same load
    # window. On a contended machine the standalone cold/warm medians
    # drift with system load and the ratio drowns in noise; the paired
    # ratio does not.
    cold_times=() warm_times=() ratios=()
    for i in $(seq 1 "$ITER"); do
        tc=$(parse_real "$("$SCRIPT_DIR/build.sh")") || tc=""
        tw=$(parse_real "$("$SCRIPT_DIR/build.sh" --keep-cache)") || tw=""
        [ -n "$tc" ] && [ -n "$tw" ] || {
            echo "error: build failed (cold='$tc' warm='$tw')" >&2
            exit 1
        }
        cold_times+=("$tc") warm_times+=("$tw")
        ratios+=("$(awk -v c="$tc" -v w="$tw" 'BEGIN { printf "%.3f", c / w }')")
        printf 'pair %2d: cold %ss  warm %ss\n' "$i" "$tc" "$tw"
        sleep 2
    done
    echo
    echo "median cold: $(median6 "${cold_times[@]}")s"
    echo "median warm: $(median6 "${warm_times[@]}")s"
    echo "median ratio cold/warm: $(median6 "${ratios[@]}")x"
    exit 0
fi

if [ "$MODE" = warm ]; then
    echo "priming sig cache (untimed build)..."
    "$SCRIPT_DIR/build.sh" --keep-cache > /dev/null
    echo
fi

times=()
for i in $(seq 1 "$ITER"); do
    out=$("$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}")
    t=$(parse_real "$out") || t=""
    [ -n "$t" ] || {
        echo "error: could not parse timing from: $out" >&2
        exit 1
    }
    times+=("$t")
    printf 'run %2d: %ss\n' "$i" "$t"
    sleep 2
done

sorted=($(printf '%s\n' "${times[@]}" | sort -n))
median="${sorted[5]}"   # 0-indexed: 6th smallest of 11
sum=$(printf '%s\n' "${times[@]}" | awk '{ s += $1 } END { printf "%.3f", s }')
mean=$(awk -v s="$sum" -v n="$ITER" 'BEGIN { printf "%.3f", s / n }')
min="${sorted[0]}"
max="${sorted[$((ITER - 1))]}"

echo
echo "median: ${median}s   mean: ${mean}s   min: ${min}s   max: ${max}s   ($MODE)"