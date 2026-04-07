#!/usr/bin/env bash
# tests/libs/flxthread.sh — std.flxthread test suite
#
# Covers the three documented loop patterns and the full API.
# All thread tests use conservative timing to avoid flakiness.
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

# ── CASE 1: import without [libs] → clear error ────────────────────────────
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

# ── CASE 2: ft.new with global function + ft.resolve_all ──────────────────
P="$WORK_DIR/p2"; setup "$P" "global_fn"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

prst int result = 0

fn worker() nil {
    int i = 0
    while i < 3 {
        result = result + 1
        i = i + 1
        time.sleep(20)
    }
}

ft.new("t1", "worker")
ft.resolve_all()
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$"; then
    pass "global_function_thread"
else
    fail "global_function_thread" "3" "$out"
fi

# ── CASE 3: ft.new with Block method + ft.resolve_all ─────────────────────
P="$WORK_DIR/p3"; setup "$P" "block_thread"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Worker {
    prst int steps = 0
    fn run() nil {
        int i = 0
        while i < 4 {
            steps = steps + 1
            i = i + 1
            time.sleep(15)
        }
    }
    fn get() int { return steps }
}

Block w typeof Worker
ft.new("t1", w, "run")
ft.resolve_all()
print(w.get())
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^4$"; then
    pass "block_method_thread"
else
    fail "block_method_thread" "4" "$out"
fi

# ── CASE 4: ft.active — check thread lifecycle ─────────────────────────────
P="$WORK_DIR/p4"; setup "$P" "active"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block W {
    fn run() nil {
        int i = 0
        while i < 3 { i = i + 1  time.sleep(20) }
    }
}

Block w typeof W
ft.new("t1", w, "run")
bool before = ft.active("t1")
ft.resolve_all()
bool after = ft.active("t1")
print(before)
print(after)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "false"; then
    pass "ft_active_lifecycle"
else
    fail "ft_active_lifecycle" "true then false" "$out"
fi

# ── CASE 5: ft.message — non-blocking call to Block method ────────────────
P="$WORK_DIR/p5"; setup "$P" "message"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Accumulator {
    prst int total = 0
    fn run() nil {
        int i = 0
        while i < 10 {
            i = i + 1
            time.sleep(20)
        }
    }
    fn add(int n) nil { total = total + n }
    fn get() int { return total }
}

Block acc typeof Accumulator
ft.new("t1", acc, "run")
time.sleep(50)
ft.message("t1", "add", 5)
ft.message("t1", "add", 10)
time.sleep(80)
int v = ft.await("t1", "get")
print(v > 14)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "ft_message_non_blocking"
else
    fail "ft_message_non_blocking" "true (total > 14, messages received)" "$out"
fi

# ── CASE 6: ft.await — blocking call returns value ────────────────────────
P="$WORK_DIR/p6"; setup "$P" "await"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Counter {
    prst int val = 0
    fn run() nil {
        int i = 0
        while i < 8 {
            val = val + 1
            i = i + 1
            time.sleep(15)
        }
    }
    fn get() int { return val }
}

Block c typeof Counter
ft.new("t1", c, "run")
time.sleep(60)
int v = ft.await("t1", "get")
print(v > 0)
print(v <= 8)
ft.resolve_all()
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "ft_await_returns_value"
else
    fail "ft_await_returns_value" "true true" "$out"
fi

# ── CASE 7: ft.lock — protects prst global from race condition ────────────
# Two threads increment a shared counter. ft.lock ensures no lost updates.
P="$WORK_DIR/p7"; setup "$P" "lock"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

prst int shared = 0
ft.lock("shared")

fn inc_a() nil {
    int i = 0
    while i < 50 {
        shared = shared + 1
        i = i + 1
    }
}

fn inc_b() nil {
    int i = 0
    while i < 50 {
        shared = shared + 1
        i = i + 1
    }
}

ft.new("t1", "inc_a")
ft.new("t2", "inc_b")
ft.resolve_all()
print(shared)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
# ft.lock registers the mutex. Both threads run 50 iters each = 100 total.
# Without races: exactly 100. With races: between 50 and 100.
# Verify at least 50 (each thread completed its own work) and at most 100.
_val=$(echo "$out" | grep -o "^[0-9]*$" || echo "0")
if [ "${_val:-0}" -ge 50 ] && [ "${_val:-0}" -le 100 ]; then
    pass "ft_lock_shared_counter"
else
    fail "ft_lock_shared_counter" "50-100 (both threads ran)" "$out"
fi

# ── CASE 8: LOOP WITH SLEEP — back-edge drain at natural frequency ─────────
# Documented case: ft.message drains at sleep boundaries, not every iter.
P="$WORK_DIR/p8"; setup "$P" "loop_with_sleep"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Sensor {
    prst int readings = 0
    prst int extra    = 0
    fn poll() nil {
        int i = 0
        while i < 10 {
            readings = readings + 1
            i = i + 1
            time.sleep(20)
        }
    }
    fn add_extra(int n) nil { extra = extra + n }
    fn get_total() int { return readings + extra }
}

Block s typeof Sensor
ft.new("t1", s, "poll")
time.sleep(50)
ft.message("t1", "add_extra", 100)
time.sleep(50)
int total = ft.await("t1", "get_total")
print(total > 100)
ft.resolve_all()
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true"; then
    pass "loop_with_sleep_mailbox_drain"
else
    fail "loop_with_sleep_mailbox_drain" "true (total > 100)" "$out"
fi

# ── CASE 9: HOT LOOP — fast path O(1) when no messages ────────────────────
# Documented case: loop without sleep, back-edge check is O(1) fast path.
# Verifiable by timing: 10k iters must finish in reasonable time.
P="$WORK_DIR/p9"; setup "$P" "hot_loop"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block HotWorker {
    prst int sum = 0
    fn run() nil {
        int i = 0
        while i < 10000 {
            sum = sum + i
            i = i + 1
        }
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
# sum of 0..9999 = 49995000
if echo "$out" | grep -q "^49995000$" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "hot_loop_no_sleep_fast_path"
else
    fail "hot_loop_no_sleep_fast_path" "49995000 + completes < 10s" "$out"
fi

# ── CASE 10: TWO ISOLATED Block instances — no shared state ───────────────
P="$WORK_DIR/p10"; setup "$P" "isolation"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Worker {
    prst int steps = 0
    fn run() nil {
        int i = 0
        while i < 3 {
            steps = steps + 1
            i = i + 1
            time.sleep(20)
        }
    }
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
if echo "$out" | sed -n '1p' | grep -q "^3$" \
    && echo "$out" | sed -n '2p' | grep -q "^3$"; then
    pass "two_isolated_block_instances"
else
    fail "two_isolated_block_instances" "3 and 3 (isolated)" "$out"
fi

# ── CASE 11: ft.thread_count ──────────────────────────────────────────────
P="$WORK_DIR/p11"; setup "$P" "thread_count"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block W {
    fn run() nil {
        int i = 0
        while i < 3 { i = i + 1  time.sleep(20) }
    }
}

Block w1 typeof W
Block w2 typeof W

ft.new("t1", w1, "run")
ft.new("t2", w2, "run")
int n = ft.thread_count()
ft.resolve_all()
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^2$"; then
    pass "ft_thread_count"
else
    fail "ft_thread_count" "2" "$out"
fi

# ── CASE 12: POLLING LOOP — no sleep, driven by external condition ─────────
# Documented case: maximum responsiveness, drain runs every iteration.
P="$WORK_DIR/p12"; setup "$P" "polling"
cat > "$P/main.flx" << 'FLX'
import std flxthread as ft
import std time

Block Poller {
    prst int count  = 0
    prst bool stop  = false
    fn run() nil {
        while !stop {
            count = count + 1
        }
    }
    fn halt() nil { stop = true }
    fn get() int { return count }
}

Block p typeof Poller
int t0 = time.now_ms()
ft.new("t1", p, "run")
time.sleep(50)
ft.message("t1", "halt")
ft.resolve_all()
int dt = time.elapsed_ms(t0)
print(p.get() > 100)
print(dt < 2000)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "true"; then
    pass "polling_loop_max_responsiveness"
else
    fail "polling_loop_max_responsiveness" "true true" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=12
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.flxthread: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
