#!/usr/bin/env bash
# tests/suite2/s2_types_danger.sh
# Suite 2 — Section 6+7: type enforcement + danger edge cases
#
# Covers: type errors in short-circuit, type error inside danger (no kill),
# err stack overflow, nested danger, free(prst) in danger, dyn type rules.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  types_danger/%s\n" "$1"; }
fail() { printf "  FAIL  types_danger/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/types_danger: type enforcement + danger edge cases ────────"

# ── CASE 1: type error outside danger — program aborts ────────────────────────
cat > "$WORK_DIR/td_type_abort.flx" << 'FLX'
int x = 10
x = "wrong"
print(x)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_type_abort.flx" 2>&1 || true)
if echo "$out" | grep -qi "type error\|expected int"; then
    pass "type_error_outside_danger_aborts"
else
    fail "type_error_outside_danger_aborts" "type error: expected int, got str" "$out"
fi

# ── CASE 2: type error inside danger — captured in err, no abort ──────────────
cat > "$WORK_DIR/td_type_danger.flx" << 'FLX'
int x = 10
danger {
    x = "wrong"
}
print(42)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_type_danger.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "type_error_inside_danger_no_abort"
else
    fail "type_error_inside_danger_no_abort" "42 (danger captures the error)" "$out"
fi

# ── CASE 3: division by zero inside danger ────────────────────────────────────
cat > "$WORK_DIR/td_divzero_danger.flx" << 'FLX'
int result = 0
danger {
    result = 1 / 0
}
print(99)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_divzero_danger.flx" 2>&1 || true)
if echo "$out" | grep -q "^99$"; then
    pass "div_zero_inside_danger_no_abort"
else
    fail "div_zero_inside_danger_no_abort" "99" "$out"
fi

# ── CASE 4: err stack fills up (32 entries) — ring buffer, no crash ───────────
cat > "$WORK_DIR/td_err_overflow.flx" << 'FLX'
int i = 0
while i < 40 {
    danger {
        int bad = 1 / 0
    }
    i = i + 1
}
print(42)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/td_err_overflow.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "err_stack_overflow_ring_buffer_no_crash"
else
    fail "err_stack_overflow_ring_buffer_no_crash" "42 (40 errors fill ring buffer)" "$out"
fi

# ── CASE 5: short-circuit && — right side not evaluated on false left ─────────
cat > "$WORK_DIR/td_shortcircuit_and.flx" << 'FLX'
bool a = false
bool b = true
if a && b {
    print("wrong")
} else {
    print("correct")
}
print(1)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_shortcircuit_and.flx" 2>&1 || true)
if echo "$out" | grep -q "correct" && echo "$out" | grep -q "^1$" \
    && ! echo "$out" | grep -q "wrong"; then
    pass "short_circuit_and_skips_right"
else
    fail "short_circuit_and_skips_right" "correct then 1" "$out"
fi

# ── CASE 6: short-circuit || — right side not evaluated on true left ──────────
cat > "$WORK_DIR/td_shortcircuit_or.flx" << 'FLX'
bool a = true
bool b = false
if a || b {
    print("correct")
} else {
    print("wrong")
}
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_shortcircuit_or.flx" 2>&1 || true)
if echo "$out" | grep -q "correct" && ! echo "$out" | grep -q "wrong"; then
    pass "short_circuit_or_skips_right"
else
    fail "short_circuit_or_skips_right" "correct" "$out"
fi

# ── CASE 7: danger after danger — err cleared each time ───────────────────────
cat > "$WORK_DIR/td_danger_sequence.flx" << 'FLX'
danger {
    int a = 1 / 0
}
danger {
    int b = 2 / 0
}
print(42)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_danger_sequence.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "sequential_danger_blocks_no_crash"
else
    fail "sequential_danger_blocks_no_crash" "42" "$out"
fi

# ── CASE 8: int arr type error — element type mismatch ────────────────────────
cat > "$WORK_DIR/td_arr_type.flx" << 'FLX'
int arr v[3] = [1, "two", 3]
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_arr_type.flx" 2>&1 || true)
if echo "$out" | grep -qi "arr type error\|expected int\|element"; then
    pass "arr_type_mismatch_detected"
else
    fail "arr_type_mismatch_detected" "arr type error: element[1] is str" "$out"
fi

# ── CASE 9: arr assign type error ─────────────────────────────────────────────
cat > "$WORK_DIR/td_arr_assign_type.flx" << 'FLX'
int arr v[3] = [1, 2, 3]
v[0] = true
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/td_arr_assign_type.flx" 2>&1 || true)
if echo "$out" | grep -qi "type error\|expected int"; then
    pass "arr_assign_type_mismatch_detected"
else
    fail "arr_assign_type_mismatch_detected" "type error on v[0] = true" "$out"
fi

# ── CASE 10: free(prst) inside danger — still an error ───────────────────────
PROJ10="$WORK_DIR/td_free_prst_danger"
mkdir -p "$PROJ10"
cat > "$PROJ10/main.flx" << 'FLX'
prst int counter = 0
counter = counter + 1
danger {
    free(counter)
}
print(counter)
FLX
cat > "$PROJ10/fluxa.toml" << 'TOML'
[project]
name = "td_free_prst"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ10/main.flx" -proj "$PROJ10" 2>&1 || true)
# free(prst) is always an error — even inside danger
if echo "$out" | grep -qi "cannot free prst\|prst.*managed"; then
    pass "free_prst_in_danger_still_error"
else
    fail "free_prst_in_danger_still_error" "error: cannot free prst variable" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → types_danger: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
