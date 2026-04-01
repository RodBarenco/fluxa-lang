#!/usr/bin/env bash
# tests/type_check.sh — Sprint 9.b: static type enforcement at runtime
set -euo pipefail

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa) FLUXA="$2"; shift 2 ;;
        *) shift ;;
    esac
done
FLUXA="${FLUXA:-./fluxa}"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

pass() { printf "  PASS  type_check/%s\n" "$1"; }
fail() { printf "  FAIL  type_check/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
FAILS=0

run1() { echo "$1" > "$WORK_DIR/t.flx"; "$FLUXA" run "$WORK_DIR/t.flx" 2>&1 || true; }

echo "── type_check: static type enforcement ─────────────────────────────"

# CASO 1: int ← str
out=$(run1 'int x = "sim"')
if echo "$out" | grep -q "type error"; then pass "decl_int_str"
else fail "decl_int_str" "type error" "$out"; fi

# CASO 2: float ← bool
out=$(run1 'float y = true')
if echo "$out" | grep -q "type error"; then pass "decl_float_bool"
else fail "decl_float_bool" "type error" "$out"; fi

# CASO 3: bool ← int
out=$(run1 'bool z = 42')
if echo "$out" | grep -q "type error"; then pass "decl_bool_int"
else fail "decl_bool_int" "type error" "$out"; fi

# CASO 4: str ← float
out=$(run1 'str s = 3.14')
if echo "$out" | grep -q "type error"; then pass "decl_str_float"
else fail "decl_str_float" "type error" "$out"; fi

# CASO 5: assign int ← str
cat > "$WORK_DIR/assign.flx" << 'FLX'
int n = 10
n = "agora string"
FLX
out=$("$FLUXA" run "$WORK_DIR/assign.flx" 2>&1 || true)
if echo "$out" | grep -q "type error"; then pass "assign_int_str"
else fail "assign_int_str" "type error" "$out"; fi

# CASO 6: tipos corretos — nenhum erro
cat > "$WORK_DIR/valid.flx" << 'FLX'
int a = 10
float b = 3.14
bool c = true
str s = "ok"
a = 20
b = 2.71
c = false
s = "fluxa"
print(a)
FLX
out=$("$FLUXA" run "$WORK_DIR/valid.flx" 2>&1)
if echo "$out" | grep -q "type error"; then fail "valid_types" "no error" "$out"
elif echo "$out" | grep -q "20"; then pass "valid_types"
else fail "valid_types" "20 in output" "$out"; fi

# CASO 7: danger isola — int bad = 1/0 não gera type error espúrio
cat > "$WORK_DIR/danger.flx" << 'FLX'
danger {
    int bad = 1 / 0
}
print("sobreviveu")
print(err[0])
FLX
out=$("$FLUXA" run "$WORK_DIR/danger.flx" 2>&1)
if echo "$out" | grep -q "sobreviveu" && echo "$out" | grep -q "division by zero"; then
    pass "danger_isolation"
else
    fail "danger_isolation" "sobreviveu + division by zero" "$out"
fi

# CASO 8: mensagem menciona variável e tipos
out=$(run1 'int x = "sim"')
if echo "$out" | grep -q "'x'" && echo "$out" | grep -q "int" && echo "$out" | grep -q "str"; then
    pass "error_message_quality"
else
    fail "error_message_quality" "msg with 'x' int str" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: 8 passed, 0 failed"
    echo "  → type_check: PASS"
    exit 0
else
    echo "  Results: $((8-FAILS)) passed, $FAILS failed"
    exit 1
fi
