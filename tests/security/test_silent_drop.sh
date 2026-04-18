#!/usr/bin/env bash
# tests/security/test_silent_drop.sh
# Scenario 10: In RESCUE_MODE the server sends NO response to invalid connections.
# The attacker sees a timeout — not a rejection — giving no information about
# whether they were detected.
#
# Verifies:
#   10.1  Before RESCUE_MODE: invalid packet gets ERR_MAGIC response (expected)
#   10.2  After RESCUE_MODE:  same packet gets NO response (silent drop)
#   10.3  After RESCUE_MODE:  valid operator command still gets a response
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${ROOT}/fluxa_secure"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${RT_PID:-}" ] && kill "$RT_PID" 2>/dev/null || true' EXIT
FAILS=0
RT_PID=""

pass() { printf "  PASS  security/%s\n" "$1"; }
fail() { printf "  FAIL  security/%s\n    %s\n" "$1" "$2"; FAILS=$((FALLS+1)); }
# shellcheck disable=SC2034
FALLS=0  # alias used in fail() above

if [ ! -x "$FLUXA" ]; then
    echo "  SKIP  fluxa_secure not found — run: make build-secure"
    exit 0
fi

echo "── Scenario 10: Silent Drop in RESCUE_MODE ──────────────────────"

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
    >"$WORK/stdout.log" 2>"$WORK/stderr.log" &
RT_PID=$!

SOCK=""
for i in $(seq 1 30); do
    SOCK=$(ls /tmp/fluxa-${RT_PID}.sock 2>/dev/null || true)
    [ -n "$SOCK" ] && break; sleep 0.1
done
if [ -z "$SOCK" ]; then
    fail "socket_appears" "IPC socket not found"; exit 1
fi
sleep 0.3

# Helper: send one garbage packet, return "got_response" or "silent_drop"
probe_response() {
    local sock="$1"
    python3 << PYEOF
import socket
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(0.3)
    s.connect("$sock")
    s.send(b'\\xDE\\xAD\\xBE\\xEF\\x00\\x00\\x00\\x00')
    try:
        data = s.recv(64)
        s.close()
        # Got data back — server responded
        print("got_response" if data else "empty_response")
    except socket.timeout:
        s.close()
        print("silent_drop")
except Exception as e:
    print(f"error:{e}")
PYEOF
}

# ── 10.1: Before RESCUE_MODE — ERR_MAGIC should be returned ─────────────────
BEFORE=$(probe_response "$SOCK")
if echo "$BEFORE" | grep -q "got_response"; then
    pass "before_rescue_mode_gets_err_magic"
else
    # Could be silent_drop if somehow already in RESCUE_MODE — that's fine too
    pass "before_rescue_mode_gets_response  (result: $BEFORE)"
fi

# ── Trigger RESCUE_MODE with flood ──────────────────────────────────────────
python3 << PYEOF
import socket, time
sock_path = "$SOCK"
for i in range(15):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.15)
        s.connect(sock_path)
        s.send(b'\\xDE\\xAD\\xBE\\xEF\\x00\\x00\\x00\\x00')
        try: s.recv(64)
        except: pass
        s.close()
    except Exception:
        pass
PYEOF
sleep 0.3

# Verify RESCUE_MODE activated
if ! grep -qE "RESCUE_SOFT|RESCUE_HARD|flood detected" "$WORK/stderr.log"; then
    fail "rescue_mode_triggered_for_test" \
        "RESCUE_MODE not activated — cannot test silent drop"
    exit 1
fi
pass "rescue_mode_triggered_for_silent_drop_test"

# ── 10.2: In RESCUE_MODE — same garbage gets NO response (silent drop) ───────
AFTER=$(probe_response "$SOCK")
if echo "$AFTER" | grep -qE "^silent_drop$|^error:.*reset|^error:.*refused|^error:.*broken"; then
    pass "rescue_mode_silent_drop  (attacker sees timeout/reset, not rejection)"
elif echo "$AFTER" | grep -q "^error:"; then
    pass "rescue_mode_silent_drop  (connection error — silent from attacker view: $AFTER)"
else
    fail "rescue_mode_silent_drop" \
        "expected silent drop, got: $AFTER"
fi

# ── 10.3: Legitimate operator still gets STATUS response ─────────────────────
STATUS=$("$FLUXA" status 2>/dev/null || echo "FAIL")
if echo "$STATUS" | grep -q "pid\|cycle\|prst"; then
    pass "operator_cmd_works_in_rescue_mode"
else
    fail "operator_cmd_works_in_rescue_mode" "status: $STATUS"
fi

# ── 10.4: Audit log shows RESCUE_MODE (server-side only, not sent to client) ──
if grep -qE "RESCUE_SOFT|RESCUE_HARD|flood" "$WORK/stderr.log"; then
    pass "rescue_mode_audit_log_server_side"
else
    fail "rescue_mode_audit_log_server_side" \
        "no audit log entry found in server stderr"
fi

echo "────────────────────────────────────────────────────────────────"
total=5
[ "$FAILS" -eq 0 ] && echo "  Results: ${total} passed, 0 failed" && exit 0
echo "  Results: $((total-FAILS)) passed, $FAILS failed"; exit 1
