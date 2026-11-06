#!/bin/bash
# test/lib.sh — Shared test runner functions.
#
# Source this file from a per-category runner script, then call
# run_test / run_category and finish with
# print_summary. All state (counters, paths) is set up here so each
# runner is self-contained.
#
# Each test file is exercised through tinyactor run — the single lisp
# path (lisp backend compile + tavm run) — which verifies the whole
# pipeline produces runnable bytecode. Negative tests (asserting the TA
# compiler rejects the program) since 7b-4 consume driver.ta's shared
# verification chain (parse_module_content + typecheck_report): same
# rejection messages as any other host.

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Global counters (reset per category)
PASSED=0
FAILED=0
TOTAL=0
FAILED_TESTS=()

# Project paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Portable timeout: GNU timeout is not installed on macOS by default.
# Fall back to gtimeout (coreutils via Homebrew) or a shell implementation
# with the same semantics for the harness: exit 124 when the command exceeds
# the limit, otherwise the command's own exit status.
if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then
    timeout() { gtimeout "$@"; }
  else
    timeout() {
      local secs=$1; shift
      "$@" &
      local pid=$!
      local waited=0
      while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$secs" ]; then
          kill -TERM "$pid" 2>/dev/null
          wait "$pid" 2>/dev/null
          return 124
        fi
        sleep 1
        waited=$((waited + 1))
      done
      wait "$pid"
      return $?
    }
  fi
fi
TINYACTOR="$PROJECT_DIR/tinyactor"

# Per-attempt wall-clock budget (seconds) for a single test. Most tests finish
# well within this; the ~5.5k-line typecheck-driven ones take ~13s locally and
# ~3x longer on slow CI runners, so 60s keeps them green without false
# timeouts. A runner may raise it for phases that are inherently slower (e.g.
# the GC stress round collects on every allocation — see run_gc_tests.sh).
TEST_TIMEOUT="${TEST_TIMEOUT:-60}"

# Skip list: tests known to be flaky (bash word-list matched per basename)
SKIP_LIST="echo_test.ta"

# is_skipped: return 0 if the given basename is in the skip list
is_skipped() {
  local base="$1"
  case " $SKIP_LIST " in
    *" $base "*) return 0 ;;
    *) return 1 ;;
  esac
}

# Negative tests (files named *-errors.ta / *-parse-errors.ta /
# *-module-errors.ta) assert that the compiler REJECTS the file: exit != 0 plus
# a message proving it was rejected for the intended reason. The suffix picks
# the contract and the table below is its single definition — run_test judges
# every kind with the same steps (rejected? then does the `// expect:` message
# match?), so adding a kind is adding a row.
#
# `;`-separated columns, no field may contain `;`:
#   suffix      basename glob; narrower kinds first, since *-errors.ta also
#               matches *-parse-errors.ta / *-module-errors.ta
#   accept-on   carrier of the "was it rejected?" ERE: log = whole log,
#               line = first line only
#   accept-RE   ERE searched in that carrier (exit != 0 is always required too)
#   expect-on   carrier of the `// expect:` fixed-string match: log | line
#   pass        verdict text when the rejection is as expected
#   fail        verdict text when the rejection is missing or its message off
#   fail-entry  FAILED_TESTS tail when the rejection is missing altogether
#
# The carriers are deliberately not uniform:
#   - module-errors reads the whole log: since 7b-4 the rejection comes from
#     driver.ta's import resolution, where the named messages "module not
#     found" / "invalid module name" (docs/modules.md E2: empty segment, ".."
#     segment, dotless identity) only appear on that path, while a crash dump
#     like "error: cdr: ..." would also satisfy a bare "error:" and mask the
#     bug (issue #105).
#   - parse-errors reads only line 1: that is where the driver reports a parse
#     error, so anything further down is somebody else's output and must not
#     count as this test having been rejected.
#   - errors accepts on line 1 ("type error" / "parse error") but matches
#     `// expect:` against the whole log — the typecheck header is line 1 and
#     the [E00xx] detail lines follow it (e.g. E0005, issue #118).
NEGATIVE_KINDS='
*-module-errors.ta;log;module not found|invalid module name;log;rejected at import resolution;expected import-resolution rejection;expected module-not-found rejection
*-parse-errors.ta;line;error:;line;rejected with error message;expected error rejection;expected error rejection
*-errors.ta;line;type error|parse error;log;rejected by compiler;expected compiler rejection;expected compiler rejection
'

# negative_row <basename> -> print the NEGATIVE_KINDS row for that file
# (empty output = not a negative test)
negative_row() {
  local base="$1" row suffix
  while read -r row; do
    case "$row" in ''|'#'*) continue ;; esac   # table padding / comment
    suffix="${row%%;*}"
    # suffix is a glob on purpose (*-errors.ta); quoting it would make every
    # negative test fall through to the positive branches.
    # shellcheck disable=SC2254
    case "$base" in $suffix) printf '%s\n' "$row"; return ;; esac
  done <<< "$NEGATIVE_KINDS"
}

# expected_pattern file -> print the first `// expect: <pattern>` line's
# pattern (fixed string, grep -F). Empty output = no expectation. Negative
# tests can use this to assert on the error message itself (e.g. the
# reported line number, issue #91) instead of only "was rejected".
expected_pattern() {
  local file="$1"
  grep -m1 '^// expect: ' "$file" 2>/dev/null | sed 's/^\/\/ expect: //'
}

# run_test: run a single .ta file via tinyactor run
run_test() {
  local file="$1"
  local base=$(basename "$file")
  local env_prefix="${2:-}"
  # Negative tests (errors/parse-errors/module-errors) since 7b-4 rely on
  # driver.ta's shared verification chain — same rejection messages.
  local log=$(mktemp "${TMPDIR:-/tmp}/tr_${base%.ta}_$$XXXXXX.log")

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "$base:"

  # Skip known-flaky network tests
  if is_skipped "$base"; then
    echo -e "${YELLOW}⏭  SKIP${NC} (flaky: port contention)"
    rm -f "$log"
    return
  fi

          # Retry on timeout for flaky network tests

  local max_attempts=3
    local exit_code=0
  local run_status=0
  local start=$SECONDS

  # Per-attempt budget is the runner-wide TEST_TIMEOUT, so a runner can widen
  # it for an inherently slower phase without touching this loop. The loop
  # still retries on 124, so a genuine hang costs max_attempts × TEST_TIMEOUT.
  local timeout_secs="$TEST_TIMEOUT"
  for ((attempt=1; attempt<=max_attempts; attempt++)); do
    if command -v timeout >/dev/null 2>&1; then
      timeout $timeout_secs bash -c "cd '$PROJECT_DIR' && $env_prefix '$TINYACTOR' run '$file'" >"$log" 2>&1
    else
      bash -c "cd '$PROJECT_DIR' && $env_prefix '$TINYACTOR' run '$file'" >"$log" 2>&1
    fi
    run_status=$?
    exit_code=$run_status
    # Retry timeouts (124) always. Also retry SIGABRT (134) for GC stress
    # tests: they are timing-sensitive and occasionally trip GC assertions
    # on slow CI runners, but pass reliably locally — a persistent failure
    # still surfaces after the final attempt.
    if [ $exit_code -ne 124 ] && { [ $exit_code -ne 134 ] || [[ "$file" != *"/gc/"* ]]; }; then
      break
    fi
  done

  local elapsed=$((SECONDS - start))
  local output=$(head -1 "$log")
  # 失败现场留痕：负例 FAIL 分支都拼上 log 头几行，CI 上才有得查
  local log_head=$(head -6 "$log" | tr '\n' '|' | cut -c1-500)

  # Output assertions for positive tests: `// expect: <pattern>` requires
  # the pattern to appear in the output (compiler warnings included);
  # `// expect-not: <pattern>` requires it to be absent. Fixed-string match.
  local expect_pat=""
  expect_pat=$(expected_pattern "$file")
  local expect_not=""
  expect_not=$(grep -m1 '^// expect-not: ' "$file" 2>/dev/null | sed 's/^\/\/ expect-not: //')

  # Negative test? Its NEGATIVE_KINDS row says what a rejection has to look
  # like here (which carrier each grep reads, and the verdict wording); the
  # judgment itself is the same for every kind.
  local neg_row=""
  neg_row=$(negative_row "$base")
  if [ -n "$neg_row" ]; then
    # column 1 (the suffix) is consumed by negative_row
    local accept_on accept_re expect_on pass_msg fail_msg fail_entry
    IFS=';' read -r _ accept_on accept_re expect_on pass_msg fail_msg fail_entry <<< "$neg_row"
    local accept_text="$output" expect_text="$output"
    if [ "$accept_on" = log ]; then accept_text=$(cat "$log"); fi
    if [ "$expect_on" = log ]; then expect_text=$(cat "$log"); fi
    if [ $exit_code -ne 0 ] && grep -qE "$accept_re" <<< "$accept_text"; then
      if [ -n "$expect_pat" ] && ! grep -qF "$expect_pat" <<< "$expect_text"; then
        echo -e "${RED}❌ FAIL${NC} (rejected but output missing: $expect_pat) (${elapsed}s)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("run $base (missing expected: $expect_pat)")
      else
        echo -e "${GREEN}✅ PASS${NC} ($pass_msg) (${elapsed}s)"
        PASSED=$((PASSED + 1))
      fi
    else
      echo -e "${RED}❌ FAIL${NC} ($fail_msg; log: $log_head) (${elapsed}s)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("run $base ($fail_entry)")
    fi
    rm -f "$log"
    return
  fi

  if [ $exit_code -eq 139 ]; then
    echo -e "${RED}❌ FAIL${NC} (SEGFAULT) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (SEGFAULT)")
    elif [ $exit_code -eq 124 ]; then
    echo -e "${RED}❌ FAIL${NC} (TIMEOUT) (${elapsed}s)"
    # 超时现场：被杀前捕获的 stdout/stderr 留痕（stderr 含 run_tavm 的
    # rebuild 标记与 driver 阶段标记），否则 CI 挂死无法定位卡在哪个阶段
    if [ -n "$output" ]; then
      echo "$output" | tail -8 | sed 's/^/    | /'
    fi
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (TIMEOUT)")
    elif [ $exit_code -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (exit $exit_code) (${elapsed}s)"
    # 失败现场：正例 exit!=0 时把捕获的输出留痕，CI 上才有得查
    sed 's/^/    | /' "$log" | tail -10
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base")
  elif [ -z "$output" ]; then
    echo -e "${RED}❌ FAIL${NC} (NO OUTPUT) (${elapsed}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run $base (NO OUTPUT)")
  else
    local fail=""
    if [ -n "$expect_pat" ] && ! grep -qF "$expect_pat" "$log"; then
      fail="missing expected: $expect_pat"
    fi
    if [ -z "$fail" ] && [ -n "$expect_not" ] && grep -qF "$expect_not" "$log"; then
      fail="unexpected output: $expect_not"
    fi
    if [ -n "$fail" ]; then
      echo -e "${RED}❌ FAIL${NC} ($fail) (${elapsed}s)"
      # 失败现场：期望输出缺失时同样留痕（CI 上才有得查）
      sed 's/^/    | /' "$log" | tail -10
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("run $base ($fail)")
    else
      echo -e "${GREEN}✅ PASS${NC} - \"$output\" (${elapsed}s)"
      PASSED=$((PASSED + 1))
    fi
  fi
  rm -f "$log"
}


# run_category: run all .ta files in a directory
run_category() {
  local cat_name="$1"
  local cat_dir="$2"
  echo -e "${BLUE}[${cat_name}]${NC}"
  local all_files=("$cat_dir"/*.ta)
  for f in "${all_files[@]}"; do
    [ -f "$f" ] || continue
    run_test "$f"
  done
  echo ""
}

# print_summary: print test results
print_summary() {
  echo -e "${BLUE}=========================================${NC}"
  echo -e "${BLUE}Test Summary${NC}"
  echo -e "${BLUE}=========================================${NC}"
  echo "Total:   $TOTAL"
  echo -e "Passed:  ${GREEN}$PASSED${NC}"
  if [ $FAILED -gt 0 ]; then
    echo -e "Failed:  ${RED}$FAILED${NC}"
  else
    echo "Failed:  $FAILED"
  fi
  echo ""
    if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
    echo -e "${YELLOW}Failed tests:${NC}"
    for t in "${FAILED_TESTS[@]}"; do
      echo -e "  ${RED}❌${NC} $t"
    done
    echo ""
  fi
}
