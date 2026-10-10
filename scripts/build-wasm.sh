#!/usr/bin/env bash
#
# build-wasm.sh — Reproducible Emscripten build of the lisp VM (wasm).
#
# Produces docs/wasm/tinyactor-vm.js + docs/wasm/tinyactor-vm.wasm for the
# Playground page (browser-side compile + run of TA code):
#
#   * C VM (src/*.c + src/tavm.c) compiled to wasm by emcc with
#     zero source changes. Single-threaded: no -pthread, so the io poller
#     thread cannot start and degrades to absent (scheduler.c
#     vm_poller_start) — pure compute and synchronous sends work; io
#     readiness / timeout wakeups are not serviced (Playground scope).
#     src/tls.c (OpenSSL) is swapped for a link stub: the TA tls module is
#     simply unregistered in wasm — tls.* misses to nil at runtime.
#   * Virtual FS payload embedded into the wasm:
#       - lib/bootstrap.tabc — lisp compile-half driver
#       - lib/*.ta, lib/bootstrap/*.ta        — TA modules the driver
#                                               resolves at compile time
#       - hello.ta / hello.tabc                 — sample (print(1 + 41) → 42),
#                                               precompiled by ./tavm
#   * MODULARIZE + callMain/FS exports — the JS bridge used by the Playground:
#         callMain(['-q','lib/bootstrap.tabc',
#                    'user.ta','user.tabc',''])   // compile
#         callMain(['-q','user.tabc'])            // run
#
# All intermediate artifacts go to a temp dir (cleaned up on exit); the only
# repository output is docs/wasm/. The script is idempotent and repeatable.
#
# Usage:
#   scripts/build-wasm.sh [options]
#     -v | --verbose    print every command + payload details
#     --no-verify       skip the node closure checks (build only)
#     -h | --help       show this help
#
# Requires: emcc (Emscripten), node, and a built ./tavm (`make tavm`).
#
set -euo pipefail

VERBOSE=0
VERIFY=1

usage() {
    sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        --no-verify) VERIFY=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build-wasm.sh: unknown option: $arg" >&2; usage >&2; exit 1 ;;
    esac
done

# --- Paths ---------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/docs/wasm"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/tavm-wasm.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

log()  { printf '[build-wasm] %s\n' "$*"; }
run() {
    if [ "$VERBOSE" -eq 1 ]; then printf '+ %s\n' "$*" >&2; fi
    "$@"
}

# --- Toolchain checks ----------------------------------------------------
command -v emcc >/dev/null 2>&1 || { echo "build-wasm.sh: emcc not found (brew install emscripten)" >&2; exit 1; }
command -v cc   >/dev/null 2>&1 || { echo "build-wasm.sh: cc not found" >&2; exit 1; }
[ -x "$REPO_ROOT/tavm" ] || { echo "build-wasm.sh: ./tavm not found — run 'make tavm' first" >&2; exit 1; }
[ -s "$REPO_ROOT/lib/bootstrap.tabc" ] || {
    echo "build-wasm.sh: lib/bootstrap.tabc missing — run 'make bootstrap'" >&2
    exit 1
}
if [ "$VERIFY" -eq 1 ]; then
    command -v node >/dev/null 2>&1 || { echo "build-wasm.sh: node not found (needed for --verify)" >&2; exit 1; }
fi

log "repo root: $REPO_ROOT"
log "emcc:      $(emcc --version 2>/dev/null | head -1)"

# --- 1. Embedded payload --------------------------------------------------
# lib/: TA modules the driver falls back to for `import <name>` in user code
# (lib/<name>.ta, then lib/bootstrap/<name>.ta). Only .ta sources — C-backed
# host modules (str, buf…) are compiled into the wasm itself.
mkdir -p "$TMP/payload/lib/bootstrap"
cp "$REPO_ROOT"/lib/*.ta "$TMP/payload/lib/"
cp "$REPO_ROOT"/lib/bootstrap/*.ta "$TMP/payload/lib/bootstrap/"
cp "$REPO_ROOT/lib/bootstrap.tabc" "$TMP/payload/driver.tabc"
mv "$TMP/payload/driver.tabc" "$TMP/payload/lib/bootstrap.tabc"

# Sample program (golden: print(1 + 41) → 42).
cat > "$TMP/payload/hello.ta" <<'EOF'
fn main() {
  print(1 + 41)
}
EOF

# Precompile the sample with the real lisp pipeline (repo tavm driving the
# committed driver.tabc) so the wasm can run it standalone without a compile
# step — same closure discipline as the old tavm build.
log "precompiling hello.tabc (repo tavm + bootstrap.tabc)"
( cd "$TMP/payload" && "$REPO_ROOT/tavm" -q "$REPO_ROOT/lib/bootstrap.tabc" hello.ta hello.tabc "" )

if [ "$VERBOSE" -eq 1 ]; then
    log "payload:"
    ( cd "$TMP/payload" && du -ah hello.ta hello.tabc lib | sort -k2 )
fi

# --- 2. Emscripten build ---------------------------------------------------
# VM sources = src/*.c except tls.c (OpenSSL — not buildable under
# emscripten; replaced by a registration stub so tavm.c's
# vm_register_tls_module call still links).
VM_SRCS=()
for f in "$REPO_ROOT"/src/*.c; do
    [ "$f" = "$REPO_ROOT/src/tls.c" ] && continue
    VM_SRCS+=("$f")
done
cat > "$TMP/tls_stub.c" <<'EOF'
#include "ta.h"
/* wasm build stub for src/tls.c (OpenSSL unavailable under emscripten):
 * the tls module is not registered, so tls.* misses to nil at runtime. */
void vm_register_tls_module(VM *vm) { (void)vm; }
EOF

# `--embed-file lib` etc. are relative to the payload dir so the embedded
# virtual FS paths are /lib/... , /hello.ta , /hello.tabc ,
# /lib/bootstrap.tabc.
log "emcc build -> $OUT_DIR"
mkdir -p "$TMP/out" "$OUT_DIR"
EMCC_FLAGS=(
    -Wall -Wextra -std=c99 -O2
    # musl gates POSIX declarations (mkstemp/fdopen in cov.c) behind feature
    # macros; native macOS headers expose them unconditionally.
    -D_GNU_SOURCE
    -I"$REPO_ROOT"
    "${VM_SRCS[@]}"
    "$TMP/tls_stub.c"
    "$REPO_ROOT/src/tavm.c"
    --embed-file lib
    --embed-file hello.ta
    --embed-file hello.tabc
    --embed-file lib/bootstrap.tabc
    -s MODULARIZE
    -s EXPORT_NAME=createTavm
    -s EXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8
    # Do not auto-run main() at module load: the Playground drives the VM
    # explicitly via callMain/FS after createTavm({noInitialRun:true}).
    -s INVOKE_RUN=0
    -s ALLOW_MEMORY_GROWTH=1
)
run bash -c 'cd "$1" && shift && emcc "$@"' _ "$TMP/payload" "${EMCC_FLAGS[@]}" -o "$TMP/out/tinyactor-vm.js"

cp "$TMP/out/tinyactor-vm.js" "$TMP/out/tinyactor-vm.wasm" "$OUT_DIR/"

JS_SIZE="$(stat -f%z "$OUT_DIR/tinyactor-vm.js" 2>/dev/null || stat -c%s "$OUT_DIR/tinyactor-vm.js")"
WASM_SIZE="$(stat -f%z "$OUT_DIR/tinyactor-vm.wasm" 2>/dev/null || stat -c%s "$OUT_DIR/tinyactor-vm.wasm")"
log "artifacts: docs/wasm/tinyactor-vm.js ($JS_SIZE B), tinyactor-vm.wasm ($WASM_SIZE B)"

# --- 3. Closure verification (node) ---------------------------------------
if [ "$VERIFY" -eq 1 ]; then
    cat > "$TMP/verify.js" <<'EOF'
// Closure checks for docs/wasm/tinyactor-vm.js
const assert = require('node:assert');
const createTavm = require('TMPOUT/tinyactor-vm.js');

async function makeModule() {
  const lines = [];
  globalThis.__lastLines = lines;
  const mod = await createTavm({
    noInitialRun: true,          // no auto-run of main() at load
    print: (s) => lines.push(String(s)),
    printErr: (s) => lines.push(String(s)),
  });
  return { mod, lines };
}

async function main() {
  // Closure 1 (minimal): run the embedded hello.tabc -> 42
  {
    const { mod, lines } = await makeModule();
    const rc = mod.callMain(['-q', 'hello.tabc']);
    assert.strictEqual(rc, 0, 'minimal run exit code');
    assert.ok(lines.includes('42'), 'minimal run stdout should contain 42, got: ' + JSON.stringify(lines));
    console.log('PASS minimal: hello.tabc ->', JSON.stringify(lines));
  }
  // Closure 2 (full): compile hello.ta -> hello-out.tabc inside wasm (driver.tabc
  // resolves imports from the embedded /lib), then run it -> 42
  {
    const { mod, lines } = await makeModule();
    const rc = mod.callMain(['-q', 'lib/bootstrap.tabc',
                             'hello.ta', 'hello-out.tabc', '']);
    assert.strictEqual(rc, 0, 'compile exit code');
    const bytes = mod.FS.readFile('hello-out.tabc');
    assert.ok(bytes.length > 0, 'compiled bytecode should be non-empty');
    lines.length = 0;
    const rc2 = mod.callMain(['-q', 'hello-out.tabc']);
    assert.strictEqual(rc2, 0, 'run exit code');
    assert.ok(lines.includes('42'), 'compiled run stdout should contain 42, got: ' + JSON.stringify(lines));
    console.log('PASS full: hello.ta -> hello-out.tabc (' + bytes.length + 'B) ->', JSON.stringify(lines));
  }
  console.log('ALL VERIFICATIONS PASSED');
  process.exit(0);
}

main().catch((e) => {
  if (typeof globalThis.__lastLines !== 'undefined') {
    console.error('captured output:', JSON.stringify(globalThis.__lastLines));
  }
  console.error('VERIFY FAILED:', e); process.exit(1);
});
EOF
    sed -i.bak "s|TMPOUT|$TMP/out|" "$TMP/verify.js" && rm -f "$TMP/verify.js.bak"
    log "verifying closures with node"
    node "$TMP/verify.js"
fi

log "done. artifacts in docs/wasm/"