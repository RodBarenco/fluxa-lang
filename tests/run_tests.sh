#!/bin/bash
# run_tests.sh — Fluxa automated test runner with PASS/FAIL
# Usage: ../fluxa-lang/tests/run_tests.sh
#        make test-runner
#
# Always run from the project root (where the fluxa binary lives).
# Exit code: 0 = all passed, 1 = any failed.
# ── Locate binary and project root ───────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Binary: explicit argument, or ./fluxa relative to project root
if [ -n "$1" ]; then
    FLUXA="$1"
else
    FLUXA="$PROJECT_ROOT/fluxa"
fi

PASS=0
FAIL=0
ERRORS=""
TMP="$(mktemp)"

run_test() {
    local name="$1"
    local file="$PROJECT_ROOT/$2"   # always relative to project root
    local expected="$3"
    local expect_fail="${4:-0}"     # 1 = test is expected to produce an error

    # Capture stdout via temp file — avoids locale/encoding issues with $()
    "$FLUXA" run "$file" >"$TMP" 2>/dev/null
    local exit_code=$?
    local actual
    actual=$(cat "$TMP")

    if [ "$expect_fail" = "1" ]; then
        local actual_err
        actual_err=$("$FLUXA" run "$file" 2>&1 >/dev/null)
        if [ $exit_code -ne 0 ] || [ -n "$actual_err" ]; then
            echo "  PASS  $name"
            PASS=$((PASS + 1))
        else
            echo "  FAIL  $name  (expected error, got none)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  $name: expected error exit"
        fi
        return
    fi

    if [ "$actual" = "$expected" ]; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
        local diff_out
        diff_out=$(diff <(printf '%s' "$expected") <(printf '%s' "$actual") | head -10)
        ERRORS="$ERRORS\n  $name:\n$(echo "$diff_out" | sed 's/^/    /')"
    fi
}

echo "── Fluxa Test Runner ──────────────────────────────────────────────"
echo "   binary  : $FLUXA"
echo "   project : $PROJECT_ROOT"
echo "──────────────────────────────────────────────────────────────────"

run_test "sprint1/hello"       tests/hello.flx           "hello world"
run_test "sprint1/types"       tests/types.flx            "string ok
42
3.14
true
false
5"
run_test "sprint2"             tests/sprint2.flx          "30
3.14
fluxa
true
200
1
99
true
false
true"
run_test "sprint3"             tests/sprint3.flx          "maior
y é três
0
1
2
10
30
99
10
99
30
dois!"
run_test "sprint4"             tests/sprint4.flx          "10
fluxa
20
99
120
3628800
300"
run_test "sprint5"             tests/sprint5.flx          "2
3
0
2
10
5"
run_test "block_isolation"     tests/block_isolation.flx  "400
100
100"
run_test "block_root"          tests/block_root.flx       "fluxa
5
fluxa-lang
99
99"
run_test "block_methods"       tests/block_methods.flx    "10
300
60
0
120
720"
run_test "block_typeof_error"  tests/block_no_instance_typeof.flx "" 1
run_test "sprint6"             tests/sprint6.flx          "10
4
sobreviveu
division by zero
nil
division by zero
division by zero
42
fim"
run_test "danger_basic"        tests/danger_basic.flx     "execucao continua
division by zero"
run_test "danger_err_stack"    tests/danger_err_stack.flx "division by zero
division by zero
division by zero"
run_test "danger_clean"        tests/danger_clean.flx     "nil
sem erros"
run_test "danger_after"        tests/danger_after.flx     "10
20
passou pelo danger"
run_test "arr_heap"            tests/arr_heap.flx         "10
50
99
219
nil
array index out of bounds: nums[10] (size 5)
alice
carol
eve"
run_test "sprint6b"            tests/sprint6b.flx         "0
0
77
0
2.71
5
nil
division by zero
tudo ok"
run_test "sprint6c"            tests/sprint6c_runtime.flx "20
40
120
3628800
5050
50005000
1
1
0
0"
run_test "sprint7a"            tests/sprint7a.flx         "1
100
3
2
100
true
1.5
5"
run_test "sprint7a_collision"  tests/sprint7a_collision.flx "10
true
11"
run_test "arr_default"         tests/arr_default.flx      "0
0
42
0
0
0
99"
run_test "arr_default_types"   tests/arr_default_types.flx "0
3.14
0
false
true
false"
run_test "ffi_libm"            tests/ffi_libm.flx         "4
1.41421
7.5"
run_test "ffi_portability"     tests/ffi_portability.flx  "3
42.5
4
nil"
run_test "err_nil_check"       tests/err_nil_check.flx    "ok: nil antes de danger
ok: nil após danger limpo
ok: err tem valor
division by zero
ok: err zerado
ok: nil == nil
ok: nil != nil é false"
run_test "arr_param"           tests/arr_param.flx        "15
1
2
10
30
42"
run_test "arr_block_field"     tests/arr_block_field.flx  "10
0
30
90
100
999
10"
run_test "sprint7b"            tests/sprint7b.flx         "10
20
99
120
3628800
300
1
100
3
2
true
1.5
0
5"

rm -f "$TMP"

# ── prst reload: source-wins logic across pool-sharing reloads ───────────────
printf "  %-56s" "prst_reload (3 successive applies, pool shared)"
prst_out=$("$FLUXA" test-reload 2>&1)
if echo "$prst_out" | grep -q "ALL PASS"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  prst_reload:
$(echo "$prst_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── Sprint 8: Handover Atômico ────────────────────────────────────────────────
run_test "sprint8/handover_basic"    tests/sprint8_handover_basic.flx    "10
ok
20
ok"
run_test "sprint8/handover_dryrun"   tests/sprint8_handover_dry_run_fail.flx "antes
depois"
run_test "sprint8/handover_prst"     tests/sprint8_handover_prst.flx     "1
2
3
4
5
15"
run_test "sprint8/handover_version"  tests/sprint8_handover_version.flx  "1000
ok"
run_test "sprint8/prst_cap"          tests/sprint8_prst_cap.flx          "1
2
3
4
5
6
7
8
9
10
55"

# ── Sprint 8: test-handover protocol suite ───────────────────────────────────
printf "  %-56s" "handover (5-step protocol suite)"
ho_out=$("$FLUXA" test-handover 2>&1)
if echo "$ho_out" | grep -q "ALL PASS"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  handover:
$(echo "$ho_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── Sprint 9: CLI commands ────────────────────────────────────────────────────
printf "  %-56s" "sprint9/cli (apply -p, init, --force, no-runtime errors)"
cli9_out=$(bash "$SCRIPT_DIR/sprint9_cli.sh" --fluxa "$FLUXA" 2>&1)
if echo "$cli9_out" | grep -q "0 failed"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  sprint9/cli:
$(echo "$cli9_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── Sprint 9: IPC live tests (skipped gracefully if runtime exits too fast) ───
printf "  %-56s" "sprint9/ipc (observe, set, logs, status, permissions)"
ipc9_out=$(bash "$SCRIPT_DIR/sprint9_ipc.sh" --fluxa "$FLUXA" 2>&1)
if echo "$ipc9_out" | grep -qE "0 failed|live tests skipped"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  sprint9/ipc:
$(echo "$ipc9_out" | grep -E "FAIL|PASS|SKIP" | sed 's/^/    /')"
fi

# ── Sprint 9.b: safe point no back-edge do while (Issue #95) ─────────────────
printf "  %-56s" "sprint9b/set_in_loop (IPC set/observe inside infinite while)"
s9b_out=$(bash "$SCRIPT_DIR/sprint9b_set_in_loop.sh" --fluxa "$FLUXA" 2>&1)
if echo "$s9b_out" | grep -q "0 failed"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  sprint9b/set_in_loop:
$(echo "$s9b_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── Sprint 9.b: fluxa explain ao vivo (Issue #96) ────────────────────────────
printf "  %-56s" "sprint9b/explain_live (fluxa explain via IPC + file mode)"
s96_out=$(bash "$SCRIPT_DIR/sprint9b_explain_live.sh" --fluxa "$FLUXA" 2>&1)
if echo "$s96_out" | grep -q "0 failed"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS
  sprint9b/explain_live:
$(echo "$s96_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── type_check (static type enforcement) ──────────────────────────────────
printf "  %-56s" "type_check (static type enforcement at runtime)"
tc_out=$(bash "$SCRIPT_DIR/type_check.sh" --fluxa "$FLUXA" 2>&1)
if echo "$tc_out" | grep -q "type_check: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  type_check:
$(echo "$tc_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint9c/logical_ops ───────────────────────────────────────────────────
printf "  %-56s" "sprint9c/logical_ops (&&, ||, ! with short-circuit)"
lo_out=$(bash "$SCRIPT_DIR/sprint9c_logical_ops.sh" --fluxa "$FLUXA" 2>&1)
if echo "$lo_out" | grep -q "logical_ops: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint9c/logical_ops:
$(echo "$lo_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint9c/dyn ───────────────────────────────────────────────────────────
printf "  %-56s" "sprint9c/dyn (heterogeneous dynamic array)"
dyn_out=$(bash "$SCRIPT_DIR/sprint9c_dyn.sh" --fluxa "$FLUXA" 2>&1)
if echo "$dyn_out" | grep -q "dyn: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint9c/dyn:
$(echo "$dyn_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint9c/dyn_block_prst ─────────────────────────────────────────────────
printf "  %-56s" "sprint9c/dyn_block_prst (Block em dyn + prst dyn)"
dyn_bp_out=$(bash "$SCRIPT_DIR/sprint9c_dyn_block_prst.sh" --fluxa "$FLUXA" 2>&1)
if echo "$dyn_bp_out" | grep -q "dyn_block_prst: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint9c/dyn_block_prst:
$(echo "$dyn_bp_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint9c/ffi_toml (issue #103) ──────────────────────────────────────────
printf "  %-56s" "sprint9c/ffi_toml ([ffi] toml auto-resolve)"
ffi_toml_out=$(bash "$SCRIPT_DIR/sprint9c_ffi_toml.sh" --fluxa "$FLUXA" 2>&1)
if echo "$ffi_toml_out" | grep -q "ffi_toml: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint9c/ffi_toml:
$(echo "$ffi_toml_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint9c/dyn_indexed_member (bugfix: dyn[i].campo / dyn[i].metodo()) ────
printf "  %-56s" "sprint9c/dyn_indexed_member (dyn[i].campo / dyn[i].metodo)"
dyn_idx_out=$(bash "$SCRIPT_DIR/sprint9c_dyn_indexed_member.sh" --fluxa "$FLUXA" 2>&1)
if echo "$dyn_idx_out" | grep -q "dyn_indexed: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint9c/dyn_indexed_member:
$(echo "$dyn_idx_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint10/gc (GC: pin/unpin/sweep, dyn rules, free) ──────────────────────
printf "  %-56s" "sprint10/gc (GC rules, free, sweep, dyn types)"
gc_out=$(bash "$SCRIPT_DIR/sprint10_gc.sh" --fluxa "$FLUXA" 2>&1)
if echo "$gc_out" | grep -q "gc: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint10/gc:
$(echo "$gc_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint10/semantics ──────────────────────────────────────────────────────
printf "  %-56s" "sprint10/semantics (dyn rules, arr type, Block isolation)"
sem_out=$(bash "$SCRIPT_DIR/sprint10_semantics.sh" --fluxa "$FLUXA" 2>&1)
if echo "$sem_out" | grep -q "semantics: PASS"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL"
    FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint10/semantics:
$(echo "$sem_out" | grep -E "FAIL|PASS" | sed 's/^/    /')"
fi

# ── sprint10b: core fixes (for..in dyn + prst arr) ──────────────────────────
printf "  %-56s" "sprint10b/core (for..in dyn, prst arr handover)"
_out=$(bash "$SCRIPT_DIR/sprint10b_core_fixes.sh" --fluxa "$FLUXA" 2>&1)
if echo "$_out" | grep -q "core: PASS"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint10b/core:
$(echo "$_out" | grep "FAIL" | sed 's/^/    /')"
fi

# ── sprint11: warm path (WHT + QJL, cold/warm/hot tiers) ─────────────────────
printf "  %-56s" "sprint11/warm_path (warm tier, edge cases)"
_out=$(bash "$SCRIPT_DIR/sprint11_warm_path.sh" --fluxa "$FLUXA" 2>&1)
if echo "$_out" | grep -q "warm_path: PASS"; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
    ERRORS="${ERRORS}
  sprint11/warm_path:
$(echo "$_out" | grep "FAIL" | sed 's/^/    /')"
fi

# ── std libs (tests/libs/) ───────────────────────────────────────────────────
for _lib_script in "$SCRIPT_DIR/libs/"*.sh; do
    _lib_name=$(basename "$_lib_script" .sh)
    printf "  %-56s" "std.${_lib_name}"
    _lib_out=$(bash "$_lib_script" --fluxa "$FLUXA" 2>&1)
    if echo "$_lib_out" | grep -q "std\.${_lib_name}: PASS"; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        echo "FAIL"
        FAIL=$((FAIL+1))
        ERRORS="${ERRORS}
  std.${_lib_name}:
$(echo "$_lib_out" | grep "FAIL" | sed 's/^/    /')"
    fi
done

echo "──────────────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    echo ""
    echo "  Failures:"
    printf '%b\n' "$ERRORS"
    exit 1
fi
exit 0
