#!/usr/bin/env bash
# tests/sprint9c_logical_ops.sh — logical operators: && || !
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  logical/%s\n" "$1"; }
fail() { printf "  FAIL  logical/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
run1() { echo "$1" > "$WORK_DIR/t.flx"; timeout 3s "$FLUXA" run "$WORK_DIR/t.flx" 2>&1 || true; }

echo "── sprint9c: logical operators (&&, ||, !) ──────────────────────────"

# basic bool combinations
out=$(run1 'bool a = true
bool b = false
print(a && b)
print(a || b)
print(!a)
print(!b)')
[[ "$out" == *"false"*"true"*"false"*"true"* ]] \
    && pass "basic_bool" || fail "basic_bool" "false true false true" "$out"

# short-circuit &&: right side must NOT execute (division by zero would crash)
cat > "$WORK_DIR/sc_and.flx" << 'FLX'
bool b = false
int x = 0
bool r = b && (x == 1/0)
print(r)
print(x)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/sc_and.flx" 2>&1 || true)
[[ "$out" == *"false"*"0"* ]] \
    && pass "short_circuit_and" || fail "short_circuit_and" "false 0" "$out"

# short-circuit ||: right side must NOT execute
cat > "$WORK_DIR/sc_or.flx" << 'FLX'
bool a = true
int x = 0
bool r = a || (x == 1/0)
print(r)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/sc_or.flx" 2>&1 || true)
[[ "$out" == *"true"* ]] \
    && pass "short_circuit_or" || fail "short_circuit_or" "true" "$out"

# chaining
out=$(run1 'print(true && true && false)
print(false || false || true)
print(!(true && false))')
[[ "$out" == *"false"*"true"*"true"* ]] \
    && pass "chaining" || fail "chaining" "false true true" "$out"

# int truthy
out=$(run1 'int x = 5
int y = 0
print(x && true)
print(y || true)')
[[ "$out" == *"true"*"true"* ]] \
    && pass "int_truthy" || fail "int_truthy" "true true" "$out"

# ! on non-bool values
out=$(run1 'print(!0)
print(!1)
print(!nil)')
[[ "$out" == *"true"*"false"*"true"* ]] \
    && pass "not_non_bool" || fail "not_non_bool" "true false true" "$out"

# if condition with logical
cat > "$WORK_DIR/if_logic.flx" << 'FLX'
int a = 10
int b = 20
if a > 5 && b > 15 {
    print("ambos")
}
if a > 100 || b > 15 {
    print("um")
}
if !(a == b) {
    print("diferentes")
}
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/if_logic.flx" 2>&1 || true)
[[ "$out" == *"ambos"*"um"*"diferentes"* ]] \
    && pass "if_condition" || fail "if_condition" "ambos um diferentes" "$out"

# while condition with logical
cat > "$WORK_DIR/while_logic.flx" << 'FLX'
int i = 0
int limit = 3
while i < limit && i >= 0 {
    print(i)
    i = i + 1
}
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/while_logic.flx" 2>&1 || true)
[[ "$out" == *"0"*"1"*"2"* ]] \
    && pass "while_condition" || fail "while_condition" "0 1 2" "$out"

echo "────────────────────────────────────────────────────────────────────"
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: 8 passed, 0 failed"
    echo "  → logical_ops: PASS"
    exit 0
else
    echo "  Results: $((8-FAILS)) passed, $FAILS failed"
    exit 1
fi
