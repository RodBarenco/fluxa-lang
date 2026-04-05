#!/usr/bin/env bash
# tests/sprint10_semantics.sh
# Semantic correctness: dyn type rules, arr type enforcement,
# arr deep copy, Block isolation in dyn.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  semantics/%s\n" "$1"; }
fail() { printf "  FAIL  semantics/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint10: semantic correctness ───────────────────────────────────"

# ── dyn type rules ────────────────────────────────────────────────────────────

# CASO 1: dyn a = 8 — rejected at parse time
cat > "$WORK_DIR/dyn_prim.flx" << 'FLX'
dyn a = 8
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/dyn_prim.flx" 2>&1 || true)
if echo "$out" | grep -qi "literal\|not allowed\|parse error"; then
    pass "dyn_bare_value_rejected"
else
    fail "dyn_bare_value_rejected" "parse error on dyn a = 8" "$out"
fi

# CASO 2: dyn = [] is valid (empty literal)
cat > "$WORK_DIR/dyn_empty.flx" << 'FLX'
dyn d = []
print(len(d))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/dyn_empty.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$"; then
    pass "dyn_empty_literal_valid"
else
    fail "dyn_empty_literal_valid" "0" "$out"
fi

# CASO 3: dyn-in-dyn rejected at runtime
cat > "$WORK_DIR/dyn_in_dyn.flx" << 'FLX'
dyn a = [1, 2]
dyn b = [a]
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/dyn_in_dyn.flx" 2>&1 || true)
if echo "$out" | grep -qi "cannot contain dyn\|dyn.*dyn"; then
    pass "dyn_in_dyn_rejected"
else
    fail "dyn_in_dyn_rejected" "error: dyn cannot contain dyn" "$out"
fi

# ── arr type enforcement ───────────────────────────────────────────────────────

# CASO 4: int arr rejects str element
cat > "$WORK_DIR/arr_type_str.flx" << 'FLX'
int arr v[3] = [1, "dois", true]
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_type_str.flx" 2>&1 || true)
if echo "$out" | grep -qi "arr type error\|expected int\|element.*str"; then
    pass "arr_rejects_wrong_type_literal"
else
    fail "arr_rejects_wrong_type_literal" "type error on mixed arr literal" "$out"
fi

# CASO 5: float arr rejects int (different type)
cat > "$WORK_DIR/arr_type_int.flx" << 'FLX'
float arr v[2] = [1.0, 2]
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_type_int.flx" 2>&1 || true)
if echo "$out" | grep -qi "arr type error\|expected float"; then
    pass "arr_rejects_int_in_float_arr"
else
    fail "arr_rejects_int_in_float_arr" "type error: float arr with int element" "$out"
fi

# CASO 6: int arr with all ints — valid
cat > "$WORK_DIR/arr_type_ok.flx" << 'FLX'
int arr v[3] = [10, 20, 30]
print(v[0])
print(v[2])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_type_ok.flx" 2>&1 || true)
if echo "$out" | grep -q "^10$" && echo "$out" | grep -q "^30$"; then
    pass "arr_accepts_correct_type"
else
    fail "arr_accepts_correct_type" "10 30" "$out"
fi

# ── arr deep copy ─────────────────────────────────────────────────────────────

# CASO 7: copy arr from Block field — independent
cat > "$WORK_DIR/arr_copy_block.flx" << 'FLX'
Block Caixa {
    int arr dados[3] = [1, 2, 3]
    fn get0() int { return dados[0] }
}
Block c typeof Caixa
int arr copia[3] = c.dados
copia[0] = 99
print(c.get0())
print(copia[0])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_copy_block.flx" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^99$"; then
    pass "arr_deep_copy_from_block"
else
    fail "arr_deep_copy_from_block" "1 then 99 (independent)" "$out"
fi

# CASO 8: copy arr — destination must be >= source size
cat > "$WORK_DIR/arr_copy_small.flx" << 'FLX'
Block Big { int arr data[5] = [1, 2, 3, 4, 5] }
Block b typeof Big
int arr small[2] = b.data
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_copy_small.flx" 2>&1 || true)
if echo "$out" | grep -qi "larger\|size\|error"; then
    pass "arr_copy_size_check"
else
    fail "arr_copy_size_check" "error: source larger than destination" "$out"
fi

# CASO 9: copy arr — destination larger than source, extra slots zeroed
cat > "$WORK_DIR/arr_copy_larger.flx" << 'FLX'
Block Src { int arr data[2] = [7, 8] }
Block s typeof Src
int arr dest[4] = s.data
print(dest[0])
print(dest[1])
print(dest[2])
print(dest[3])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_copy_larger.flx" 2>&1 || true)
if echo "$out" | grep -q "^7$" && echo "$out" | grep -q "^8$" \
    && echo "$out" | grep -q "^0$"; then
    pass "arr_copy_larger_dest_zero_padded"
else
    fail "arr_copy_larger_dest_zero_padded" "7 8 0 0" "$out"
fi

# ── Block isolation in dyn ────────────────────────────────────────────────────

# CASO 10: exact test from spec — Block with arr in dyn, only clone affected
cat > "$WORK_DIR/block_arr_dyn_isolation.flx" << 'FLX'
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
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/block_arr_dyn_isolation.flx" 2>&1 || true)
# A unchanged: 1 2 3 — c[0] doubled: 2 4 6
a0=$(echo "$out" | sed -n '1p')
a1=$(echo "$out" | sed -n '2p')
a2=$(echo "$out" | sed -n '3p')
c0=$(echo "$out" | sed -n '4p')
c1=$(echo "$out" | sed -n '5p')
c2=$(echo "$out" | sed -n '6p')
if [ "$a0" = "1" ] && [ "$a1" = "2" ] && [ "$a2" = "3" ] \
    && [ "$c0" = "2" ] && [ "$c1" = "4" ] && [ "$c2" = "6" ]; then
    pass "block_arr_dyn_full_isolation"
else
    fail "block_arr_dyn_full_isolation" "A=1,2,3 c[0]=2,4,6" "$out"
fi

# CASO 11: changes to original Block after dyn insertion don't affect clone
cat > "$WORK_DIR/block_dyn_post_insert.flx" << 'FLX'
Block Contador {
    prst int total = 0
    fn inc() nil { total = total + 1 }
    fn get() int { return total }
}
Block c typeof Contador
c.total = 10
dyn pool = [c]
c.inc()
c.inc()
print(c.get())
print(pool[0].get())
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/block_dyn_post_insert.flx" 2>&1 || true)
if echo "$out" | grep -q "^12$" && echo "$out" | grep -q "^10$"; then
    pass "block_original_changes_dont_affect_clone"
else
    fail "block_original_changes_dont_affect_clone" "c=12, pool[0]=10" "$out"
fi

# CASO 12: multiple blocks in dyn all isolated from each other
cat > "$WORK_DIR/block_dyn_multi_isolation.flx" << 'FLX'
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
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/block_dyn_multi_isolation.flx" 2>&1 || true)
if echo "$out" | grep -q "^10" && echo "$out" | grep -q "^20" \
    && echo "$out" | grep -q "^1.0$\|^1$"; then
    pass "multiple_block_clones_isolated"
else
    fail "multiple_block_clones_isolated" "10 20 1 1 (all independent)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=12
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → semantics: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
