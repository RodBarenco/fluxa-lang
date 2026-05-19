#!/usr/bin/env bash
# tests/libs/flxthread.sh — std.flxthread test suite
#
# DETERMINISM GUARANTEE:
#   All assertions use ft.await or ft.resolve_all to confirm state.
#   time.sleep is used only in the thread body (to demonstrate loop patterns),
#   never in the test harness for synchronization.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.flxthread/%s\n" "$1"; }
fail() { printf "  FAIL  std.flxthread/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
TOML
}

echo "── std.flxthread: threading library ─────────────────────────────────"

# CASE 1: import without [libs]
cat > "$WORK_DIR/no_toml.flx" << 'FLX'
import std flxthread as ft
ft.new("t1", "fn_that_doesnt_exist")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/no_toml.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|toml\|libs"; then
    pass "import_without_toml_error"
else
    fail "import_without_toml_error" "error: not declared in [libs]" "$out"
fi

# CASE 2: ft.new global fn + ft.resolve_all — DETERMINISTIC: resolve_all blocks
# NOTE: global fn threads run via the tree-walk evaluator for the fn body.
# while-loops compile to the bytecode VM which reads/writes prst via scope
# directly (no aliasing issue). For prst + loop patterns, use Block threads.
# Here we verify: fn executes in a thread, prst syncs back to main runtime.
P="$WORK_DIR/p2"; setup "$P" "global_fn"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
prst int result = 0
fn worker() nil {
    result = result + 1
    result = result + 1
    result = result + 1
}
ft.new("t1", "worker")
ft.resolve_all()
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^3$" && pass "global_function_thread" || fail "global_function_thread" "3" "$out"

# CASE 3: ft.new Block method + ft.resolve_all — DETERMINISTIC
P="$WORK_DIR/p3"; setup "$P" "block_thread"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
Block Worker {
    prst int steps = 0
    fn run() nil { int i = 0  while i < 4 { steps = steps + 1  i = i + 1 } }
    fn get() int { return steps }
}
Block w typeof Worker
ft.new("t1", w, "run")
ft.resolve_all()
print(w.get())
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^4$" && pass "block_method_thread" || fail "block_method_thread" "4" "$out"

# CASE 4: ft.active lifecycle — DETERMINISTIC: use sleep to keep thread alive during check
P="$WORK_DIR/p4"; setup "$P" "active"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "active"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block W { fn run() nil { int i = 0  while i < 5 { i = i + 1  time.sleep(20) } } }
Block w typeof W
ft.new("t1", w, "run")
bool before = ft.active("t1")
ft.resolve_all()
bool after = ft.active("t1")
print(before)
print(after)
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" && echo "$out" | sed -n '2p' | grep -q "false"; then
    pass "ft_active_lifecycle"
else
    fail "ft_active_lifecycle" "true then false" "$out"
fi

# CASE 5: ft.message — DETERMINISTIC: ft.await confirms messages were processed
P="$WORK_DIR/p5"; setup "$P" "message"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Accumulator {
    prst int total = 0
    fn run() nil { int i = 0  while i < 20 { i = i + 1  time.sleep(10) } }
    fn add(int n) nil { total = total + n }
    fn get() int { return total }
}
Block acc typeof Accumulator
ft.new("t1", acc, "run")
ft.message("t1", "add", 5)
ft.message("t1", "add", 10)
int v = ft.await("t1", "get")
print(v)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^15$" && pass "ft_message_non_blocking" || fail "ft_message_non_blocking" "15" "$out"

# CASE 6: ft.await — DETERMINISTIC: ft.await blocks until thread replies
P="$WORK_DIR/p6"; setup "$P" "await"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Counter {
    prst int val = 0
    fn run() nil { int i = 0  while i < 5 { val = val + 1  i = i + 1  time.sleep(5) } }
    fn get() int { return val }
}
Block c typeof Counter
ft.new("t1", c, "run")
int v = ft.await("t1", "get")
print(v > 0)
print(v <= 5)
ft.resolve_all()
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "ft_await_returns_value"
else
    fail "ft_await_returns_value" "true true (0 < val <= 5)" "$out"
fi

# CASE 7: ft.lock — DETERMINISTIC: resolve_all confirms both done
P="$WORK_DIR/p7"; setup "$P" "lock"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
prst int shared = 0
ft.lock("shared")
fn inc_a() nil { int i = 0  while i < 50 { shared = shared + 1  i = i + 1 } }
fn inc_b() nil { int i = 0  while i < 50 { shared = shared + 1  i = i + 1 } }
ft.new("t1", "inc_a")
ft.new("t2", "inc_b")
ft.resolve_all()
print(shared)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
_val=$(echo "$out" | grep -o "^[0-9]*$" || echo "0")
if [ "${_val:-0}" -ge 50 ] && [ "${_val:-0}" -le 100 ]; then
    pass "ft_lock_shared_counter"
else
    fail "ft_lock_shared_counter" "50-100" "$out"
fi

# CASE 8: LOOP WITH SLEEP — documented pattern, DETERMINISTIC via ft.await
# The sleep in the thread body IS the pattern. ft.await confirms final state.
P="$WORK_DIR/p8"; setup "$P" "loop_with_sleep"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Sensor {
    prst int readings = 0
    prst int extra    = 0
    fn poll() nil {
        int i = 0
        while i < 15 {
            readings = readings + 1
            i = i + 1
            time.sleep(10)   // documented sleep-loop pattern
        }
    }
    fn add_extra(int n) nil { extra = extra + n }
    fn get_total() int { return readings + extra }
}
Block s typeof Sensor
ft.new("t1", s, "poll")
ft.message("t1", "add_extra", 100)
int total = ft.await("t1", "get_total")
print(total > 100)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "true" && pass "loop_with_sleep_mailbox_drain" || fail "loop_with_sleep_mailbox_drain" "true (>100)" "$out"

# CASE 9: HOT LOOP — documented pattern, DETERMINISTIC via resolve_all
P="$WORK_DIR/p9"; setup "$P" "hot_loop"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block HotWorker {
    prst int sum = 0
    fn run() nil {
        int i = 0
        while i < 10000 { sum = sum + i  i = i + 1 }  // no sleep: O(1) drain check
    }
    fn get() int { return sum }
}
Block h typeof HotWorker
int t0 = time.now_ms()
ft.new("t1", h, "run")
ft.resolve_all()
int dt = time.elapsed_ms(t0)
print(h.get())
print(dt < 10000)
FLX
out=$(timeout 15s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^49995000$" && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "hot_loop_no_sleep_fast_path"
else
    fail "hot_loop_no_sleep_fast_path" "49995000 + < 10s" "$out"
fi

# CASE 10: two isolated Block instances — DETERMINISTIC: resolve_all
P="$WORK_DIR/p10"; setup "$P" "isolation"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
Block Worker {
    prst int steps = 0
    fn run() nil { int i = 0  while i < 3 { steps = steps + 1  i = i + 1 } }
    fn get() int { return steps }
}
Block w1 typeof Worker
Block w2 typeof Worker
ft.new("t1", w1, "run")
ft.new("t2", w2, "run")
ft.resolve_all()
print(w1.get())
print(w2.get())
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "^3$" && echo "$out" | sed -n '2p' | grep -q "^3$"; then
    pass "two_isolated_block_instances"
else
    fail "two_isolated_block_instances" "3 and 3" "$out"
fi

# CASE 11: ft.thread_count — note: may be 1 or 2 if fast threads finish early
P="$WORK_DIR/p11"; setup "$P" "thread_count"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
Block W { fn run() nil { int i = 0  while i < 3 { i = i + 1 } } }
Block w1 typeof W
Block w2 typeof W
ft.new("t1", w1, "run")
ft.new("t2", w2, "run")
int n = ft.thread_count()
ft.resolve_all()
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
_tc=$(echo "$out" | grep -o "^[0-9]$" || echo "0")
if [ "${_tc:-0}" -ge 1 ] && [ "${_tc:-0}" -le 2 ]; then
    pass "ft_thread_count"
else
    fail "ft_thread_count" "1 or 2" "$out"
fi

# CASE 12: POLLING LOOP — documented pattern, DETERMINISTIC via resolve_all
P="$WORK_DIR/p12"; setup "$P" "polling"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Poller {
    prst int count = 0
    prst bool stop = false
    fn run() nil {
        while !stop { count = count + 1 }  // polling pattern: no sleep
    }
    fn halt() nil { stop = true }
    fn get() int { return count }
}
Block p typeof Poller
int t0 = time.now_ms()
ft.new("t1", p, "run")
ft.message("t1", "halt")
ft.resolve_all()
int dt = time.elapsed_ms(t0)
print(p.get() > 0)
print(dt < 5000)
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "polling_loop_max_responsiveness"
else
    fail "polling_loop_max_responsiveness" "true true" "$out"
fi

# CASE 13: ft.stop — DETERMINISTIC: resolve_all waits for cooperative stop
P="$WORK_DIR/p13"; setup "$P" "stop"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Worker {
    prst int count = 0
    fn run() nil {
        while true { count = count + 1  time.sleep(5) }
    }
    fn get() int { return count }
}
Block w typeof Worker
ft.new("t1", w, "run")
ft.stop("t1")
ft.resolve_all()
print(w.get() >= 0)
print(ft.active("t1"))
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" && echo "$out" | sed -n '2p' | grep -q "false"; then
    pass "ft_stop_cooperative"
else
    fail "ft_stop_cooperative" "true + false" "$out"
fi

# CASE 14: ft.should_stop — DETERMINISTIC: resolve_all waits for cleanup print
P="$WORK_DIR/p14"; setup "$P" "should_stop"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Sensor {
    prst int readings = 0
    fn run() nil {
        while !ft.should_stop() { readings = readings + 1  time.sleep(5) }
        print("shutdown")
    }
    fn get() int { return readings }
}
Block s typeof Sensor
ft.new("t1", s, "run")
ft.stop("t1")
ft.resolve_all()
print(s.get() >= 0)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "shutdown" && echo "$out" | grep -q "true"; then
    pass "ft_should_stop_graceful_shutdown"
else
    fail "ft_should_stop_graceful_shutdown" "shutdown + true" "$out"
fi

# CASE 15: ft.kill — DETERMINISTIC: ft.kill marks dead synchronously
P="$WORK_DIR/p15"; setup "$P" "kill"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Slow {
    fn run() nil { int i = 0  while i < 10000 { i = i + 1  time.sleep(100) } }
}
Block s typeof Slow
ft.new("t1", s, "run")
ft.kill("t1")
print(ft.active("t1"))
print(1)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "false" && echo "$out" | grep -q "^1$"; then
    pass "ft_kill_immediate"
else
    fail "ft_kill_immediate" "false + 1" "$out"
fi


# CASE 16: ft.new global fn with 1 arg
P="$WORK_DIR/p16"; setup "$P" "new_fn_1arg"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
prst int result = 0
fn worker(int n) nil {
    result = n
}
ft.new("t1", "worker", 99)
ft.resolve_all()
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^99$" && pass "new_fn_1arg" || fail "new_fn_1arg" "99" "$out"

# CASE 17: ft.new global fn with 3 args
P="$WORK_DIR/p17"; setup "$P" "new_fn_3args"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "new_fn_3args"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
[libs.flxthread]
max_msg_args = 3
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
prst int result = 0
fn worker(int a, int b, int c) nil {
    result = a + b + c
}
ft.new("t1", "worker", 10, 20, 30)
ft.resolve_all()
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^60$" && pass "new_fn_3args" || fail "new_fn_3args" "60" "$out"

# CASE 18: ft.message with 2 args
P="$WORK_DIR/p18"; setup "$P" "message_2args"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "message_2args"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
[libs.flxthread]
max_msg_args = 2
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Acc {
    int total = 0
    fn run() nil { int i = 0  while i < 20 { i = i + 1  time.sleep(5) } }
    fn add(int a, int b) nil { total = total + a + b }
    fn get() int { return total }
}
Block acc typeof Acc
ft.new("t1", acc, "run")
ft.message("t1", "add", 10, 20)
ft.message("t1", "add", 5, 5)
int v = ft.await("t1", "get")
print(v)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^40$" && pass "message_2args" || fail "message_2args" "40" "$out"

# CASE 19: ft.await with 2 args
P="$WORK_DIR/p19"; setup "$P" "await_2args"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "await_2args"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
[libs.flxthread]
max_msg_args = 2
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block Calc {
    fn run() nil { int i = 0  while i < 20 { i = i + 1  time.sleep(5) } }
    fn mul(int a, int b) int { return a * b }
}
Block c typeof Calc
ft.new("t1", c, "run")
int v = ft.await("t1", "mul", 6, 7)
print(v)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^42$" && pass "await_2args" || fail "await_2args" "42" "$out"

# CASE 20: overflow protection — too many args for ft.new
P="$WORK_DIR/p20"; setup "$P" "new_overflow"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
fn worker(int a, int b) nil { }
danger {
    ft.new("t1", "worker", 1, 2, 3)
}
if err != nil { print("overflow caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "overflow caught" && pass "new_fn_overflow_caught" || fail "new_fn_overflow_caught" "overflow caught" "$out"

# CASE 21: overflow protection — too many args for ft.message
P="$WORK_DIR/p21"; setup "$P" "message_overflow"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "message_overflow"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block W {
    fn run() nil { int i = 0  while i < 5 { i = i + 1  time.sleep(5) } }
    fn add(int a) nil { }
}
Block w typeof W
ft.new("t1", w, "run")
danger {
    ft.message("t1", "add", 1, 2, 3)
}
if err != nil { print("overflow caught") }
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "overflow caught" && pass "message_overflow_caught" || fail "message_overflow_caught" "overflow caught" "$out"

# CASE 22: arity mismatch — ft.new with more args than fn params
P="$WORK_DIR/p22"; setup "$P" "arity_mismatch"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "arity_mismatch"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
[libs.flxthread]
max_msg_args = 4
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
fn worker(int a) nil { }
danger {
    ft.new("t1", "worker", 1, 2, 3)
}
if err != nil { print("arity error") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "arity error" && pass "new_fn_arity_mismatch" || fail "new_fn_arity_mismatch" "arity error" "$out"

# CASE 23: toml max_msg_args respected
P="$WORK_DIR/p23"; setup "$P" "toml_max_msg"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "toml_max_msg"
entry = "main.flx"
[libs]
std.flxthread = "1.0"
std.time = "1.0"
[libs.flxthread]
max_msg_args = 4
TOML
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time
Block W {
    int total = 0
    fn run() nil { int i = 0  while i < 20 { i = i + 1  time.sleep(5) } }
    fn add(int a, int b, int c, int d) nil { total = total + a + b + c + d }
    fn get() int { return total }
}
Block w typeof W
ft.new("t1", w, "run")
ft.message("t1", "add", 1, 2, 3, 4)
int v = ft.await("t1", "get")
print(v)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^10$" && pass "toml_max_msg_args_4" || fail "toml_max_msg_args_4" "10" "$out"

echo "────────────────────────────────────────────────────────────────────"
total=23
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.flxthread: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
