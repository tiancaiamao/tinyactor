#!/usr/bin/env bash
# run_fmt_guard_tests.sh — clang-format version guard for `make fmt` /
# `make fmt-check` (issue #263).
#
# CI's fmt-check runs only on the macOS job with brew llvm@18 pinned
# (18.1.8). A machine whose PATH resolves a different clang-format major
# would realign tavm.c macro-continuation backslashes and churn pure
# whitespace, so the guard must reject the run BEFORE any formatting
# happens — a rejected run must leave the working tree untouched.
#
# Both cases use PATH shims, so the test is independent of whatever
# clang-format the host has installed.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

# Invoked from a `make test` recipe, this nested make would inherit the
# parent jobserver flags; drop them for a plain serial invocation.
unset MAKEFLAGS MFLAGS MAKELEVEL

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SHIM="$WORK/shim"
mkdir -p "$SHIM"

fail() { echo "FMT GUARD TEST FAIL: $*" >&2; exit 1; }

dump() {
  echo "--- $1 ---" >&2
  cat "$1" >&2
  echo "--- end $1 ---" >&2
}

before="$(git status --porcelain)"

# --- wrong major: both targets must fail the guard, before formatting ---
cat > "$SHIM/clang-format" <<'EOF'
#!/bin/sh
echo "clang-format version 99.0.0"
EOF
chmod +x "$SHIM/clang-format"

for target in fmt fmt-check; do
  log="$WORK/$target-wrongver.out"
  if PATH="$SHIM:$PATH" make "$target" >"$log" 2>&1; then
    dump "$log"
    fail "make $target accepted clang-format 99.0.0"
  fi
  grep -q "99.0.0" "$log" || { dump "$log"; fail "make $target error lacks actual version"; }
  grep -q "expected" "$log" || { dump "$log"; fail "make $target error lacks expected version"; }
  grep -q "brew install llvm@18" "$log" || { dump "$log"; fail "make $target error lacks install hint"; }
done

after="$(git status --porcelain)"
[ "$before" = "$after" ] || fail "wrong-version run modified the working tree: $after"
echo "ok guard rejects wrong version (fails, names expected/actual/install, touches nothing)"

# --- pinned major: both targets must run through, tree unchanged ---
cat > "$SHIM/clang-format" <<'EOF'
#!/bin/sh
case "${1:-}" in
  --version) echo "Homebrew clang-format version 18.1.8" ;;
esac
exit 0
EOF

for target in fmt fmt-check; do
  log="$WORK/$target-okver.out"
  if ! PATH="$SHIM:$PATH" make "$target" >"$log" 2>&1; then
    dump "$log"
    fail "make $target rejected clang-format 18.1.8"
  fi
done

after="$(git status --porcelain)"
[ "$before" = "$after" ] || fail "correct-version run modified the working tree: $after"
echo "ok guard passes pinned version (fmt + fmt-check run, tree unchanged)"

echo "FMT GUARD PASS"