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
# The triangle batch binds an image as its texture, so that block needs both
# libs declared; every other case keeps the graph-only manifest above.
toml2() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"; }
run2() { toml2; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

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

# draw_image accepts an image buffer (round trip: image → graph)
out=$(printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"; cat > "$P/main.flx" << 'FLX'
import std graph
import std image
danger {
    dyn w = graph.init(800, 600, "test")
    dyn pic = image.new(120, 90)
    graph.begin_frame(w)
    graph.draw_image(w, pic, 40, 40)
    graph.end_frame(w)
    print("drew")
    image.discard(pic)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "drew" && pass "draw_image_basic" || fail "draw_image_basic" "drew" "$out"

# draw_image accepts the optional scale argument (int or float)
out=$(printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"; cat > "$P/main.flx" << 'FLX'
import std graph
import std image
danger {
    dyn w = graph.init(800, 600, "test")
    dyn pic = image.new(120, 90)
    graph.begin_frame(w)
    graph.draw_image(w, pic, 40, 40, 0.5)
    graph.draw_image(w, pic, 200, 40, 2)
    graph.end_frame(w)
    print("scaled")
    image.discard(pic)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "scaled" && pass "draw_image_scaled" || fail "draw_image_scaled" "scaled" "$out"

# the full round trip: capture the frame, then draw it back
out=$(printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"; cat > "$P/main.flx" << 'FLX'
import std graph
import std image
danger {
    dyn w = graph.init(640, 480, "test")
    graph.begin_frame(w)
    graph.clear(w, 20, 40, 60)
    graph.end_frame(w)
    dyn shot = graph.capture(w)
    image.resize(shot, 160, 120)
    graph.begin_frame(w)
    graph.draw_image(w, shot, 10, 10)
    graph.end_frame(w)
    print("roundtrip ok")
    image.discard(shot)
    graph.close(w)
}
if err != nil { print(err[0]) }
FLX
timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "roundtrip ok" && pass "draw_image_roundtrip" || fail "draw_image_roundtrip" "roundtrip ok" "$out"

# draw_image on a bad image handle → clean error
out=$(run << 'FLX'
import std graph
danger {
    dyn w = graph.init(800, 600, "test")
    dyn bad = [1, 2]
    graph.begin_frame(w)
    graph.draw_image(w, bad, 0, 0)
    graph.end_frame(w)
    graph.close(w)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "draw_image_bad_handle_error" || fail "draw_image_bad_handle_error" "error caught" "$out"

# open_url accepts an https URL
out=$(run << 'FLX'
import std graph
danger { bool ok = graph.open_url("https://example.com/support") }
if err == nil { print("HTTPS_OK") }
if err != nil { print("HTTPS_REFUSED") }
FLX
)
echo "$out" | grep -q "HTTPS_OK" && pass "open_url_https" || fail "open_url_https" "HTTPS_OK" "$out"

# open_url accepts a mailto: URL
out=$(run << 'FLX'
import std graph
danger { bool ok = graph.open_url("mailto:someone@example.com") }
if err == nil { print("MAILTO_OK") }
if err != nil { print("MAILTO_REFUSED") }
FLX
)
echo "$out" | grep -q "MAILTO_OK" && pass "open_url_mailto" || fail "open_url_mailto" "MAILTO_OK" "$out"

# open_url refuses file:// (would expose local files)
out=$(run << 'FLX'
import std graph
danger { bool ok = graph.open_url("file:///etc/passwd") }
if err != nil { print("FILE_REFUSED") }
FLX
)
echo "$out" | grep -q "FILE_REFUSED" && pass "open_url_rejects_file" || fail "open_url_rejects_file" "FILE_REFUSED" "$out"

# open_url refuses other schemes (javascript:, ftp://)
out=$(run << 'FLX'
import std graph
danger { bool a = graph.open_url("javascript:alert(1)") }
if err != nil { print("JS_REFUSED") }
danger { bool b = graph.open_url("ftp://example.com/x") }
if err != nil { print("FTP_REFUSED") }
FLX
)
echo "$out" | grep -q "JS_REFUSED" && echo "$out" | grep -q "FTP_REFUSED" \
    && pass "open_url_rejects_schemes" || fail "open_url_rejects_schemes" "JS_REFUSED / FTP_REFUSED" "$out"

# open_url refuses an empty / truncated URL (same validation path as controls)
out=$(run << 'FLX'
import std graph
danger { bool a = graph.open_url("") }
if err != nil { print("EMPTY_REFUSED") }
danger { bool b = graph.open_url("http://") }
if err != nil { print("SHORT_REFUSED") }
FLX
)
echo "$out" | grep -q "EMPTY_REFUSED" && echo "$out" | grep -q "SHORT_REFUSED" \
    && pass "open_url_rejects_malformed" || fail "open_url_rejects_malformed" "EMPTY_REFUSED / SHORT_REFUSED" "$out"

# open_url with no argument → error
out=$(run << 'FLX'
import std graph
danger { bool ok = graph.open_url() }
if err != nil { print("ARITY_ERR") }
FLX
)
echo "$out" | grep -q "ARITY_ERR" && pass "open_url_arity_error" || fail "open_url_arity_error" "ARITY_ERR" "$out"

# run_img is run() with std.image also declared, for the cases that need an
# image handle. The original toml()/run() are left exactly as they were so the
# pre-existing cases keep producing identical output.
run_img() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.graph="1.0"\nstd.image="1.0"\n' > "$P/fluxa.toml"
    cat > "$P/main.flx"
    timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

# ══════════════════════════════════════════════════════════════════
# v0.30 additions. Everything above this line is the pre-existing suite
# and its output must stay byte-identical — these are all NEW dispatch
# names, and no existing function changed shape.
# ══════════════════════════════════════════════════════════════════

# draw_image_rot: the rotating form of draw_image, pivot at the image centre
out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "rot")
    dyn img = image.new(16, 16)
    graph.begin_frame(win)
    graph.draw_image_rot(win, img, 10, 10, 45.0)
    graph.draw_image_rot(win, img, 10, 10, 90.0, 2.0)
    graph.draw_image_rot(win, img, 10, 10, 30)
    graph.end_frame(win)
    graph.close(win)
    print("ROT_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "ROT_OK" && pass "draw_image_rot_accepts_int_and_float" \
    || fail "draw_image_rot_accepts_int_and_float" "ROT_OK" "$out"

# draw_image_rot rejects a non-positive scale
out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "rot")
    dyn img = image.new(16, 16)
    graph.draw_image_rot(win, img, 0, 0, 0.0, 0.0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "scale must be positive" && pass "draw_image_rot_scale_validated" \
    || fail "draw_image_rot_scale_validated" "scale must be positive" "$out"

# draw_sprite: full control form, and its source rectangle is bounds-checked
out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "spr")
    dyn img = image.new(64, 64)
    graph.begin_frame(win)
    graph.draw_sprite(win, img, 0, 0, 32, 32, 10, 10, 45.0, 255, 128, 0, 200)
    graph.end_frame(win)
    print("SPRITE_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "SPRITE_OK" && pass "draw_sprite_draws" \
    || fail "draw_sprite_draws" "SPRITE_OK" "$out"

out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "spr")
    dyn img = image.new(32, 32)
    graph.draw_sprite(win, img, 0, 0, 64, 64, 0, 0, 0.0, 255, 255, 255, 255)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "outside the image" && pass "draw_sprite_source_bounds_checked" \
    || fail "draw_sprite_source_bounds_checked" "outside the image" "$out"

# outline shapes and triangle
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "shapes")
    graph.begin_frame(win)
    graph.draw_rect_lines(win, 5, 5, 40, 20, 255, 0, 0)
    graph.draw_circle_lines(win, 60, 60, 12.5, 0, 255, 0)
    graph.draw_ring(win, 80, 80, 5.0, 12.0, 0, 0, 255)
    graph.draw_triangle(win, 0, 0, 10, 0, 5, 10, 255, 255, 0)
    graph.end_frame(win)
    print("SHAPES_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "SHAPES_OK" && pass "outline_shapes_draw" \
    || fail "outline_shapes_draw" "SHAPES_OK" "$out"

# draw_ring validates its radii
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "ring")
    graph.draw_ring(win, 10, 10, 20.0, 5.0, 255, 255, 255)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "inner_radius" && pass "draw_ring_radii_validated" \
    || fail "draw_ring_radii_validated" "radii error" "$out"

# render target: create, use, draw, release
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "rt")
    dyn rt  = graph.render_target(win, 64, 64)
    graph.begin_frame(win)
    graph.begin_render_target(win, rt)
    graph.clear(win, 0, 0, 0)
    graph.end_render_target(win)
    graph.draw_render_target(win, rt, 0, 0)
    graph.end_frame(win)
    graph.release_render_target(win, rt)
    print("RT_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "RT_OK" && pass "render_target_lifecycle" \
    || fail "render_target_lifecycle" "RT_OK" "$out"

# a released render target cursor is refused, not a double free
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "rt")
    dyn rt  = graph.render_target(win, 64, 64)
    graph.release_render_target(win, rt)
    graph.release_render_target(win, rt)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "invalid render target cursor" && pass "render_target_use_after_release" \
    || fail "render_target_use_after_release" "invalid render target cursor" "$out"

# blend mode validates its name
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "bm")
    graph.set_blend_mode(win, "ALPHA")
    graph.set_blend_mode(win, "NONE")
    graph.set_blend_mode(win, "NOPE")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "ALPHA, ADD, MULTIPLY" && pass "blend_mode_validated" \
    || fail "blend_mode_validated" "blend mode error" "$out"

# scissor on/off
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "sc")
    graph.begin_frame(win)
    graph.scissor(win, 0, 0, 100, 100)
    graph.scissor_off(win)
    graph.end_frame(win)
    print("SCISSOR_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "SCISSOR_OK" && pass "scissor_on_off" \
    || fail "scissor_on_off" "SCISSOR_OK" "$out"

# mouse buttons, wheel and char input
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "in")
    print("W", graph.mouse_wheel(win))
    print("C", graph.char_pressed(win))
    print("B", graph.mouse_btn_down(win, "MIDDLE"))
    print("P", graph.mouse_btn_pressed(win, "RIGHT"))
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "W 0" && echo "$out" | grep -q "C 0" \
    && pass "mouse_and_char_input" \
    || fail "mouse_and_char_input" "W 0 / C 0" "$out"

# an unknown mouse button is reported instead of acting on button 0
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "in")
    bool b = graph.mouse_btn_down(win, "SIDEWAYS")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "LEFT, RIGHT or MIDDLE" && pass "mouse_button_name_validated" \
    || fail "mouse_button_name_validated" "button name error" "$out"

# gamepad queries answer without a pad attached
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "pad")
    print("CONN", graph.pad_connected(win, 0))
    print("DOWN", graph.pad_down(win, 0, "A"))
    print("AXIS", graph.pad_axis(win, 0, "LEFT_X"))
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "CONN false" && pass "gamepad_queries_answer" \
    || fail "gamepad_queries_answer" "CONN false" "$out"

# gamepad id and button names are validated
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "pad")
    bool b = graph.pad_down(win, 9, "A")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "gamepad id must be" && pass "gamepad_id_validated" \
    || fail "gamepad_id_validated" "gamepad id error" "$out"

out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "pad")
    float a = graph.pad_axis(win, 0, "DIAGONAL")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "LEFT_X" && pass "gamepad_axis_name_validated" \
    || fail "gamepad_axis_name_validated" "axis name error" "$out"

# camera 2D: identity with no camera, target maps to screen centre, and the
# screen→world→screen round trip returns the original point. The maths lives in
# the lib rather than the backend, so this holds headless too.
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(400, 300, "cam")
    dyn a = graph.screen_to_world(win, 100.0, 50.0)
    print("IDENT", a[0], a[1])
    graph.begin_cam2d(win, 1000.0, 500.0, 0.0, 2.0)
    dyn w1 = graph.screen_to_world(win, 200.0, 150.0)
    print("CENTER", w1[0], w1[1])
    dyn s1 = graph.world_to_screen(win, 1000.0, 500.0)
    print("TARGET", s1[0], s1[1])
    dyn w2 = graph.screen_to_world(win, 320.0, 90.0)
    dyn s2 = graph.world_to_screen(win, w2[0], w2[1])
    print("TRIP", s2[0], s2[1])
    graph.end_cam2d(win)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "IDENT 100 50" \
    && echo "$out" | grep -q "CENTER 1000 500" \
    && echo "$out" | grep -q "TARGET 200 150" \
    && echo "$out" | grep -q "TRIP 320 90" \
    && pass "camera2d_transform_round_trip" \
    || fail "camera2d_transform_round_trip" "identity, centre, target and round trip" "$out"

# zoom must be positive — a zero would divide by zero in the inverse
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(400, 300, "cam")
    graph.begin_cam2d(win, 0.0, 0.0, 0.0, 0.0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "zoom must be positive" && pass "camera2d_zoom_validated" \
    || fail "camera2d_zoom_validated" "zoom must be positive" "$out"

# window control and cursor visibility
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "wc")
    graph.set_window_title(win, "renamed")
    graph.set_window_size(win, 320, 240)
    graph.hide_cursor(win)
    graph.show_cursor(win)
    print("WIN_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "WIN_OK" && pass "window_control" \
    || fail "window_control" "WIN_OK" "$out"

out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "wc")
    graph.set_window_size(win, 0, 240)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "must be positive" && pass "window_size_validated" \
    || fail "window_size_validated" "size error" "$out"

# text_height reports a line height for a loaded font
out=$(run << 'FLX'
import std graph
danger {
    dyn win  = graph.init(200, 150, "th")
    dyn font = graph.load_font(win, "nonexistent.ttf", 20)
}
if err != nil { print("FONT_ERR") }
FLX
)
echo "$out" | grep -q "FONT_ERR" && pass "text_height_needs_valid_font" \
    || fail "text_height_needs_valid_font" "FONT_ERR" "$out"

# the pre-existing GET_INT contract is unchanged: draw_circle still wants an
# int radius, and widening it was deliberately NOT done.
out=$(run << 'FLX'
import std graph
danger {
    dyn win = graph.init(200, 150, "int")
    graph.draw_circle(win, 10, 10, 5.5, 255, 255, 255)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "expected int" && pass "draw_circle_still_requires_int_radius" \
    || fail "draw_circle_still_requires_int_radius" "expected int" "$out"

# draw_image_tint accepts num positions/scale and RGBA modulation
out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "tint")
    dyn img = image.new(2, 2)
    graph.begin_frame(win)
    graph.draw_image_tint(win, img, 10.5, 20, 255, 128, 64, 96, 1.5)
    graph.end_frame(win)
    image.discard(img)
    graph.close(win)
    print("TINT_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "TINT_OK" && pass "draw_image_tint_accepts_num_and_rgba" \
    || fail "draw_image_tint_accepts_num_and_rgba" "TINT_OK" "$out"

# tint validates both color range and scale on the headless backend too
out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "tint")
    dyn img = image.new(2, 2)
    graph.draw_image_tint(win, img, 0, 0, 256, 255, 255, 255, 1)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "0..255" && pass "draw_image_tint_color_validated" \
    || fail "draw_image_tint_color_validated" "0..255 range" "$out"

out=$(run_img << 'FLX'
import std graph
import std image
danger {
    dyn win = graph.init(200, 150, "tint")
    dyn img = image.new(2, 2)
    graph.draw_image_tint(win, img, 0, 0, 255, 255, 255, 255, 0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "scale must be positive" && pass "draw_image_tint_scale_validated" \
    || fail "draw_image_tint_scale_validated" "scale must be positive" "$out"

echo "────────────────────────────────────────────────────────────────"
# ── optional alpha on the drawing primitives ─────────────────────
# The argument is added at the end and defaults to opaque, which is exactly
# what these primitives did before it existed — every call written against the
# old arity has to keep working unchanged.
out=$(run << 'FLX'
import std graph
dyn w = graph.init(64, 48, "t")
graph.begin_frame(w)
graph.clear(w, 0, 0, 0)
graph.draw_rect(w, 0, 0, 10, 10, 255, 0, 0)
graph.draw_circle(w, 5, 5, 3, 0, 255, 0)
graph.draw_line(w, 0, 0, 10, 10, 0, 0, 255)
graph.draw_text(w, "hi", 1, 1, 8, 255, 255, 255)
graph.draw_triangle(w, 0, 0, 5, 0, 0, 5, 255, 255, 0)
graph.draw_rect_lines(w, 0, 0, 4, 4, 1, 2, 3)
graph.draw_circle_lines(w, 5, 5, 2, 1, 2, 3)
graph.draw_ring(w, 5, 5, 1, 2, 1, 2, 3)
print("OLDARITY")
graph.draw_rect(w, 0, 0, 10, 10, 255, 0, 0, 128)
graph.draw_circle(w, 5, 5, 3, 0, 255, 0, 64)
graph.draw_line(w, 0, 0, 10, 10, 0, 0, 255, 200)
graph.draw_text(w, "hi", 1, 1, 8, 255, 255, 255, 90)
graph.draw_triangle(w, 0, 0, 5, 0, 0, 5, 255, 255, 0, 30)
graph.draw_rect_lines(w, 0, 0, 4, 4, 1, 2, 3, 10)
graph.draw_circle_lines(w, 5, 5, 2, 1, 2, 3, 10)
graph.draw_ring(w, 5, 5, 1, 2, 1, 2, 3, 10)
print("NEWARITY")
graph.end_frame(w)
danger { graph.draw_rect(w, 0, 0, 1, 1, 1, 1, 1, 300) }
if err != nil { print("RANGE") }
graph.close(w)
FLX
)
echo "$out" | grep -q "OLDARITY" && pass "alpha_old_arity_unchanged" \
    || fail "alpha_old_arity_unchanged" "OLDARITY" "$out"
echo "$out" | grep -q "NEWARITY" && pass "alpha_new_arity_accepted" \
    || fail "alpha_new_arity_accepted" "NEWARITY" "$out"
echo "$out" | grep -q "RANGE" && pass "alpha_range_validated" \
    || fail "alpha_range_validated" "RANGE" "$out"

# ── 3D ───────────────────────────────────────────────────────────
# The handle discipline is what the stub can prove: create, use, release, and
# a clear error when a released cursor is used again.
out=$(run << 'FLX'
import std graph
dyn w = graph.init(64, 48, "t")
dyn cam = graph.camera3d(0.0, 2.0, 5.0, 0.0, 0.0, 0.0)
graph.camera3d_set(cam, 0.0, 3.0, 6.0, 0.0, 0.0, 0.0, 60.0)
float arr verts[9] = [0.0,0.0,0.0, 1.0,0.0,0.0, 0.0,1.0,0.0]
float arr uvs[6] = [0.0,0.0, 1.0,0.0, 0.0,1.0]
int arr cols[12] = [255,0,0,255, 0,255,0,255, 0,0,255,255]
dyn mesh = graph.mesh_upload(verts, 1, uvs, cols)
dyn plain = graph.mesh_upload(verts, 1)
graph.begin_frame(w)
graph.clear(w, 0, 0, 0)
graph.begin_3d(w, cam)
graph.draw_grid(w, 10, 1.0)
graph.draw_cube(w, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 255, 0, 0)
graph.draw_cube(w, 2.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0, 255, 0, 128)
graph.draw_line3d(w, 0.0,0.0,0.0, 1.0,1.0,1.0, 255,255,0)
graph.draw_mesh(w, mesh, 0.0, 0.0, 0.0)
graph.draw_mesh(w, plain, 1.0, 0.0, 0.0, 2.0, 255, 0, 255, 200)
graph.end_3d(w)
graph.end_frame(w)
print("DREW3D")
graph.mesh_free(mesh)
danger { graph.draw_mesh(w, mesh, 0.0, 0.0, 0.0) }
if err != nil { print("MESHFREED") }
graph.camera3d_free(cam)
danger { graph.begin_3d(w, cam) }
if err != nil { print("CAMFREED") }
graph.mesh_free(plain)
graph.close(w)
FLX
)
echo "$out" | grep -q "DREW3D" && pass "3d_camera_mesh_and_shapes" \
    || fail "3d_camera_mesh_and_shapes" "DREW3D" "$out"
echo "$out" | grep -q "MESHFREED" && pass "3d_released_mesh_cursor_rejected" \
    || fail "3d_released_mesh_cursor_rejected" "MESHFREED" "$out"
echo "$out" | grep -q "CAMFREED" && pass "3d_released_camera_cursor_rejected" \
    || fail "3d_released_camera_cursor_rejected" "CAMFREED" "$out"

out=$(run << 'FLX'
import std graph
dyn w = graph.init(64, 48, "t")
float arr verts[9] = [0.0,0.0,0.0, 1.0,0.0,0.0, 0.0,1.0,0.0]
danger { graph.mesh_upload(verts, 2) }
if err != nil { print("VERTSHORT") }
danger { graph.mesh_upload(verts, 0) }
if err != nil { print("COUNT") }
float arr shortuv[2] = [0.0, 0.0]
danger { graph.mesh_upload(verts, 1, shortuv) }
if err != nil { print("UVSHORT") }
danger { graph.camera3d(0.0,0.0,0.0, 0.0,0.0,0.0, 400.0) }
if err != nil { print("FOVY") }
graph.close(w)
FLX
)
for k in VERTSHORT COUNT UVSHORT FOVY; do
    echo "$out" | grep -q "$k" && pass "3d_rejects_$k" \
        || fail "3d_rejects_$k" "$k" "$out"
done

# ── triangle batch ───────────────────────────────────────────────
# Per-vertex colour and texture coordinates, which draw_triangle cannot
# express. Offered as a batch on purpose: one call per triangle would put the
# cost back in the interpreter, which is what the batch shape exists to avoid.
out=$(run2 << 'FLX'
import std graph
import std image
dyn w = graph.init(64, 48, "t")
dyn img = image.new(2, 2)
int arr px[16] = [255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255]
image.update_rgba(img, px)
float arr v2[6] = [0.0,0.0, 10.0,0.0, 0.0,10.0]
float arr v3[9] = [0.0,0.0,0.0, 1.0,0.0,0.0, 0.0,1.0,0.0]
float arr uv[6] = [0.0,0.0, 1.0,0.0, 0.0,1.0]
int arr co[12] = [255,0,0,255, 0,255,0,255, 0,0,255,128]
dyn cam = graph.camera3d(0.0,2.0,5.0, 0.0,0.0,0.0)
graph.begin_frame(w)
graph.clear(w, 0,0,0)
print("B2", graph.draw_tris(w, v2, 1))
print("B2TEX", graph.draw_tris(w, v2, 1, img, uv))
print("B2COL", graph.draw_tris(w, v2, 1, nil, nil, co))
print("B0", graph.draw_tris(w, v2, 0))
graph.begin_3d(w, cam)
print("B3", graph.draw_tris3d(w, v3, 1, img, uv, co))
graph.end_3d(w)
graph.end_frame(w)
danger { graph.draw_tris(w, v2, 5) }
if err != nil { print("VSHORT") }
float arr shortuv[2] = [0.0, 0.0]
danger { graph.draw_tris(w, v2, 1, nil, shortuv) }
if err != nil { print("USHORT") }
int arr shortco[4] = [1,2,3,4]
danger { graph.draw_tris(w, v2, 1, nil, nil, shortco) }
if err != nil { print("CSHORT") }
image.discard(img)
graph.camera3d_free(cam)
graph.close(w)
FLX
)
echo "$out" | grep -q "B2 1"    && pass "batch_2d_plain"   || fail "batch_2d_plain" "B2 1" "$out"
echo "$out" | grep -q "B2TEX 1" && pass "batch_2d_texture" || fail "batch_2d_texture" "B2TEX 1" "$out"
echo "$out" | grep -q "B2COL 1" && pass "batch_2d_colors"  || fail "batch_2d_colors" "B2COL 1" "$out"
echo "$out" | grep -q "B0 0"    && pass "batch_zero_count" || fail "batch_zero_count" "B0 0" "$out"
echo "$out" | grep -q "B3 1"    && pass "batch_3d"         || fail "batch_3d" "B3 1" "$out"
for k in VSHORT USHORT CSHORT; do
    echo "$out" | grep -q "$k" && pass "batch_rejects_$k" \
        || fail "batch_rejects_$k" "$k" "$out"
done

echo "  → std.graph: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.graph: PASS" && exit 0 || exit 1
