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

# Security regression tests for get_text. These require the real codec/zlib path
# because the stub intentionally stops at the no-codec gate.
if echo "$version_out" | grep -qi "raylib codec"; then
    # Helper: construct a minimal structurally valid PNG and insert one custom
    # iTXt chunk immediately before IEND.

    # 39. CRC mismatch is rejected; corrupted metadata is never returned.
    python3 - "$P/bad_crc.png" << 'PYPNG'
import base64, struct, sys
png = bytearray(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
iend = png.rfind(b'IEND') - 4
data = b'proof\0\0\0\0\0trusted-looking'
chunk = struct.pack('>I', len(data)) + b'iTXt' + data + b'\x00\x00\x00\x00'  # deliberately wrong CRC
png[iend:iend] = chunk
open(sys.argv[1], 'wb').write(png)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/bad_crc.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "CRC" && pass "get_text_rejects_bad_crc" || fail "get_text_rejects_bad_crc" "CRC mismatch" "$out"

    # 40. Invalid UTF-8 is rejected instead of entering a Fluxa str.
    python3 - "$P/bad_utf8.png" << 'PYPNG'
import base64, binascii, struct, sys
png = bytearray(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
iend = png.rfind(b'IEND') - 4
data = b'proof\0\0\0\0\0' + b'\xff\xfe'
body = b'iTXt' + data
chunk = struct.pack('>I', len(data)) + body + struct.pack('>I', binascii.crc32(body) & 0xffffffff)
png[iend:iend] = chunk
open(sys.argv[1], 'wb').write(png)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/bad_utf8.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "UTF-8" && pass "get_text_rejects_invalid_utf8" || fail "get_text_rejects_invalid_utf8" "UTF-8 error" "$out"

    # 41. A tiny compressed payload that expands beyond 1 MiB is stopped.
    python3 - "$P/zlib_bomb.png" << 'PYPNG'
import base64, binascii, struct, sys, zlib
png = bytearray(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
iend = png.rfind(b'IEND') - 4
payload = zlib.compress(b'A' * (1024 * 1024 + 1), 9)
data = b'proof\0\1\0\0\0' + payload
body = b'iTXt' + data
chunk = struct.pack('>I', len(data)) + body + struct.pack('>I', binascii.crc32(body) & 0xffffffff)
png[iend:iend] = chunk
open(sys.argv[1], 'wb').write(png)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/zlib_bomb.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "exceeds limit" && pass "get_text_blocks_zlib_bomb" || fail "get_text_blocks_zlib_bomb" "decompressed text exceeds limit" "$out"

    # 42. Oversized iTXt is rejected before allocating its declared payload.
    python3 - "$P/huge_itxt.png" << 'PYPNG'
import base64, struct, sys
png = bytearray(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
iend = png.rfind(b'IEND') - 4
# 1 MiB + 1 byte data, valid enough to reach the explicit iTXt size gate.
data = b'proof\0\0\0\0\0' + (b'A' * (1024 * 1024 - 8))
import binascii
body = b'iTXt' + data
chunk = struct.pack('>I', len(data)) + body + struct.pack('>I', binascii.crc32(body) & 0xffffffff)
png[iend:iend] = chunk
open(sys.argv[1], 'wb').write(png)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/huge_itxt.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "iTXt chunk exceeds limit" && pass "get_text_rejects_oversized_itxt" || fail "get_text_rejects_oversized_itxt" "iTXt chunk exceeds limit" "$out"

    # 43. A hostile 32-bit chunk length is rejected by size arithmetic before
    # any allocation/read; this also guards the LLP64 Win64 long-width pitfall.
    python3 - "$P/hostile_length.png" << 'PYPNG'
import struct, sys
sig = b'\x89PNG\r\n\x1a\n'
# Valid IHDR first, then a chunk claiming 32 MiB but providing no payload.
import binascii
ihdr_data = struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)
ihdr_body = b'IHDR' + ihdr_data
ihdr = struct.pack('>I', 13) + ihdr_body + struct.pack('>I', binascii.crc32(ihdr_body) & 0xffffffff)
hostile = struct.pack('>I', 32 * 1024 * 1024) + b'iTXt'
open(sys.argv[1], 'wb').write(sig + ihdr + hostile)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/hostile_length.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "scan limit\|iTXt chunk exceeds limit" && pass "get_text_rejects_hostile_chunk_length" || fail "get_text_rejects_hostile_chunk_length" "bounded chunk-length error" "$out"

    # 44. Even after finding the first matching key, corruption later in the PNG
    # still invalidates the file; the function does not bless a partial parse.
    python3 - "$P/corrupt_after_match.png" << 'PYPNG'
import base64, binascii, struct, sys
png = bytearray(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
iend = png.rfind(b'IEND') - 4
data = b'proof\0\0\0\0\0first'
body = b'iTXt' + data
good = struct.pack('>I', len(data)) + body + struct.pack('>I', binascii.crc32(body) & 0xffffffff)
bad_data = b'other\0\0\0\0\0bad'
bad = struct.pack('>I', len(bad_data)) + b'iTXt' + bad_data + b'\x00\x00\x00\x00'
png[iend:iend] = good + bad
open(sys.argv[1], 'wb').write(png)
PYPNG
    out=$(run << FLX
import std image
danger { str t = image.get_text("$P/corrupt_after_match.png", "proof") }
if err != nil { print(err[0]) }
FLX
)
    echo "$out" | grep -qi "CRC" && pass "get_text_validates_full_png" || fail "get_text_validates_full_png" "CRC mismatch after first match" "$out"
fi

# update_rgba replaces the complete buffer and accepts all byte endpoints
out=$(run << 'FLX'
import std image
dyn im = image.new(2, 1)
int arr pixels[8] = [255, 0, 128, 255, 1, 2, 3, 0]
image.update_rgba(im, pixels)
print("UPDATED", image.width(im), image.height(im))
image.discard(im)
FLX
)
echo "$out" | grep -q "UPDATED 2 1" && pass "update_rgba_replaces_pixels" \
    || fail "update_rgba_replaces_pixels" "UPDATED 2 1" "$out"

# size mismatch is rejected before the image is modified
out=$(run << 'FLX'
import std image
dyn im = image.new(2, 1)
int arr pixels[4] = [255, 0, 0, 255]
danger { image.update_rgba(im, pixels) }
if err != nil { print(err[0]) }
image.discard(im)
FLX
)
echo "$out" | grep -qi "expected 8 components" && pass "update_rgba_size_validated" \
    || fail "update_rgba_size_validated" "expected 8 components" "$out"

# components outside the RGBA byte range are rejected
out=$(run << 'FLX'
import std image
dyn im = image.new(1, 1)
int arr pixels[4] = [0, 256, 0, 255]
danger { image.update_rgba(im, pixels) }
if err != nil { print(err[0]) }
image.discard(im)
FLX
)
echo "$out" | grep -qi "0..255" && pass "update_rgba_range_validated" \
    || fail "update_rgba_range_validated" "0..255 range" "$out"

# ── update_rgba_rect ─────────────────────────────────────────────
# Same rules as update_rgba over a named rectangle: inside the image, exactly
# w*h*4 components, every one an int in 0..255, and nothing written unless all
# of them are accepted.
out=$(run << 'FLX'
import std image
dyn im = image.new(4, 2)
int arr full[32] = 0
image.update_rgba(im, full)
int arr px[8] = [9,9,9,255, 8,8,8,255]
image.update_rgba_rect(im, px, 1, 0, 2, 1)
print("RECT ok")
danger { image.update_rgba_rect(im, px, 3, 0, 2, 1) }
if err != nil { print("outside") }
danger { image.update_rgba_rect(im, px, 0, 0, 1, 1) }
if err != nil { print("size") }
int arr bad[8] = [1,2,3,4, 5,6,999,8]
danger { image.update_rgba_rect(im, bad, 0, 0, 2, 1) }
if err != nil { print("range") }
image.discard(im)
FLX
)
echo "$out" | grep -q "RECT ok" && pass "update_rgba_rect_writes" \
    || fail "update_rgba_rect_writes" "RECT ok" "$out"
echo "$out" | grep -q "outside" && pass "update_rgba_rect_bounds" \
    || fail "update_rgba_rect_bounds" "outside" "$out"
echo "$out" | grep -q "size" && pass "update_rgba_rect_size" \
    || fail "update_rgba_rect_size" "size" "$out"
echo "$out" | grep -q "range" && pass "update_rgba_rect_range" \
    || fail "update_rgba_rect_range" "range" "$out"

# ── fill_tris ────────────────────────────────────────────────────
# The rules a caller depends on bit for bit: which winding is a front face,
# that depth keeps the larger z by default, that a degenerate triangle draws
# nothing, and that the depth-write threshold keeps a translucent texel from
# hiding what is behind it.
out=$(run << 'FLX'
import std image
dyn im = image.new(8, 8)
int arr dep[64] = 0
// screen y grows downward, so a positive signed area is the back face
int arr back[15]  = [0,0,10,0,0,  7,0,10,0,0,  0,7,10,0,0]
int arr front[15] = [0,0,10,0,0,  0,7,10,0,0,  7,0,10,0,0]
print("FRONTFLAG", image.fill_tris(im, nil, front, 1, nil,0,0,0, 255, 1, 255),
      image.fill_tris(im, nil, back, 1, nil,0,0,0, 255, 1, 255))
print("BACKFLAG", image.fill_tris(im, nil, back, 1, nil,0,0,0, 255, 2, 255),
      image.fill_tris(im, nil, front, 1, nil,0,0,0, 255, 2, 255))
print("NOFLAG", image.fill_tris(im, nil, front, 1, nil,0,0,0, 255, 0, 255))
int arr degen[15] = [0,0,1,0,0,  0,0,1,0,0,  0,0,1,0,0]
print("DEGEN", image.fill_tris(im, nil, degen, 1, nil,0,0,0, 255, 3, 255))
// depth: larger z wins, so the same shape drawn deeper writes nothing
int arr near[15] = [0,0,20,0,0,  0,7,20,0,0,  7,0,20,0,0]
int arr far[15]  = [0,0,5,0,0,   0,7,5,0,0,   7,0,5,0,0]
print("DEPTH", image.fill_tris(im, dep, near, 1, nil,0,0,0, 255, 1, 255),
      image.fill_tris(im, dep, far, 1, nil,0,0,0, 255, 1, 255))
image.discard(im)
FLX
)
echo "$out" | grep -q "FRONTFLAG 36 0" && pass "fill_tris_front_winding" \
    || fail "fill_tris_front_winding" "FRONTFLAG 36 0" "$out"
echo "$out" | grep -q "BACKFLAG 36 0" && pass "fill_tris_back_winding" \
    || fail "fill_tris_back_winding" "BACKFLAG 36 0" "$out"
echo "$out" | grep -q "NOFLAG 0" && pass "fill_tris_no_face_flag_draws_nothing" \
    || fail "fill_tris_no_face_flag_draws_nothing" "NOFLAG 0" "$out"
echo "$out" | grep -q "DEGEN 0" && pass "fill_tris_degenerate_skipped" \
    || fail "fill_tris_degenerate_skipped" "DEGEN 0" "$out"
echo "$out" | grep -q "DEPTH 36 0" && pass "fill_tris_depth_keeps_larger_z" \
    || fail "fill_tris_depth_keeps_larger_z" "DEPTH 36 0" "$out"

# The texel's alpha multiplies the argument, and depth is written only above
# the threshold in the high byte of flags — a translucent texel must not hide
# what is behind it.
out=$(run << 'FLX'
import std image
dyn im = image.new(2, 1)
int arr bg[8] = [0,0,0,255, 0,0,0,255]
image.update_rgba(im, bg)
int arr opaque[4] = [200, 100, 50, 255]
int arr half[4]   = [0, 0, 0, 128]
int arr t[15] = [0,0,50,0,0,  0,1,50,0,0,  2,0,50,0,0]
int arr dep[2] = 0
int thr = 1 + 51200            // FRONT, depth-write threshold 200
print("TRANS", image.fill_tris(im, dep, t, 1, half, 1,1,1, 255, thr), dep[0])
print("OPAQUE", image.fill_tris(im, dep, t, 1, opaque, 1,1,1, 255, thr), dep[0])
image.discard(im)
FLX
)
echo "$out" | grep -q "TRANS 2 0" && pass "fill_tris_translucent_skips_depth" \
    || fail "fill_tris_translucent_skips_depth" "TRANS 2 0" "$out"
echo "$out" | grep -q "OPAQUE 2 50" && pass "fill_tris_opaque_writes_depth" \
    || fail "fill_tris_opaque_writes_depth" "OPAQUE 2 50" "$out"

# Sizes and handles are refused before anything is drawn, so the destination
# survives a rejected call intact.
out=$(run << 'FLX'
import std image
dyn im = image.new(4, 4)
int arr t[15] = [0,0,1,0,0,  0,3,1,0,0,  3,0,1,0,0]
int arr shortd[4] = 0
danger { image.fill_tris(im, shortd, t, 1, nil,0,0,0, 255, 1) }
if err != nil { print("depthsize") }
danger { image.fill_tris(im, nil, t, 2, nil,0,0,0, 255, 1) }
if err != nil { print("trissize") }
int arr tex[4] = [1,2,3,4]
danger { image.fill_tris(im, nil, t, 1, tex, 4,4,4, 255, 1) }
if err != nil { print("texsize") }
danger { image.fill_tris(im, nil, t, 1, nil,0,0,0, 300, 1) }
if err != nil { print("alpha") }
image.discard(im)
FLX
)
for k in depthsize trissize texsize alpha; do
    echo "$out" | grep -q "$k" && pass "fill_tris_rejects_$k" \
        || fail "fill_tris_rejects_$k" "$k" "$out"
done

# tex_at: where the texture starts inside the array, so one array can hold
# several. There is no pixel readback in the API, so the proof runs through
# alpha instead: a fully transparent texel writes nothing, an opaque one writes
# every pixel, and the same call with only the offset changed returns both.
out=$(run << 'FLX'
import std image
int arr atlas[8] = [9,9,9,0,  9,9,9,255]
int arr t[15] = [0,0,10,0,0,  0,1,10,0,0,  2,0,10,0,0]
dyn im = image.new(2, 1)
print("AT0", image.fill_tris(im, nil, t, 1, atlas, 1,1,1, 255, 1))
print("AT4", image.fill_tris(im, nil, t, 1, atlas, 1,1,1, 255, 1, 16777215, 4))
print("EXACT", image.fill_tris(im, nil, t, 1, atlas, 1,1,1, 255, 1, 16777215, 4))
danger { image.fill_tris(im, nil, t, 1, atlas, 1,1,1, 255, 1, 16777215, 0-1) }
if err != nil { print("NEG") }
danger { image.fill_tris(im, nil, t, 1, atlas, 1,1,1, 255, 1, 16777215, 5) }
if err != nil { print("PAST") }
image.discard(im)
FLX
)
echo "$out" | grep -q "AT0 0" && pass "fill_tris_tex_at_defaults_to_zero" \
    || fail "fill_tris_tex_at_defaults_to_zero" "AT0 0" "$out"
echo "$out" | grep -q "AT4 2" && pass "fill_tris_tex_at_selects_texel" \
    || fail "fill_tris_tex_at_selects_texel" "AT4 2" "$out"
echo "$out" | grep -q "EXACT 2" && pass "fill_tris_tex_at_exact_fit_accepted" \
    || fail "fill_tris_tex_at_exact_fit_accepted" "EXACT 2" "$out"
echo "$out" | grep -q "NEG" && pass "fill_tris_tex_at_rejects_negative" \
    || fail "fill_tris_tex_at_rejects_negative" "NEG" "$out"
echo "$out" | grep -q "PAST" && pass "fill_tris_tex_at_rejects_past_end" \
    || fail "fill_tris_tex_at_rejects_past_end" "PAST" "$out"

# ── fill_rect and fill_tri ───────────────────────────────────────
out=$(run << 'FLX'
import std image
dyn im = image.new(8, 8)
print("RECT", image.fill_rect(im, 1, 1, 3, 2, 16711680))
print("CLIP", image.fill_rect(im, 6, 6, 10, 10, 255))
print("ALPHA0", image.fill_rect(im, 0, 0, 4, 4, 255, 0))
print("TRI", image.fill_tri(im, 0, 0, 7, 0, 0, 7, 255))
print("TRIREV", image.fill_tri(im, 0, 0, 0, 7, 7, 0, 255))
print("TRIDEGEN", image.fill_tri(im, 1, 1, 1, 1, 1, 1, 255))
image.discard(im)
FLX
)
echo "$out" | grep -q "RECT 6" && pass "fill_rect_area" \
    || fail "fill_rect_area" "RECT 6" "$out"
echo "$out" | grep -q "CLIP 4" && pass "fill_rect_clips_to_image" \
    || fail "fill_rect_clips_to_image" "CLIP 4" "$out"
echo "$out" | grep -q "ALPHA0 0" && pass "fill_rect_alpha_zero_draws_nothing" \
    || fail "fill_rect_alpha_zero_draws_nothing" "ALPHA0 0" "$out"
# fill_tri draws either winding — a lone 2D triangle should not have to know
# about the face rules fill_tris needs for depth-sorted geometry
echo "$out" | grep -q "TRI 36" && pass "fill_tri_draws" \
    || fail "fill_tri_draws" "TRI 36" "$out"
echo "$out" | grep -q "TRIREV 36" && pass "fill_tri_either_winding" \
    || fail "fill_tri_either_winding" "TRIREV 36" "$out"
echo "$out" | grep -q "TRIDEGEN 0" && pass "fill_tri_degenerate_skipped" \
    || fail "fill_tri_degenerate_skipped" "TRIDEGEN 0" "$out"

echo "  → std.image: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.image: PASS" && exit 0 || exit 1
