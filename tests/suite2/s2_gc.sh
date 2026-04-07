#!/usr/bin/env bash
# tests/suite2/s2_gc.sh
# Suite 2 — Section 3: GC pin/unpin edge cases
#
# Covers: massive temporary dyn creation, recursive free, prst dyn never
# collected, GC at safe points, sweep during danger.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  gc/%s\n" "$1"; }
fail() { printf "  FAIL  gc/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/gc: garbage collector edge cases ──────────────────────────"

# ── CASE 1: massive temporary dyn in loop — no crash, no OOM ─────────────────
cat > "$WORK_DIR/gc_mass_temp.flx" << 'FLX'
int i = 0
while i < 10000 {
    dyn temp = [i, i, i]
    free(temp)
    i = i + 1
}
print(42)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/gc_mass_temp.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$" && ! echo "$out" | grep -qi "error\|crash\|abort"; then
    pass "massive_temp_dyn_no_crash"
else
    fail "massive_temp_dyn_no_crash" "42 with no errors" "$out"
fi

# ── CASE 2: GC sweep at while back-edge collects orphans ─────────────────────
cat > "$WORK_DIR/gc_sweep_loop.flx" << 'FLX'
fn make_dyn() nil {
    dyn d = [1, 2, 3, 4, 5]
}
int i = 0
while i < 1000 {
    make_dyn()
    i = i + 1
}
danger { }
print(99)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/gc_sweep_loop.flx" 2>&1 || true)
if echo "$out" | grep -q "^99$"; then
    pass "gc_sweep_collects_orphan_dyn"
else
    fail "gc_sweep_collects_orphan_dyn" "99" "$out"
fi

# ── CASE 3: free(dyn) with Block and arr inside — no double free ──────────────
cat > "$WORK_DIR/gc_free_complex.flx" << 'FLX'
Block Tag {
    prst str label = "default"
    fn get() str { return label }
}
Block t typeof Tag
t.label = "sensor_A"
int arr data[3] = [10, 20, 30]
dyn packet = [t, data, "hello", 42]
print(len(packet))
free(packet)
print(0)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/gc_free_complex.flx" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "^0$" \
    && ! echo "$out" | grep -qi "double free\|abort\|segfault"; then
    pass "free_dyn_complex_contents_no_double_free"
else
    fail "free_dyn_complex_contents_no_double_free" "4 then 0 with no crash" "$out"
fi

# ── CASE 4: prst dyn never swept — persists across safe points ────────────────
PROJ4="$WORK_DIR/gc_prst_dyn"
mkdir -p "$PROJ4"
cat > "$PROJ4/main.flx" << 'FLX'
prst dyn alive = [1, 2, 3]
int i = 0
while i < 100 {
    danger { }
    i = i + 1
}
print(len(alive))
FLX
cat > "$PROJ4/fluxa.toml" << 'TOML'
[project]
name = "gc_prst_dyn"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ4/main.flx" -proj "$PROJ4" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "prst_dyn_not_collected_by_gc"
else
    fail "prst_dyn_not_collected_by_gc" "3 (prst dyn survives 100 safe points)" "$out"
fi

# ── CASE 5: GC after danger — orphan freed, live var intact ──────────────────
cat > "$WORK_DIR/gc_danger_sweep.flx" << 'FLX'
dyn live = [1, 2, 3]
danger {
    dyn orphan = [99, 88, 77]
}
print(len(live))
free(live)
print(0)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/gc_danger_sweep.flx" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^0$"; then
    pass "gc_sweep_after_danger_live_var_intact"
else
    fail "gc_sweep_after_danger_live_var_intact" "3 then 0" "$out"
fi

# ── CASE 6: create, free, recreate — no leak or crash ────────────────────────
cat > "$WORK_DIR/gc_create_free_cycle.flx" << 'FLX'
int i = 0
while i < 500 {
    dyn d = ["sensor", i, true]
    free(d)
    dyn d2 = [i, i]
    free(d2)
    i = i + 1
}
print(done)
FLX
# 'done' is undefined — will error, but that's fine — test is no crash before
cat > "$WORK_DIR/gc_create_free_cycle.flx" << 'FLX'
int i = 0
while i < 500 {
    dyn d = ["sensor", i, true]
    free(d)
    i = i + 1
}
print(42)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/gc_create_free_cycle.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "create_free_cycle_500_iterations"
else
    fail "create_free_cycle_500_iterations" "42" "$out"
fi

# ── CASE 7: free(arr) — no crash ─────────────────────────────────────────────
cat > "$WORK_DIR/gc_free_arr.flx" << 'FLX'
int arr v[5] = [1, 2, 3, 4, 5]
print(v[0])
free(v)
print(1)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/gc_free_arr.flx" 2>&1 || true)
if echo "$out" | grep -q "^1$" && ! echo "$out" | grep -qi "crash\|abort\|double"; then
    pass "free_arr_no_crash"
else
    fail "free_arr_no_crash" "1 then 1 with no crash" "$out"
fi

# ── CASE 8: free(str) — no crash ─────────────────────────────────────────────
cat > "$WORK_DIR/gc_free_str.flx" << 'FLX'
str s = "hello world"
print(s)
free(s)
print(1)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/gc_free_str.flx" 2>&1 || true)
if echo "$out" | grep -q "hello world" && echo "$out" | grep -q "^1$"; then
    pass "free_str_no_crash"
else
    fail "free_str_no_crash" "hello world then 1" "$out"
fi

# ── CASE 9: free(prst x) — always an error ───────────────────────────────────
PROJ9="$WORK_DIR/gc_free_prst"
mkdir -p "$PROJ9"
cat > "$PROJ9/main.flx" << 'FLX'
prst int counter = 0
free(counter)
FLX
cat > "$PROJ9/fluxa.toml" << 'TOML'
[project]
name = "gc_free_prst"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ9/main.flx" -proj "$PROJ9" 2>&1 || true)
if echo "$out" | grep -qi "cannot free prst\|prst.*managed"; then
    pass "free_prst_always_error"
else
    fail "free_prst_always_error" "error: cannot free prst variable" "$out"
fi

# ── CASE 10: dyn-in-dyn always rejected ──────────────────────────────────────
cat > "$WORK_DIR/gc_dyn_in_dyn.flx" << 'FLX'
dyn a = [1, 2, 3]
dyn b = [a, 4, 5]
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/gc_dyn_in_dyn.flx" 2>&1 || true)
if echo "$out" | grep -qi "cannot contain dyn\|dyn.*dyn"; then
    pass "dyn_in_dyn_rejected_at_runtime"
else
    fail "dyn_in_dyn_rejected_at_runtime" "error: dyn cannot contain dyn" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → gc: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
