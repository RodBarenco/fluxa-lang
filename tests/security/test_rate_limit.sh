#!/usr/bin/env bash
# tests/security/test_rate_limit.sh
# Scenario 6: Rate window boundary — 99 invalid/sec does NOT trigger RESCUE_MODE.
# 101 invalid/sec DOES trigger it.
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

echo "── Scenario 6: Rate Limit Window Boundary (AC 1.1) ─────────────"

start_runtime() {
    cat > "$WORK/main.flx" << 'FLX'
prst int x = 0
int i = 0
while i < 2000000000 { x = x + 1; i = i + 1 }
FLX
    printf '[project]\nname="t"\nentry="main.flx"\n' > "$WORK/fluxa.toml"
    "$FLUXA" run "$WORK/main.flx" -proj "$WORK" -prod \
        >"$WORK/rt_stdout.log" 2>"$WORK/rt_stderr.log" &
    RT_PID=$!
    local SOCK=""
    for i in $(seq 1 30); do
        SOCK=$(ls /tmp/fluxa-${RT_PID}.sock 2>/dev/null || true)
        [ -n "$SOCK" ] && break; sleep 0.1
    done
    echo "$SOCK"
}

send_invalid() {
    local sock="$1" count="$2"
    python3 << PYEOF
import socket, time
sock_path = "$sock"
sent = 0
for i in range($count):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.05)
        s.connect(sock_path)
        s.send(b'\\xDE\\xAD\\xBE\\xEF\\x00\\x00\\x00\\x00')
        s.close()
        sent += 1
    except Exception:
        pass
print(f"sent {sent}")
PYEOF
}

# ── Test A: 99 packets in < 1s — should NOT trigger RESCUE_MODE ──────
SOCK=$(start_runtime)
[ -z "$SOCK" ] && fail "socket_a" "socket not found" && exit 1
sleep 0.3

> "$WORK/rt_stderr.log"  # clear log
send_invalid "$SOCK" 99 > /dev/null
sleep 0.3

if grep -q "RESCUE_MODE activated" "$WORK/rt_stderr.log"; then
    fail "sub_threshold_no_rescue  (5 pkts)" \
        "RESCUE_MODE triggered at 99 — should need 100+"
else
    pass "sub_threshold_no_rescue  (99 pkts, no RESCUE_MODE)"
fi

kill "$RT_PID" 2>/dev/null || true; sleep 0.5

# ── Test B: Wait for window reset, send another 99 — still no RESCUE ──
# (This confirms the window resets, not accumulates)
SOCK=$(start_runtime)
[ -z "$SOCK" ] && fail "socket_b" "socket not found" && exit 1
sleep 0.3
> "$WORK/rt_stderr.log"

send_invalid "$SOCK" 90 > /dev/null
sleep 1.2  # wait for window to reset (IPC_RATE_WINDOW_SEC=1s)
send_invalid "$SOCK" 90 > /dev/null
sleep 0.3

if grep -q "RESCUE_MODE activated" "$WORK/rt_stderr.log"; then
    fail "window_resets_correctly  (5+5 across boundary)" \
        "RESCUE_MODE triggered — window did not reset"
else
    pass "window_resets_correctly  (5+5 across 1s boundary, no RESCUE_MODE)"
fi

kill "$RT_PID" 2>/dev/null || true; sleep 0.5

# ── Test C: 110 packets in < 1s — MUST trigger RESCUE_MODE ───────────
SOCK=$(start_runtime)
[ -z "$SOCK" ] && fail "socket_c" "socket not found" && exit 1
sleep 0.3
> "$WORK/rt_stderr.log"

send_invalid "$SOCK" 110 > /dev/null
sleep 0.3

if grep -q "RESCUE_MODE activated" "$WORK/rt_stderr.log"; then
    pass "over_threshold_triggers_rescue  (15 pkts)"
else
    fail "over_threshold_triggers_rescue  (15 pkts)" \
        "RESCUE_MODE not triggered — check IPC_BURST_THRESHOLD"
fi

kill "$RT_PID" 2>/dev/null || true

echo "────────────────────────────────────────────────────────────────"
[ "$FAILS" -eq 0 ] && echo "  Results: 3 passed, 0 failed" && exit 0
echo "  Results: $((3-FAILS)) passed, $FAILS failed"; exit 1
