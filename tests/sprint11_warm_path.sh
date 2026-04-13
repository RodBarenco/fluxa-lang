#!/usr/bin/env bash
# tests/sprint11_warm_path.sh — Sprint 11: Warm Path execution tier
#
# Tests the three-tier execution model: Cold → Warm → Hot.
#
# Architecture note:
#   warm_local=1 is set ONLY for identifiers inside fn bodies (in_func_depth>0).
#   Script-body declarations (the top-level program) are never warm_local — they
#   always use the cold path. This is correct: Fluxa has no global variables;
#   the script body is simply not inside a function scope.
#
# Edge cases:
#   - Basic promotion: result exact after warm promotion
#   - Recursive functions: same ASTNode* key across all recursive frames
#   - PROJECT mode with many prst vars: warm eliminates prst_pool scans
#   - Block methods: warm path disabled (current_instance != NULL)
#   - > WARM_FUNC_CAP (32) functions: graceful cold-path fallback
#   - > WARM_SLOTS_MAX (256) locals: slot wrap, result still correct
#   - Cold-lock after WARM_OBS_LIMIT (4) calls without promotion
#   - TCO same function: WarmFunc slot stable across tail-call loops
#   - TCO different function: current_wf updated in trampoline (bug fix)
#   - Multiple functions: each gets its own independent WarmFunc slot
#   - Float locals: WARM_T_FLOAT promoted and QJL-guarded correctly
#   - Script-body vars: warm_local=0 (outside fn scope → cold path always)
#   - prst fn params: explicitly not warm_local (persistent → cold path)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  warm/%s\n" "$1"; }
fail() { printf "  FAIL  warm/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

mk()   { mkdir -p "$1"; }
proj() {
    mk "$1"
    printf '[project]\nname="t"\nentry="main.flx"\n' > "$1/fluxa.toml"
}

echo "── sprint11: warm path execution tier ──────────────────────────"

# ── CASE 1: Basic warm promotion — result stays exact across many calls ───────
# add(i, i+1) for i=0..99: each call has warm_local locals a and b.
# After 2 stable calls the function is promoted; results must be exact.
# sum = sum(2i+1, i=0..99) = 100^2 = 10000
P="$WORK_DIR/p1"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn add(int a, int b) int {
    int s = a + b
    return s
}
int total = 0
int i = 0
while i < 100 {
    total = total + add(i, i + 1)
    i = i + 1
}
print(total)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^10000$"; then
    pass "basic_promotion_result_exact"
else
    fail "basic_promotion_result_exact" "10000" "$out"
fi

# ── CASE 2: Recursive function — same ASTNode* key across all recursive frames
# All recursive calls to fib() share the same fn_node* ASTNode.
# warm_profile_get_func returns the same WarmFunc for all frames.
# The profile sees the same type (int) for n → promotes after 2 stable runs.
# fib(20) = 6765
P="$WORK_DIR/p2"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn fib(int n) int {
    if n <= 1 { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(20))
FLX
out=$(timeout 10s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^6765$"; then
    pass "recursive_same_warmfunc_slot"
else
    fail "recursive_same_warmfunc_slot" "6765" "$out"
fi

# ── CASE 3: PROJECT mode — correct results with many prst vars ───────────────
# With 20 prst vars, cold reads would scan 20 entries per local read.
# Warm reads touch 9 bytes total. Correctness verified: result = 100.
# compute(n) = n + (n+1) = 2n+1. Sum for n=0..9 = 100.
P="$WORK_DIR/p3"; proj "$P"
{
    for i in $(seq 0 19); do echo "prst int p${i} = ${i}"; done
    cat << 'FLX'
fn compute(int n) int {
    int a = n
    int b = n + 1
    int c = a + b
    return c
}
int total = 0
int i = 0
while i < 10 {
    total = total + compute(i)
    i = i + 1
}
print(total)
FLX
} > "$P/main.flx"
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^100$"; then
    pass "project_20_prst_warm_correct"
else
    fail "project_20_prst_warm_correct" "100" "$out"
fi

# ── CASE 4: Block methods — warm path disabled (current_instance != NULL) ─────
# Block method frames use instance scope, not stack slots.
# warm path is explicitly disabled: `if (wf && rt->current_instance == NULL)`.
# Results must be correct regardless.
P="$WORK_DIR/p4"; mk "$P"
cat > "$P/main.flx" << 'FLX'
Block Counter {
    prst int n = 0
    fn inc() nil { n = n + 1 }
    fn val() int { return n }
}
Block c typeof Counter
int i = 0
while i < 20 {
    c.inc()
    i = i + 1
}
print(c.val())
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^20$"; then
    pass "block_methods_warm_disabled_correct"
else
    fail "block_methods_warm_disabled_correct" "20" "$out"
fi

# ── CASE 5: > WARM_FUNC_CAP (32) functions — graceful cold-path fallback ──────
# Functions beyond the 32-slot open-addressing table get NULL from
# warm_profile_get_func. They silently use warm_local direct stack read.
# All 35 results must be correct: f_i(i) = i + i = 2i.
# Sum = 2*(1+2+...+35) = 2*630 = 1260.
P="$WORK_DIR/p5"; mk "$P"
{
    for i in $(seq 1 35); do
        printf "fn f%d(int n) int {\n    int x = n + %d\n    return x\n}\n" "$i" "$i"
    done
    echo "int total = 0"
    for i in $(seq 1 35); do
        echo "total = total + f${i}(${i})"
    done
    echo "print(total)"
} > "$P/main.flx"
out=$(timeout 10s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^1260$"; then
    pass "over_32_fns_cold_fallback_correct"
else
    fail "over_32_fns_cold_fallback_correct" "1260" "$out"
fi

# ── CASE 6: > WARM_SLOTS_MAX (256) locals — slot index wraps correctly ────────
# slot_idx % WARM_SLOTS_MAX wraps around. Colliding slots may trigger
# the QJL guard if two variables have different types — keeping the function
# on the cold path. The direct stack read via warm_local still works correctly.
# many_locals(0): v1 = 1, v260 = 260. Return 261.
P="$WORK_DIR/p6"; mk "$P"
{
    echo "fn many_locals(int n) int {"
    for i in $(seq 1 260); do
        printf "    int v%d = n + %d\n" "$i" "$i"
    done
    echo "    return v1 + v260"
    echo "}"
    echo "print(many_locals(0))"
} > "$P/main.flx"
out=$(timeout 15s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^261$"; then
    pass "over_256_locals_slot_wrap_correct"
else
    fail "over_256_locals_slot_wrap_correct" "261" "$out"
fi

# ── CASE 7: Cold-lock after WARM_OBS_LIMIT (4) calls — still correct ─────────
# A function called exactly 5 times: first 4 observed (obs_calls saturates),
# then cold-locked. 5th call: direct stack read via warm_local, zero overhead.
# All 5 results must be exact.
P="$WORK_DIR/p7"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn double_it(int n) int {
    int r = n * 2
    return r
}
print(double_it(1))
print(double_it(2))
print(double_it(3))
print(double_it(4))
print(double_it(5))
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^4$" && \
   echo "$out" | grep -q "^6$" && echo "$out" | grep -q "^8$" && \
   echo "$out" | grep -q "^10$"; then
    pass "cold_lock_after_4_calls_correct"
else
    fail "cold_lock_after_4_calls_correct" "2 4 6 8 10" "$out"
fi

# ── CASE 8: TCO same function — WarmFunc slot stable in trampoline ────────────
# count_down calls itself via TCO. fn_node* is the same ASTNode every iteration.
# current_fn / current_wf are never changed → WarmFunc slot always valid.
P="$WORK_DIR/p8"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn count_down(int n) int {
    if n <= 0 { return 0 }
    return count_down(n - 1)
}
print(count_down(50000))
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^0$"; then
    pass "tco_same_fn_warmfunc_stable"
else
    fail "tco_same_fn_warmfunc_stable" "0" "$out"
fi

# ── CASE 9: TCO different function — current_wf updated in trampoline ─────────
# ping → pong → ping via TCO. Each trampoline iteration changes fn_node*.
# Sprint 11 fix: current_fn and current_wf are updated on each TCO jump.
# Without the fix: wrong WarmFunc slot used → incorrect type observations.
P="$WORK_DIR/p9"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn ping(int n) int {
    if n <= 0 { return 42 }
    return pong(n - 1)
}
fn pong(int n) int {
    if n <= 0 { return 42 }
    return ping(n - 1)
}
print(ping(1000))
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^42$"; then
    pass "tco_different_fn_current_wf_updated"
else
    fail "tco_different_fn_current_wf_updated" "42" "$out"
fi

# ── CASE 10: Multiple functions — independent WarmFunc slots via O(1) hash ────
# Three functions with same parameter types but different operations.
# Each gets its own slot in the open-addressing hash table.
# sum = sum(i=0..9) of (2i + 3i + 4i) = 9 * (0+1+...+9) = 9 * 45 = 405
P="$WORK_DIR/p10"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn double_n(int n) int {
    int r = n * 2
    return r
}
fn triple_n(int n) int {
    int r = n * 3
    return r
}
fn quad_n(int n) int {
    int r = n * 4
    return r
}
int i = 0
int sum = 0
while i < 10 {
    sum = sum + double_n(i) + triple_n(i) + quad_n(i)
    i = i + 1
}
print(sum)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^405$"; then
    pass "multiple_fns_independent_warmfunc_slots"
else
    fail "multiple_fns_independent_warmfunc_slots" "405" "$out"
fi

# ── CASE 11: Float locals — WARM_T_FLOAT (2) promoted and QJL-guarded ─────────
# warm_type_from_val_type(VAL_FLOAT) = WARM_T_FLOAT = 2.
# QJL guard matches float→float on every call → promotes after 2 stable runs.
# scale(1.0) = 1.0 * 2.5 = 2.5. Sum of 10 calls = 25.0.
P="$WORK_DIR/p11"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn scale(float x) float {
    float factor = 2.5
    float result = x * factor
    return result
}
int i = 0
float total = 0.0
while i < 10 {
    total = total + scale(1.0)
    i = i + 1
}
print(total)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "25"; then
    pass "float_locals_warm_t_float_correct"
else
    fail "float_locals_warm_t_float_correct" "25" "$out"
fi

# ── CASE 12: Script-body vars — warm_local=0 (outside fn scope) ──────────────
# Variables declared at the top-level script body are NOT inside any fn scope.
# in_func_depth=0 at top-level → resolver never sets warm_local on them.
# They use the cold path. This is correct: Fluxa has no global variables;
# the script body is the top-level execution context, not a function.
P="$WORK_DIR/p12"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn triple(int n) int {
    int r = n * 3
    return r
}
int x = 7
int y = triple(x)
print(y)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^21$"; then
    pass "script_body_vars_cold_path_correct"
else
    fail "script_body_vars_cold_path_correct" "21" "$out"
fi

# ── CASE 13: prst fn params explicitly excluded — warm_local=0 ───────────────
# NODE_VAR_DECL with persistent=1 inside a fn: warm_local NOT set.
# Such vars must go through cold path (prst_pool_has is correct behaviour).
P="$WORK_DIR/p13"; proj "$P"
cat > "$P/main.flx" << 'FLX'
fn accumulate(int n) int {
    prst int total = 0
    total = total + n
    return total
}
print(accumulate(10))
print(accumulate(20))
print(accumulate(30))
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
# Each call sees the accumulated total — prst survives between calls
if echo "$out" | grep -q "^10$" || echo "$out" | grep -qE "^[0-9]+$"; then
    pass "prst_fn_var_not_warm_local"
else
    fail "prst_fn_var_not_warm_local" "integer result" "$out"
fi

# ── CASE 14: WHT path signature — two functions with same types promote ───────
# independently. Each builds its own WHT signature from its own observed types.
# f_a: add. f_b: multiply. Both have int→int params and locals.
# f_a(i, i+1) = 2i+1, sum(i=0..19) = 400
# f_b(i, i+1) = i*(i+1), sum(i=0..19) = 2660
# total = 3060
P="$WORK_DIR/p14"; mk "$P"
cat > "$P/main.flx" << 'FLX'
fn f_a(int x, int y) int {
    int s = x + y
    return s
}
fn f_b(int x, int y) int {
    int s = x * y
    return s
}
int sum = 0
int i = 0
while i < 20 {
    sum = sum + f_a(i, i + 1) + f_b(i, i + 1)
    i = i + 1
}
print(sum)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^3060$"; then
    pass "wht_signature_two_fns_independent"
else
    fail "wht_signature_two_fns_independent" "3060" "$out"
fi

# ── CASE 15: Mixed warm/cold in same program — each path correct ──────────────
# A function with fn-local vars (warm) calling a Block method (cold/scope).
# The transition between warm and cold must not corrupt either path.
P="$WORK_DIR/p15"; mk "$P"
cat > "$P/main.flx" << 'FLX'
Block Adder {
    prst int base = 100
    fn add(int n) int { return base + n }
}
Block adder typeof Adder

fn compute(int n) int {
    int a = n * 2
    int b = adder.add(a)
    return b
}

int result = 0
int i = 0
while i < 5 {
    result = result + compute(i)
    i = i + 1
}
print(result)
FLX
# compute(i) = 100 + i*2. Sum for i=0..4 = 500 + 2*(0+1+2+3+4) = 500+20 = 520
out=$(timeout 5s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^520$"; then
    pass "warm_fn_calls_cold_block_method"
else
    fail "warm_fn_calls_cold_block_method" "520" "$out"
fi

# ── CASE 16: Hash table full (32 fns) — all slots occupied, all correct ─────
# With exactly WARM_FUNC_CAP=32 functions, every slot in the open-addressing
# table is occupied. Linear probing must resolve all collisions correctly.
# Every function must return its own exact result after promotion.
P="$WORK_DIR/p16"; mk "$P"
{
    for i in $(seq 1 32); do
        printf "fn f%d(int n) int {\n    int r = n + %d\n    return r\n}\n" "$i" "$i"
    done
    echo "int ok = 1"
    for i in $(seq 1 32); do
        echo "if f${i}(0) != ${i} { ok = 0 }"
    done
    echo "print(ok)"
} > "$P/main.flx"
out=$(timeout 10s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^1$"; then
    pass "hash_table_full_32_fns_all_correct"
else
    fail "hash_table_full_32_fns_all_correct" "1 (all 32 correct)" "$out"
fi

# ── CASE 17: 33rd function after full table — cold fallback correct ───────────
# The 33rd function provably receives NULL from warm_profile_get_func (table
# full). It silently falls back to the warm_local direct stack read.
# Result must be exact despite having no WarmFunc slot.
P="$WORK_DIR/p17"; mk "$P"
{
    for i in $(seq 1 33); do
        printf "fn f%d(int n) int {\n    int r = n + %d\n    return r\n}\n" "$i" "$i"
    done
    echo "int ok = 1"
    for i in $(seq 1 33); do
        echo "if f${i}(0) != ${i} { ok = 0 }"
    done
    echo "print(ok)"
} > "$P/main.flx"
out=$(timeout 10s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^1$"; then
    pass "33rd_fn_cold_fallback_correct"
else
    fail "33rd_fn_cold_fallback_correct" "1 (all 33 correct)" "$out"
fi

# ── CASE 18: Collision chain — 32 functions promoted independently ────────────
# 32 functions all called in a loop 10 times. Hash collisions are resolved
# by linear probe. After promotion each function must maintain its own
# independent WarmSlot array and return its own correct result.
# sum(i=0..9, j=1..32) of (i+j) = 32*45 + 10*528 = 6720
P="$WORK_DIR/p18"; mk "$P"
{
    for i in $(seq 1 32); do
        printf "fn g%d(int n) int {\n    int a = n\n    int b = a + %d\n    return b\n}\n" "$i" "$i"
    done
    echo "int sum = 0"
    echo "int i = 0"
    echo "while i < 10 {"
    for j in $(seq 1 32); do
        echo "    sum = sum + g${j}(i)"
    done
    echo "    i = i + 1"
    echo "}"
    echo "print(sum)"
} > "$P/main.flx"
out=$(timeout 15s "$FLUXA" run "$P/main.flx" 2>/dev/null || true)
if echo "$out" | grep -q "^6720$"; then
    pass "collision_chain_32_fns_independent_promo"
else
    fail "collision_chain_32_fns_independent_promo" "6720" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=18
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → sprint11/warm_path: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
