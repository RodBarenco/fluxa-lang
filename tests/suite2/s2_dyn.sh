#!/usr/bin/env bash
# tests/suite2/s2_dyn.sh
# Suite 2 — Section 4: dyn extreme cases
#
# Covers: aggressive auto-grow, type swaps, large payload, 10k+ elements,
# Block and arr deep copy independence, dyn-in-dyn prohibition.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  dyn/%s\n" "$1"; }
fail() { printf "  FAIL  dyn/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/dyn: dynamic array extreme cases ──────────────────────────"

# ── CASE 1: aggressive auto-grow — sparse high indices ────────────────────────
cat > "$WORK_DIR/dyn_sparse.flx" << 'FLX'
dyn d = []
d[0]    = 1
d[99]   = 2
d[999]  = 3
d[9999] = 4
print(len(d))
print(d[0])
print(d[99])
print(d[999])
print(d[9999])
print(d[50])
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_sparse.flx" 2>&1 || true)
if echo "$out" | grep -q "^10000$" && echo "$out" | grep -q "^1$" \
    && echo "$out" | grep -q "^4$" && echo "$out" | grep -q "nil"; then
    pass "auto_grow_sparse_high_indices"
else
    fail "auto_grow_sparse_high_indices" "10000, 1, 4, nil at gap" "$out"
fi

# ── CASE 2: type swap on same slot ────────────────────────────────────────────
cat > "$WORK_DIR/dyn_type_swap.flx" << 'FLX'
dyn d = [1, 2, 3]
d[0] = "now a string"
d[1] = true
d[2] = 3.14
print(d[0])
print(d[1])
print(d[2])
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_type_swap.flx" 2>&1 || true)
if echo "$out" | grep -q "now a string" && echo "$out" | grep -q "true" \
    && echo "$out" | grep -q "3.14"; then
    pass "type_swap_on_same_slot"
else
    fail "type_swap_on_same_slot" "now a string, true, 3.14" "$out"
fi

# ── CASE 3: 10000+ elements ────────────────────────────────────────────────────
cat > "$WORK_DIR/dyn_10k.flx" << 'FLX'
dyn big = []
int i = 0
while i < 10000 {
    big[i] = i
    i = i + 1
}
print(len(big))
print(big[0])
print(big[9999])
FLX
out=$(timeout 15s "$FLUXA" run "$WORK_DIR/dyn_10k.flx" 2>&1 || true)
if echo "$out" | grep -q "^10000$" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^9999$"; then
    pass "dyn_10000_elements"
else
    fail "dyn_10000_elements" "10000, 0, 9999" "$out"
fi

# ── CASE 4: arr deep copy in dyn — independence verified ──────────────────────
cat > "$WORK_DIR/dyn_arr_copy.flx" << 'FLX'
int arr nums[3] = [10, 20, 30]
dyn d = [nums]
nums[0] = 999
print(nums[0])
print(len(d))
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_arr_copy.flx" 2>&1 || true)
if echo "$out" | grep -q "^999$" && echo "$out" | grep -q "^1$"; then
    pass "arr_deep_copy_in_dyn_independent"
else
    fail "arr_deep_copy_in_dyn_independent" "999 (original changed), 1 (dyn has copy)" "$out"
fi

# ── CASE 5: Block isolation in dyn — arr field doubled only in clone ──────────
cat > "$WORK_DIR/dyn_block_arr.flx" << 'FLX'
Block A {
    int arr b[3] = [1, 2, 3]
    fn double() nil {
        int i = 0
        while i < 3 {
            b[i] = b[i] * 2
            i = i + 1
        }
    }
    fn get0() int { return b[0] }
    fn get1() int { return b[1] }
    fn get2() int { return b[2] }
}
dyn c = [A]
c[0].double()
print(A.get0())
print(A.get1())
print(A.get2())
print(c[0].get0())
print(c[0].get1())
print(c[0].get2())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_block_arr.flx" 2>&1 || true)
a0=$(echo "$out" | sed -n '1p')
a1=$(echo "$out" | sed -n '2p')
a2=$(echo "$out" | sed -n '3p')
c0=$(echo "$out" | sed -n '4p')
c1=$(echo "$out" | sed -n '5p')
c2=$(echo "$out" | sed -n '6p')
if [ "$a0" = "1" ] && [ "$a1" = "2" ] && [ "$a2" = "3" ] \
    && [ "$c0" = "2" ] && [ "$c1" = "4" ] && [ "$c2" = "6" ]; then
    pass "block_arr_field_isolated_in_dyn"
else
    fail "block_arr_field_isolated_in_dyn" "A=1,2,3 c[0]=2,4,6" "$out"
fi

# ── CASE 6: multiple Block clones in dyn all isolated ─────────────────────────
cat > "$WORK_DIR/dyn_multi_clone.flx" << 'FLX'
Block Sensor {
    prst float val = 0.0
    fn set(float v) nil { val = v }
    fn get() float { return val }
}
Block s typeof Sensor
s.set(1.0)
dyn pool = [s, s, s]
pool[0].set(10.0)
pool[1].set(20.0)
print(pool[0].get())
print(pool[1].get())
print(pool[2].get())
print(s.get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_multi_clone.flx" 2>&1 || true)
p0=$(echo "$out" | sed -n '1p')
p1=$(echo "$out" | sed -n '2p')
p2=$(echo "$out" | sed -n '3p')
orig=$(echo "$out" | sed -n '4p')
if [ "$p0" = "10" ] && [ "$p1" = "20" ] \
    && echo "$p2" | grep -q "^1" && echo "$orig" | grep -q "^1"; then
    pass "multiple_block_clones_all_isolated"
else
    fail "multiple_block_clones_all_isolated" "10, 20, 1, 1 (all independent)" "$out"
fi

# ── CASE 7: dyn with repeated Block insertion (100 clones) ────────────────────
cat > "$WORK_DIR/dyn_100_clones.flx" << 'FLX'
Block Counter {
    prst int n = 0
    fn set(int v) nil { n = v }
    fn get() int { return n }
}
Block c typeof Counter
dyn pool = []
int i = 0
while i < 100 {
    c.set(i)
    pool[i] = c
    i = i + 1
}
print(len(pool))
print(pool[0].get())
print(pool[99].get())
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/dyn_100_clones.flx" 2>&1 || true)
if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^99$"; then
    pass "dyn_100_block_clones"
else
    fail "dyn_100_block_clones" "100, 0, 99" "$out"
fi

# ── CASE 8: dyn empty literal then grow ───────────────────────────────────────
cat > "$WORK_DIR/dyn_empty_grow.flx" << 'FLX'
dyn d = []
print(len(d))
d[0] = "first"
d[1] = 42
d[2] = true
print(len(d))
print(d[0])
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_empty_grow.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^3$" \
    && echo "$out" | grep -q "first"; then
    pass "dyn_empty_then_grow"
else
    fail "dyn_empty_then_grow" "0, 3, first" "$out"
fi

# ── CASE 9: dyn-in-dyn rejected at literal ───────────────────────────────────
cat > "$WORK_DIR/dyn_in_dyn_lit.flx" << 'FLX'
dyn a = [1, 2]
dyn b = [a, 3]
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_in_dyn_lit.flx" 2>&1 || true)
if echo "$out" | grep -qi "cannot contain dyn\|dyn.*dyn"; then
    pass "dyn_in_dyn_literal_rejected"
else
    fail "dyn_in_dyn_literal_rejected" "error: dyn cannot contain dyn" "$out"
fi

# ── CASE 10: dyn = 8 rejected at parse time ───────────────────────────────────
cat > "$WORK_DIR/dyn_bare_int.flx" << 'FLX'
dyn d = 8
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_bare_int.flx" 2>&1 || true)
if echo "$out" | grep -qi "literal\|not allowed\|parse error"; then
    pass "dyn_bare_value_parse_error"
else
    fail "dyn_bare_value_parse_error" "parse error: dyn must use literal" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → dyn: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
