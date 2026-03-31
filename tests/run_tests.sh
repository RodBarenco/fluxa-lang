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

echo "──────────────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    echo ""
    echo "  Failures:"
    printf '%b\n' "$ERRORS"
    exit 1
fi
exit 0
