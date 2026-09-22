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

echo "CLI TEST PASS"