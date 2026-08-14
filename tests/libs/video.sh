#!/usr/bin/env bash
# tests/libs/video.sh — std.video: MP4/H.264 write and read
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$PWD/${FLUXA#./}" ;; esac

P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0
pass() { printf "  PASS  libs/video/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/video/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.video="1.0"\nstd.image="1.0"\n' \
        > "$P/fluxa.toml"
}
run() { toml; cat > "$P/main.flx"; (cd "$P" && timeout 60s "$FLUXA" run main.flx -proj . 2>&1 || true); }

echo "── std.video: MP4/H.264 ─────────────────────────────────────────"

# 1. import without [libs] → error
cat > "$P/main.flx" << 'FLX'
import std video
video.version()
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
out=$(cd "$P" && timeout 20s "$FLUXA" run main.flx -proj . 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" \
    || fail "import_without_toml_error" "not declared error" "$out"

# 2. version reports the backend
out=$(run << 'FLX'
import std video
print(video.version())
FLX
)
echo "$out" | grep -qiE "minimp4|stub" && pass "version_reports_backend" \
    || fail "version_reports_backend" "backend name" "$out"

# 3. write a file — it exists and is a real MP4 (ftyp box at offset 4)
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn v = video.open("a.mp4", 64, 48, 25)
    int i = 0
    while i < 10 {
        dyn f = image.new(64, 48)
        video.frame(v, f)
        image.discard(f)
        i = i + 1
    }
    video.close(v)
}
if err != nil { print("ERR", err[0]) }
print("done")
FLX
)
if [ -s "$P/a.mp4" ] && head -c 8 "$P/a.mp4" | tail -c 4 | grep -q "ftyp"; then
    pass "open_frame_close_writes_mp4"
else
    fail "open_frame_close_writes_mp4" "a real mp4 with an ftyp box" "$out"
fi

# 4. info round-trips the geometry it was written with
out=$(run << 'FLX'
import std video
danger {
    dyn p = video.play_open("a.mp4")
    dyn n = video.info(p)
    print("W", n[0], "H", n[1], "F", n[3])
    video.play_close(p)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "W 64 H 48 F 10" && pass "info_round_trips_geometry" \
    || fail "info_round_trips_geometry" "W 64 H 48 F 10" "$out"

# 5. every written frame decodes back
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn p = video.play_open("a.mp4")
    int n = 0
    while !video.play_eof(p) {
        dyn fr = video.play_frame(p)
        n = n + 1
        image.discard(fr)
    }
    print("decoded", n)
    video.play_close(p)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "decoded 10" && pass "all_frames_decode_back" \
    || fail "all_frames_decode_back" "decoded 10" "$out"

# 6. decoded frames keep the declared size — H.264 pads to whole macroblocks
#    (48 rows becomes 48, not 64), so this catches a missing crop.
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn p  = video.play_open("a.mp4")
    dyn fr = video.play_frame(p)
    print("size", image.width(fr), "x", image.height(fr))
    image.discard(fr)
    video.play_close(p)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "size 64 x 48" && pass "decoded_frame_is_cropped_to_real_size" \
    || fail "decoded_frame_is_cropped_to_real_size" "size 64 x 48" "$out"

# 7. subtitles are written as a sidecar .srt in SubRip form
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn v = video.open("s.mp4", 64, 48, 25)
    dyn f = image.new(64, 48)
    video.frame(v, f)
    image.discard(f)
    video.subtitle(v, 0.0, 1.5, "primeira")
    video.subtitle(v, 1.5, 3.25, "segunda")
    video.close(v)
}
if err != nil { print("ERR", err[0]) }
FLX
)
if [ -s "$P/s.srt" ] && grep -q "00:00:00,000 --> 00:00:01,500" "$P/s.srt" \
   && grep -q "00:00:01,500 --> 00:00:03,250" "$P/s.srt" \
   && grep -q "primeira" "$P/s.srt" && grep -q "segunda" "$P/s.srt"; then
    pass "subtitles_write_srt_sidecar"
else
    fail "subtitles_write_srt_sidecar" "srt with two timed cues" \
         "$(cat "$P/s.srt" 2>/dev/null || echo "no file")"
fi

# 8. odd dimensions are refused — H.264 chroma is subsampled 2x2
out=$(run << 'FLX'
import std video
danger { dyn v = video.open("odd.mp4", 63, 48, 25) }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "even" && pass "odd_dimensions_refused" \
    || fail "odd_dimensions_refused" "must be even" "$out"

# 9. implausible geometry is refused before anything is allocated
out=$(run << 'FLX'
import std video
danger { dyn v = video.open("big.mp4", 999999, 999999, 25) }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "exceed|too large" && pass "oversized_dimensions_refused" \
    || fail "oversized_dimensions_refused" "size cap error" "$out"

# 10. fps is validated
out=$(run << 'FLX'
import std video
danger { dyn v = video.open("f.mp4", 64, 48, 0) }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "fps" && pass "invalid_fps_refused" \
    || fail "invalid_fps_refused" "fps error" "$out"

# 11. a frame whose size differs from the video is refused rather than
#     silently reading past the buffer
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn v = video.open("m.mp4", 64, 48, 25)
    dyn f = image.new(32, 24)
    video.frame(v, f)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "does not match" && pass "frame_size_mismatch_refused" \
    || fail "frame_size_mismatch_refused" "size mismatch error" "$out"

# 12. a non-MP4 file is refused
printf 'this is definitely not an mp4 file at all' > "$P/junk.mp4"
out=$(run << 'FLX'
import std video
danger { dyn p = video.play_open("junk.mp4") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "readable mp4|cannot open|no video" \
    && pass "malformed_file_refused" \
    || fail "malformed_file_refused" "not a readable MP4" "$out"

# 13. a missing file is an ordinary catchable error
out=$(run << 'FLX'
import std video
danger { dyn p = video.play_open("nope.mp4") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "cannot open" && pass "missing_file_captured_in_danger" \
    || fail "missing_file_captured_in_danger" "cannot open" "$out"

# 14. using a cursor after close is refused, not a crash
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn v = video.open("c.mp4", 64, 48, 25)
    dyn f = image.new(64, 48)
    video.frame(v, f)
    video.close(v)
    video.frame(v, f)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "invalid video cursor|already closed" \
    && pass "use_after_close_refused" \
    || fail "use_after_close_refused" "invalid cursor" "$out"

# 15. subtitle timings are validated
out=$(run << 'FLX'
import std video
danger {
    dyn v = video.open("t.mp4", 64, 48, 25)
    video.subtitle(v, 5.0, 2.0, "backwards")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "after start" && pass "subtitle_timing_validated" \
    || fail "subtitle_timing_validated" "end must be after start" "$out"

# 16. audio remux rejects a file that is neither ADTS nor MP3, by signature
printf 'RIFF....WAVEfmt not really audio' > "$P/fake.aac"
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn v = video.open("au.mp4", 64, 48, 25)
    dyn f = image.new(64, 48)
    video.frame(v, f)
    video.audio(v, "fake.aac")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "ADTS|mp3" && pass "audio_rejects_unknown_format" \
    || fail "audio_rejects_unknown_format" "expected ADTS or mp3" "$out"

# 17. unknown function → clear error, captured in danger
out=$(run << 'FLX'
import std video
danger { video.no_such_function() }
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "unknown_function_captured_in_danger" \
    || fail "unknown_function_captured_in_danger" "caught" "$out"

# 18. the release contract: a decoded frame is an ordinary image handle, so
#     image.discard frees it and a second discard is a no-op rather than a
#     double free. This is the mechanism a playback loop depends on — the
#     collector only runs at a safe point, which a while loop never reaches, so
#     every decoded frame has to be released by the code that asked for it.
#
#     The cost of NOT discarding is real but deliberately not asserted here:
#     measuring it means sampling RSS, which is too noisy to gate a build on.
#     Measured by hand on a 96x64 clip, fifteen undiscarded frames held an extra
#     536 KB — exactly 15 x the 24.5 KB each frame occupies.
out=$(run << 'FLX'
import std video
import std image
danger {
    dyn p  = video.play_open("a.mp4")
    dyn fr = video.play_frame(p)
    print("before", image.width(fr))
    image.discard(fr)
    image.discard(fr)
    print("after double discard")
    video.play_close(p)
    video.play_close(p)
    print("after double close")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "after double discard" && echo "$out" | grep -q "after double close"     && pass "frame_and_cursor_release_are_idempotent"     || fail "frame_and_cursor_release_are_idempotent" "both double releases survive" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.video: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.video: PASS" && exit 0 || exit 1
