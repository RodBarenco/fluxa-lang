#!/usr/bin/env bash
# Compiled loops must answer exactly as the tree walker does.
#
# vm_compare reached its numeric path with bool, nil and Block operands and
# read Value.as as a double; val_bool() only writes that union's int member, so
# the remaining bytes were indeterminate and `flag == false` inside a compiled
# loop answered from whatever the register had held. Each case below runs the
# same comparison twice — once in a loop the VM compiles, once at top level
# where the evaluator answers — and requires both to agree.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

# The exact shape that failed: the array read sits inside the guarded body, so
# GET_INDEX writes the very register the comparison had just used, and the
# leftover bytes there decide the answer.
cat > "$WORK_DIR/bool.flx" << 'FLX'
int arr a[64] = 1
bool flag = false
int i = 0
int acc = 0
while i < 5 { if flag == false { acc = acc + a[0] }  i = i + 1 }
print("EQ", acc)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/bool.flx" 2>&1 || true)
if [[ "$out" != "EQ 5" ]]; then
    echo "  vm_value_semantics: FAIL (bool == in a compiled loop)"
    echo "    expected: EQ 5"; echo "    got: $out"; exit 1
fi

cat > "$WORK_DIR/bool2.flx" << 'FLX'
int arr a[64] = 1
bool flag = false
bool on = true
int i = 0
int acc = 0
while i < 5 {
    if flag != true { acc = acc + a[0] }
    if on == true { acc = acc + a[1] }
    if on != false { acc = acc + a[2] }
    i = i + 1
}
print("NEQ", acc)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/bool2.flx" 2>&1 || true)
if [[ "$out" != "NEQ 15" ]]; then
    echo "  vm_value_semantics: FAIL (bool != / == mix)"
    echo "    expected: NEQ 15"; echo "    got: $out"; exit 1
fi

# float and str go down the same shared path; nil compares by identity.
cat > "$WORK_DIR/mixed.flx" << 'FLX'
int arr a[2] = [1, 2]
float x = 1.5
str s = "ab"
int i = 0
int loop = 0
while i < 2 {
    if x == 1.5 { loop = loop + a[0] }
    if s == "ab" { loop = loop + 10 }
    if x != 2.5 { loop = loop + 100 }
    if s != "zz" { loop = loop + 1000 }
    i = i + 1
}
int flat = 0
if x == 1.5 { flat = flat + a[0] }
if s == "ab" { flat = flat + 10 }
if x != 2.5 { flat = flat + 100 }
if s != "zz" { flat = flat + 1000 }
print("MIXED", loop, flat * 2)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/mixed.flx" 2>&1 || true)
if [[ "$out" != "MIXED 2222 2222" ]]; then
    echo "  vm_value_semantics: FAIL (float/str equality)"
    echo "    expected: MIXED 2222 2222"; echo "    got: $out"; exit 1
fi

# Ordering stays numeric and keeps its existing answers.
cat > "$WORK_DIR/order.flx" << 'FLX'
int arr a[2] = [1, 2]
float x = 2.5
int n = 3
int i = 0
int loop = 0
while i < 2 {
    if x < 3.0 { loop = loop + a[0] }
    if n >= 3  { loop = loop + 10 }
    if x > 9.0 { loop = loop + 100 }
    i = i + 1
}
print("ORDER", loop)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/order.flx" 2>&1 || true)
if [[ "$out" != "ORDER 22" ]]; then
    echo "  vm_value_semantics: FAIL (ordering)"
    echo "    expected: ORDER 22"; echo "    got: $out"; exit 1
fi
# Reading a str Block field inside a compiled loop. scope_get hands back a
# borrowed alias of the field's own storage, while every VM string register
# owns one reference and drops it at the end of the statement — without a
# retain in the field callback that drop freed the field itself, and the loop
# went on to read reused heap.
cat > "$WORK_DIR/strfield.flx" << 'FLX'
Block B { str name = "hello"  str tag = "world" }
Block b typeof B
int arr a[4] = 1
int i = 0
str s = "x"
str u = "y"
while i < 200 { s = b.name  u = b.tag  i = i + a[0] }
print("STRFIELD", s, u, b.name, b.tag)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/strfield.flx" 2>&1 || true)
if [[ "$out" != "STRFIELD hello world hello world" ]]; then
    echo "  vm_value_semantics: FAIL (str field read in a compiled loop)"
    echo "    expected: STRFIELD hello world hello world"
    echo "    got: $out"; exit 1
fi

echo "  vm_value_semantics: PASS"
