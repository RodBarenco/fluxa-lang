#!/usr/bin/env bash
# tests/security/test_handshake_timeout.sh
# Scenario 1: Connections that never send data must be closed within IPC_TIMEOUT_MS
# Requires: ./fluxa_secure (make build-secure)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${ROOT}/fluxa_secure"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"; kill "$RT_PID" 2>/dev/null || true' EXIT
FAILS=0

pass() { printf "  PASS  security/%s\n" "$1"; }
fail() { printf "  FAIL  security/%s\n    %s\n" "$1" "$2"; FAILS=$((FAILS+1)); }

if [ ! -x "$FLUXA" ]; then
    echo "  SKIP  fluxa_secure not found — run: make build-secure"
    exit 0
fi

echo "── Scenario 1: Handshake Timeout (AC 1.2) ──────────────────────"

# Start a long-running program in prod mode
cat > "$WORK/main.flx" << 'FLX'
prst int counter = 0
int i = 0
while i < 2000000000 { counter = counter + 1; i = i + 1 }
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$WORK/fluxa.toml"

"$FLUXA" run "$WORK/main.flx" -proj "$WORK" -prod \
    >"$WORK/rt.log" 2>&1 &
RT_PID=$!

# Wait for socket
SOCK=""
for i in $(seq 1 30); do
    SOCK=$(ls /tmp/fluxa-${RT_PID}.sock 2>/dev/null || true)
    [ -n "$SOCK" ] && break
    sleep 0.1
done

if [ -z "$SOCK" ]; then
    fail "socket_appears" "IPC socket not found for pid $RT_PID"
    exit 1
fi
sleep 0.2

# Open a connection but send NOTHING — measure how long before it closes
START=$(date +%s%3N)
# socat connects and stays silent until server closes it
timeout 2s socat UNIX:"$SOCK" - < /dev/null > /dev/null 2>&1 || true
END=$(date +%s%3N)
ELAPSED=$((END - START))

# IPC_TIMEOUT_MS=50ms — connection should die in < 200ms
if [ "$ELAPSED" -lt 200 ]; then
    pass "silent_conn_closed_fast  (${ELAPSED}ms < 200ms)"
else
    fail "silent_conn_closed_fast" "took ${ELAPSED}ms, expected < 200ms"
fi

# Now verify legitimate commands still work after the silent connection
STATUS=$("$FLUXA" status 2>/dev/null || echo "FAIL")
if echo "$STATUS" | grep -q "pid\|cycle\|prst"; then
    pass "legitimate_cmd_works_after_silent_conn"
else
    fail "legitimate_cmd_works_after_silent_conn" "status: $STATUS"
fi

echo "────────────────────────────────────────────────────────────────"
[ "$FAILS" -eq 0 ] && echo "  Results: 2 passed, 0 failed" && exit 0
echo "  Results: $((2-FAILS)) passed, $FAILS failed"; exit 1
