#!/usr/bin/env bash
# tests/libs/pid.sh — std.pid test suite
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/pid/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/pid/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

run() {
    local name="$1" code="$2"
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.pid="1.0"\n' > "$P/fluxa.toml"
    printf '%s\n' "$code" > "$P/main.flx"
    timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

echo "── std.pid ──────────────────────────────────────────────────────"

# 1. pid.new returns a dyn cursor
out=$(run "new_returns_dyn" 'import std pid
danger {
    dyn ctrl = pid.new(1.0, 0.0, 0.0)
    print("ok")
}')
echo "$out" | grep -q "ok" && pass "new_returns_cursor" || fail "new_returns_cursor" "ok" "$out"

# 2. P-only: output = kp * error
out=$(run "p_only" 'import std pid
danger {
    dyn ctrl = pid.new(2.0, 0.0, 0.0)
    float out = pid.compute(ctrl, 10.0, 7.0)
    print(out)
}')
echo "$out" | grep -q "6" && pass "p_only_kp2_error3_equals_6" || fail "p_only_kp2_error3_equals_6" "6" "$out"

# 3. Integral accumulates
out=$(run "integral" 'import std pid
danger {
    dyn ctrl = pid.new(0.0, 1.0, 0.0)
    float o1 = pid.compute(ctrl, 5.0, 4.0)
    float o2 = pid.compute(ctrl, 5.0, 4.0)
    print(o2)
}')
echo "$out" | grep -q "2" && pass "integral_accumulates" || fail "integral_accumulates" "2" "$out"

# 4. reset zeroes integral
out=$(run "reset" 'import std pid
danger {
    dyn ctrl = pid.new(0.0, 1.0, 0.0)
    float o1 = pid.compute(ctrl, 5.0, 0.0)
    pid.reset(ctrl)
    float o2 = pid.compute(ctrl, 5.0, 0.0)
    print(o2)
}')
echo "$out" | grep -q "5" && pass "reset_zeroes_integral" || fail "reset_zeroes_integral" "5" "$out"

# 5. set_limits clamps output
out=$(run "clamp" 'import std pid
danger {
    dyn ctrl = pid.new(10.0, 0.0, 0.0)
    pid.set_limits(ctrl, -5.0, 5.0)
    float o = pid.compute(ctrl, 10.0, 0.0)
    print(o)
}')
echo "$out" | grep -q "5" && pass "set_limits_clamps_output" || fail "set_limits_clamps_output" "5" "$out"

# 6. set_limits min >= max → error
# 6. set_limits min >= max → error
printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.pid="1.0"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std pid
danger {
    dyn ctrl = pid.new(1.0, 0.0, 0.0)
    pid.set_limits(ctrl, 5.0, 5.0)
}
if err != nil { print(err[0]) }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "min must be" && pass "set_limits_invalid_error" || fail "set_limits_invalid_error" "min must be" "$out"

# 7. deadband — small error ignored
out=$(run "deadband" 'import std pid
danger {
    dyn ctrl = pid.new(1.0, 0.0, 0.0)
    pid.set_deadband(ctrl, 2.0)
    float o = pid.compute(ctrl, 1.0, 0.0)
    print(o)
}')
echo "$out" | grep -q "^0" && pass "deadband_ignores_small_error" || fail "deadband_ignores_small_error" "0" "$out"

# 8. state() returns dyn with 7 elements
out=$(run "state" 'import std pid
danger {
    dyn ctrl = pid.new(1.0, 2.0, 3.0)
    dyn s = pid.state(ctrl)
    print(len(s))
    print(s[0])
    print(s[1])
    print(s[2])
}')
echo "$out" | grep -q "7" && pass "state_returns_7_elements" || fail "state_returns_7_elements" "7" "$out"
echo "$out" | grep -q "1" && pass "state_kp_correct" || fail "state_kp_correct" "1" "$out"

# 9. invalid cursor → error
# 9. invalid cursor → error
printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.pid="1.0"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std pid
danger {
    dyn bad = [1, 2, 3]
    float o = pid.compute(bad, 1.0, 0.0)
}
if err != nil { print(err[0]) }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "invalid controller" && pass "bad_cursor_error" || fail "bad_cursor_error" "invalid controller" "$out"

# 10. anti-windup: integral back-calculated when clamped
out=$(run "antiwindup" 'import std pid
danger {
    dyn ctrl = pid.new(0.0, 1.0, 0.0)
    pid.set_limits(ctrl, -10.0, 10.0)
    int i = 0
    while i < 20 {
        pid.compute(ctrl, 100.0, 0.0)
        i = i + 1
    }
    dyn s = pid.state(ctrl)
    print(s[3])
}')
val=$(echo "$out" | grep -oE "^-?[0-9]+(\.[0-9]+)?" || echo "")
[ -n "$val" ] && python3 -c "import sys; v=float('$val'); sys.exit(0 if abs(v) <= 11 else 1)" 2>/dev/null \
    && pass "antiwindup_integral_bounded" \
    || fail "antiwindup_integral_bounded" "integral <= 11" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.pid: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.pid: PASS"
[ "$FAILS" -eq 0 ] && exit 0 || exit 1
