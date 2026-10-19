#!/bin/bash
# benchmark/vs-go/probe.sh — run the yield probe (yield_probe.ta) against the
# benchmark's workload module.
#
#   bash benchmark/vs-go/probe.sh 50         # chunked loop: steady ~20 ms wakes
#   bash benchmark/vs-go/probe.sh 10000000   # one call for everything: a burst
#
# The probe needs the module tree (lib/work.dylib + vendor/), which is what
# workenv.sh builds; it is run from inside it, like the servers. NWORKERS=1 so
# what is measured is scheduling policy, not core count.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TINYACTOR="$REPO_ROOT/tinyactor"

WORK="$(mktemp -d /tmp/ta-yield-probe-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck source=workenv.sh
. "$SCRIPT_DIR/workenv.sh"
build_work_module "$WORK"

CHUNK="${1:-50}"
echo "chunk=$CHUNK  (50 = chunked loop, 10000000 = one unpreemptible call)"
( cd "$WORK" && NWORKERS=1 "$TINYACTOR" run "$SCRIPT_DIR/yield_probe.ta" "$CHUNK" )