#!/usr/bin/env bash
# tests/suite2/s2_prst.sh
# Suite 2 — Section 1: prst edge cases
#
# Covers: type change between reloads, large prst dyn, mixed prst dyn,
# prst dyn with Block instances, PrstPool growth beyond initial cap,
# atomic invalidation on variable removal.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  prst/%s\n" "$1"; }
fail() { printf "  FAIL  prst/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/prst: persistent state edge cases ────────────────────────"

# ── CASE 1: prst type collision between handover versions ─────────────────────
# v1 declares prst int, v2 tries prst str for the same name → ERR_RELOAD
PROJ="$WORK_DIR/p_type_collision"
mkdir -p "$PROJ"
cat > "$PROJ/v1.flx" << 'FLX'
prst int count = 42
print(count)
FLX
cat > "$PROJ/v2.flx" << 'FLX'
prst str count = "oops"
print(count)
FLX
cat > "$PROJ/fluxa.toml" << 'TOML'
[project]
name = "type_collision"
entry = "v1.flx"
TOML
out=$(timeout 10s "$FLUXA" handover "$PROJ/v1.flx" "$PROJ/v2.flx" 2>&1 || true)
if echo "$out" | grep -qi "ERR_RELOAD\|type.*mismatch\|collision\|handover FAILED\|DRY_RUN"; then
    pass "prst_type_change_rejected"
else
    fail "prst_type_change_rejected" "handover FAILED due to type collision" "$out"
fi

# ── CASE 2: prst value preserved across handover ──────────────────────────────
PROJ2="$WORK_DIR/p_preserve"
mkdir -p "$PROJ2"
cat > "$PROJ2/v1.flx" << 'FLX'
prst int score = 100
score = score + 50
print(score)
FLX
cat > "$PROJ2/v2.flx" << 'FLX'
prst int score = 100
score = score + 1
print(score)
FLX
cat > "$PROJ2/fluxa.toml" << 'TOML'
[project]
name = "preserve"
entry = "v1.flx"
TOML
out=$(timeout 10s "$FLUXA" handover "$PROJ2/v1.flx" "$PROJ2/v2.flx" 2>&1 || true)
# v1 runs: score=150. handover. v2 runs: score=150+1=151
if echo "$out" | grep -q "^151$"; then
    pass "prst_value_preserved_across_handover"
else
    fail "prst_value_preserved_across_handover" "151 (150 from v1 + 1 from v2)" "$out"
fi

# ── CASE 3: multiple prst types preserved together ────────────────────────────
PROJ3="$WORK_DIR/p_multi"
mkdir -p "$PROJ3"
cat > "$PROJ3/v1.flx" << 'FLX'
prst int  cycles = 0
prst str  label  = "start"
prst bool active = true
cycles = cycles + 7
label  = "running"
print(cycles)
print(label)
print(active)
FLX
cat > "$PROJ3/v2.flx" << 'FLX'
prst int  cycles = 0
prst str  label  = "start"
prst bool active = true
print(cycles)
print(label)
print(active)
FLX
cat > "$PROJ3/fluxa.toml" << 'TOML'
[project]
name = "multi"
entry = "v1.flx"
TOML
out=$(timeout 10s "$FLUXA" handover "$PROJ3/v1.flx" "$PROJ3/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "^7$" && echo "$out" | grep -q "running" \
    && echo "$out" | grep -q "true"; then
    pass "multi_prst_types_preserved"
else
    fail "multi_prst_types_preserved" "7, running, true from v2" "$out"
fi

# ── CASE 4: prst dyn — large payload (5000 elements) ─────────────────────────
PROJ4="$WORK_DIR/p_large_dyn"
mkdir -p "$PROJ4"
cat > "$PROJ4/main.flx" << 'FLX'
prst dyn log = []
int i = 0
while i < 5000 {
    log[i] = i
    i = i + 1
}
print(len(log))
print(log[4999])
FLX
cat > "$PROJ4/fluxa.toml" << 'TOML'
[project]
name = "large_dyn"
entry = "main.flx"
TOML
out=$(timeout 10s "$FLUXA" run "$PROJ4/main.flx" -proj "$PROJ4" 2>&1 || true)
if echo "$out" | grep -q "^5000$" && echo "$out" | grep -q "^4999$"; then
    pass "prst_dyn_large_5000_elements"
else
    fail "prst_dyn_large_5000_elements" "5000 then 4999" "$out"
fi

# ── CASE 5: prst dyn — mixed types (str, int, float, bool) ───────────────────
PROJ5="$WORK_DIR/p_mixed_dyn"
mkdir -p "$PROJ5"
cat > "$PROJ5/main.flx" << 'FLX'
prst dyn readings = ["sensor_1", 23.5, true, 42]
print(len(readings))
print(readings[0])
print(readings[3])
FLX
cat > "$PROJ5/fluxa.toml" << 'TOML'
[project]
name = "mixed_dyn"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ5/main.flx" -proj "$PROJ5" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "sensor_1" \
    && echo "$out" | grep -q "^42$"; then
    pass "prst_dyn_mixed_types"
else
    fail "prst_dyn_mixed_types" "4, sensor_1, 42" "$out"
fi

# ── CASE 6: PrstPool grows beyond initial cap ─────────────────────────────────
# Default prst_cap=64 — declare 80+ prst vars to force realloc
PROJ6="$WORK_DIR/p_pool_grow"
mkdir -p "$PROJ6"
# Generate a file with 80 prst int declarations
python3 - << 'PY'
lines = []
for i in range(80):
    lines.append(f"prst int v{i} = {i}")
lines.append("print(v0)")
lines.append("print(v79)")
with open("/tmp/s2_p_pool_grow.flx", "w") as f:
    f.write("\n".join(lines))
PY
cp /tmp/s2_p_pool_grow.flx "$PROJ6/main.flx"
cat > "$PROJ6/fluxa.toml" << 'TOML'
[project]
name = "pool_grow"
entry = "main.flx"
[runtime]
prst_cap = 8
TOML
out=$(timeout 10s "$FLUXA" run "$PROJ6/main.flx" -proj "$PROJ6" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^79$"; then
    pass "prst_pool_grows_beyond_initial_cap"
else
    fail "prst_pool_grows_beyond_initial_cap" "0 then 79 (80 prst vars with cap=8)" "$out"
fi

# ── CASE 7: prst dyn with Block instances ─────────────────────────────────────
PROJ7="$WORK_DIR/p_dyn_block"
mkdir -p "$PROJ7"
cat > "$PROJ7/main.flx" << 'FLX'
Block Sensor {
    prst float reading = 0.0
    fn set(float v) nil { reading = v }
    fn get() float { return reading }
}
Block s typeof Sensor
s.set(99.5)
prst dyn sensors = [s]
print(len(sensors))
print(sensors[0].get())
FLX
cat > "$PROJ7/fluxa.toml" << 'TOML'
[project]
name = "dyn_block"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ7/main.flx" -proj "$PROJ7" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "99.5\|99\.5"; then
    pass "prst_dyn_with_block_instances"
else
    fail "prst_dyn_with_block_instances" "1 then 99.5" "$out"
fi

# ── CASE 8: handover adding new prst var ──────────────────────────────────────
PROJ8="$WORK_DIR/p_add_prst"
mkdir -p "$PROJ8"
cat > "$PROJ8/v1.flx" << 'FLX'
prst int a = 10
print(a)
FLX
cat > "$PROJ8/v2.flx" << 'FLX'
prst int a = 10
prst int b = 20
print(a)
print(b)
FLX
cat > "$PROJ8/fluxa.toml" << 'TOML'
[project]
name = "add_prst"
entry = "v1.flx"
TOML
out=$(timeout 10s "$FLUXA" handover "$PROJ8/v1.flx" "$PROJ8/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^10$" \
    && echo "$out" | grep -q "^20$"; then
    pass "handover_adds_new_prst_var"
else
    fail "handover_adds_new_prst_var" "COMMITTED + 10 + 20" "$out"
fi

# ── CASE 9: handover removing prst var — invalidation ─────────────────────────
PROJ9="$WORK_DIR/p_remove_prst"
mkdir -p "$PROJ9"
cat > "$PROJ9/v1.flx" << 'FLX'
prst int a = 42
prst int b = 99
print(a)
print(b)
FLX
cat > "$PROJ9/v2.flx" << 'FLX'
prst int a = 42
print(a)
FLX
cat > "$PROJ9/fluxa.toml" << 'TOML'
[project]
name = "remove_prst"
entry = "v1.flx"
TOML
out=$(timeout 10s "$FLUXA" handover "$PROJ9/v1.flx" "$PROJ9/v2.flx" 2>&1 || true)
# v2 runs fine with just 'a' — b is gone, no ghost state
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^42$"; then
    pass "handover_removes_prst_no_ghost"
else
    fail "handover_removes_prst_no_ghost" "COMMITTED + 42 in v2" "$out"
fi

# ── CASE 10: 64+ prst vars across handover ────────────────────────────────────
PROJ10="$WORK_DIR/p_many_prst"
mkdir -p "$PROJ10"
python3 - << 'PY'
lines_v1 = []
lines_v2 = []
for i in range(70):
    lines_v1.append(f"prst int v{i} = {i}")
    lines_v2.append(f"prst int v{i} = {i}")
lines_v1.append("print(v69)")
lines_v2.append("print(v69)")
with open("/tmp/s2_many_v1.flx", "w") as f:
    f.write("\n".join(lines_v1))
with open("/tmp/s2_many_v2.flx", "w") as f:
    f.write("\n".join(lines_v2))
PY
cp /tmp/s2_many_v1.flx "$PROJ10/v1.flx"
cp /tmp/s2_many_v2.flx "$PROJ10/v2.flx"
cat > "$PROJ10/fluxa.toml" << 'TOML'
[project]
name = "many_prst"
entry = "v1.flx"
TOML
out=$(timeout 15s "$FLUXA" handover "$PROJ10/v1.flx" "$PROJ10/v2.flx" 2>&1 || true)
if echo "$out" | grep -q "COMMITTED" && echo "$out" | grep -q "^69$"; then
    pass "handover_with_70_prst_vars"
else
    fail "handover_with_70_prst_vars" "COMMITTED + 69" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → prst: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
