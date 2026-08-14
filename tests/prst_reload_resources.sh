#!/usr/bin/env bash
# tests/prst_reload_resources.sh — hot reload of prst holding external resources
#
# Regression suite for v0.18. Every case here failed before the fix:
#
#   1. `prst dyn win = graph.init(...)` re-ran its initializer on every reload,
#      so each save opened a fresh window instead of continuing in the old one.
#   2. The teardown collect swept dyn wrappers the surviving prst pool still
#      pointed at, leaving the pool with a dangling handle.
#   3. -dev threw away the prst pool produced by the very first run, so the
#      first save always restarted from the declared value.
#   4. A script that returned on its own was re-executed in a tight loop.
#   5. The watcher only ever looked at the entry file, never at live/ or
#      static/ modules.
#
# These are the invariants Fluxa Turtle depends on: write, save, watch the next
# step happen in the same window.
set -euo pipefail
set +o pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$PWD/${FLUXA#./}" ;; esac

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
FAILS=0; PASS=0
pass() { printf "  PASS  prst_reload/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  prst_reload/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── prst reload with external resources ──────────────────────────"

# ── 1: a prst dyn window is created once, no matter how many reloads ──────
P="$W/c1"; mkdir -p "$P"
printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std graph
prst dyn win = graph.init(320, 240, "reload-probe")
prst int step = 0
step = step + 1
print("STEP", step)
FLX
LOG="$P/dev.log"
( cd "$P" && timeout 14s stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG" 2>&1 ) &
DEV=$!
sleep 2; touch "$P/main.flx"; sleep 2; touch "$P/main.flx"; sleep 2; touch "$P/main.flx"; sleep 2
kill "$DEV" 2>/dev/null; wait "$DEV" 2>/dev/null || true

wins=$(grep -c "reload-probe" "$LOG" 2>/dev/null || echo 0)
[ "$wins" = "1" ] && pass "window_created_once_across_reloads" \
    || fail "window_created_once_across_reloads" "1 window" "$wins windows"

# ── 2: the prst counter never resets — including across the FIRST reload ──
steps=$(grep "^STEP" "$LOG" | awk '{print $2}' | tr '\n' ' ')
echo "$steps" | grep -q "^1 2 3" && pass "prst_survives_first_reload" \
    || fail "prst_survives_first_reload" "1 2 3 ..." "$steps"

# ── 3: the restored window handle is still usable after a reload ──────────
# graph.fps() dereferences the cursor. If the teardown collect had freed the
# wrapper this reads freed memory or reports an invalid cursor.
grep -qi "invalid window cursor" "$LOG" \
    && fail "restored_window_handle_usable" "no cursor error" "invalid window cursor" \
    || pass "restored_window_handle_usable"

# ── 4: a script that returns on its own is NOT re-executed in a loop ──────
P="$W/c4"; mkdir -p "$P"
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
prst int n = 0
n = n + 1
print("RUN", n)
FLX
LOG4="$P/dev.log"
( cd "$P" && timeout 9s stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG4" 2>&1 ) &
DEV=$!
sleep 6
kill "$DEV" 2>/dev/null; wait "$DEV" 2>/dev/null || true
runs=$(grep -c "^RUN" "$LOG4" 2>/dev/null || echo 0)
[ "$runs" -le 2 ] && pass "finished_script_does_not_respawn" \
    || fail "finished_script_does_not_respawn" "<=2 runs in 6s" "$runs runs"

# ── 5: editing a scalar initializer still wins over the pooled value ──────
P="$W/c5"; mkdir -p "$P"
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
prst int base = 12
base = base + 1
print("BASE", base)
FLX
LOG5="$P/dev.log"
( cd "$P" && timeout 12s stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG5" 2>&1 ) &
DEV=$!
sleep 2; touch "$P/main.flx"; sleep 2
sed -i 's/= 12/= 99/' "$P/main.flx"; sleep 3
kill "$DEV" 2>/dev/null; wait "$DEV" 2>/dev/null || true
grep -q "BASE 100" "$LOG5" && pass "edited_scalar_initializer_still_wins" \
    || fail "edited_scalar_initializer_still_wins" "BASE 100" "$(grep BASE "$LOG5" | tr '\n' ' ')"

# ── 6: saving a live/ module triggers a reload ────────────────────────────
P="$W/c6"; mkdir -p "$P/live"
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/live/counter.flx" << 'FLX'
Block Counter {
    prst int total = 0
    fn bump() nil { total = total + 1 }
    fn get()  int { return total }
}
FLX
cat > "$P/main.flx" << 'FLX'
import live counter
Block c typeof counter.Counter
c.bump()
print("TICK", c.get())
FLX
LOG6="$P/dev.log"
( cd "$P" && timeout 12s stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG6" 2>&1 ) &
DEV=$!
# Measure a quiet baseline first: a runtime that respawns finished scripts would
# already be in the dozens here, and counting only the total afterwards would let
# that bug masquerade as a working watcher.
sleep 4
before=$(grep -c "^TICK" "$LOG6" 2>/dev/null || echo 0)
echo "// touched" >> "$P/live/counter.flx"
sleep 4
after=$(grep -c "^TICK" "$LOG6" 2>/dev/null || echo 0)
kill "$DEV" 2>/dev/null; wait "$DEV" 2>/dev/null || true
[ "$before" -eq 1 ] && [ "$after" -ge 2 ] && pass "live_module_save_triggers_reload" \
    || fail "live_module_save_triggers_reload" "1 run idle, >=2 after touch" \
            "$before idle, $after after"

# ── 7: a static/ module is watched too ────────────────────────────────────
P="$W/c7"; mkdir -p "$P/static"
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
printf 'fn two() int { return 2 }\n' > "$P/static/nums.flx"
cat > "$P/main.flx" << 'FLX'
import static nums
prst int seen = 0
seen = seen + nums.two()
print("SEEN", seen)
FLX
LOG7="$P/dev.log"
( cd "$P" && timeout 12s stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG7" 2>&1 ) &
DEV=$!
sleep 4
before=$(grep -c "^SEEN" "$LOG7" 2>/dev/null || echo 0)
echo "// touched" >> "$P/static/nums.flx"
sleep 4
after=$(grep -c "^SEEN" "$LOG7" 2>/dev/null || echo 0)
kill "$DEV" 2>/dev/null; wait "$DEV" 2>/dev/null || true
[ "$before" -eq 1 ] && [ "$after" -ge 2 ] && pass "static_module_save_triggers_reload" \
    || fail "static_module_save_triggers_reload" "1 run idle, >=2 after touch" \
            "$before idle, $after after"

# ── 8/9: runtime swap — serializable state is restored, the resource is reborn ──
# A reload keeps the pointer; a runtime swap cannot. The wire format carries no
# VAL_DYN, so `win` comes back as a headstone (VAL_NIL under declared_type
# VAL_DYN) and the window must be rebuilt from the declaration — while the
# counter next to it is restored from the snapshot.
SRC_ROOT="${SRC_ROOT:-$PWD}"
if [ -f "$SRC_ROOT/tests/tools/mk_restart_snapshot.c" ] && [ -f "$SRC_ROOT/src/scope.c" ]; then
    P="$W/c8"; mkdir -p "$P"
    if cc -std=c99 -D_POSIX_C_SOURCE=200809L -I"$SRC_ROOT/src" -I"$SRC_ROOT/vendor" \
          "$SRC_ROOT/tests/tools/mk_restart_snapshot.c" "$SRC_ROOT/src/scope.c" \
          -o "$P/mksnap" 2>/dev/null; then
        printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\n' > "$P/fluxa.toml"
        cat > "$P/main.flx" << 'FLX'
import std graph
prst dyn win      = graph.init(320, 240, "after-swap")
prst int readings = 0
readings = readings + 1
print("READINGS", readings)
print("FPS", graph.fps(win))
FLX
        "$P/mksnap" "$P/snap.bin" > /dev/null
        out=$(cd "$P" && FLUXA_RESTART_SNAPSHOT=./snap.bin timeout 8s "$FLUXA" \
              run main.flx -proj . 2>&1)

        echo "$out" | grep -q "READINGS 42" && pass "swap_restores_serializable_state" \
            || fail "swap_restores_serializable_state" "READINGS 42" \
                    "$(echo "$out" | grep READINGS)"

        # The window must be rebuilt, not restored as nil: a live handle answers
        # graph.fps, a headstone reports an undefined variable.
        if echo "$out" | grep -q "FPS 60" && ! echo "$out" | grep -q "undefined variable: win"; then
            pass "swap_rebuilds_external_resource"
        else
            fail "swap_rebuilds_external_resource" "FPS 60, no undefined variable" \
                 "$(echo "$out" | grep -E 'FPS|undefined' | tr '\n' ' ')"
        fi
    else
        printf "  SKIP  prst_reload/swap_semantics  (snapshot tool did not build)\n"
    fi
else
    printf "  SKIP  prst_reload/swap_semantics  (run from the repo root)\n"
fi

# ── 10/11: handover — same rules, and the Dry Run must not leak a handle ──
# Stage 2 serializes even though it never leaves the process, so a resource is a
# headstone here too. On top of that the Dry Run really executes lib calls, and
# step 3 collects B's GC while letting the pool survive — so anything the
# rehearsal opened must be marked dead before the pool crosses into step 4.
P="$W/c10"; mkdir -p "$P"
printf '[project]\nname="t"\nentry="a.flx"\n[libs]\nstd.graph="1.0"\n' > "$P/fluxa.toml"
cat > "$P/a.flx" << 'FLX'
import std graph
prst dyn win      = graph.init(320, 240, "W")
prst int readings = 0
readings = readings + 1
print("A readings=", readings, " fps=", graph.fps(win))
FLX
sed 's/"A readings/"B readings/' "$P/a.flx" > "$P/b.flx"
out=$(cd "$P" && timeout 20s "$FLUXA" handover a.flx b.flx 2>&1) && rc=0 || rc=$?

[ "$rc" -eq 0 ] && pass "handover_completes_with_resource_prst" \
    || fail "handover_completes_with_resource_prst" "exit 0" "exit $rc"

echo "$out" | grep -q "B readings= 2" && pass "handover_preserves_measurements" \
    || fail "handover_preserves_measurements" "B readings= 2" \
            "$(echo "$out" | grep 'B readings' )"

# fps= nil means the pool handed over a headstone that nobody rebuilt; a crash
# or ASan report means it handed over a handle the Dry Run's collect had freed.
echo "$out" | grep -q "B readings= 2  fps= 60" && pass "handover_rebuilds_resource_after_dry_run" \
    || fail "handover_rebuilds_resource_after_dry_run" "fps= 60" \
            "$(echo "$out" | grep 'B readings')"

# ── 13: the platform clock fails closed on bare metal ─────────────────────
# handover.c is in SRCS_EMBEDDED, so it must cross-compile without POSIX time.
# The fallback is a weak hook the SDK overrides; if nobody wires it, step 4 has
# to refuse rather than invent a deadline — an upgrade is expendable, the
# running service is not.
if [ -f "$SRC_ROOT/tests/tools/handover_no_clock.c" ]; then
    if cc -std=c99 -O1 -DFLUXA_EMBEDDED=1 -DFLUXA_IPC_NONE=1 -DFLUXA_HAS_FFI=0 \
          -I"$SRC_ROOT/src" -I"$SRC_ROOT/vendor" \
          "$SRC_ROOT/tests/tools/handover_no_clock.c" \
          "$SRC_ROOT/src/handover.c" "$SRC_ROOT/src/scope.c" \
          "$SRC_ROOT/src/resolver.c" "$SRC_ROOT/src/bytecode.c" \
          "$SRC_ROOT/src/builtins.c" "$SRC_ROOT/src/block.c" \
          "$SRC_ROOT/src/runtime.c" "$SRC_ROOT/src/lexer.c" \
          "$SRC_ROOT/src/parser.c" "$SRC_ROOT/src/ffi.c" \
          "$SRC_ROOT/src/ipc_server.c" \
          -o "$W/hclk" -lm -lpthread 2>/dev/null; then
        if "$W/hclk" > "$W/hclk.out" 2>/dev/null; then
            pass "embedded_clock_fails_closed"
        else
            fail "embedded_clock_fails_closed" "all clock checks pass" \
                 "$(grep FAIL "$W/hclk.out" | tr '\n' ' ')"
        fi
    else
        printf "  SKIP  prst_reload/embedded_clock_fails_closed  (probe did not build)\n"
    fi
fi

# ── 14: the script thread is the SAME across reloads ──────────────────────
# -dev used to spawn a thread per execution. That broke anything holding
# thread-local state: a GLFW context is current per thread, so the one
# graph.init made current on the first run stopped being current when that
# thread exited, and every GL call from the second run onward was a silent
# no-op — the window froze on the last frame of run 1, graph.capture returned a
# zeroed buffer, and texture uploads failed. Checking thread identity is the
# cheap, headless way to catch a regression back to per-execution threads.
if [ -r /proc/self/task ]; then
    P="$W/c14"; mkdir -p "$P"
    printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
    cat > "$P/main.flx" << 'FLX'
prst int run = 0
run = run + 1
print("RUN", run)
int i = 0
while i < 2000000 { i = i + 1 }
FLX
    LOG14="$P/dev.log"
    ( cd "$P" && setsid stdbuf -o0 "$FLUXA" run main.flx -proj . -dev > "$LOG14" 2>&1 ) &
    sleep 2
    DPID=$(grep -o "fluxa-[0-9]*\.sock" "$LOG14" 2>/dev/null | head -1 | grep -o "[0-9]*")
    if [ -n "$DPID" ] && [ -d "/proc/$DPID/task" ]; then
        tids_before=$(ls "/proc/$DPID/task" 2>/dev/null | sort | tr '\n' ' ')
        seen_extra=0
        for _ in 1 2 3; do
            touch "$P/main.flx"
            sleep 2
            now=$(ls "/proc/$DPID/task" 2>/dev/null | sort | tr '\n' ' ')
            [ "$now" != "$tids_before" ] && seen_extra=1
        done
        kill "$DPID" 2>/dev/null
        runs=$(grep -c "^RUN" "$LOG14" 2>/dev/null || echo 0)
        if [ "$seen_extra" -eq 0 ] && [ "$runs" -ge 3 ]; then
            pass "script_thread_is_stable_across_reloads"
        else
            fail "script_thread_is_stable_across_reloads" \
                 "the same thread ids across every reload" \
                 "before='$tids_before' changed=$seen_extra runs=$runs"
        fi
    else
        kill "$DPID" 2>/dev/null
        printf "  SKIP  prst_reload/script_thread_is_stable_across_reloads  (no pid)\n"
    fi
else
    printf "  SKIP  prst_reload/script_thread_is_stable_across_reloads  (no /proc)\n"
fi

echo "  → prst_reload: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → prst_reload: PASS" && exit 0 || exit 1
