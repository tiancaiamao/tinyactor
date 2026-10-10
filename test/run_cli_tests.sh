#!/usr/bin/env bash
# run_cli_tests.sh — CLI behavior from outside the TinyActor checkout.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TINYACTOR="${TINYACTOR:-$PROJECT_DIR/tinyactor}"
LISPVM="${LISPVM:-$PROJECT_DIR/lispvm}"
LISP_DRIVER_BC="$PROJECT_DIR/vm-demo/lisp/boot/backend_driver.tabc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$TINYACTOR" ] || { echo "CLI TEST FAIL: tinyactor not executable: $TINYACTOR" >&2; exit 1; }
[ -x "$LISPVM" ] || { echo "CLI TEST FAIL: lispvm not executable: $LISPVM" >&2; exit 1; }
[ -s "$LISP_DRIVER_BC" ] || { echo "CLI TEST FAIL: lisp backend driver missing: $LISP_DRIVER_BC" >&2; exit 1; }

# lisp 编译半程：与 tinyactor run 的 run_lisp 同参布局（driver 编 .tabc）。
# 测试直接喂 driver 是为了在 .tabc 之上测 VM 级 flag（--profile /
# TA_DUMP_INTERNS）——tinyactor run 不透传它们。
compile_lisp() {
    local src="$1" out="$2"
    local cache="${TA_SIG_CACHE_DIR:-$PROJECT_DIR/.build/modules}"
    (cd "$PROJECT_DIR" && env -u TA_ACTOR_HEAP "$LISPVM" -q "$LISP_DRIVER_BC" "$src" "$out" "$cache")
}

cat > "$WORK/fs_cli.ta" <<'EOF'
import fs
fn main() {
  if fs.mkdir_p("cli-test-output/nested") == 1 {
    print("CLI RUN PASS")
  } else {
    print("CLI RUN FAIL")
  }
}
EOF

(cd "$WORK" && "$TINYACTOR" run fs_cli.ta | grep -qx 'CLI RUN PASS')
[ -d "$WORK/cli-test-output/nested" ] || { echo "CLI TEST FAIL: run did not create directory" >&2; exit 1; }
echo "ok run from external CWD"

cat > "$WORK/fmt.ta" <<'EOF'
fn main(){print("fmt")}
EOF
(cd "$WORK" && "$TINYACTOR" fmt fmt.ta)
(cd "$WORK" && "$TINYACTOR" fmt --check fmt.ta)
echo "ok fmt from external CWD"

if (cd "$WORK" && "$TINYACTOR" run missing.ta) >"$WORK/missing.out" 2>&1; then
  echo "CLI TEST FAIL: missing source unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: TinyActor source file not found: $WORK/missing.ta" "$WORK/missing.out"
echo "ok missing source diagnostic"

if (cd "$WORK" && "$TINYACTOR" fmt --check missing.ta) >"$WORK/missing-fmt.out" 2>&1; then
  echo "CLI TEST FAIL: missing fmt source unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: TinyActor source file not found: $WORK/missing.ta" "$WORK/missing-fmt.out"
echo "ok missing fmt source diagnostic"

# Sampling profiler smoke test: --profile must produce speedscope json +
# folded stacks even for a trivial program (exercises src/prof.c sampling,
# collection and both writers; the plain suite never enables the flag).
cat > "$WORK/prof.ta" <<'EOF'
fn work(n) {
  if n == 0 { 0 } else { work(n - 1) + 1 }
}

fn main() {
  work(200000)
  print("PROF RUN PASS")
}
EOF
compile_lisp "$WORK/prof.ta" "$WORK/prof.tabc"
[ -s "$WORK/prof.tabc" ] || { echo "CLI TEST FAIL: lisp compile produced no bytecode" >&2; exit 1; }
echo "ok lisp backend compile"

"$LISPVM" -q --profile="$WORK/prof-out" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'
[ -s "$WORK/prof-out.json" ] || { echo "CLI TEST FAIL: --profile produced no speedscope json" >&2; exit 1; }
[ -s "$WORK/prof-out.folded" ] || { echo "CLI TEST FAIL: --profile produced no folded stacks" >&2; exit 1; }
grep -qF "work" "$WORK/prof-out.folded" || {
  echo "CLI TEST FAIL: folded stacks contain no function names" >&2
  exit 1
}
echo "ok --profile smoke"

# Intern-table dump diagnostic: TA_DUMP_INTERNS=<path> must write one
# "idx name" line per interned symbol after a normal run (PR #104).
TA_DUMP_INTERNS="$WORK/interns.txt" "$LISPVM" -q "$WORK/prof.tabc" > /dev/null
[ -s "$WORK/interns.txt" ] || { echo "CLI TEST FAIL: TA_DUMP_INTERNS produced no dump" >&2; exit 1; }
grep -qE '^0 [a-z]' "$WORK/interns.txt" || {
  echo "CLI TEST FAIL: intern dump missing 'idx name' lines" >&2
  exit 1
}
echo "ok TA_DUMP_INTERNS"

# Loader hardening: empty and truncated images must be rejected with a
# non-zero exit, never executed (lispvm parse_unit file/hardening paths).
: > "$WORK/empty.tabc"
if "$LISPVM" "$WORK/empty.tabc" > "$WORK/empty.out" 2>&1; then
  echo "CLI TEST FAIL: empty .tabc unexpectedly loaded" >&2
  exit 1
fi
head -c 20 "$WORK/prof.tabc" > "$WORK/trunc.tabc"
if "$LISPVM" "$WORK/trunc.tabc" > "$WORK/trunc.out" 2>&1; then
  echo "CLI TEST FAIL: truncated .tabc unexpectedly loaded" >&2
  exit 1
fi
echo "ok empty/truncated .tabc rejected"

# TA_MAX_PROCS: invalid values are ignored (never truncate the table);
# the program still runs normally. api.c 的进程表 cap 同时约束 lispvm。
if TA_MAX_PROCS=notanumber "$TINYACTOR" run "$WORK/prof.ta" | grep -qx 'PROF RUN PASS' &&
   TA_MAX_PROCS=1 "$TINYACTOR" run "$WORK/prof.ta" | grep -qx 'PROF RUN PASS'; then
  echo "ok TA_MAX_PROCS invalid values ignored"
else
  echo "CLI TEST FAIL: TA_MAX_PROCS=invalid broke a normal run" >&2
  exit 1
fi

echo "CLI TEST PASS"