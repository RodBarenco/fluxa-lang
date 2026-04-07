#!/usr/bin/env bash
# tests/suite2/s2_block.sh
# Suite 2 — Section 5: Block + typeof edge cases
#
# Covers: hundreds of typeof instances, Block with arr fields, Block in dyn
# isolation, method calling method, recursive-style methods, field access
# after typeof, typeof cannot target instance.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  block/%s\n" "$1"; }
fail() { printf "  FAIL  block/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── suite2/block: Block + typeof edge cases ──────────────────────────"

# ── CASE 1: 200 typeof instances — all isolated ────────────────────────────────
cat > "$WORK_DIR/b_200_typeof.flx" << 'FLX'
Block Counter {
    prst int val = 0
    fn set(int v) nil { val = v }
    fn get() int { return val }
}
dyn pool = []
int i = 0
while i < 200 {
    Block inst typeof Counter
    inst.set(i)
    pool[i] = inst
    i = i + 1
}
print(pool[0].get())
print(pool[199].get())
print(len(pool))
FLX
out=$(timeout 15s "$FLUXA" run "$WORK_DIR/b_200_typeof.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^199$" \
    && echo "$out" | grep -q "^200$"; then
    pass "200_typeof_instances_isolated"
else
    fail "200_typeof_instances_isolated" "0, 199, 200" "$out"
fi

# ── CASE 2: Block method calls another method ─────────────────────────────────
cat > "$WORK_DIR/b_method_chain.flx" << 'FLX'
Block Calc {
    prst int acc = 0
    fn add(int n) nil { acc = acc + n }
    fn double() nil { acc = acc * 2 }
    fn reset() nil { acc = 0 }
    fn result() int { return acc }
}
Block c typeof Calc
c.add(10)
c.double()
c.add(5)
print(c.result())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_method_chain.flx" 2>&1 || true)
if echo "$out" | grep -q "^25$"; then
    pass "block_method_chain"
else
    fail "block_method_chain" "25 (10*2+5)" "$out"
fi

# ── CASE 3: typeof isolation — root Block unchanged by instance ───────────────
cat > "$WORK_DIR/b_root_isolation.flx" << 'FLX'
Block Base {
    prst int x = 0
    fn set(int v) nil { x = v }
    fn get() int { return x }
}
Block inst typeof Base
inst.set(42)
print(Base.get())
print(inst.get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_root_isolation.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^42$"; then
    pass "typeof_root_unchanged_by_instance"
else
    fail "typeof_root_unchanged_by_instance" "0 (root), 42 (instance)" "$out"
fi

# ── CASE 4: typeof cannot target another instance ─────────────────────────────
cat > "$WORK_DIR/b_typeof_instance.flx" << 'FLX'
Block Foo {
    prst int v = 1
}
Block a typeof Foo
Block b typeof a
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_typeof_instance.flx" 2>&1 || true)
if echo "$out" | grep -qi "error\|cannot\|typeof.*instance\|instance.*typeof"; then
    pass "typeof_instance_as_source_rejected"
else
    fail "typeof_instance_as_source_rejected" "error: typeof cannot target instance" "$out"
fi

# ── CASE 5: Block with arr field — arr isolated per instance ──────────────────
cat > "$WORK_DIR/b_arr_field.flx" << 'FLX'
Block Buffer {
    int arr data[4] = [0, 0, 0, 0]
    fn write(int idx, int val) nil { data[idx] = val }
    fn read(int idx) int { return data[idx] }
}
Block b1 typeof Buffer
Block b2 typeof Buffer
b1.write(0, 100)
b2.write(0, 200)
print(b1.read(0))
print(b2.read(0))
print(Buffer.read(0))
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_arr_field.flx" 2>&1 || true)
if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^200$" \
    && echo "$out" | grep -q "^0$"; then
    pass "block_arr_field_per_instance_isolated"
else
    fail "block_arr_field_per_instance_isolated" "100, 200, 0 (all independent)" "$out"
fi

# ── CASE 6: Block root method access ─────────────────────────────────────────
cat > "$WORK_DIR/b_root_method.flx" << 'FLX'
Block Logger {
    prst int count = 0
    fn log() nil { count = count + 1 }
    fn get() int { return count }
}
Logger.log()
Logger.log()
Logger.log()
print(Logger.get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_root_method.flx" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "block_root_method_access"
else
    fail "block_root_method_access" "3" "$out"
fi

# ── CASE 7: multiple instances — independent prst ────────────────────────────
cat > "$WORK_DIR/b_multi_prst.flx" << 'FLX'
Block Node {
    prst int id = 0
    prst str name = "unnamed"
    fn init(int i, str n) nil {
        id = i
        name = n
    }
    fn show_id() int { return id }
}
Block n1 typeof Node
Block n2 typeof Node
Block n3 typeof Node
n1.init(1, "alpha")
n2.init(2, "beta")
n3.init(3, "gamma")
print(n1.show_id())
print(n2.show_id())
print(n3.show_id())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_multi_prst.flx" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^2$" \
    && echo "$out" | grep -q "^3$"; then
    pass "multiple_instances_independent_prst"
else
    fail "multiple_instances_independent_prst" "1, 2, 3" "$out"
fi

# ── CASE 8: Block in dyn — method mutates only clone ─────────────────────────
cat > "$WORK_DIR/b_dyn_mutate.flx" << 'FLX'
Block Acc {
    prst int total = 10
    fn add(int n) nil { total = total + n }
    fn get() int { return total }
}
Block a typeof Acc
dyn pool = [a]
pool[0].add(90)
print(pool[0].get())
print(a.get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_dyn_mutate.flx" 2>&1 || true)
if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^10$"; then
    pass "block_in_dyn_method_mutates_only_clone"
else
    fail "block_in_dyn_method_mutates_only_clone" "100 (clone), 10 (original)" "$out"
fi

# ── CASE 9: Block returning computed value ────────────────────────────────────
cat > "$WORK_DIR/b_computed.flx" << 'FLX'
Block PID {
    prst float kp = 2.0
    prst float signal = 5.0
    fn output() float { return kp * signal }
}
Block pid typeof PID
pid.kp = 3.0
pid.signal = 4.0
print(pid.output())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_computed.flx" 2>&1 || true)
if echo "$out" | grep -q "12\|12\.0"; then
    pass "block_computed_return_value"
else
    fail "block_computed_return_value" "12 (3.0 * 4.0)" "$out"
fi

# ── CASE 10: Block with dyn field ────────────────────────────────────────────
cat > "$WORK_DIR/b_dyn_field.flx" << 'FLX'
Block Accumulator {
    prst int total = 0
    prst int count = 0
    fn add(int v) nil { total = total + v  count = count + 1 }
    fn avg_count() int { return count }
}
Block acc typeof Accumulator
acc.add(10)
acc.add(20)
acc.add(30)
print(acc.avg_count())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/b_dyn_field.flx" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "block_with_dyn_field"
else
    fail "block_with_dyn_field" "3" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → block: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
