#!/bin/bash
# test/run_tls_tests.sh — TLS handshake happy path for lib/tls.ta.
#
# test/basic/tls-lib.ta's scenario 4 (real handshake) is env-gated on
# TA_TLS_TEST=host:port because TA has no TLS server and CI has no
# guaranteed TLS endpoint. This harness supplies one: `openssl s_server
# -rev` (the line-reverse echo recipe lib/tls.ta documents) with a
# throwaway self-signed cert, then runs tls-lib.ta against it. Sourced by
# test/run_basic_tests.sh after run_category and invoked via
# run_tls_tests, so the shared PASSED/FAILED counters stay in one process.
# Can also be run directly: bash test/run_tls_tests.sh
#
# Skipped (loudly) when the openssl CLI is absent — minimal containers
# may not ship it; the manual recipe in lib/tls.ta stays available.

run_tls_tests() {
  local basic_dir="$SCRIPT_DIR/basic"

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "tls-handshake (openssl s_server):"

  if ! command -v openssl >/dev/null 2>&1; then
    echo -e "${YELLOW}⏭  SKIP${NC} (openssl CLI not found — see lib/tls.ta manual recipe)"
    return
  fi

  local workdir=$(mktemp -d "${TMPDIR:-/tmp}/tls_srv_$$XXXXXX")
  local outlog=$(mktemp "${TMPDIR:-/tmp}/tls_srv_$$XXXXXX.out")
  local port=""
  local srv_pid=""

  cleanup() {
    [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
    rm -rf "$workdir"
    rm -f "$outlog"
  }

  if ! openssl req -x509 -newkey rsa:2048 -keyout "$workdir/key.pem" \
      -out "$workdir/cert.pem" -days 1 -nodes -subj "/CN=localhost" \
      >/dev/null 2>&1; then
    echo -e "${RED}FAIL${NC} (self-signed cert generation)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("tls-handshake (cert generation)")
    cleanup
    return
  fi

  # Port-bind retry, mirroring bind_random_port(50) in
  # net-localhost-connect.ta: a random port in [20000, 40000); if the
  # chosen port is taken (s_server dies immediately) pick another and
  # retry, up to 50 attempts. The runner's own 3x retry sits outside this
  # function, so a hard FAIL here means 50 collisions — a real problem,
  # not a flake.
  local ready=0
  local tries=50
  while [ $tries -gt 0 ]; do
    port=$(( (RANDOM % 20000) + 20000 ))
    openssl s_server -rev -quiet -accept "$port" \
      -key "$workdir/key.pem" -cert "$workdir/cert.pem" \
      >"$workdir/server.log" 2>&1 &
    srv_pid=$!

    # Poll until the port accepts (or the server dies on a taken port).
    local i=0
    while [ $i -lt 50 ]; do
      if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        exec 3>&- 3<&-
        ready=1
        break
      fi
      # s_server dies immediately if its port is taken — don't poll a
      # corpse; break to the outer loop to re-pick a port.
      kill -0 "$srv_pid" 2>/dev/null || break
      sleep 0.1
      i=$((i + 1))
    done

    [ $ready -eq 1 ] && break
    # Port collision (or slow start): reap and re-pick.
    kill "$srv_pid" 2>/dev/null
    wait "$srv_pid" 2>/dev/null
    srv_pid=""
    tries=$((tries - 1))
  done

  if [ $ready -ne 1 ]; then
    echo -e "${RED}FAIL${NC} (s_server never came up after 50 ports)"
    cat "$workdir/server.log" 2>/dev/null
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("tls-handshake (s_server startup)")
    cleanup
    return
  fi

  TA_TLS_TEST="127.0.0.1:$port" "$TINYACTOR" run "$basic_dir/tls-lib.ta" \
    >"$outlog" 2>&1
  local rc=$?

  if [ $rc -ne 0 ] || grep -q "FAIL" "$outlog" || ! grep -q "ok - real handshake echo" "$outlog"; then
    echo -e "${RED}FAIL${NC} (rc=$rc)"
    sed 's/^/    /' "$outlog"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run tls-handshake (openssl s_server scenario)")
  else
            echo -e "${GREEN}ok${NC} (real TLS handshake against s_server -rev)"
    PASSED=$((PASSED + 1))
  fi

    # Concurrency: 8 actors wrap/read/close at the same time. The TLS
  # module state now lives per-handle in Resource payloads (lib/tls.c,
  # no shared globals), but the regression still needs this race window:
  # a shared anything hands two actors the same SSL* and one close frees
  # the other's connection. NWORKERS=8 is the point: the single-worker
  # scheduler never interleaves two actors.
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "tls-concurrency (8 actors, one resource module):"

  TA_TLS_TEST="127.0.0.1:$port" NWORKERS=8 "$TINYACTOR" run \
    "$SCRIPT_DIR/actor/multithread-tls.ta" >"$outlog" 2>&1
  rc=$?

  if [ $rc -ne 0 ] || grep -q "FAIL" "$outlog" ||
     ! grep -q "ok - 8 actors x 20 rounds" "$outlog"; then
    echo -e "${RED}FAIL${NC} (rc=$rc)"
    sed 's/^/    /' "$outlog"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("run tls-concurrency (8 actors, one resource module)")
  else
    echo -e "${GREEN}ok${NC} (8 concurrent TLS actors, per-handle payloads)"
    PASSED=$((PASSED + 1))
  fi

  cleanup
  wait "$srv_pid" 2>/dev/null
}

# Direct execution (not sourced): set up the shared counters and run.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  source "$(dirname "$0")/lib.sh"
  run_tls_tests
  print_summary
  exit $([ $FAILED -eq 0 ])
fi