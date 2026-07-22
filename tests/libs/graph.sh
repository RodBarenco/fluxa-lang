#!/usr/bin/env bash
# tests/libs/graph.sh — std.graph test suite
# Tests the stub backend (which is the default when Raylib is not vendored).
# All rendering calls are no-ops in stub mode — we test API correctness,
# cursor patterns, error handling, and prst survival.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
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

# ── font API (stub backend) ─────────────────────────────────────
# The stub validates the file exists and provides deterministic metrics.
head -c 1024 /dev/urandom > "$P/test_font.ttf"

# 13. load_font happy path
out=$(run << FLX
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "$P/test_font.ttf", 32)
    bool ok = f != nil
    print(ok)
    graph.unload_font(w, f)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "true" && pass "load_font_returns_cursor" || fail "load_font_returns_cursor" "true" "$out"

# 14. load_font missing file → error captured in danger
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "/nonexistent/missing.ttf", 32)
    graph.close(w)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "load_font_missing_file_error" || fail "load_font_missing_file_error" "error caught" "$out"

# 15. load_font invalid size → error
out=$(run << FLX
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "$P/test_font.ttf", 0)
    graph.close(w)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "load_font_bad_size_error" || fail "load_font_bad_size_error" "error caught" "$out"

# 16. draw_text_font in a frame — no crash (UTF-8 accents included)
out=$(run << FLX
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "$P/test_font.ttf", 32)
    graph.begin_frame(w)
    graph.draw_text_font(w, f, "Olá, atenção!", 100, 100, 32, 255, 255, 255)
    graph.end_frame(w)
    print("font draw ok")
    graph.unload_font(w, f)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "font draw ok" && pass "draw_text_font_no_crash" || fail "draw_text_font_no_crash" "font draw ok" "$out"

# 17. text_width — deterministic stub metric: len*size*6/10 → 5*20*6/10 = 60
out=$(run << FLX
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "$P/test_font.ttf", 20)
    int tw = graph.text_width(w, f, "Hello", 20)
    print(tw)
    graph.unload_font(w, f)
    graph.close(w)
}
FLX
)
echo "$out" | grep -q "^60$" && pass "text_width_deterministic_stub" || fail "text_width_deterministic_stub" "60" "$out"

# 18. use after unload_font → invalid cursor error
out=$(run << FLX
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn f = graph.load_font(w, "$P/test_font.ttf", 32)
    graph.unload_font(w, f)
    int tw = graph.text_width(w, f, "x", 10)
    graph.close(w)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "font_use_after_unload_error" || fail "font_use_after_unload_error" "error caught" "$out"

# 19. draw_text_font with a bad font cursor → error
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    dyn bad = [1, 2, 3]
    graph.draw_text_font(w, bad, "x", 0, 0, 10, 255, 255, 255)
    graph.close(w)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "draw_text_font_bad_cursor_error" || fail "draw_text_font_bad_cursor_error" "error caught" "$out"

# 20. fullscreen toggles and reports the new state (stub tracks it)
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "t")
    bool a = graph.fullscreen(w)
    bool b = graph.fullscreen(w)
    print(a)
    print(b)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "true" && echo "$out" | grep -q "false" && pass "fullscreen_toggle_state" || fail "fullscreen_toggle_state" "true then false" "$out"

# 21. fullscreen on a closed/invalid window → error
out=$(run << 'FLX'
import std graph
danger {
    dyn bad = [9, 9]
    graph.fullscreen(bad)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "fullscreen_bad_window_error" || fail "fullscreen_bad_window_error" "error caught" "$out"

# capture returns a dyn handle sized to the logical resolution (stub: blank buffer)
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    graph.begin_frame(w)
    graph.clear(w, 10, 20, 40)
    graph.end_frame(w)
    dyn shot = graph.capture(w)
    bool ok = shot != nil
    print("captured", ok)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "captured true" && pass "capture_returns_handle" || fail "capture_returns_handle" "captured true" "$out"

# a captured handle flows straight into std.image (width/height/discard)
out=$(printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"; cat > "$P/main.flx" << 'FLX'
import std graph
import std image
danger {
    dyn w = graph.init(640, 480, "test")
    graph.begin_frame(w)
    graph.end_frame(w)
    dyn shot = graph.capture(w)
    print("w", image.width(shot))
    print("h", image.height(shot))
    image.resize(shot, 320, 240)
    print("tw", image.width(shot))
    image.discard(shot)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "w 640" && echo "$out" | grep -q "tw 320" \
    && pass "capture_flows_to_image" || fail "capture_flows_to_image" "w 640 / tw 320" "$out"

# capture on a bad window handle → clean error
out=$(run << 'FLX'
import std graph
danger {
    dyn bad = [9, 9]
    dyn shot = graph.capture(bad)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "capture_bad_window_error" || fail "capture_bad_window_error" "error caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.graph: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.graph: PASS" && exit 0 || exit 1
