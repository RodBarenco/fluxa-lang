#!/usr/bin/env bash
# tests/security/test_fd_exhaustion.sh
# Scenario 2: Slowloris-style flood — open IPC_MAX_CONNS+N connections,
# verify legitimate commands still work within 2s.
# Requires: ./fluxa_secure (make build-secure), socat
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${ROOT}/fluxa_secure"
WORK="$(mktemp -d)"
FLOOD_PIDS=()
trap 'rm -rf "$WORK"; kill "$RT_PID" 2>/dev/null || true
      for p in "${FLOOD_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done' EXIT
FAILS=0

pass() { printf "  PASS  security/%s\n" "$1"; }
fail() { printf "  FAIL  security/%s\n    %s\n" "$1" "$2"; FAILS=$((FAILS+1)); }

if [ ! -x "$FLUXA" ]; then
    echo "  SKIP  fluxa_secure not found — run: make build-secure"
    exit 0
fi
if ! command -v socat &>/dev/null; then
    echo "  SKIP  socat not installed (apt install socat)"
    exit 0
fi

echo "── Scenario 2: FD Exhaustion / Connection Cap (AC 4.1) ─────────"

cat > "$WORK/main.flx" << 'FLX'
prst int counter = 0
int i = 0
while i < 2000000000 { counter = counter + 1; i = i + 1 }
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$WORK/fluxa.toml"

"$FLUXA" run "$WORK/main.flx" -proj "$WORK" -prod >"$WORK/rt.log" 2>&1 &
RT_PID=$!

SOCK=""
for i in $(seq 1 30); do
    SOCK=$(ls /tmp/fluxa-${RT_PID}.sock 2>/dev/null || true)
    [ -n "$SOCK" ] && break; sleep 0.1
done
[ -z "$SOCK" ] && fail "socket_appears" "socket not found" && exit 1
sleep 0.3

# Open 20 slow connections (IPC_MAX_CONNS=16, so 4 should be dropped)
echo "  Opening 20 slow connections..."
for i in $(seq 1 20); do
    socat UNIX:"$SOCK" - < /dev/null > /dev/null 2>&1 &
    FLOOD_PIDS+=($!)
done
sleep 0.5

# Legitimate command must succeed within 2s
START=$(date +%s%3N)
STATUS=$("$FLUXA" status 2>/dev/null || echo "TIMEOUT")
END=$(date +%s%3N)
ELAPSED=$((END - START))

if echo "$STATUS" | grep -q "pid\|cycle\|prst"; then
    pass "status_succeeds_under_flood  (${ELAPSED}ms)"
else
    fail "status_succeeds_under_flood" "got: $STATUS (${ELAPSED}ms)"
fi

if [ "$ELAPSED" -lt 2000 ]; then
    pass "response_within_2s  (${ELAPSED}ms)"
else
    fail "response_within_2s" "${ELAPSED}ms > 2000ms"
fi

# Kill flood connections
for p in "${FLOOD_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
FLOOD_PIDS=()
sleep 0.2

# Verify normal operation resumes
STATUS2=$("$FLUXA" status 2>/dev/null || echo "FAIL")
if echo "$STATUS2" | grep -q "pid\|cycle\|prst"; then
    pass "normal_operation_resumes_after_flood"
else
    fail "normal_operation_resumes_after_flood" "$STATUS2"
fi

echo "────────────────────────────────────────────────────────────────"
[ "$FAILS" -eq 0 ] && echo "  Results: 3 passed, 0 failed" && exit 0
echo "  Results: $((3-FAILS)) passed, $FAILS failed"; exit 1
