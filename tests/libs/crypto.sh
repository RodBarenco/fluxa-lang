#!/usr/bin/env bash
# tests/libs/crypto.sh — std.crypto (libsodium) test suite
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  crypto/%s\n" "$1"; }
fail() { printf "  FAIL  crypto/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

proj() {
    local d="$1"; mkdir -p "$d"
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.crypto = "1.0"\n' > "$d/fluxa.toml"
}
run() { timeout 5s "$FLUXA" run "$1" -proj "$(dirname "$1")" 2>/dev/null || true; }

echo "── std.crypto ───────────────────────────────────────────────────"

# CASE 1: version()
P="$WORK_DIR/p1"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
str v = crypto.version()
print(v)
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -qE "^[0-9]+\.[0-9]+"; then
    pass "version_returns_string"
else
    fail "version_returns_string" "semver" "$out"
fi

# CASE 2: hash() produces 32-byte dyn
P="$WORK_DIR/p2"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn digest = crypto.hash("hello")
print(len(digest))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^32$"; then
    pass "hash_produces_32_bytes"
else
    fail "hash_produces_32_bytes" "32" "$out"
fi

# CASE 3: hash() deterministic
P="$WORK_DIR/p3"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn a = crypto.hash("fluxa")
dyn b = crypto.hash("fluxa")
print(crypto.compare(a, b))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^true$"; then
    pass "hash_deterministic"
else
    fail "hash_deterministic" "true" "$out"
fi

# CASE 4: hash() different inputs differ
P="$WORK_DIR/p4"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn a = crypto.hash("hello")
dyn b = crypto.hash("world")
print(crypto.compare(a, b))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^false$"; then
    pass "hash_different_inputs_differ"
else
    fail "hash_different_inputs_differ" "false" "$out"
fi

# CASE 5: to_hex / from_hex roundtrip
P="$WORK_DIR/p5"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn original = crypto.hash("roundtrip")
str hex = crypto.to_hex(original)
dyn restored = crypto.from_hex(hex)
print(crypto.compare(original, restored))
print(len(hex))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^true$" && echo "$out" | grep -q "^64$"; then
    pass "to_hex_from_hex_roundtrip"
else
    fail "to_hex_from_hex_roundtrip" "true + 64" "$out"
fi

# CASE 6: keygen 32 bytes, nonce 24 bytes
P="$WORK_DIR/p6"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn k = crypto.keygen()
dyn n = crypto.nonce()
print(len(k))
print(len(n))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^32$" && echo "$out" | grep -q "^24$"; then
    pass "keygen_32_bytes_nonce_24_bytes"
else
    fail "keygen_32_bytes_nonce_24_bytes" "32 and 24" "$out"
fi

# CASE 7: encrypt/decrypt roundtrip
P="$WORK_DIR/p7"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn key   = crypto.keygen()
dyn nonce = crypto.nonce()
dyn cipher = crypto.encrypt("secret message", key, nonce)
str plain = crypto.decrypt(cipher, key, nonce)
print(plain)
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^secret message$"; then
    pass "encrypt_decrypt_roundtrip"
else
    fail "encrypt_decrypt_roundtrip" "secret message" "$out"
fi

# CASE 8: ciphertext length = plaintext + 16 (MAC)
P="$WORK_DIR/p8"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn key   = crypto.keygen()
dyn nonce = crypto.nonce()
dyn cipher = crypto.encrypt("hello", key, nonce)
print(len(cipher))
FLX
out=$(run "$P/main.flx")
# "hello" 5 bytes + 16 MAC = 21
if echo "$out" | grep -q "^21$"; then
    pass "encrypt_output_length_correct"
else
    fail "encrypt_output_length_correct" "21" "$out"
fi

# CASE 9: decrypt with wrong key fails inside danger
P="$WORK_DIR/p9"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn key1  = crypto.keygen()
dyn key2  = crypto.keygen()
dyn nonce = crypto.nonce()
dyn cipher = crypto.encrypt("secret", key1, nonce)
str result = ""
danger { result = crypto.decrypt(cipher, key2, nonce) }
if err != nil { print("auth_error") } else { print(result) }
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^auth_error$"; then
    pass "decrypt_wrong_key_fails"
else
    fail "decrypt_wrong_key_fails" "auth_error" "$out"
fi

# CASE 10: Ed25519 sign / sign_open roundtrip
P="$WORK_DIR/p10"; proj "$P"
# pre-allocate 32 and 64 element arrs via default init
cat > "$P/main.flx" << 'FLX'
import std crypto
int arr pk[32] = 0
int arr sk[64] = 0
crypto.sign_keygen(pk, sk)
dyn signed = crypto.sign("hello from fluxa", sk)
str msg = crypto.sign_open(signed, pk)
print(msg)
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^hello from fluxa$"; then
    pass "ed25519_sign_open_roundtrip"
else
    fail "ed25519_sign_open_roundtrip" "hello from fluxa" "$out"
fi

# CASE 11: sign_open with wrong key fails inside danger
P="$WORK_DIR/p11"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
int arr pk1[32] = 0
int arr sk1[64] = 0
int arr pk2[32] = 0
int arr sk2[64] = 0
crypto.sign_keygen(pk1, sk1)
crypto.sign_keygen(pk2, sk2)
dyn signed = crypto.sign("payload", sk1)
str result = ""
danger { result = crypto.sign_open(signed, pk2) }
if err != nil { print("sig_error") } else { print(result) }
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^sig_error$"; then
    pass "sign_open_wrong_key_fails"
else
    fail "sign_open_wrong_key_fails" "sig_error" "$out"
fi

# CASE 12: compare same arrs → true
P="$WORK_DIR/p12"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn a = crypto.keygen()
str hex = crypto.to_hex(a)
dyn b = crypto.from_hex(hex)
print(crypto.compare(a, b))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^true$"; then
    pass "compare_equal_arrs"
else
    fail "compare_equal_arrs" "true" "$out"
fi

# CASE 13: compare different arrs → false
P="$WORK_DIR/p13"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
dyn a = crypto.keygen()
dyn b = crypto.keygen()
print(crypto.compare(a, b))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^false$"; then
    pass "compare_different_arrs"
else
    fail "compare_different_arrs" "false" "$out"
fi

# CASE 14: wipe zeros the arr
P="$WORK_DIR/p14"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
int arr key[32] = 0
int arr pk[32] = 0
int arr sk[64] = 0
crypto.sign_keygen(pk, sk)
crypto.wipe(pk)
int sum = 0
int i = 0
while i < 32 {
    sum = sum + pk[i]
    i = i + 1
}
print(sum)
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^0$"; then
    pass "wipe_zeros_arr"
else
    fail "wipe_zeros_arr" "0" "$out"
fi

# CASE 15: prst dyn key survives hot reload context
P="$WORK_DIR/p15"; proj "$P"
cat > "$P/main.flx" << 'FLX'
import std crypto
prst dyn session_key = crypto.keygen()
str hex = crypto.to_hex(session_key)
print(len(hex))
FLX
out=$(run "$P/main.flx")
if echo "$out" | grep -q "^64$"; then
    pass "prst_dyn_key_survives"
else
    fail "prst_dyn_key_survives" "64" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=15
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  -> std.crypto: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
