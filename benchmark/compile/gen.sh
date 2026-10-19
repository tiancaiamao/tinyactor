#!/bin/bash
# benchmark/compile/gen.sh — synthesize the compile-time benchmark corpus
# (langcompilebench 口径: N source files x M trivial pub fns each,
# plus a main that imports every module and calls every function so
# nothing is dead-stripped from the measured pipeline).
#
# Output (all generated, gitignored — the script is the committed artifact):
#   benchmark/compile/src/a1.ta .. a{N}.ta   (M `pub fn helloK() { "worldK" }`)
#   benchmark/compile/src/main.ta            (import all, call all, fn main)
#
# Usage: gen.sh [files] [fns-per-file]     defaults: 100 100

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILES="${1:-100}"
FNS="${2:-100}"

rm -rf "$SCRIPT_DIR/src"
mkdir -p "$SCRIPT_DIR/src"

for n in $(seq 1 "$FILES"); do
    f="$SCRIPT_DIR/src/a$n.ta"
    : > "$f"
    for k in $(seq 1 "$FNS"); do
        printf 'pub fn hello%d() {\n  "world%d"\n}\n\n' "$k" "$k" >> "$f"
    done
done

main="$SCRIPT_DIR/src/main.ta"
{
    for n in $(seq 1 "$FILES"); do
        echo "import a$n"
    done
    echo
    echo "fn main() {"
    for n in $(seq 1 "$FILES"); do
        for k in $(seq 1 "$FNS"); do
            echo "  a$n.hello$k()"
        done
    done
    echo "}"
} > "$main"

echo "gen.sh: $FILES files x $FNS fns -> $SCRIPT_DIR/src/ (+ main.ta)"