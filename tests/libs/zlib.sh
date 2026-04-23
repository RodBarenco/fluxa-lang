#!/usr/bin/env bash
# tests/libs/zlib.sh — std.zlib test suite
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/zlib/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/zlib/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.zlib="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.zlib ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std zlib
danger { str c = zlib.compress("hello") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a string
out=$(run << 'FLX'
import std zlib
str v = zlib.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "non-empty" "$out"

# 3. compress → decompress roundtrip
out=$(run << 'FLX'
import std zlib
str original = "hello fluxa world, this is a test string for compression!"
str compressed = zlib.compress(original)
str recovered  = zlib.decompress(compressed)
print(recovered)
FLX
)
echo "$out" | grep -q "hello fluxa world" \
    && pass "compress_decompress_roundtrip" || fail "compress_decompress_roundtrip" "hello fluxa world" "$out"

# 4. compressed is shorter than original (for repetitive data)
out=$(run << 'FLX'
import std zlib
str data = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
str comp = zlib.compress(data)
print(len(data))
print(len(comp))
FLX
)
orig_len=$(echo "$out" | head -1)
comp_len=$(echo "$out" | tail -1)
[ "${orig_len:-0}" -gt "${comp_len:-999}" ] \
    && pass "compressed_shorter_than_original" \
    || fail "compressed_shorter_than_original" "orig > comp" "$out"

# 5. gzip → gunzip roundtrip
out=$(run << 'FLX'
import std zlib
str msg = "gzip test message for fluxa"
str gz  = zlib.gzip(msg)
str out = zlib.gunzip(gz)
print(out)
FLX
)
echo "$out" | grep -q "gzip test message" \
    && pass "gzip_gunzip_roundtrip" || fail "gzip_gunzip_roundtrip" "gzip test message" "$out"

# 6. crc32 — same input same result
out=$(run << 'FLX'
import std zlib
int a = zlib.crc32("hello")
int b = zlib.crc32("hello")
int c = zlib.crc32("world")
print(a)
print(b)
print(c)
FLX
)
crc_a=$(echo "$out" | sed -n '1p')
crc_b=$(echo "$out" | sed -n '2p')
crc_c=$(echo "$out" | sed -n '3p')
[ "$crc_a" = "$crc_b" ] && pass "crc32_deterministic" || fail "crc32_deterministic" "same" "$out"
[ "$crc_a" != "$crc_c" ] && pass "crc32_different_inputs" || fail "crc32_different_inputs" "different" "$out"

# 7. adler32 — known value ("hello" → 103547413)
out=$(run << 'FLX'
import std zlib
int a = zlib.adler32("hello")
print(a)
FLX
)
echo "$out" | grep -q "^103547413$" && pass "adler32_known_value" || fail "adler32_known_value" "103547413" "$out"

# 8. ratio()
out=$(run << 'FLX'
import std zlib
float r = zlib.ratio(1000.0, 400.0)
print(r)
FLX
)
echo "$out" | grep -q "^2\.5$" && pass "ratio_correct" || fail "ratio_correct" "2.5" "$out"

# 9. crc32 non-zero for non-empty string
out=$(run << 'FLX'
import std zlib
int c = zlib.crc32("fluxa")
print(c)
FLX
)
val=$(echo "$out" | grep -oE "^[0-9]+")
[ "${val:-0}" -gt 0 ] && pass "crc32_nonzero" || fail "crc32_nonzero" "> 0" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.zlib: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.zlib: PASS" && exit 0 || exit 1
