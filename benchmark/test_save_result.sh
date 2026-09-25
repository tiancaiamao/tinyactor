#!/bin/bash
# test_save_result.sh — self-test for benchmark/lib.sh result storage (issue #179)
#
# Verifies that after any sequence of save_result calls (including hostile
# strings and a pre-corrupted results.json), results.json is a valid JSON
# array, and that check_regression reads baselines from it correctly.
#
# Run directly: benchmark/test_save_result.sh   (not part of `make test`)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT
RESULTS_FILE="$TMPDIR_TEST/results.json"
HISTORY_FILE="$TMPDIR_TEST/history.csv"

FAILURES=0
fail() {
    echo "FAIL: $1"
    FAILURES=$((FAILURES + 1))
}

validate_json() {
    jq empty "$RESULTS_FILE" 2>&1 \
        && python3 -c "import json; json.load(open('$RESULTS_FILE'))"
}

# --- 1. stress: >=10 sequential calls with hostile strings --------------------
# NOTE: check_regression sets -e would abort on detected regression; we call
# it only in the dedicated section below, guarded.

hostile_outputs=(
    'plain output'
    'has "double quotes" inside'
    'backslash \ and "mixed \ together'
    'line one
line two
line three'
    'closes ] and } brackets ]}'
    'unicode: 你好 ✅ emoji 🎉'
    "single 'quotes' and \$dollar and \`backtick\`"
    'tab	separated	values'
    '{"looks like":"json"}'
    'multi
"line"
\mix'
    ''
    'last one'
)

for i in "${!hostile_outputs[@]}"; do
    n=$((i + 1))
    if ! save_result "stress" "bench-$n" "0.$n" "${hostile_outputs[$i]}" 0; then
        fail "save_result call $n returned error"
        continue
    fi
    if ! validate_json; then
        fail "results.json invalid after call $n"
    fi
    count=$(jq 'length' "$RESULTS_FILE")
    [ "$count" -eq "$n" ] || fail "expected $n entries after call $n, got $count"
done

# Round-trip: the hostile strings must survive intact in the JSON.
last_output=$(jq -r '.[-1].output' "$RESULTS_FILE")
[ "$last_output" = "last one" ] || fail "last entry output round-trip mismatch: '$last_output'"
escaped=$(jq -r '.[1].output' "$RESULTS_FILE")
[ "$escaped" = 'has "double quotes" inside' ] || fail "quote output round-trip mismatch: '$escaped'"
multiline=$(jq -r '.[3].output' "$RESULTS_FILE")
[ "$multiline" = 'line one
line two
line three' ] || fail "multiline output round-trip mismatch"
n=$(jq -r '.[4].name' "$RESULTS_FILE")
[ "$n" = "bench-5" ] || fail "name mismatch: '$n'"
t=$(jq -r '.[2].time' "$RESULTS_FILE")
[ "$t" = "0.3" ] || fail "time should be numeric 0.3, got: $t"
ec=$(jq -r '.[0].exit_code' "$RESULTS_FILE")
[ "$ec" = "0" ] || fail "exit_code should be numeric 0, got: $ec"

# --- 2. recovery from a pre-corrupted results.json ----------------------------
echo 'not json at all {{{' > "$RESULTS_FILE"
if ! save_result "stress" "after-corruption" "0.5" "recovered" 0; then
    fail "save_result failed on corrupt file"
fi
validate_json || fail "results.json invalid after corrupt-file recovery"
count=$(jq 'length' "$RESULTS_FILE")
[ "$count" -eq 1 ] || fail "expected fresh array (1 entry) after recovery, got $count"

# --- 3. check_regression reads baseline from the fixed file -------------------
# Two prior rounds at ~1.0s -> baseline is the first of the last two (1.0).
rm -f "$RESULTS_FILE"
save_result "core" "fib" "1.0" "run1" 0
save_result "core" "fib" "1.0" "run2" 0

if check_regression "core" "fib" "1.2" >/dev/null 2>&1; then
    : # 1.2 vs 1.0 = 20% slowdown > threshold: wait, default threshold 1.10 → regression
    fail "check_regression should detect 1.2 vs baseline 1.0 as regression"
fi

if check_regression "core" "fib" "1.05" >/dev/null 2>&1; then
    : # 5% slower, within default 10% threshold -> OK
else
    fail "check_regression should accept 1.05 vs baseline 1.0"
fi

if check_regression "core" "unknown-bench" "99" >/dev/null 2>&1; then
    : # no baseline -> first run, always OK
else
    fail "check_regression should pass when no baseline exists"
fi

# --- 4. no .bak litter ---------------------------------------------------------
ls "$TMPDIR_TEST"/*.bak >/dev/null 2>&1 && fail ".bak files left behind"

# --- summary -------------------------------------------------------------------
if [ "$FAILURES" -gt 0 ]; then
    echo "test_save_result: $FAILURES failure(s)"
    exit 1
fi
echo "test_save_result: all checks passed ($(jq 'length' "$RESULTS_FILE") entries, corrupt-file recovery, regression gate)"