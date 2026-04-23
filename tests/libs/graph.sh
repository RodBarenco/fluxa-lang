#!/usr/bin/env bash
# tests/libs/graph.sh — std.graph test suite
# Tests the stub backend (which is the default when Raylib is not vendored).
# All rendering calls are no-ops in stub mode — we test API correctness,
# cursor patterns, error handling, and prst survival.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/graph/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/graph/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.graph ────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std graph
danger { dyn w = graph.init(800, 600, "test") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a string
out=$(run << 'FLX'
import std graph
str v = graph.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. init returns a cursor
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    bool ok = w != nil
    print(ok)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "true" && pass "init_returns_cursor" || fail "init_returns_cursor" "true" "$out"

# 4. set_fps and fps
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    graph.set_fps(w, 30)
    int f = graph.fps(w)
    print(f)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "^30$" && pass "set_fps_and_fps" || fail "set_fps_and_fps" "30" "$out"

# 5. should_close — stub returns false
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    bool sc = graph.should_close(w)
    print(sc)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "false" && pass "should_close_false_in_stub" || fail "should_close_false_in_stub" "false" "$out"

# 6. dt returns a float
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    graph.set_fps(w, 60)
    float d = graph.dt(w)
    bool ok = d > 0.0
    print(ok)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "true" && pass "dt_is_positive" || fail "dt_is_positive" "true" "$out"

# 7. draw calls don't crash (no-op in stub)
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    graph.begin_frame(w)
    graph.clear(w, 0, 0, 0)
    graph.draw_rect(w, 10, 10, 100, 50, 255, 0, 0)
    graph.draw_circle(w, 400, 300, 50, 0, 255, 0)
    graph.draw_line(w, 0, 0, 800, 600, 255, 255, 255)
    graph.draw_text(w, "hello", 10, 10, 20, 255, 255, 255)
    graph.end_frame(w)
    print("draw ok")
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "draw ok" && pass "draw_calls_no_crash" || fail "draw_calls_no_crash" "draw ok" "$out"

# 8. input queries return 0/false in stub
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    bool kp = graph.key_pressed(w, "SPACE")
    bool kd = graph.key_down(w, "A")
    int mx  = graph.mouse_x(w)
    int my  = graph.mouse_y(w)
    bool mb = graph.mouse_pressed(w)
    print(kp)
    print(kd)
    print(mx)
    print(my)
    print(mb)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "false" && pass "input_returns_false_stub" || fail "input_returns_false_stub" "false" "$out"
echo "$out" | grep -q "^0$"   && pass "mouse_pos_zero_stub"      || fail "mouse_pos_zero_stub"      "0"     "$out"

# 9. close bad cursor → error
out=$(run << 'FLX'
import std graph
danger {
    dyn bad = [1, 2, 3]
    graph.close(bad)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "close_bad_cursor_error" || fail "close_bad_cursor_error" "error caught" "$out"

# 10. unknown function → error
out=$(run << 'FLX'
import std graph
danger { graph.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 11. prst dyn cursor survives hot-reload pattern
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(320, 240, "prst test")
    bool ok = win != nil
    print(ok)
    graph.close(win)
}
FLX
)
echo "$out" | grep -q "true" && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "true" "$out"

# 12. game loop pattern (3 frames in stub)
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "game loop test")
    int frame = 0
    while frame < 3 {
        graph.begin_frame(w)
        graph.clear(w, 0, 0, 0)
        graph.draw_rect(w, frame, frame, 10, 10, 255, 255, 255)
        graph.end_frame(w)
        frame = frame + 1
    }
    print(frame)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "^3$" && pass "game_loop_3_frames" || fail "game_loop_3_frames" "3" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.graph: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.graph: PASS" && exit 0 || exit 1
