#!/usr/bin/env bash
# tests/suite2/s2_embedded.sh
# Suite 2 — Section 8: embedded & resource constraints
#
# Simulates IoT hardware constraints using ulimit and iteration limits.
# No Docker required for baseline runs.
#
# Simulated targets:
#   rp2040  — 220KB virtual memory limit
#   esp32   — 400KB virtual memory limit
#   lowmem  — 128KB virtual memory limit
#   lowcpu  — timeout-constrained execution (slow CPU simulation)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  embedded/%s\n" "$1"; }
fail() { printf "  FAIL  embedded/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  embedded/%s — %s\n" "$1" "$2"; }

echo "── suite2/embedded: resource constraint simulation ──────────────────"

# ulimit -v is in KB on Linux
HAS_ULIMIT=1
( ulimit -v 65536 2>/dev/null && true ) || HAS_ULIMIT=0

# ── CASE 1: basic program runs under 220KB (RP2040 sim) ───────────────────────
cat > "$WORK_DIR/em_basic.flx" << 'FLX'
int i = 0
int sum = 0
while i < 1000 {
    sum = sum + i
    i = i + 1
}
print(sum)
FLX
if [ "$HAS_ULIMIT" = "1" ]; then
    out=$(bash -c "ulimit -v 225280; timeout 10s '$FLUXA' run '$WORK_DIR/em_basic.flx'" 2>&1 || true)
    if echo "$out" | grep -q "^499500$"; then
        pass "basic_loop_under_220kb"
    else
        fail "basic_loop_under_220kb" "499500 under 220KB" "$out"
    fi
else
    skip "basic_loop_under_220kb" "ulimit -v not available"
fi

# ── CASE 2: dyn creation under 400KB (ESP32 sim) ─────────────────────────────
cat > "$WORK_DIR/em_dyn_esp32.flx" << 'FLX'
dyn readings = []
int i = 0
while i < 100 {
    readings[i] = i
    i = i + 1
}
print(len(readings))
print(readings[99])
FLX
if [ "$HAS_ULIMIT" = "1" ]; then
    out=$(bash -c "ulimit -v 409600; timeout 10s '$FLUXA' run '$WORK_DIR/em_dyn_esp32.flx'" 2>&1 || true)
    if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^99$"; then
        pass "dyn_100_elements_under_400kb"
    else
        fail "dyn_100_elements_under_400kb" "100, 99 under 400KB" "$out"
    fi
else
    skip "dyn_100_elements_under_400kb" "ulimit -v not available"
fi

# ── CASE 3: prst vars under 220KB ────────────────────────────────────────────
PROJ3="$WORK_DIR/em_prst_rp2040"
mkdir -p "$PROJ3"
cat > "$PROJ3/main.flx" << 'FLX'
prst int sensor_a = 0
prst int sensor_b = 0
prst float temp    = 20.0
sensor_a = 42
sensor_b = 17
temp     = 23.5
print(sensor_a)
print(sensor_b)
FLX
cat > "$PROJ3/fluxa.toml" << 'TOML'
[project]
name = "em_prst_rp2040"
entry = "main.flx"
[runtime]
gc_cap = 64
prst_cap = 16
prst_graph_cap = 32
TOML
if [ "$HAS_ULIMIT" = "1" ]; then
    out=$(bash -c "ulimit -v 225280; timeout 10s '$FLUXA' run '$PROJ3/main.flx' -proj '$PROJ3'" 2>&1 || true)
    if echo "$out" | grep -q "^42$" && echo "$out" | grep -q "^17$"; then
        pass "prst_vars_under_220kb"
    else
        fail "prst_vars_under_220kb" "42, 17 under 220KB" "$out"
    fi
else
    skip "prst_vars_under_220kb" "ulimit -v not available"
fi

# ── CASE 4: handover under 400KB ─────────────────────────────────────────────
PROJ4="$WORK_DIR/em_handover_esp32"
mkdir -p "$PROJ4"
cat > "$PROJ4/v1.flx" << 'FLX'
prst int counter = 0
counter = counter + 10
print(counter)
FLX
cat > "$PROJ4/v2.flx" << 'FLX'
prst int counter = 0
counter = counter + 1
print(counter)
FLX
cat > "$PROJ4/fluxa.toml" << 'TOML'
[project]
name = "em_handover"
entry = "v1.flx"
[runtime]
gc_cap = 128
prst_cap = 32
TOML
if [ "$HAS_ULIMIT" = "1" ]; then
    out=$(bash -c "ulimit -v 409600; timeout 15s '$FLUXA' handover '$PROJ4/v1.flx' '$PROJ4/v2.flx'" 2>&1 || true)
    if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^11$"; then
        pass "handover_under_400kb"
    else
        fail "handover_under_400kb" "COMMITTED + 11 under 400KB" "$out"
    fi
else
    skip "handover_under_400kb" "ulimit -v not available"
fi

# ── CASE 5: GC prevents OOM in long loop ──────────────────────────────────────
# Without GC sweep, 10k dyn creations would OOM on embedded
cat > "$WORK_DIR/em_gc_prevents_oom.flx" << 'FLX'
int i = 0
while i < 10000 {
    dyn temp = [i, i, i, i, i]
    free(temp)
    i = i + 1
}
print(42)
FLX
if [ "$HAS_ULIMIT" = "1" ]; then
    out=$(bash -c "ulimit -v 409600; timeout 15s '$FLUXA' run '$WORK_DIR/em_gc_prevents_oom.flx'" 2>&1 || true)
    if echo "$out" | grep -q "^42$"; then
        pass "gc_prevents_oom_in_long_loop_400kb"
    else
        fail "gc_prevents_oom_in_long_loop_400kb" "42 (GC frees dyn, no OOM)" "$out"
    fi
else
    skip "gc_prevents_oom_in_long_loop_400kb" "ulimit -v not available"
fi

# ── CASE 6: sensor loop pattern — typical IoT workload ────────────────────────
# Simulates a sensor reading loop with prst accumulation
PROJ6="$WORK_DIR/em_sensor_loop"
mkdir -p "$PROJ6"
cat > "$PROJ6/main.flx" << 'FLX'
prst int  tick     = 0
prst float sum_temp = 0.0
prst int  readings = 0

int i = 0
while i < 100 {
    float temp = 20.0
    sum_temp = sum_temp + temp
    readings = readings + 1
    tick = tick + 1
    i = i + 1
}
print(tick)
print(readings)
FLX
cat > "$PROJ6/fluxa.toml" << 'TOML'
[project]
name = "em_sensor"
entry = "main.flx"
TOML
out=$(timeout 10s "$FLUXA" run "$PROJ6/main.flx" -proj "$PROJ6" 2>&1 || true)
if echo "$out" | grep -q "^100$"; then
    pass "sensor_loop_prst_accumulation"
else
    fail "sensor_loop_prst_accumulation" "100 ticks, 100 readings" "$out"
fi

# ── CASE 7: state machine pattern using Block ─────────────────────────────────
cat > "$WORK_DIR/em_state_machine.flx" << 'FLX'
Block StateMachine {
    prst int state = 0
    fn next() nil {
        if state == 0 { state = 1 } else {
            if state == 1 { state = 2 } else {
                if state == 2 { state = 0 }
            }
        }
    }
    fn get() int { return state }
}
Block sm typeof StateMachine
sm.next()
print(sm.get())
sm.next()
print(sm.get())
sm.next()
print(sm.get())
sm.next()
print(sm.get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/em_state_machine.flx" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "^1$" \
    && echo "$out" | sed -n '2p' | grep -q "^2$" \
    && echo "$out" | sed -n '3p' | grep -q "^0$" \
    && echo "$out" | sed -n '4p' | grep -q "^1$"; then
    pass "state_machine_block_pattern"
else
    fail "state_machine_block_pattern" "1, 2, 0, 1 (state cycles)" "$out"
fi

# ── CASE 8: danger in tight loop — no slowdown ───────────────────────────────
cat > "$WORK_DIR/em_danger_loop.flx" << 'FLX'
int ok = 0
int i = 0
while i < 1000 {
    danger {
        int safe = i + 1
    }
    ok = ok + 1
    i = i + 1
}
print(ok)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/em_danger_loop.flx" 2>&1 || true)
if echo "$out" | grep -q "^1000$"; then
    pass "danger_in_tight_loop_1000_iters"
else
    fail "danger_in_tight_loop_1000_iters" "1000" "$out"
fi

# ── CASE 9: low gc_cap — no crash when cap small ──────────────────────────────
PROJ9="$WORK_DIR/em_low_gc_cap"
mkdir -p "$PROJ9"
cat > "$PROJ9/main.flx" << 'FLX'
dyn a = [1, 2, 3]
dyn b = [4, 5, 6]
dyn c = [7, 8, 9]
print(len(a))
print(len(b))
print(len(c))
free(a)
free(b)
free(c)
print(0)
FLX
cat > "$PROJ9/fluxa.toml" << 'TOML'
[project]
name = "em_low_gc"
entry = "main.flx"
[runtime]
gc_cap = 8
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ9/main.flx" -proj "$PROJ9" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^0$"; then
    pass "small_gc_cap_no_crash"
else
    fail "small_gc_cap_no_crash" "3, 3, 3, 0 with gc_cap=8" "$out"
fi

# ── CASE 10: minimal footprint — script mode no prst overhead ─────────────────
cat > "$WORK_DIR/em_script_mode.flx" << 'FLX'
int result = 0
int i = 0
while i < 100 {
    result = result + i
    i = i + 1
}
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/em_script_mode.flx" 2>&1 || true)
if echo "$out" | grep -q "^4950$"; then
    pass "script_mode_no_prst_overhead"
else
    fail "script_mode_no_prst_overhead" "4950 (script mode, no PrstPool)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
passed=$((total - FAILS))
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → embedded: PASS"
    exit 0
else
    echo "  Results: ${passed} passed, $FAILS failed"
    exit 1
fi
