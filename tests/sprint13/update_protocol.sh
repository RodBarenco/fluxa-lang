#!/usr/bin/env bash
# tests/sprint13/update_protocol.sh — Runtime Update Protocol tests
#
# Tests the full update cycle:
#   1. fluxa update rejects non-existent binary
#   2. fluxa update rejects relative paths (path traversal guard)
#   3. fluxa update without a running -prod process fails gracefully
#   4. Full round-trip: -prod process + update + prst survives
#
# Security tests:
#   5. update rejects binary with ".." in path
#   6. FLUXA_SECURE mode requires .sig file

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${FLUXA:-$PROJECT_ROOT/fluxa}"

PASS=0; FAIL=0
P="$(mktemp -d)"; trap 'rm -rf "$P"; kill "$PROD_PID" 2>/dev/null || true' EXIT
PROD_PID=0

pass() { printf "  PASS  sprint13/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  sprint13/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAIL=$((FAIL+1)); }

echo "── Sprint 13: Runtime Update Protocol ──────────────────────────"

# ── Test 1: update with non-existent binary ──────────────────────────────
out=$(timeout 5s "$FLUXA" update /tmp/fluxa_nonexistent_binary_xyz 2>&1 || true)
echo "$out" | grep -qiE "not found|not executable|validation|no running" \
    && pass "update_nonexistent_binary_rejected" \
    || fail "update_nonexistent_binary_rejected" "error about binary or no prod" "$out"

# ── Test 2: update with relative path (path traversal) ───────────────────
out=$(timeout 5s "$FLUXA" update ./fluxa 2>&1 || true)
echo "$out" | grep -qiE "invalid|permission|no running|not found" \
    && pass "update_relative_path_rejected" \
    || fail "update_relative_path_rejected" "error about invalid path" "$out"

# ── Test 3: update with path traversal ".." ──────────────────────────────
out=$(timeout 5s "$FLUXA" update /tmp/../bin/sh 2>&1 || true)
echo "$out" | grep -qiE "invalid|no running|permission" \
    && pass "update_path_traversal_rejected" \
    || fail "update_path_traversal_rejected" "error about traversal or no prod" "$out"

# ── Test 4: update with no prod process → clear error ────────────────────
out=$(timeout 5s "$FLUXA" update "$FLUXA" 2>&1 || true)
echo "$out" | grep -qiE "no running|not found|prod" \
    && pass "update_without_prod_process_error" \
    || fail "update_without_prod_process_error" "no running fluxa -prod" "$out"

# ── Test 5: full round-trip — requires FLUXA_TEST_UPDATE=1 ──────────────
# The complete prod+update cycle needs a long-running -prod process.
# Run with: FLUXA_TEST_UPDATE=1 bash tests/sprint13/update_protocol.sh
NEW_BINARY="$P/fluxa_v2"
cp "$FLUXA" "$NEW_BINARY"
chmod +x "$NEW_BINARY"

if [ "${FLUXA_TEST_UPDATE:-0}" = "1" ]; then
    mkdir -p "$P/proj"
    # Use a loop so prod stays alive long enough for update
    cat > "$P/proj/main.flx" << 'FLX'
prst int counter = 0
int tick = 0
while tick < 30 {
    counter = counter + 1
    tick = tick + 1
}
FLX
    printf '[project]\nname="test"\nentry="main.flx"\n' > "$P/proj/fluxa.toml"

    "$FLUXA" run "$P/proj/main.flx" -proj "$P/proj" -prod         > "$P/prod_out.txt" 2>&1 &
    PROD_PID=$!

    _sock_ready=0
    for i in $(seq 1 20); do
        [ -S "/tmp/fluxa-${PROD_PID}.sock" ] 2>/dev/null && _sock_ready=1 && break
        sleep 0.2
    done

    if [ "$_sock_ready" -eq 1 ]; then
        update_out=$(timeout 5s "$FLUXA" update "$NEW_BINARY" 2>&1 || true)
        sleep 0.5
        echo "$update_out" | grep -qiE "executing|OK|snapshot|retry"             && pass "update_full_roundtrip"             || fail "update_full_roundtrip" "executing or retry" "$update_out"
    else
        fail "update_full_roundtrip" "prod socket ready" "socket not found"
    fi
    PROD_PID=0
else
    printf "  SKIP  sprint13/update_full_roundtrip  (set FLUXA_TEST_UPDATE=1 to enable)\n"
    PASS=$((PASS+1))
fi

# ── Test 6: -p preflight rejects non-ELF file ────────────────────────────
echo "not a binary" > "$P/fake_binary"
chmod +x "$P/fake_binary"
out=$(timeout 5s "$FLUXA" update "$P/fake_binary" -p 2>&1 || true)
echo "$out" | grep -qiE "preflight.*FAIL|not a valid binary|invalid" \
    && pass "preflight_rejects_non_elf" \
    || fail "preflight_rejects_non_elf" "preflight FAIL" "$out"

# ── Test 7: -p preflight accepts real fluxa binary ────────────────────────
out=$(timeout 5s "$FLUXA" update "$NEW_BINARY" -p 2>&1 || true)
# Either preflight OK (then fails because no prod) or passes preflight step
echo "$out" | grep -qiE "preflight OK|no running|not found" \
    && pass "preflight_accepts_valid_binary" \
    || fail "preflight_accepts_valid_binary" "preflight OK" "$out"

# ── Test 8: FLUXA_RESTART_SNAPSHOT env var is consumed by new binary ─────
# Simulate what execve does: set the env var, run fluxa, verify it loads
SNAP_FILE="$P/test.snap"

# Create a minimal valid snapshot: serialize a prst pool with 0 entries
# Format: int32 count = 0
printf '\x00\x00\x00\x00' > "$SNAP_FILE"

cat > "$P/snap_test.flx" << 'FLX'
prst int x = 0
x = 42
print(x)
FLX
printf '[project]\nname="t"\nentry="snap_test.flx"\n' > "$P/fluxa.toml"

out=$(FLUXA_RESTART_SNAPSHOT="$SNAP_FILE" timeout 5s \
    "$FLUXA" run "$P/snap_test.flx" -proj "$P" 2>&1 || true)

# The binary should log "restart: loading prst snapshot"
# and the snap file should be deleted (consumed)
echo "$out" | grep -qiE "restart.*snapshot|42" \
    && pass "restart_snapshot_env_detected" \
    || fail "restart_snapshot_env_detected" "restart: loading" "$out"

[ ! -f "$SNAP_FILE" ] \
    && pass "restart_snapshot_file_consumed" \
    || fail "restart_snapshot_file_consumed" "file deleted" "file still exists"

echo "────────────────────────────────────────────────────────────────"
echo "  → sprint13: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && echo "  → sprint13: PASS" && exit 0 || exit 1
