#!/usr/bin/env bash
# tests/sprint10b_core_fixes.sh
# Tests for three core fixes:
#   1. for x in dyn — iteration over heterogeneous dynamic arrays
#   2. prst arr same run — persistent arrays within a single run
#   3. prst arr handover — persistent arrays survive Atomic Handover
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  core/%s\n" "$1"; }
fail() { printf "  FAIL  core/%s\n    expected: %s\n    got:      %s\n%s\n" \
    "$1" "$2" "$3" "────────────────────────────────────────────────────────────────────";
    FAILS=$((FAILS+1)); }

mkproj() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
TOML
}

echo "── core fixes: for..in dyn + prst arr ───────────────────────────────"

# ── CASE 1: for x in dyn — basic count ────────────────────────────────────
cat > "$WORK_DIR/for_dyn1.flx" << 'FLX'
dyn d = [1, "hello", true]
int count = 0
for item in d {
    count = count + 1
}
print(count)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/for_dyn1.flx" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "for_in_dyn_count"
else
    fail "for_in_dyn_count" "3" "$out"
fi

# ── CASE 2: for x in dyn — element access ─────────────────────────────────
cat > "$WORK_DIR/for_dyn2.flx" << 'FLX'
dyn names = ["alice", "bob", "carol"]
for name in names {
    print(name)
}
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/for_dyn2.flx" 2>&1 || true)
if echo "$out" | grep -q "^alice$" && echo "$out" | grep -q "^bob$" \
    && echo "$out" | grep -q "^carol$"; then
    pass "for_in_dyn_elements"
else
    fail "for_in_dyn_elements" "alice, bob, carol" "$out"
fi

# ── CASE 3: for x in dyn — empty dyn ─────────────────────────────────────
cat > "$WORK_DIR/for_dyn3.flx" << 'FLX'
dyn empty = []
int count = 0
for item in empty {
    count = count + 1
}
print(count)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/for_dyn3.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$"; then
    pass "for_in_dyn_empty"
else
    fail "for_in_dyn_empty" "0" "$out"
fi

# ── CASE 4: for x in dyn — accumulate sum ────────────────────────────────
cat > "$WORK_DIR/for_dyn4.flx" << 'FLX'
dyn nums = [10, 20, 30, 40]
int total = 0
for n in nums {
    total = total + n
}
print(total)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/for_dyn4.flx" 2>&1 || true)
if echo "$out" | grep -q "^100$"; then
    pass "for_in_dyn_sum"
else
    fail "for_in_dyn_sum" "100" "$out"
fi

# ── CASE 5: for x in arr still works ──────────────────────────────────────
cat > "$WORK_DIR/for_arr.flx" << 'FLX'
int arr vals[4] = [5, 10, 15, 20]
int sum = 0
for v in vals {
    sum = sum + v
}
print(sum)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/for_arr.flx" 2>&1 || true)
if echo "$out" | grep -q "^50$"; then
    pass "for_in_arr_unchanged"
else
    fail "for_in_arr_unchanged" "50" "$out"
fi

# ── CASE 6: prst arr — declared, mutated, read in same run ────────────────
P="$WORK_DIR/p6"; mkproj "$P" "prst_arr_same"
cat > "$P/main.flx" << 'FLX'
prst int arr readings[3] = [0, 0, 0]
readings[0] = 42
readings[1] = 17
print(readings[0])
print(readings[1])
print(readings[2])
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "^42$" \
    && echo "$out" | sed -n '2p' | grep -q "^17$" \
    && echo "$out" | sed -n '3p' | grep -q "^0$"; then
    pass "prst_arr_same_run"
else
    fail "prst_arr_same_run" "42, 17, 0" "$out"
fi

# ── CASE 7: prst arr — mutations survive handover ─────────────────────────
P="$WORK_DIR/p7"; mkproj "$P" "prst_arr_handover"
cat > "$P/v1.flx" << 'FLX'
prst int arr readings[5] = [0, 0, 0, 0, 0]
readings[0] = 10
readings[1] = 20
readings[2] = 30
FLX
cat > "$P/v2.flx" << 'FLX'
prst int arr readings[5] = [0, 0, 0, 0, 0]
print(readings[0])
print(readings[1])
print(readings[2])
print(readings[3])
FLX
out=$(timeout 10s "$FLUXA" handover "$P/v1.flx" "$P/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" \
    && echo "$out" | grep -q "^10$" \
    && echo "$out" | grep -q "^20$" \
    && echo "$out" | grep -q "^30$" \
    && echo "$out" | grep -q "^0$"; then
    pass "prst_arr_handover_values_preserved"
else
    fail "prst_arr_handover_values_preserved" "COMMITTED, 10, 20, 30, 0" "$out"
fi

# ── CASE 8: prst arr — mixed with prst int across handover ────────────────
P="$WORK_DIR/p8"; mkproj "$P" "prst_arr_mixed"
cat > "$P/v1.flx" << 'FLX'
prst int tick = 0
prst float arr samples[3] = [0.0, 0.0, 0.0]
tick = tick + 1
samples[0] = 1.5
samples[1] = 2.5
FLX
cat > "$P/v2.flx" << 'FLX'
prst int tick = 0
prst float arr samples[3] = [0.0, 0.0, 0.0]
tick = tick + 1
print(tick)
print(samples[0])
print(samples[1])
print(samples[2])
FLX
out=$(timeout 10s "$FLUXA" handover "$P/v1.flx" "$P/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" \
    && echo "$out" | grep -q "^2$" \
    && echo "$out" | grep -q "1.5" \
    && echo "$out" | grep -q "2.5" \
    && echo "$out" | grep -q "^0$"; then
    pass "prst_arr_mixed_with_prst_int"
else
    fail "prst_arr_mixed_with_prst_int" "COMMITTED, tick=2, 1.5, 2.5, 0" "$out"
fi

# ── CASE 9: for x in dyn from csv.chunk ───────────────────────────────────
P="$WORK_DIR/p9"; mkproj "$P" "for_dyn_csv"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "for_dyn_csv"
entry = "main.flx"
[libs]
std.csv = "1.0"
TOML
CSV="$WORK_DIR/data.csv"
printf "s1,10\ns2,20\ns3,30\n" > "$CSV"
cat > "$P/main.flx" << FLX
import std csv
prst int total = 0
danger {
    dyn rows = csv.load("$CSV")
    for row in rows {
        total = total + 1
    }
}
print(total)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "for_in_dyn_from_csv"
else
    fail "for_in_dyn_from_csv" "3" "$out"
fi

# ── CASE 10: prst arr — source edit (new init) detected on handover ───────
# prst arr source edit: if declared size changes, new value is used
P="$WORK_DIR/p10"; mkproj "$P" "prst_arr_resize"
cat > "$P/v1.flx" << 'FLX'
prst int arr readings[3] = [0, 0, 0]
readings[0] = 99
FLX
cat > "$P/v2.flx" << 'FLX'
prst int arr readings[3] = [0, 0, 0]
print(readings[0])
FLX
out=$(timeout 10s "$FLUXA" handover "$P/v1.flx" "$P/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^99$"; then
    pass "prst_arr_mutation_preserved_on_handover"
else
    fail "prst_arr_mutation_preserved_on_handover" "COMMITTED + 99" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → core: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
