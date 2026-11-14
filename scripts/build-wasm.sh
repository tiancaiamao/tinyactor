#!/usr/bin/env bash
#
# build-wasm.sh — Reproducible Emscripten build of the backend VM (tavm, wasm).
#
# Produces docs/wasm/tinyactor-vm.js + docs/wasm/tinyactor-vm.wasm for the
# Playground page (browser-side compile + run of TA code):
#
#   * C VM (src/*.c + src/main.c) compiled to wasm by emcc with
#     zero source changes. Single-threaded: no -pthread, so the io poller
#     thread cannot start and degrades to absent (scheduler.c
#     vm_poller_start) — pure compute and synchronous sends work; io
#     readiness / timeout wakeups are not serviced (Playground scope).
#     src/tls.c moved to a lazy dylib (lib/tls.dylib, OpenSSL) — the wasm
#     build carries no tls module at all, so any tls.* reference fails
#     loudly at load (cannot load C module 'tls', issue #281) instead of
#     linking OpenSSL into the wasm. Same since the net/timer sinks: those
#     modules also fail loudly — the wasm Playground is single-threaded (no
#     io poller), net/timer suspends could never wake anyway
#     （net/timer 的 import 仍是 no-op：is_builtin_module 对它们恒真，见
#     api.c）。
#   * Virtual FS payload embedded into the wasm:
#       - vendor/tinyactor/lib/bootstrap.tabc — backend compile-half driver
#       - vendor/tinyactor/lib/**/*.ta         — TA module sources a user
#                                               `import "tinyactor:lib/X"`
#                                               resolves against
#       - hello.ta / hello.tabc                 — sample (print(1 + 41) → 42),
#                                               precompiled by ./tavm
#     The layout mirrors the repo on purpose: import resolves as
#     <root>/vendor/<tree>/<path>.ta, root being the first ancestor of the
#     entry source with a vendor/ dir — with the payload at /vendor the
#     comment at /user.ta finds the same tree it would find in a checkout.
#   * MODULARIZE + callMain/FS exports — the JS bridge used by the Playground:
#         callMain(['-q','vendor/tinyactor/lib/bootstrap.tabc',
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
# Payload root = /vendor/tinyactor, i.e. the same shape a checkout has. The
# compiler resolves `import "tinyactor:lib/X"` to
# <root>/vendor/tinyactor/lib/X.ta, where root is the first ancestor of the
# entry source holding a vendor/ — so /user.ta + /vendor works untouched.
mkdir -p "$TMP/payload/vendor/tinyactor/lib/bootstrap"
cp "$REPO_ROOT"/lib/*.ta "$TMP/payload/vendor/tinyactor/lib/"
cp "$REPO_ROOT"/lib/bootstrap/*.ta "$TMP/payload/vendor/tinyactor/lib/bootstrap/"
cp "$REPO_ROOT/lib/bootstrap.tabc" "$TMP/payload/vendor/tinyactor/lib/bootstrap.tabc"

# Sample program (golden: print(1 + 41) → 42).
cat > "$TMP/payload/hello.ta" <<'EOF'
fn main() {
  print(1 + 41)
}
EOF

# Precompile the sample with the real backend pipeline (repo tavm driving the
# committed driver.tabc) so the wasm can run it standalone without a compile
# step — same closure discipline as the old tavm build.
log "precompiling hello.tabc (repo tavm + bootstrap.tabc)"
( cd "$TMP/payload" && "$REPO_ROOT/tavm" -q "$REPO_ROOT/lib/bootstrap.tabc" hello.ta hello.tabc "" )

if [ "$VERBOSE" -eq 1 ]; then
    log "payload:"
    ( cd "$TMP/payload" && du -ah hello.ta hello.tabc vendor | sort -k2 )
fi

# --- 2. Emscripten build ---------------------------------------------------
# VM sources = all of src/*.c except main.c (it has main();
# it is linked explicitly below, exactly once). src/ holds nothing but the
# bytecode VM runtime since the native backend moved to lib/native/ (#458).
VM_SRCS=()
for f in "$REPO_ROOT"/src/*.c; do
    [ "$f" = "$REPO_ROOT/src/main.c" ] && continue
    VM_SRCS+=("$f")
done

# C modules the compiler itself calls (str: 读源码拼字符串；file: 读模块源；
# os: 出错退出)。原生构建里它们是 lazy dylib（首次引用时 dlopen），wasm 没有
# dlopen，所以静态链进来。每个 lib/<mod>.c 都导出同名 vm_load_self，静态
# 链接会撞车——这一遍编译用 -Dvm_load_self=ta_wasm_<mod>_load_self 改名，
# 注册表在 src/host-wasm.c（两边名字必须一致，漏改 = 链接期 undefined）。
WASM_C_MODULES=(str buffer file os)

# `--embed-file vendor` etc. are relative to the payload dir so the embedded
# virtual FS paths are /vendor/tinyactor/lib/... , /hello.ta , /hello.tabc .
log "emcc build -> $OUT_DIR"
mkdir -p "$TMP/out" "$TMP/obj" "$OUT_DIR"
# The warnings/c99 set matches WARN_FLAGS in the Makefile: same compiler
# front-end (clang), so the same warnings are meaningful here. The lists are
# duplicated on purpose — emcc wants a bash array, make wants a variable, and a
# generated/shared flags file buys one more way for the two builds to drift
# (generation order, quoting, which CI step runs first). Keep them in sync by
# hand: -Wno-unused-parameter in particular is a deliberate silence (host-ABI
# signatures such as (VM*, Val*, int) leave parameters unused), and missing it
# here once measured 2 → 50 warnings on this path.
EMCC_FLAGS=(
    -Wall -Wextra -Wno-unused-parameter -std=c99 -O2
        # musl gates POSIX declarations behind feature macros; native macOS
    # headers expose them unconditionally. (-D_GNU_SOURCE predates the
    # cov sink — it was cov.c's mkstemp/fdopen; kept since it is harmless.)
    -D_GNU_SOURCE
    -I"$REPO_ROOT"
)

# 静态 C 模块：逐文件编译（改名宏只能按文件给，所以不能和 VM 源码同一趟）。
for m in "${WASM_C_MODULES[@]}"; do
    run emcc "${EMCC_FLAGS[@]}" -Dvm_load_self="ta_wasm_${m}_load_self" \
        -c "$REPO_ROOT/lib/$m.c" -o "$TMP/obj/$m.o"
done

EMCC_LINK_FLAGS=(
    "${EMCC_FLAGS[@]}"
    "${VM_SRCS[@]}"
    "$REPO_ROOT/src/main.c"
    "$TMP"/obj/*.o
    --embed-file vendor
    --embed-file hello.ta
    --embed-file hello.tabc
    # vendor/ already contains lib/bootstrap.tabc (seeded into the payload
    # above), so --embed-file vendor covers it — no explicit re-embed.
    -s MODULARIZE
    -s EXPORT_NAME=createTavm
    -s EXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8
    # Do not auto-run main() at module load: the Playground drives the VM
    # explicitly via callMain/FS after createTavm({noInitialRun:true}).
    -s INVOKE_RUN=0
    -s ALLOW_MEMORY_GROWTH=1
)
run bash -c 'cd "$1" && shift && emcc "$@"' _ "$TMP/payload" "${EMCC_LINK_FLAGS[@]}" -o "$TMP/out/tinyactor-vm.js"

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
  // Closure 2 (full): compile hello.ta -> hello-out.tabc inside wasm, then
  // run it -> 42. The driver lives in the embedded /vendor tree, exactly
  // where a checkout keeps it.
  {
    const { mod, lines } = await makeModule();
    const rc = mod.callMain(['-q', 'vendor/tinyactor/lib/bootstrap.tabc',
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
  // Closure 3 (import): a user program importing a stdlib module — the case
  // the tour's modules lesson leans on (import "tinyactor:lib/list").
  {
    const { mod, lines } = await makeModule();
    mod.FS.writeFile('imp.ta',
      'import "tinyactor:lib/list"\n\nfn main() {\n  print(list.length([1, 2, 3]))\n}\n');
    const rc = mod.callMain(['-q', 'vendor/tinyactor/lib/bootstrap.tabc',
                             'imp.ta', 'imp.tabc', '']);
    assert.strictEqual(rc, 0, 'import compile exit code: ' + JSON.stringify(lines));
    lines.length = 0;
    assert.strictEqual(mod.callMain(['-q', 'imp.tabc']), 0, 'import run exit code');
    assert.ok(lines.includes('3'), 'import run should print 3, got: ' + JSON.stringify(lines));
    console.log('PASS import: import "tinyactor:lib/list" ->', JSON.stringify(lines));
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