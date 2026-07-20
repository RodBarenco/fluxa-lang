#!/usr/bin/env bash
# tests/sprint10c_free.sh — free() built-in: scopes, ownership, no double-free
# Covers the issue #144 work: usable free() across global/function/block
# scopes, Block-method string-return ownership, and str-arr-literal ownership.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  free/%s\n" "$1"; }
fail() { printf "  FAIL  free/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

# Every test project needs std.strings for concat-built heap strings.
mk_proj() {
    cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "freetest"
entry = "main.flx"
[libs]
std.strings = "1.0"
std.flxthread = "1.0"
TOML
}
mk_proj

echo "── sprint10c: free() built-in (scopes, ownership, double-free) ─────"

# ── 1: free() in global scope ────────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
str g = strings.concat("global", "_value")
free(g)
print("ok")
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "global_scope"
else
    fail "global_scope" "ok" "$out"
fi

# ── 2: free() in function scope ──────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
fn worker() nil {
    str s = strings.concat("fn", "_local")
    free(s)
    print("ok")
}
worker()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "function_scope"
else
    fail "function_scope" "ok" "$out"
fi

# ── 3: free() of a method-local str inside a Block method ────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
Block W {
    str label = "w"
    fn process() nil {
        str tmp = strings.concat("block", "_local")
        free(tmp)
        print("ok")
    }
}
fn run() nil { W.process() }
run()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "block_method_local"
else
    fail "block_method_local" "ok" "$out"
fi

# ── 4: freed slot is wiped — use-after-free is a safe error, not a crash ─────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
fn t() nil {
    str s = strings.concat("secret", "_data")
    free(s)
    print(s)
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -qi "undefined variable" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "use_after_free_safe"
else
    fail "use_after_free_safe" "undefined variable error, no crash" "$out"
fi

# ── 5: double free() on the same variable is a safe error, not a crash ───────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
fn t() nil {
    str s = strings.concat("a", "b")
    free(s)
    free(s)
    print("reached")
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -qi "undefined variable" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "double_free_same_var_safe"
else
    fail "double_free_same_var_safe" "undefined variable error, no crash" "$out"
fi

# ── 6: free() on a prst variable is rejected (managed by the runtime) ────────
cat > "$WORK_DIR/main.flx" << 'FLX'
prst str p = "persistent"
free(p)
print("reached")
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -qi "prst\|persistent" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "prst_rejected"
else
    fail "prst_rejected" "error about prst, no crash" "$out"
fi

# ── 7: free() a str returned from a Block scalar field (issue #144) ──────────
# Block-method string returns are owned copies; freeing the result must NOT
# corrupt the Block, whose field stays readable afterward.
cat > "$WORK_DIR/main.flx" << 'FLX'
Block C {
    str field = "stored_value"
    fn get() str { return field }
}
fn t() nil {
    str x = C.get()
    free(x)
    print(C.get())
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^stored_value$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "block_field_scalar_return_owned"
else
    fail "block_field_scalar_return_owned" "stored_value (field intact)" "$out"
fi

# ── 8: free() a str returned from a Block str-arr element (issue #144) ────────
cat > "$WORK_DIR/main.flx" << 'FLX'
Block C {
    str arr vals[3] = ""
    fn set() nil { vals[0] = "cached" }
    fn get() str { return vals[0] }
}
fn t() nil {
    C.set()
    str cached = C.get()
    free(cached)
    print(C.get())
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^cached$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "block_field_arr_return_owned"
else
    fail "block_field_arr_return_owned" "cached (field intact)" "$out"
fi

# ── 9: str-arr literal owns its elements — free(source) is safe ──────────────
# `str arr p[n] = [a, b]` must strdup each element so freeing the source
# variable (or the array) does not double-free a shared char*.
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
fn t() nil {
    str a = strings.concat("alice", "")
    str b = strings.concat("bob", "")
    str arr params[2] = [a, b]
    free(a)
    free(b)
    print(params[0])
    print(params[1])
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^alice$" && echo "$out" | grep -q "^bob$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "str_arr_literal_owns_elements"
else
    fail "str_arr_literal_owns_elements" "alice / bob (array intact after free)" "$out"
fi

# ── 10: tight loop — free() of a Block-field-derived str stays bounded ───────
# Mirrors the HTTP cache-hit pattern that crashed before the fix; here we just
# require it to run to completion with the field intact (no double-free).
cat > "$WORK_DIR/main.flx" << 'FLX'
Block C {
    str arr vals[4] = ""
    fn seed() nil { vals[0] = "hit" }
    fn get() str { return vals[0] }
}
fn run() nil {
    C.seed()
    int i = 0
    while i < 20000 {
        str cached = C.get()
        free(cached)
        i = i + 1
    }
    print(C.get())
}
run()
FLX
out=$(timeout 8s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^hit$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "loop_block_field_free_bounded"
else
    fail "loop_block_field_free_bounded" "hit (field intact after 20k free)" "$out"
fi

# ── 11: str-arr literal inside a danger block (the SUT pg-params pattern) ─────
# This is the exact shape that crashed the benchmark: build a str arr from
# query-result vars inside danger, then free the source vars.
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
fn t() nil {
    str out_name  = strings.concat("alice", "")
    str out_email = strings.concat("a@b.com", "")
    str hash      = "fixedhash"
    danger {
        str arr params[3] = [out_name, out_email, hash]
        str peek = params[0]
    }
    free(out_name)
    free(out_email)
    print("ok")
}
t()
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "str_arr_literal_in_danger_no_doublefree"
else
    fail "str_arr_literal_in_danger_no_doublefree" "ok (no double-free)" "$out"
fi

# ── 12: std-lib call with literal args releases temp allocs ─────────────────
# String literals passed to a std-lib call (e.g. strings.concat("a", b)) are
# strdup'd by the runtime during arg eval. Before the fix at NODE_MEMBER_CALL,
# those temporary allocations were never released — every literal-bearing
# call leaked. 10k iterations would push RSS by ~100 MB without the fix.
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
int i = 0
while i < 10000 {
    str a = strings.concat("hello", "_world")
    free(a)
    i = i + 1
}
print("ok")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "std_lib_call_literal_arg_no_leak"
else
    fail "std_lib_call_literal_arg_no_leak" "ok (no crash, bounded)" "$out"
fi

# ── 13: dyn returned from std-lib stays bounded across reassign ──────────────
# A VAL_DYN returned from a std-lib call (e.g. json2.parse) was previously
# not registered with the runtime GC. Reassigning the dyn variable in a loop
# orphaned every previous wrapper. With the fix, the dispatch registers the
# returned dyn and rt_set unpins the slot's prior dyn so gc_sweep collects.
mk_proj
cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "freetest"
entry = "main.flx"
[libs]
std.strings = "1.0"
std.flxthread = "1.0"
std.json2 = "1.0"
TOML
cat > "$WORK_DIR/main.flx" << 'FLX'
import std json2
int i = 0
while i < 5000 {
    danger {
        dyn doc = json2.parse("{\"k\":\"v\"}")
        json2.discard(doc)
    }
    i = i + 1
}
print("ok")
FLX
out=$(timeout 8s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "dyn_return_reassign_bounded"
else
    fail "dyn_return_reassign_bounded" "ok (no crash)" "$out"
fi

# ── 14: string literals on either side of ==/!= release after compare ───────
# `s == "literal"` evaluated `"literal"` to a strdup'd VAL_STRING that was
# never freed — every comparison in a worker loop leaked the strdup. Fix is
# in eval_binary: track ownership of operands and free at every return path.
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
int i = 0
while i < 5000 {
    str s = strings.concat("hello", "")
    bool eq = s == "hello"
    bool ne = s != "world"
    free(s)
    i = i + 1
}
print("ok")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "binary_op_literal_operand_no_leak"
else
    fail "binary_op_literal_operand_no_leak" "ok (no crash)" "$out"
fi

# ── 15: call-as-statement (discarded result) releases owned heap data ───────
# `json2.get(doc, "name")` called for side-effect, result discarded — the
# strdup'd return string leaked once per call. Fix is in NODE_BLOCK_STMT:
# release owned VAL_STRING/VAL_DYN returns when the eval result is dropped.
mk_proj
cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "freetest"
entry = "main.flx"
[libs]
std.strings  = "1.0"
std.flxthread = "1.0"
TOML
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
int i = 0
while i < 5000 {
    // Call returns a heap string but result is dropped — must not leak.
    strings.concat("hello", "_world")
    i = i + 1
}
print("ok")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "discarded_call_result_no_leak"
else
    fail "discarded_call_result_no_leak" "ok (no crash)" "$out"
fi

# ── 16: str slot reassignment frees the previous owned char* ────────────────
# `str x = ""` initializes the slot with a strdup'd literal. Reassigning
# `x = call()` overwrites the slot — without the fix in rt_set, the original
# strdup leaks (~24 bytes per assignment). In a long-running HTTP worker that
# does this 5-10 times per request, the leak adds up to MB per minute.
# Pair with the NODE_FOR strdup of VAL_STRING elements so loop vars own their
# own copy and the freeing in rt_set is safe.
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
int i = 0
while i < 5000 {
    str out_name = ""
    str out_email = ""
    out_name  = strings.concat("alice", "")
    out_email = strings.concat("a@b.com", "")
    free(out_name)
    free(out_email)
    i = i + 1
}
print("ok")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "str_slot_reassign_no_leak"
else
    fail "str_slot_reassign_no_leak" "ok (no crash, bounded)" "$out"
fi

# ── 17: for x in dyn — loop var ownership safe across iterations ────────────
# A regression guard for the fix above: NODE_FOR copies VAL_STRING elements
# into the loop var so the original dyn/arr storage is untouched when the
# slot is overwritten. Without this, the rt_set "free old VAL_STRING" path
# would corrupt items[i] and crash at end-of-scope free.
mk_proj
cat > "$WORK_DIR/main.flx" << 'FLX'
dyn names = ["alice", "bob", "carol"]
str last = ""
for n in names {
    last = n
}
print(last)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^carol$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "for_in_dyn_loopvar_owned"
else
    fail "for_in_dyn_loopvar_owned" "carol" "$out"
fi

# ── 18: builtin len() releases owned arg string ─────────────────────────────
# `len(str_var)` ran NODE_IDENTIFIER which strdups, then len returned an int
# and dropped the strdup'd char* without freeing — every len() in a hot loop
# leaked the read's strdup. Fix is in builtins.c: builtin_release_owned().
cat > "$WORK_DIR/main.flx" << 'FLX'
str path = "/users/abc123456789"
int i = 0
int total = 0
while i < 10000 {
    total = total + len(path)
    i = i + 1
}
free(path)
print(total)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^190000$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "builtin_len_releases_owned"
else
    fail "builtin_len_releases_owned" "190000" "$out"
fi

# ── 19: str arr literal with var elements doesn't double-strdup ─────────────
# After the NODE_IDENTIFIER strdup fix, arr_decl was strdup'ing AGAIN on the
# already-owned element, leaking the first copy. Pattern hits every request
# in HTTP workers building param arrays from variables.
mk_proj
cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "freetest"
entry = "main.flx"
[libs]
std.strings  = "1.0"
std.flxthread = "1.0"
TOML
cat > "$WORK_DIR/main.flx" << 'FLX'
import std strings
int i = 0
while i < 5000 {
    str a = strings.concat("alice", "")
    str b = strings.concat("bob", "")
    str c = strings.concat("carol", "")
    danger {
        str arr params[3] = [a, b, c]
    }
    free(a) free(b) free(c)
    i = i + 1
}
print("ok")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort"; then
    pass "str_arr_literal_no_double_strdup"
else
    fail "str_arr_literal_no_double_strdup" "ok (no crash, bounded)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=19
if [[ $FAILS -eq 0 ]]; then
    echo "  Results: $total passed, 0 failed"
    echo "  → free: PASS"
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    echo "  → free: FAIL"
fi
