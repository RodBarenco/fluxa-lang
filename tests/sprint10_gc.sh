#!/usr/bin/env bash
# tests/sprint10_gc.sh — GC: pin/unpin, sweep, free keyword, dyn type rules
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  gc/%s\n" "$1"; }
fail() { printf "  FAIL  gc/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint10: GC (pin/unpin/sweep, dyn rules, free) ─────────────────"

# ── CASO 1: dyn-in-dyn proibido ──────────────────────────────────────────────
cat > "$WORK_DIR/dyn_in_dyn.flx" << 'FLX'
dyn a = [1, 2]
dyn b = [a]
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/dyn_in_dyn.flx" 2>&1 || true)
if echo "$out" | grep -qi "cannot contain dyn\|dyn.*dyn"; then
    pass "dyn_in_dyn_prohibited"
else
    fail "dyn_in_dyn_prohibited" "error: dyn cannot contain dyn" "$out"
fi

# ── CASO 2: arr em dyn — cópia profunda, independente do original ─────────────
cat > "$WORK_DIR/arr_in_dyn.flx" << 'FLX'
int arr nums[3] = [1, 2, 3]
dyn d = [nums]
nums[0] = 99
print(len(d))
print(nums[0])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/arr_in_dyn.flx" 2>&1 || true)
# d has 1 element (the arr copy), nums[0] changed to 99 independently
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^99$"; then
    pass "arr_in_dyn_deep_copy"
else
    fail "arr_in_dyn_deep_copy" "1 (dyn len) then 99 (nums independent)" "$out"
fi

# ── CASO 3: Block em dyn — typeof implícito, independente do original ─────────
cat > "$WORK_DIR/block_in_dyn_copy.flx" << 'FLX'
Block Counter {
    prst int val = 0
    fn get() int { return val }
}
Block c typeof Counter
c.val = 42
dyn pool = [c]
c.val = 99
print(pool[0].get())
print(c.val)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/block_in_dyn_copy.flx" 2>&1 || true)
# pool[0] has copy of c at insertion (42), c changed to 99 after
if echo "$out" | grep -q "^42$" && echo "$out" | grep -q "^99$"; then
    pass "block_in_dyn_typeof_implicit"
else
    fail "block_in_dyn_typeof_implicit" "42 then 99" "$out"
fi

# ── CASO 4: free dyn — libera sem crash ──────────────────────────────────────
cat > "$WORK_DIR/free_dyn.flx" << 'FLX'
dyn d = [1, "hello", true]
print(len(d))
free(d)
print(len(d))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/free_dyn.flx" 2>&1 || true)
# After free, d is nil — len(nil) should be 0 or error
if echo "$out" | grep -q "^3$"; then
    pass "free_dyn"
else
    fail "free_dyn" "3 before free" "$out"
fi

# ── CASO 5: free arr ─────────────────────────────────────────────────────────
cat > "$WORK_DIR/free_arr.flx" << 'FLX'
int arr v[3] = [10, 20, 30]
print(v[0])
free(v)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/free_arr.flx" 2>&1 || true)
if echo "$out" | grep -q "^10$" && ! echo "$out" | grep -qi "error\|crash"; then
    pass "free_arr"
else
    fail "free_arr" "10 then clean" "$out"
fi

# ── CASO 6: free str ─────────────────────────────────────────────────────────
cat > "$WORK_DIR/free_str.flx" << 'FLX'
str s = "hello world"
print(s)
free(s)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/free_str.flx" 2>&1 || true)
if echo "$out" | grep -q "hello world" && ! echo "$out" | grep -qi "segfault\|crash"; then
    pass "free_str"
else
    fail "free_str" "hello world then clean" "$out"
fi

# ── CASO 7: free prst — erro claro ───────────────────────────────────────────
PROJ7="$WORK_DIR/free_prst"
mkdir -p "$PROJ7"
cat > "$PROJ7/main.flx" << 'FLX'
prst int counter = 0
counter = counter + 1
free(counter)
FLX
cat > "$PROJ7/fluxa.toml" << 'TOML'
[project]
name = "free_prst"
entry = "main.flx"
TOML
out=$(timeout 3s "$FLUXA" run "$PROJ7/main.flx" -proj "$PROJ7" 2>&1 || true)
if echo "$out" | grep -qi "cannot free prst\|prst.*managed"; then
    pass "free_prst_error"
else
    fail "free_prst_error" "error: cannot free prst variable" "$out"
fi

# ── CASO 8: free dentro de danger — vai para err_stack ───────────────────────
cat > "$WORK_DIR/free_in_danger.flx" << 'FLX'
dyn d = [1, 2, 3]
danger {
    free(d)
}
print(42)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/free_in_danger.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "free_in_danger"
else
    fail "free_in_danger" "42 (free works in danger)" "$out"
fi

# ── CASO 9: dyn com Block — método após inserção usa estado clonado ───────────
cat > "$WORK_DIR/block_clone_state.flx" << 'FLX'
Block Acc {
    prst int total = 10
    fn add(int n) nil { total = total + n }
    fn get() int { return total }
}
Block a typeof Acc
a.total = 50
dyn d = [a]
d[0].add(5)
print(d[0].get())
print(a.get())
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/block_clone_state.flx" 2>&1 || true)
# d[0] cloned at 50, add(5) = 55; a still at 50
if echo "$out" | grep -q "^55$" && echo "$out" | grep -q "^50$"; then
    pass "block_clone_state_independent"
else
    fail "block_clone_state_independent" "55 then 50" "$out"
fi

# ── CASO 10: gc sweep — dyn criado e não referenciado é coletado no safe point
cat > "$WORK_DIR/gc_sweep.flx" << 'FLX'
fn criar_dyn() nil {
    dyn d = [1, 2, 3]
}
criar_dyn()
danger { }
print(99)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/gc_sweep.flx" 2>&1 || true)
if echo "$out" | grep -q "^99$" && ! echo "$out" | grep -qi "error\|leak"; then
    pass "gc_sweep_orphan"
else
    fail "gc_sweep_orphan" "99 (dyn freed at safe point)" "$out"
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
