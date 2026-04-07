#!/usr/bin/env bash
# tests/libs/time.sh — std.time test suite
#
# Test cases explicitly cover the three loop patterns documented in STDLIB.md:
#   - Loop with sleep (normal game/sensor loop)
#   - Hot loop without sleep (fast path guard O(1))
#   - Polling loop (maximum responsiveness)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.time/%s\n" "$1"; }
fail() { printf "  FAIL  std.time/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"
    FAILS=$((FAILS+1)); }

setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.time = "1.0"
TOML
}

echo "── std.time: time library ───────────────────────────────────────────"

# CASE 1: import without [libs] → clear error
cat > "$WORK_DIR/no_toml.flx" << 'FLX'
import std time
int t = time.now_ms()
print(t)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/no_toml.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|toml\|libs"; then
    pass "import_without_toml_error"
else
    fail "import_without_toml_error" "error: not declared in [libs]" "$out"
fi

# CASE 2: now_ms returns positive integer
P="$WORK_DIR/p2"; setup "$P" "now_ms"
cat > "$P/main.flx" << 'FLX'
import std time
int t = time.now_ms()
print(t > 0)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "now_ms_positive"
else
    fail "now_ms_positive" "true" "$out"
fi

# CASE 3: sleep + elapsed_ms — timing
P="$WORK_DIR/p3"; setup "$P" "sleep"
cat > "$P/main.flx" << 'FLX'
import std time
int t0 = time.now_ms()
time.sleep(80)
int dt = time.elapsed_ms(t0)
print(dt >= 70)
print(dt < 300)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true" && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "sleep_elapsed_ms"
else
    fail "sleep_elapsed_ms" "true true (dt in [70,300]ms)" "$out"
fi

# CASE 4: sleep_us — microsecond sleep
P="$WORK_DIR/p4"; setup "$P" "sleep_us"
cat > "$P/main.flx" << 'FLX'
import std time
int t0 = time.now_us()
time.sleep_us(50000)
int dt = time.elapsed_ms(t0 / 1000)
print(dt >= 40)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "sleep_us"
else
    fail "sleep_us" "true (>=40ms elapsed after 50ms sleep)" "$out"
fi

# CASE 5: timeout — returns false before, true after
P="$WORK_DIR/p5"; setup "$P" "timeout"
cat > "$P/main.flx" << 'FLX'
import std time
int t0    = time.now_ms()
bool too_early = time.timeout(t0, 5000)
time.sleep(60)
bool done = time.timeout(t0, 50)
print(too_early)
print(done)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "false" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "timeout_before_after"
else
    fail "timeout_before_after" "false then true" "$out"
fi

# CASE 6: now_us returns value >= now_ms * 1000
P="$WORK_DIR/p6"; setup "$P" "now_us"
cat > "$P/main.flx" << 'FLX'
import std time
int ms = time.now_ms()
int us = time.now_us()
print(us >= ms)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "now_us_resolution"
else
    fail "now_us_resolution" "true (us >= ms)" "$out"
fi

# CASE 7: ticks returns positive integer
P="$WORK_DIR/p7"; setup "$P" "ticks"
cat > "$P/main.flx" << 'FLX'
import std time
int t = time.ticks()
print(t >= 0)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "ticks_positive"
else
    fail "ticks_positive" "true" "$out"
fi

# CASE 8: format — returns non-empty string with colons (datetime or elapsed)
P="$WORK_DIR/p8"; setup "$P" "format"
cat > "$P/main.flx" << 'FLX'
import std time
int t0 = time.now_ms()
str s = time.format(t0)
print(len(s) > 5)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "format_nonempty"
else
    fail "format_nonempty" "true (len > 5)" "$out"
fi

# CASE 9: LOOP WITH SLEEP — normal game/sensor loop pattern
# Documented case: back-edge mailbox drain at natural frequency
# Expected: loop runs N times in ~N*sleep_ms time
P="$WORK_DIR/p9"; setup "$P" "loop_with_sleep"
cat > "$P/main.flx" << 'FLX'
import std time
int t0    = time.now_ms()
int count = 0
while count < 5 {
    time.sleep(20)
    count = count + 1
}
int dt = time.elapsed_ms(t0)
print(count)
print(dt >= 90)
print(dt < 500)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^5$" \
    && echo "$out" | sed -n '2p' | grep -q "true" \
    && echo "$out" | sed -n '3p' | grep -q "true"; then
    pass "loop_with_sleep_timing"
else
    fail "loop_with_sleep_timing" "5, true, true" "$out"
fi

# CASE 10: HOT LOOP WITHOUT SLEEP — fast path guard
# Documented case: no sleep, back-edge runs at max speed
# O(1) check per iteration — verifiable by timing: 10k iters must finish fast
P="$WORK_DIR/p10"; setup "$P" "hot_loop"
cat > "$P/main.flx" << 'FLX'
import std time
int t0  = time.now_ms()
int i   = 0
int sum = 0
while i < 10000 {
    sum = sum + i
    i = i + 1
}
int dt = time.elapsed_ms(t0)
print(sum)
print(dt < 5000)
FLX
out=$(timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
# sum of 0..9999 = 49995000
if echo "$out" | grep -q "^49995000$" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "hot_loop_no_sleep_fast"
else
    fail "hot_loop_no_sleep_fast" "49995000 + completes fast" "$out"
fi

# CASE 11: POLLING LOOP — timeout-driven exit
# Documented case: maximum responsiveness, no sleep, exits via timeout
P="$WORK_DIR/p11"; setup "$P" "polling_loop"
cat > "$P/main.flx" << 'FLX'
import std time
int t0    = time.now_ms()
int count = 0
while !time.timeout(t0, 100) {
    count = count + 1
}
int dt = time.elapsed_ms(t0)
print(count > 0)
print(dt >= 100)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "polling_loop_timeout_driven"
else
    fail "polling_loop_timeout_driven" "count>0, dt>=100ms" "$out"
fi

# CASE 12: elapsed_ms with prst — survives across blocks
P="$WORK_DIR/p12"; setup "$P" "elapsed_prst"
cat > "$P/main.flx" << 'FLX'
import std time
prst int start_time = 0
start_time = time.now_ms()
time.sleep(30)
int dt = time.elapsed_ms(start_time)
print(dt >= 25)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "elapsed_ms_with_prst"
else
    fail "elapsed_ms_with_prst" "true (>=25ms)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=12
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.time: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
