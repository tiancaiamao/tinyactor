# serverctl.sh — shared by run.sh and run-fairness.sh: starting a server
# on a free port, waiting for it to answer, and stopping it.
#
# Sourced, not executed. The caller sets SCRIPT_DIR and REPO_ROOT.

# free_port: an unused localhost port.
free_port() {
    python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# SRV_PID / SRV_PORT: the server started by the last start_server call.
SRV_PID=""
SRV_PORT=""

# start_server <label> <logfile> <cmd...> — the port is appended to the
# command. Starts the server in the background and waits until it answers
# a request. The readiness probe is POST /light: the fairness endpoint
# takes seconds, so it is no probe for "the server is up".
start_server() {
    local label="$1" log="$2"; shift 2
    SRV_PORT="$(free_port)"
    ( cd "$SCRIPT_DIR" && exec "$@" "$SRV_PORT" ) >"$log" 2>&1 &
    SRV_PID=$!
    local i=0
    until curl -fsS -o /dev/null -X POST "http://127.0.0.1:$SRV_PORT/light" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge 300 ]; then
            echo "error: $label server did not come up on port $SRV_PORT; log:" >&2
            cat "$log" >&2
            return 1
        fi
        sleep 0.1
    done
}

# stop_server: stop the server start_server started.
stop_server() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
        SRV_PID=""
    fi
}