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

echo "CLI TEST PASS"