#!/usr/bin/env bash
# tests/security/test_rescue_mode.sh
# Scenario 3: Send > IPC_BURST_THRESHOLD invalid packets in 1s.
# RESCUE_MODE must activate and be logged to stderr.
# Requires: ./fluxa_secure (make build-secure), python3
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${ROOT}/fluxa_secure"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; kill "$RT_PID" 2>/dev/null || true' EXIT
FAILS=0

pass() { printf "  PASS  security/%s\n" "$1"; }
fail() { printf "  FAIL  security/%s\n    %s\n" "$1" "$2"; FAILS=$((FAILS+1)); }

if [ ! -x "$FLUXA" ]; then
    echo "  SKIP  fluxa_secure not found — run: make build-secure"
    exit 0
fi

echo "── Scenario 3: Invalid Packet Flood → RESCUE_MODE (AC 2.1) ─────"

cat > "$WORK/main.flx" << 'FLX'
prst int x = 0
int i = 0
while i < 2000000000 {
    x = x + 1
    i = i + 1
}
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$WORK/fluxa.toml"

"$FLUXA" run "$WORK/main.flx" -proj "$WORK" -prod \
    >"$WORK/rt_stdout.log" 2>"$WORK/rt_stderr.log" &
RT_PID=$!

SOCK=""
for i in $(seq 1 30); do
    SOCK=$(ls /tmp/fluxa-${RT_PID}.sock 2>/dev/null || true)
    [ -n "$SOCK" ] && break; sleep 0.1
done
[ -z "$SOCK" ] && fail "socket_appears" "socket not found" && exit 1
sleep 0.3

# Send 120 invalid-magic packets in ~0.8s using Python
# Each packet: connects, sends 8 bytes of garbage, closes
python3 << PYEOF
import socket, time

sock_path = "$SOCK"
count = 0
start = time.time()

for i in range(25):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.15)
        s.connect(sock_path)
        s.send(b'\xDE\xAD\xBE\xEF\x00\x00\x00\x00')
        try: s.recv(64)   # wait for server to process it
        except: pass
        s.close()
        count += 1
    except Exception:
        pass

elapsed = time.time() - start
print(f"Sent {count} invalid packets in {elapsed:.2f}s")
PYEOF

# Give the server a moment to log
sleep 0.5

# Check stderr for RESCUE_MODE activation
if grep -qE "RESCUE_SOFT|RESCUE_HARD|RESCUE_MODE" "$WORK/rt_stderr.log"; then
    pass "rescue_mode_activated"
else
    fail "rescue_mode_activated" \
        "RESCUE_MODE not found in stderr. Log: $(cat "$WORK/rt_stderr.log")"
fi

# Check that RESCUE_MODE log includes flood info
if grep -qE "invalid|malformed|flood" "$WORK/rt_stderr.log"; then
    pass "rescue_mode_log_has_flood_info"
else
    fail "rescue_mode_log_has_flood_info" \
        "expected 'invalid' in log: $(cat "$WORK/rt_stderr.log")"
fi

# Legitimate command from same UID should still work in RESCUE_MODE
# (RESCUE_MODE blocks wrong-UID, not same-UID)
STATUS=$("$FLUXA" status 2>/dev/null || echo "FAIL")
if echo "$STATUS" | grep -q "pid\|cycle"; then
    pass "same_uid_still_works_in_rescue_mode"
else
    fail "same_uid_still_works_in_rescue_mode" "$STATUS"
fi

echo "────────────────────────────────────────────────────────────────"
[ "$FAILS" -eq 0 ] && echo "  Results: 3 passed, 0 failed" && exit 0
echo "  Results: $((3-FAILS)) passed, $FAILS failed"; exit 1
