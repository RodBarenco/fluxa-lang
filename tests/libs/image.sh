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

echo "  → std.image: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.image: PASS" && exit 0 || exit 1
