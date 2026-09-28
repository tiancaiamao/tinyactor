#!/usr/bin/env bash
# run_cli_tests.sh — CLI behavior from outside the TinyActor checkout.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TINYACTOR="${TINYACTOR:-$PROJECT_DIR/tinyactor}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$TINYACTOR" ] || { echo "CLI TEST FAIL: tinyactor not executable: $TINYACTOR" >&2; exit 1; }

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

(cd "$WORK" && "$TINYACTOR" build fs_cli.ta fs_cli.tabc)
[ -s "$WORK/fs_cli.tabc" ] || { echo "CLI TEST FAIL: build produced no bytecode" >&2; exit 1; }
echo "ok build from external CWD"

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

# Bytecode version gate: v2 images still encoded the actor primitives as their
# own opcodes, so a v3 VM must refuse them instead of dispatching reserved
# numbers. Patch the version field of a good image down to 2 and require the
# loader to name the version it saw.
cp "$WORK/fs_cli.tabc" "$WORK/v2.tabc"
python3 - "$WORK/v2.tabc" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
assert data[:4] == b"TABC", "not a .tabc image"
data[4:8] = (2).to_bytes(4, "little")
open(path, "wb").write(data)
PY
TAVM_BIN="${TAVM:-$PROJECT_DIR/tavm}"
if "$TAVM_BIN" "$WORK/v2.tabc" >"$WORK/v2.out" 2>&1; then
  echo "CLI TEST FAIL: v2 .tabc unexpectedly loaded" >&2
  exit 1
fi
grep -qF "version 2" "$WORK/v2.out" || {
  echo "CLI TEST FAIL: v2 .tabc rejection did not report the version" >&2
  cat "$WORK/v2.out" >&2
  exit 1
}
echo "ok v2 .tabc rejected with version diagnostic"

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
(cd "$WORK" && "$TINYACTOR" build prof.ta prof.tabc)
[ -s "$WORK/prof.tabc" ] || { echo "CLI TEST FAIL: prof build produced no bytecode" >&2; exit 1; }
"$TAVM_BIN" --profile="$WORK/prof-out" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'
[ -s "$WORK/prof-out.json" ] || { echo "CLI TEST FAIL: --profile produced no speedscope json" >&2; exit 1; }
[ -s "$WORK/prof-out.folded" ] || { echo "CLI TEST FAIL: --profile produced no folded stacks" >&2; exit 1; }
grep -qF "work" "$WORK/prof-out.folded" || {
  echo "CLI TEST FAIL: folded stacks contain no function names" >&2
  exit 1
}
echo "ok --profile smoke"

# Intern-table dump diagnostic: TA_DUMP_INTERNS=<path> must write one
# "idx name" line per interned symbol after a normal run (PR #104).
"$TAVM_BIN" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'
TA_DUMP_INTERNS="$WORK/interns.txt" "$TAVM_BIN" "$WORK/prof.tabc" > /dev/null
[ -s "$WORK/interns.txt" ] || { echo "CLI TEST FAIL: TA_DUMP_INTERNS produced no dump" >&2; exit 1; }
grep -qE '^0 [a-z]' "$WORK/interns.txt" || {
  echo "CLI TEST FAIL: intern dump missing 'idx name' lines" >&2
  exit 1
}
echo "ok TA_DUMP_INTERNS"

# Loader hardening: empty and truncated .tabc files must be rejected with
# a non-zero exit, never executed (vm_load_tabc file/append error paths).
: > "$WORK/empty.tabc"
if "$TAVM_BIN" "$WORK/empty.tabc" > "$WORK/empty.out" 2>&1; then
  echo "CLI TEST FAIL: empty .tabc unexpectedly loaded" >&2
  exit 1
fi
head -c 20 "$WORK/prof.tabc" > "$WORK/trunc.tabc"
if "$TAVM_BIN" "$WORK/trunc.tabc" > "$WORK/trunc.out" 2>&1; then
  echo "CLI TEST FAIL: truncated .tabc unexpectedly loaded" >&2
  exit 1
fi
echo "ok empty/truncated .tabc rejected"

# Coverage instrumentation: `build --cov` rewrites fn entries AND every
# statement / match-arm body to cov.hit(k) and emits a <out>.covmap side
# table ("k file:line name" rows, line 0 = fn entry). Running the
# instrumented image and dumping must reproduce the covmap ids with the
# right counts (join-ability is the whole contract), including missed
# statements reporting count 0.
cat > "$WORK/covprog.ta" <<'EOF'
fn leaf(x) {
  x + 1
}

fn grade(x) {
  let y = x + 1
  match y {
    1 -> 10
    _ -> 20
  }
}

fn main() {
  let a = leaf(1)
  let g = grade(0)
  cov.hit(99)
  cov.dump("/tmp/tinyactor-cov-cli-dump.txt")
  print(a + g)
}

fn noop() { }
EOF
(cd "$WORK" && "$TINYACTOR" build --cov covprog.ta covprog.tabc)
[ -s "$WORK/covprog.tabc" ] || { echo "CLI TEST FAIL: --cov build produced no bytecode" >&2; exit 1; }
grep -qE '^0 .*covprog\.ta:0 leaf$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '0 covprog.ta:0 leaf'" >&2
  exit 1
}
grep -qE '^2 .*covprog\.ta:0 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '2 covprog.ta:0 grade'" >&2
  exit 1
}
grep -qE '^3 .*covprog\.ta:6 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing statement '3 covprog.ta:6 grade'" >&2
  exit 1
}
grep -qE '^5 .*covprog\.ta:8 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '5 covprog.ta:8 grade'" >&2
  exit 1
}
grep -qE '^6 .*covprog\.ta:9 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '6 covprog.ta:9 grade'" >&2
  exit 1
}
grep -qE '^7 .*covprog\.ta:0 main$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '7 covprog.ta:0 main'" >&2
  exit 1
}
# Empty-bodied fn: entry row with an empty file component (regression).
grep -qE '^13 :0 noop$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '13 :0 noop' (empty fn body)" >&2
  exit 1
}
"$TAVM_BIN" "$WORK/covprog.tabc" | grep -qx '12'
grep -qE '^0 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '0 1' (leaf hit once)" >&2
  exit 1
}
grep -qE '^1 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '1 1' (leaf body hit once)" >&2
  exit 1
}
grep -qE '^5 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '5 1' (taken match arm hit once)" >&2
  exit 1
}
grep -qE '^6 0$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '6 0' (untaken match arm counted zero)" >&2
  exit 1
}
grep -qE '^7 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '7 1' (main hit once)" >&2
  exit 1
}
grep -qE '^99 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing user hit id 99" >&2
  exit 1
}
echo "ok --cov instrument + covmap + dump"

# coverage_html.ta smoke: TA-native go-cover-style HTML report.
mkdir -p "$WORK/dumps"
cp /tmp/tinyactor-cov-cli-dump.txt "$WORK/dumps/prog.cov"
printf '%s\n' "$WORK/uncovered.ta" > "$WORK/sources.txt"
printf 'fn untouched() {\n  1\n}\n' > "$WORK/uncovered.ta"
"$TINYACTOR" run "$PROJECT_DIR/tools/coverage_html.ta" \
  "$WORK/covprog.tabc.covmap" "$WORK/dumps" "$WORK/coverage.html" "$WORK/misses.txt" "$WORK/sources.txt" || {
  echo "CLI TEST FAIL: coverage_html.ta crashed" >&2
  exit 1
}
grep -q 'class="l hit"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no hit lines" >&2
  exit 1
}
grep -q 'class="l miss"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no miss lines (missed match arm)" >&2
  exit 1
}
grep -q '<select id="source-select"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no source selector" >&2
  exit 1
}
grep -q 'function select_source' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no source selection handler" >&2
  exit 1
}
grep -q "data-path=\"$WORK/uncovered.ta\"" "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html omitted a zero-execution source from catalog" >&2
  exit 1
}
grep -A8 "data-path=\"$WORK/uncovered.ta\"" "$WORK/coverage.html" | grep -q 'class="l miss"' || {
  echo "CLI TEST FAIL: zero-execution source lines were not marked missed" >&2
  exit 1
}


if grep -q 'coverage-ta-sources.txt' "$PROJECT_DIR/Makefile"; then :; else
  echo "CLI TEST FAIL: coverage target does not create full-repo source catalog" >&2
  exit 1
fi

grep -q '^line ' "$WORK/misses.txt" || {
  echo "CLI TEST FAIL: misses.txt has no line entry (missed match arm)" >&2
  exit 1
}
echo "ok coverage html"

# TA_MAX_PROCS: invalid values are ignored (never truncate the table);
# the program still runs normally.
if TA_MAX_PROCS=notanumber "$TAVM_BIN" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS' &&
   TA_MAX_PROCS=1 "$TAVM_BIN" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'; then
  echo "ok TA_MAX_PROCS invalid values ignored"
else
  echo "CLI TEST FAIL: TA_MAX_PROCS=invalid broke a normal run" >&2
  exit 1
fi

echo "CLI TEST PASS"