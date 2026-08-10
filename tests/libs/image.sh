#!/usr/bin/env bash
# tests/libs/image.sh — std.image test suite
# Tests the stub backend (the default when the raylib codec is not vendored).
# The buffer transforms that need no codec — new, width, height, resize, discard
# — run for real; save/load assert their "no codec" and validation errors.
# With FLUXA_IMAGE_RAYLIB=1 the same API additionally encodes/decodes real files.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/image/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/image/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.image="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.image ────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std image
dyn im = image.new(10, 10)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a non-empty string
out=$(run << 'FLX'
import std image
str v = image.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. new returns a handle with the requested size
out=$(run << 'FLX'
import std image
dyn im = image.new(64, 48)
print("w", image.width(im))
print("h", image.height(im))
image.discard(im)
FLX
)
echo "$out" | grep -q "w 64" && echo "$out" | grep -q "h 48" \
    && pass "new_size" || fail "new_size" "w 64 / h 48" "$out"

# 4. new with non-positive dimensions → error
out=$(run << 'FLX'
import std image
danger { dyn im = image.new(0, 100) }
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "new_zero_error" || fail "new_zero_error" "caught" "$out"

# 5. resize changes the dimensions in place
out=$(run << 'FLX'
import std image
dyn im = image.new(100, 80)
image.resize(im, 50, 40)
print("w", image.width(im))
print("h", image.height(im))
image.discard(im)
FLX
)
echo "$out" | grep -q "w 50" && echo "$out" | grep -q "h 40" \
    && pass "resize_in_place" || fail "resize_in_place" "w 50 / h 40" "$out"

# 6. resize upscales too
out=$(run << 'FLX'
import std image
dyn im = image.new(32, 32)
image.resize(im, 128, 96)
print("w", image.width(im))
print("h", image.height(im))
image.discard(im)
FLX
)
echo "$out" | grep -q "w 128" && echo "$out" | grep -q "h 96" \
    && pass "resize_upscale" || fail "resize_upscale" "w 128 / h 96" "$out"

# 7. resize with a bad size → error
out=$(run << 'FLX'
import std image
dyn im = image.new(20, 20)
danger { image.resize(im, -5, 10) }
if err != nil { print("caught") }
image.discard(im)
FLX
)
echo "$out" | grep -q "caught" && pass "resize_bad_size_error" || fail "resize_bad_size_error" "caught" "$out"

# 8. save with no file extension → clear error (validated before codec)
out=$(run << 'FLX'
import std image
dyn im = image.new(16, 16)
danger { image.save(im, "noext") }
if err != nil { print(err[0]) }
image.discard(im)
FLX
)
echo "$out" | grep -qi "extension" && pass "save_no_extension_error" || fail "save_no_extension_error" "extension" "$out"

# 9. save with an unsupported extension → clear error
out=$(run << 'FLX'
import std image
dyn im = image.new(16, 16)
danger { image.save(im, "bad.xyz") }
if err != nil { print(err[0]) }
image.discard(im)
FLX
)
echo "$out" | grep -qi "unsupported" && pass "save_bad_format_error" || fail "save_bad_format_error" "unsupported" "$out"

# 10. save recognizes the known extensions (png/jpg/bmp/tga/qoi)
#     stub reports "no codec" AFTER accepting the extension → proves it's recognized
out=$(run << 'FLX'
import std image
dyn im = image.new(8, 8)
danger { image.save(im, "shot.png") }
if err != nil { print(err[0]) }
image.discard(im)
FLX
)
if echo "$out" | grep -qi "codec"; then
    pass "save_png_recognized"
elif echo "$out" | grep -qi "unsupported\|extension"; then
    fail "save_png_recognized" "png recognized (codec-level msg)" "$out"
else
    # a raylib build actually wrote the file → no error at all is also a pass
    pass "save_png_recognized"
fi

# 11. discard is idempotent (double discard does not crash)
out=$(run << 'FLX'
import std image
dyn im = image.new(10, 10)
image.discard(im)
image.discard(im)
print("survived")
FLX
)
echo "$out" | grep -q "survived" && pass "discard_idempotent" || fail "discard_idempotent" "survived" "$out"

# 12. using a discarded handle → clean error, not a crash
out=$(run << 'FLX'
import std image
dyn im = image.new(10, 10)
image.discard(im)
danger { int w = image.width(im) }
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "use_after_discard_error" || fail "use_after_discard_error" "caught" "$out"

# 13. width/height on a fresh buffer are exactly what was requested (no off-by-one)
out=$(run << 'FLX'
import std image
dyn im = image.new(1, 1)
print("w", image.width(im))
print("h", image.height(im))
image.discard(im)
FLX
)
echo "$out" | grep -q "w 1" && echo "$out" | grep -q "h 1" \
    && pass "one_by_one" || fail "one_by_one" "w 1 / h 1" "$out"

# 14. unknown function → error
out=$(run << 'FLX'
import std image
danger { dyn im = image.frobnicate(1) }
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "unknown_fn_error" || fail "unknown_fn_error" "caught" "$out"

# 15. load on the stub → clear "no codec" error (raylib build would decode)
out=$(run << 'FLX'
import std image
danger { dyn im = image.load("missing.png") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "codec|could not|decode" && pass "load_reports_error" || fail "load_reports_error" "codec/decode" "$out"

# 16. blit composes without a mask (pure RGBA — works on stub)
out=$(run << 'FLX'
import std image
dyn canvas = image.new(420, 456)
dyn frame = image.new(400, 300)
image.blit(canvas, frame, 10, 40)
print("w", image.width(canvas))
image.discard(canvas)
image.discard(frame)
print("done")
FLX
)
echo "$out" | grep -q "w 420" && echo "$out" | grep -q "done" \
    && pass "blit_no_mask" || fail "blit_no_mask" "w 420 / done" "$out"

# 17. blit with a mask (optional 5th arg) composes and clips by mask
out=$(run << 'FLX'
import std image
dyn canvas = image.new(420, 456)
dyn frame = image.new(400, 300)
dyn mask = image.new(400, 300)
image.blit(canvas, frame, 10, 40, mask)
print("masked ok")
image.discard(canvas)
image.discard(frame)
image.discard(mask)
FLX
)
echo "$out" | grep -q "masked ok" && pass "blit_with_mask" || fail "blit_with_mask" "masked ok" "$out"

# 18. blit clips out-of-bounds source instead of crashing
out=$(run << 'FLX'
import std image
dyn canvas = image.new(100, 100)
dyn frame = image.new(80, 80)
image.blit(canvas, frame, 60, 60)
print("clipped ok")
image.discard(canvas)
image.discard(frame)
FLX
)
echo "$out" | grep -q "clipped ok" && pass "blit_clips_oob" || fail "blit_clips_oob" "clipped ok" "$out"

# 19. blit with a mismatched-size mask → clean error
out=$(run << 'FLX'
import std image
dyn canvas = image.new(200, 200)
dyn frame = image.new(80, 80)
dyn mask = image.new(50, 50)
danger { image.blit(canvas, frame, 0, 0, mask) }
if err != nil { print("caught") }
image.discard(canvas)
image.discard(frame)
image.discard(mask)
FLX
)
echo "$out" | grep -q "caught" && pass "blit_mask_size_error" || fail "blit_mask_size_error" "caught" "$out"

# 20. set_text signature: key length validated before codec (short key → error)
out=$(run << 'FLX'
import std image
danger { bool ok = image.set_text("card.png", "", "text") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "1.79|key|character" && pass "set_text_key_validated" || fail "set_text_key_validated" "key length msg" "$out"

# 21. set_text on the stub → clear "no codec" error (raylib build embeds iTXt)
out=$(run << 'FLX'
import std image
danger { bool ok = image.set_text("card.png", "starfight-proof", "B2:abc") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "codec" && pass "set_text_reports_no_codec" || fail "set_text_reports_no_codec" "codec" "$out"

# 22. set_text accepts the optional 4th (compress) argument without a parse/arity error
out=$(run << 'FLX'
import std image
danger { bool ok = image.set_text("card.png", "proof", "B2:abc", 1) }
if err != nil { print(err[0]) }
FLX
)
# stub still reports no-codec, but NOT an arity error → proves the 4th arg is accepted
echo "$out" | grep -qi "codec" && pass "set_text_optional_compress_arg" || fail "set_text_optional_compress_arg" "codec (arg accepted)" "$out"

# ── sload: secure load with pre-decode validation ────────────────────────
# These checks (size, magic bytes) run BEFORE the codec, so they hold in the
# stub build. A valid PNG/QOI passes validation and reaches the "no codec"
# message; hostile inputs are rejected before any decode.

# 23. sload rejects an empty file
printf '' > "$P/empty.png"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/empty.png") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "empty" && pass "sload_rejects_empty" || fail "sload_rejects_empty" "empty file" "$out"

# 24. sload rejects an unexpected format (JPEG magic, not PNG/QOI)
printf '\xFF\xD8\xFF\xE0\x00\x10JFIF' > "$P/evil.jpg"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/evil.jpg") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "unsupported\|unexpected" && pass "sload_rejects_bad_format" || fail "sload_rejects_bad_format" "unsupported format" "$out"

# 25. sload rejects a file too short to identify
printf 'ab' > "$P/tiny.bin"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/tiny.bin") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "too short\|unsupported" && pass "sload_rejects_too_short" || fail "sload_rejects_too_short" "too short" "$out"

# 26. sload rejects a missing file
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/does_not_exist.png") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "could not open" && pass "sload_rejects_missing" || fail "sload_rejects_missing" "could not open" "$out"

# 27. sload accepts a valid PNG signature (passes validation → stub reports no codec)
printf '\x89\x50\x4E\x47\x0D\x0A\x1A\x0A\x00\x00\x00\x0D' > "$P/valid.png"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/valid.png") }
if err != nil { print(err[0]) }
FLX
)
# validation passed (no size/format error); stub then reports no codec
echo "$out" | grep -qi "codec" && pass "sload_accepts_valid_png" || fail "sload_accepts_valid_png" "codec (validation passed)" "$out"

# 28. sload accepts a valid QOI signature (passes validation → stub reports no codec)
printf 'qoif\x00\x00\x04\x00\x00\x00\x04\x00' > "$P/valid.qoi"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/valid.qoi") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "codec" && pass "sload_accepts_valid_qoi" || fail "sload_accepts_valid_qoi" "codec (validation passed)" "$out"

# 29. sload with a tightened max_bytes rejects a file that would pass the default.
#     A 100 KB PNG passes the 24 MB default but not a 50 KB caller limit.
printf '\x89\x50\x4E\x47\x0D\x0A\x1A\x0A' > "$P/big.png"
head -c 100000 /dev/zero >> "$P/big.png"
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/big.png", 50000) }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "too large" && pass "sload_tight_max_bytes_rejects" || fail "sload_tight_max_bytes_rejects" "file too large" "$out"

# 30. sload with a generous caller limit accepts the same file (passes → no codec)
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/big.png", 200000, 1200) }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "codec" && pass "sload_tight_limits_accept" || fail "sload_tight_limits_accept" "codec (within limits)" "$out"

# 31. sload with a non-int limit is a clear error
out=$(run << FLX
import std image
danger { dyn im = image.sload("$P/valid.qoi", "big") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "max_bytes must be int" && pass "sload_bad_limit_type" || fail "sload_bad_limit_type" "max_bytes must be int" "$out"

# Real-codec-only fixtures for iTXt readback. The default stub suite above still
# verifies argument validation and the required no-codec error path.
version_out=$(run << 'FLX'
import std image
print(image.version())
FLX
)
if echo "$version_out" | grep -qi "raylib codec"; then
    # A tiny valid PNG; metadata operations do not decode its pixels.
    python3 - "$P/card.png" << 'PYPNG'
import base64, sys
png = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
)
open(sys.argv[1], "wb").write(png)
PYPNG

    # 32. uncompressed iTXt round-trip
    out=$(run << FLX
import std image
danger {
    image.set_text("$P/card.png", "proof", "Olá Fluxa ✓")
    str t = image.get_text("$P/card.png", "proof")
    print(t)
}
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -q "Olá Fluxa ✓" && pass "get_text_uncompressed_itxt" || fail "get_text_uncompressed_itxt" "Olá Fluxa ✓" "$out"

    # 33. compressed iTXt round-trip (set_text 4th arg → compressionFlag=1)
    out=$(run << FLX
import std image
danger {
    image.set_text("$P/card.png", "compressed", "B2:compressed-payload", 1)
    str t = image.get_text("$P/card.png", "compressed")
    print(t)
}
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -q "B2:compressed-payload" && pass "get_text_deflated_itxt" || fail "get_text_deflated_itxt" "B2:compressed-payload" "$out"

    # 34. missing keyword returns the empty string
    out=$(run << FLX
import std image
danger {
    str t = image.get_text("$P/card.png", "does-not-exist")
    print("len", len(t))
}
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -q "len 0" && pass "get_text_missing_returns_empty" || fail "get_text_missing_returns_empty" "len 0" "$out"

    # 35. duplicate keyword returns the first chunk, not the most recent one
    cp "$P/card.png" "$P/dupe.png"
    out=$(run << FLX
import std image
danger {
    image.set_text("$P/dupe.png", "duplicate", "first")
    image.set_text("$P/dupe.png", "duplicate", "second")
    str t = image.get_text("$P/dupe.png", "duplicate")
    print(t)
}
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -q "first" && ! echo "$out" | grep -q "second" \
        && pass "get_text_duplicate_returns_first" || fail "get_text_duplicate_returns_first" "first" "$out"

    # 36. non-PNG input is rejected cleanly
    printf 'not a png' > "$P/not.png"
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/not.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "not a PNG\|too small" && pass "get_text_rejects_non_png" || fail "get_text_rejects_non_png" "PNG error" "$out"
fi

# 37. get_text key length is validated before codec
out=$(run << 'FLX'
import std image
danger { str t = image.get_text("card.png", "") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "1.79|key|character" && pass "get_text_key_validated" || fail "get_text_key_validated" "key length msg" "$out"

# 38. get_text on the stub → clear "no codec" error
out=$(run << 'FLX'
import std image
danger { str t = image.get_text("card.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "codec" && pass "get_text_reports_no_codec" || fail "get_text_reports_no_codec" "codec" "$out"

echo "  → std.image: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.image: PASS" && exit 0 || exit 1
