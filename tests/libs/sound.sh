#!/usr/bin/env bash
# tests/libs/sound.sh — std.sound test suite
# Tests the stub backend (the default when miniaudio is not vendored).
# The stub tracks engine/sound state (loaded, playing, paused, volume),
# so play/stop/pause/resume/is_playing logic is fully testable headless.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/sound/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/sound/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.sound="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# dummy audio file — the stub only checks readability
echo "RIFF-dummy" > "$P/beep.wav"

echo "── std.sound ────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std sound
int eng = sound.init()
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a nonempty string
out=$(run << 'FLX'
import std sound
str v = sound.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. init returns handle 1
out=$(run << 'FLX'
import std sound
int eng = sound.init()
print(eng)
sound.close(eng)
FLX
)
echo "$out" | grep -q "^1$" && pass "init_returns_handle" || fail "init_returns_handle" "1" "$out"

# 4. load + play happy path
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    bool ok = sound.play(eng, h)
    print(ok)
    sound.close(eng)
}
FLX
)
echo "$out" | grep -q "true" && pass "load_and_play" || fail "load_and_play" "true" "$out"

# 5. is_playing reflects play
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.play(eng, h)
    print(sound.is_playing(eng, h))
    sound.close(eng)
}
FLX
)
echo "$out" | grep -q "true" && pass "is_playing_after_play" || fail "is_playing_after_play" "true" "$out"

# 6. stop clears playing
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.play(eng, h)
    sound.stop(eng, h)
    print(sound.is_playing(eng, h))
    sound.close(eng)
}
FLX
)
echo "$out" | grep -q "false" && pass "stop_clears_playing" || fail "stop_clears_playing" "false" "$out"

# 7. pause / resume cycle
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.play(eng, h)
    sound.pause(eng, h)
    bool a = sound.is_playing(eng, h)
    sound.resume(eng, h)
    bool b = sound.is_playing(eng, h)
    print(a)
    print(b)
    sound.close(eng)
}
FLX
)
echo "$out" | grep -q "false" && echo "$out" | grep -q "true" \
    && pass "pause_resume_cycle" || fail "pause_resume_cycle" "false then true" "$out"

# 8. load missing file → error captured in danger
out=$(run << 'FLX'
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "/nonexistent/no.wav")
}
if err != nil { print("caught") }
sound.close(eng)
FLX
)
echo "$out" | grep -q "caught" && pass "load_missing_file_danger" || fail "load_missing_file_danger" "caught" "$out"

# 9. invalid sound handle → error captured in danger
out=$(run << 'FLX'
import std sound
int eng = sound.init()
danger {
    sound.play(eng, 99)
}
if err != nil { print("caught") }
sound.close(eng)
FLX
)
echo "$out" | grep -q "caught" && pass "invalid_sound_handle_danger" || fail "invalid_sound_handle_danger" "caught" "$out"

# 10. invalid engine handle outside danger → execution aborts
#     (lib errors outside danger abort silently with rc!=0 — the line
#      after the failing call must never run)
out=$(run << 'FLX'
import std sound
print("before")
sound.play(42, 1)
print("after")
FLX
)
echo "$out" | grep -q "before" && ! echo "$out" | grep -q "after" \
    && pass "invalid_engine_aborts_outside_danger" \
    || fail "invalid_engine_aborts_outside_danger" "before only (abort)" "$out"

# 11. double close → second close is an error (captured in danger)
out=$(run << 'FLX'
import std sound
int eng = sound.init()
sound.close(eng)
danger {
    sound.close(eng)
}
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "double_close_error" || fail "double_close_error" "caught" "$out"

# 12. volume out of range → error captured in danger
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.volume(eng, h, 2.0)
}
if err != nil { print("caught") }
sound.close(eng)
FLX
)
echo "$out" | grep -q "caught" && pass "volume_out_of_range" || fail "volume_out_of_range" "caught" "$out"

# 13. volume accepts int and float in range
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.volume(eng, h, 1)
    sound.volume(eng, h, 0.25)
    print("ok")
    sound.close(eng)
}
FLX
)
echo "$out" | grep -q "^ok$" && pass "volume_int_and_float" || fail "volume_int_and_float" "ok" "$out"

# 14. tone returns true; bad frequency is an error
out=$(run << 'FLX'
import std sound
int eng = sound.init()
print(sound.tone(eng, 440, 50))
danger {
    sound.tone(eng, 99999, 50)
}
if err != nil { print("caught") }
sound.close(eng)
FLX
)
echo "$out" | grep -q "true" && echo "$out" | grep -q "caught" \
    && pass "tone_ok_and_range_check" || fail "tone_ok_and_range_check" "true + caught" "$out"

# 15. unload frees the slot — handle becomes invalid
out=$(run << FLX
import std sound
int eng = sound.init()
danger {
    int h = sound.load(eng, "$P/beep.wav")
    sound.unload(eng, h)
    sound.play(eng, h)
}
if err != nil { print("caught") }
sound.close(eng)
FLX
)
echo "$out" | grep -q "caught" && pass "unload_invalidates_handle" || fail "unload_invalidates_handle" "caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.sound: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.sound: PASS" && exit 0 || exit 1
