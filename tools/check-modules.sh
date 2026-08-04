#!/bin/bash
# tools/check-modules.sh — verify C module functions match typecheck registrations.
#
# Two directions (see docs/c-module.md §6):
#   PHANTOM     typecheck promises a module func the runtime does not provide
#               → ERROR (must fix: remove registration or add implementation)
#   UNREGISTERED a C module func with no type signature in typecheck.ta
#               → WARNING (callable but no type promise — D1 #2 backlog)
#
# Usage: tools/check-modules.sh   (run from repo root or anywhere)
set -uo pipefail
cd "$(dirname "$0")/.."

# module -> source file
mod_file() {
    case "$1" in
        str)  echo "src/str.c" ;;
        net)  echo "src/net.c" ;;
        file) echo "src/file.c" ;;
        buf)  echo "src/buf.c" ;;
        http) echo "lib/http.c" ;;
        vm)   echo "src/api.c" ;;
        *)    echo "" ;;
    esac
}

fail=0
warn=0

# Functions exempt from the type-signature requirement:
#   net.read      — variadic (fd[, max_len]); arrow types can't express varargs
#   vm.resolve_imports / make_tok_vec / tok_type / tok_val / free_tok_vec
#                 — compiler-internal API (used by driver/parser), not user-facing
EXEMPT="net.read vm.resolve_imports vm.make_tok_vec vm.tok_type vm.tok_val vm.free_tok_vec"

# 1) typecheck 注册的 'mod.func（TA 侧承诺，排除注释行）
tc_regs=$(grep -E "extend\(e[0-9][0-9a-z]*, '" lib/typecheck.ta |
          grep -oE "'[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*" | tr -d "'" | sort -u)

# PHANTOM check: every registered func must exist in its C module
for reg in $tc_regs; do
    mod="${reg%%.*}"
    func="${reg#*.}"
    f="$(mod_file "$mod")"
    if [ -z "$f" ]; then
        echo "ERROR: '$reg' registered but module '$mod' has no known C file" >&2
        fail=1
    elif ! grep -q "\"$func\"" "$f"; then
        echo "PHANTOM: '$reg' in typecheck.ta has no implementation in $f" >&2
        fail=1
    fi
done

# UNREGISTERED check (warning): C TaFunc entries with no typecheck signature
for f in src/str.c src/net.c src/file.c src/buf.c lib/http.c src/api.c; do
    case "$f" in
        src/str.c)  mod=str ;;
        src/net.c)  mod=net ;;
        src/file.c) mod=file ;;
        src/buf.c)  mod=buf ;;
        lib/http.c) mod=http ;;
        src/api.c)  mod=vm ;;
    esac
        for name in $(grep -oE '\{"[a-z][a-z0-9_]*", [a-z_][a-z0-9_]*, [0-9]+\}' "$f" |
                  sed -E 's/\{"([a-z][a-z0-9_]*)".*/\1/'); do
        if ! echo "$tc_regs" | grep -qx "$mod.$name"; then
            if echo "$EXEMPT" | grep -qw "$mod.$name"; then
                continue
            fi
            echo "WARN: '$mod.$name' in $f has no type signature in typecheck.ta (D1 #2 backlog)" >&2
            warn=1
        fi
    done
done

if [ $fail -ne 0 ]; then
    echo "check-modules: FAILED (phantom builtins)" >&2
    exit 1
fi
if [ $warn -ne 0 ]; then
    echo "check-modules: OK (no phantoms) — $warn unregistered funcs (warnings only)" >&2
    exit 0
fi
echo "check-modules: OK ($(echo "$tc_regs" | grep -c . ) registered module funcs verified)" >&2