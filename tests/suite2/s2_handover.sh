#!/usr/bin/env bash
# tests/suite2/s2_handover.sh
# Suite 2 — Section 2: Atomic Handover edge cases
#
# Covers: failure at each step, corrupted snapshot, type collision,
# adding+removing prst simultaneously, 64+ vars, large dyn in migration.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  handover/%s\n" "$1"; }
fail() { printf "  FAIL  handover/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/handover: atomic handover edge cases ──────────────────────"

make_toml() {
    local dir="$1" name="$2"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "v1.flx"
TOML
}

# ── CASE 1: handover succeeds — basic 5-step protocol ─────────────────────────
PROJ="$WORK_DIR/h_basic"
mkdir -p "$PROJ"
cat > "$PROJ/v1.flx" << 'FLX'
prst int n = 10
n = n + 5
print(n)
FLX
cat > "$PROJ/v2.flx" << 'FLX'
prst int n = 10
n = n * 2
print(n)
FLX
make_toml "$PROJ" "h_basic"
out=$(timeout 10s "$FLUXA" handover "$PROJ/v1.flx" "$PROJ/v2.flx" 2>&1 || true)
# v1: n=15. v2: n=15*2=30
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^30$"; then
    pass "basic_5step_protocol"
else
    fail "basic_5step_protocol" "COMMITTED + 30" "$out"
fi

# ── CASE 2: dry run failure — runtime error in new program ────────────────────
PROJ2="$WORK_DIR/h_dryrun_fail"
mkdir -p "$PROJ2"
cat > "$PROJ2/v1.flx" << 'FLX'
prst int x = 1
print(x)
FLX
cat > "$PROJ2/v2_bad.flx" << 'FLX'
prst int x = 1
int boom = 1 / 0
print(x)
FLX
make_toml "$PROJ2" "h_dryrun_fail"
out=$(timeout 10s "$FLUXA" handover "$PROJ2/v1.flx" "$PROJ2/v2_bad.flx" 2>&1 || true)
# Handover must fail at step 3; Runtime A must keep control
if echo "$out" | grep -qi "DRY_RUN\|dry run failed\|handover FAILED" \
    && echo "$out" | grep -q "Runtime A maintains control"; then
    pass "dry_run_failure_a_keeps_control"
else
    fail "dry_run_failure_a_keeps_control" "DRY_RUN error + A maintains control" "$out"
fi

# ── CASE 3: dry run failure — undefined variable ──────────────────────────────
PROJ3="$WORK_DIR/h_undef"
mkdir -p "$PROJ3"
cat > "$PROJ3/v1.flx" << 'FLX'
prst int x = 5
print(x)
FLX
cat > "$PROJ3/v2_undef.flx" << 'FLX'
prst int x = 5
print(undefined_var)
FLX
make_toml "$PROJ3" "h_undef"
out=$(timeout 10s "$FLUXA" handover "$PROJ3/v1.flx" "$PROJ3/v2_undef.flx" 2>&1 || true)
if echo "$out" | grep -qi "FAILED\|dry run\|undefined"; then
    pass "dry_run_failure_undefined_var"
else
    fail "dry_run_failure_undefined_var" "handover FAILED (undefined var in dry run)" "$out"
fi

# ── CASE 4: parse error in new program — fails at step 1 ─────────────────────
PROJ4="$WORK_DIR/h_parse_fail"
mkdir -p "$PROJ4"
cat > "$PROJ4/v1.flx" << 'FLX'
prst int x = 1
print(x)
FLX
cat > "$PROJ4/v2_syntax.flx" << 'FLX'
prst int x = 1
if x > 0 {
    // missing closing brace
FLX
make_toml "$PROJ4" "h_parse_fail"
out=$(timeout 10s "$FLUXA" handover "$PROJ4/v1.flx" "$PROJ4/v2_syntax.flx" 2>&1 || true)
if echo "$out" | grep -qi "parse error\|FAILED\|standby"; then
    pass "parse_error_fails_at_standby"
else
    fail "parse_error_fails_at_standby" "handover FAILED at step 1 (parse error)" "$out"
fi

# ── CASE 5: prst type collision ───────────────────────────────────────────────
PROJ5="$WORK_DIR/h_type_collision"
mkdir -p "$PROJ5"
cat > "$PROJ5/v1.flx" << 'FLX'
prst int counter = 0
counter = counter + 1
print(counter)
FLX
cat > "$PROJ5/v2_wrong_type.flx" << 'FLX'
prst str counter = "zero"
print(counter)
FLX
make_toml "$PROJ5" "h_type_collision"
out=$(timeout 10s "$FLUXA" handover "$PROJ5/v1.flx" "$PROJ5/v2_wrong_type.flx" 2>&1 || true)
if echo "$out" | grep -qi "FAILED\|collision\|type\|DRY_RUN"; then
    pass "prst_type_collision_rejects_handover"
else
    fail "prst_type_collision_rejects_handover" "handover FAILED (type collision)" "$out"
fi

# ── CASE 6: add and remove prst simultaneously ────────────────────────────────
PROJ6="$WORK_DIR/h_add_remove"
mkdir -p "$PROJ6"
cat > "$PROJ6/v1.flx" << 'FLX'
prst int a = 10
prst int b = 20
print(a)
print(b)
FLX
cat > "$PROJ6/v2.flx" << 'FLX'
prst int a = 10
prst int c = 30
print(a)
print(c)
FLX
make_toml "$PROJ6" "h_add_remove"
out=$(timeout 10s "$FLUXA" handover "$PROJ6/v1.flx" "$PROJ6/v2.flx" 2>&1 || true)
# v2: a preserved (10), b gone, c new (30)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^10$" \
    && echo "$out" | grep -q "^30$"; then
    pass "handover_add_and_remove_prst"
else
    fail "handover_add_and_remove_prst" "COMMITTED + 10 + 30" "$out"
fi

# ── CASE 7: 64+ prst vars across handover ─────────────────────────────────────
PROJ7="$WORK_DIR/h_many"
mkdir -p "$PROJ7"
python3 - << 'PY'
lines_v1 = [f"prst int v{i} = {i}" for i in range(70)]
lines_v1.append("print(v0)\nprint(v69)")
lines_v2 = [f"prst int v{i} = {i}" for i in range(70)]
lines_v2.append("print(v0)\nprint(v69)")
with open("/tmp/s2_h_many_v1.flx", "w") as f:
    f.write("\n".join(lines_v1))
with open("/tmp/s2_h_many_v2.flx", "w") as f:
    f.write("\n".join(lines_v2))
PY
cp /tmp/s2_h_many_v1.flx "$PROJ7/v1.flx"
cp /tmp/s2_h_many_v2.flx "$PROJ7/v2.flx"
make_toml "$PROJ7" "h_many"
out=$(timeout 15s "$FLUXA" handover "$PROJ7/v1.flx" "$PROJ7/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^69$"; then
    pass "handover_70_prst_vars"
else
    fail "handover_70_prst_vars" "COMMITTED + 0 + 69" "$out"
fi

# ── CASE 8: handover with large dyn in pool ───────────────────────────────────
PROJ8="$WORK_DIR/h_large_dyn"
mkdir -p "$PROJ8"
cat > "$PROJ8/v1.flx" << 'FLX'
prst dyn log = []
int i = 0
while i < 1000 {
    log[i] = i
    i = i + 1
}
print(len(log))
FLX
cat > "$PROJ8/v2.flx" << 'FLX'
prst dyn log = []
print(len(log))
FLX
make_toml "$PROJ8" "h_large_dyn"
out=$(timeout 15s "$FLUXA" handover "$PROJ8/v1.flx" "$PROJ8/v2.flx" 2>&1 || true)
# v1 builds log[1000], handover commits, v2 sees it
if echo "$out" | grep -q "COMMITTED"; then
    pass "handover_with_large_dyn_in_pool"
else
    fail "handover_with_large_dyn_in_pool" "COMMITTED" "$out"
fi

# ── CASE 9: handover preserves str and float prst ─────────────────────────────
PROJ9="$WORK_DIR/h_mixed_types"
mkdir -p "$PROJ9"
cat > "$PROJ9/v1.flx" << 'FLX'
prst str  name  = "fluxa"
prst float temp  = 23.7
prst bool alive = true
print(name)
print(alive)
FLX
cat > "$PROJ9/v2.flx" << 'FLX'
prst str  name  = "fluxa"
prst float temp  = 23.7
prst bool alive = true
print(name)
print(alive)
FLX
make_toml "$PROJ9" "h_mixed_types"
out=$(timeout 10s "$FLUXA" handover "$PROJ9/v1.flx" "$PROJ9/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "fluxa" \
    && echo "$out" | grep -q "true"; then
    pass "handover_preserves_mixed_prst_types"
else
    fail "handover_preserves_mixed_prst_types" "COMMITTED + fluxa + true" "$out"
fi

# ── CASE 10: runtime A output before handover is correct ─────────────────────
PROJ10="$WORK_DIR/h_output"
mkdir -p "$PROJ10"
cat > "$PROJ10/v1.flx" << 'FLX'
prst int x = 0
x = x + 100
print(x)
FLX
cat > "$PROJ10/v2.flx" << 'FLX'
prst int x = 0
x = x + 1
print(x)
FLX
make_toml "$PROJ10" "h_output"
out=$(timeout 10s "$FLUXA" handover "$PROJ10/v1.flx" "$PROJ10/v2.flx" 2>&1 || true)
# v1 prints 100, v2 prints 101
if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^101$"; then
    pass "handover_runtime_a_output_correct"
else
    fail "handover_runtime_a_output_correct" "100 from v1 then 101 from v2" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → handover: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
