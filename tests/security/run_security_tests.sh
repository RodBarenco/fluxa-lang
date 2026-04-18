#!/usr/bin/env bash
# tests/security/run_security_tests.sh
# Master runner for all Fluxa security tests.
# Requires: ./fluxa_secure (make build-secure)
#
# Usage:
#   bash tests/security/run_security_tests.sh           # all tests
#   bash tests/security/run_security_tests.sh --fast    # skip 30s drain test
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${ROOT}/fluxa_secure"
FAST=""
TOTAL_PASS=0
TOTAL_FAIL=0

for arg in "$@"; do [ "$arg" = "--fast" ] && FAST="--fast"; done

echo "════════════════════════════════════════════════════════════════"
echo "  Fluxa Security Test Suite (FLUXA_SECURE=1)"
echo "  Binary: $FLUXA"
[ -n "$FAST" ] && echo "  Mode: --fast (skipping 30s drain wait)"
echo "════════════════════════════════════════════════════════════════"
echo ""

if [ ! -x "$FLUXA" ]; then
    echo "  ERROR: fluxa_secure not found."
    echo "  Build it with: make build-secure"
    echo ""
    exit 1
fi

run_test() {
    local script="$1" label="$2"
    echo ""
    local out
    if out=$(bash "$script" $FAST 2>&1); then
        echo "$out"
        local p f
        p=$(echo "$out" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+' || echo 0)
        f=$(echo "$out" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+' || echo 0)
        TOTAL_PASS=$((TOTAL_PASS + p))
        TOTAL_FAIL=$((TOTAL_FAIL + f))
    else
        echo "$out"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
}

run_test "$SCRIPT_DIR/test_handshake_timeout.sh"   "Scenario 1: Handshake Timeout"
run_test "$SCRIPT_DIR/test_fd_exhaustion.sh"        "Scenario 2: FD Exhaustion"
run_test "$SCRIPT_DIR/test_rescue_mode.sh"          "Scenario 3: RESCUE_MODE Activation"
run_test "$SCRIPT_DIR/test_rescue_drain.sh"         "Scenario 4: RESCUE_MODE Auto-Drain"
run_test "$SCRIPT_DIR/test_handover_under_flood.sh" "Scenario 5: Handover Under Flood"
run_test "$SCRIPT_DIR/test_rate_limit.sh"           "Scenario 6: Rate Limit Window"
run_test "$SCRIPT_DIR/test_keygen.sh"               "Scenarios 7-9: Key Gen + Toml + Permissions"
run_test "$SCRIPT_DIR/test_silent_drop.sh"          "Scenario 10: Silent Drop in RESCUE_MODE"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  Security Suite Results: ${TOTAL_PASS} passed, ${TOTAL_FAIL} failed"
if [ "$TOTAL_FAIL" -eq 0 ]; then
    echo "  → FLUXA_SECURE: PASS — hardened binary meets security spec"
else
    echo "  → FLUXA_SECURE: FAIL — see failures above"
fi
echo "════════════════════════════════════════════════════════════════"

[ "$TOTAL_FAIL" -eq 0 ] && exit 0 || exit 1
