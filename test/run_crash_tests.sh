#!/bin/bash
# test/run_crash_tests.sh — crash visibility tests (issue #28).
#
# Verifies that runtime crashes are observable:
#   a) an actor's div-zero crash prints a CRASH report to stderr (pid,
#      reason symbol, stack frames leaf..root) while the main process
#      survives and the program still exits 0;
#   b) a main-process crash makes tavm exit non-zero;
#   c) a normal program exits 0 with no CRASH on stderr.
#
# Normal category runners (run_test in lib.sh) assert exit 0 and only
# check stdout, so these cases need bespoke assertions (stderr content +
# exit codes). Sourced by test/run_actor_tests.sh after lib.sh and invoked
# via run_crash_tests, so the shared PASSED/FAILED counters stay in one
# process. Can also be run directly: bash test/run_crash_tests.sh

run_crash_tests() {
  local crash_dir="$SCRIPT_DIR/crash"

  # --- run_crash_case: build a .ta, run the .tabc, capture rc + logs ----
  # Sets CRASH_RC / CRASH_EXPECT / CRASH_ERRLOG / CRASH_OUTLOG / CRASH_ID
  # for the assertion helpers below.
  run_crash_case() {
    local id="$1" file="$2" expect_rc="$3"
    local out=$(mktemp "${TMPDIR:-/tmp}/crash_${id}_$$XXXXXX.tabc")
    local outlog=$(mktemp "${TMPDIR:-/tmp}/crash_${id}_$$XXXXXX.out")
    local errlog=$(mktemp "${TMPDIR:-/tmp}/crash_${id}_$$XXXXXX.err")

    TOTAL=$((TOTAL + 1))
    printf "  %-50s " "$id:"

    local build_rc=0
    if command -v timeout >/dev/null 2>&1; then
      timeout 60 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$out'" >"$outlog" 2>&1
    else
      bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$file' '$out'" >"$outlog" 2>&1
    fi
    build_rc=$?
    if [ $build_rc -ne 0 ] || [ ! -s "$out" ]; then
      echo -e "${RED}❌ FAIL${NC} (build failed)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("crash $id (build failed)")
      rm -f "$out" "$outlog" "$errlog"
      return 1
    fi

    local run_rc=0
    if command -v timeout >/dev/null 2>&1; then
      timeout 60 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$out'" >"$outlog" 2>"$errlog"
    else
      bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$out'" >"$outlog" 2>"$errlog"
    fi
    run_rc=$?
    rm -f "$out"

    CRASH_RC=$run_rc
    CRASH_EXPECT=$expect_rc
    CRASH_ERRLOG=$errlog
    CRASH_OUTLOG=$outlog
    CRASH_ID=$id
    return 0
  }

  # --- assertion helpers (consume the CRASH_* globals) ---
  crash_ok() {
    echo -e "${GREEN}✅ PASS${NC}"
    PASSED=$((PASSED + 1))
    rm -f "$CRASH_OUTLOG" "$CRASH_ERRLOG"
  }

  crash_fail() {
    local why="$1"
    echo -e "${RED}❌ FAIL${NC} ($why)"
    sed 's/^/      | /' "$CRASH_ERRLOG"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("crash $CRASH_ID ($why)")
    rm -f "$CRASH_OUTLOG" "$CRASH_ERRLOG"
  }

  assert_exit() { [ "$CRASH_RC" -eq "$CRASH_EXPECT" ]; }

  assert_stderr_has() { # needles... — every one must appear in stderr
    local n
    for n in "$@"; do
      grep -qF "$n" "$CRASH_ERRLOG" || return 1
    done
    return 0
  }

  assert_stderr_lacks() { # needles... — none may appear in stderr
    local n
    for n in "$@"; do
      grep -qF "$n" "$CRASH_ERRLOG" && return 1
    done
    return 0
  }

  # ---------------------------------------------------------------
  # (a) actor div-zero crash: report on stderr, main survives, rc=0
  # ---------------------------------------------------------------
  run_crash_case "actor-crash" "$crash_dir/actor-crash.ta" 0
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at crasher"; then
    crash_ok
  else
    crash_fail "expected rc=0, stderr CRASH report with pid + 'divzero + crasher frame"
  fi

  # ---------------------------------------------------------------
  # (b) main process crash: tavm exits non-zero + report
  # ---------------------------------------------------------------
  run_crash_case "main-crash" "$crash_dir/main-crash.ta" 1
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at main"; then
    crash_ok
  else
    crash_fail "expected rc=1, stderr CRASH report with pid + 'divzero + main frame"
  fi

  # ---------------------------------------------------------------
  # (c) normal program: rc=0, no CRASH on stderr
  # ---------------------------------------------------------------
  run_crash_case "normal-exit" "$crash_dir/normal-exit.ta" 0
  if assert_exit && assert_stderr_lacks "CRASH"; then
    crash_ok
  else
    crash_fail "expected rc=0 and no CRASH on stderr"
  fi
}

# Allow direct execution: bash test/run_crash_tests.sh
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  source "$(dirname "$0")/lib.sh"
  run_crash_tests
  print_summary
  exit $([ $FAILED -eq 0 ])
fi