#!/usr/bin/env bash
# tests/libs/i2c.sh — std.i2c test suite
# Physical I2C tests are skipped when /dev/i2c-* is absent.
# Error-path and API-shape tests run on any Linux system.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/i2c/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/i2c/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/i2c/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.i2c="1.0"\n' > "$P/fluxa.toml"; }
run() {
    toml; cat > "$P/main.flx"
    timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

echo "── std.i2c ──────────────────────────────────────────────────────"

# 1. import without toml → clear error
cat > "$P/main.flx" << 'FLX'
import std i2c
dyn bus = i2c.open("/dev/i2c-1", 72)
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared error" "$out"

# 2. open non-existent device → error captured
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bus = i2c.open("/dev/i2c-99", 72)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "open_bad_device_error" || fail "open_bad_device_error" "error caught" "$out"

# 3. write to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bad = [1, 2, 3]
    int arr data[2] = [1, 0]
    int n = i2c.write(bad, data)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "write_bad_cursor_error" || fail "write_bad_cursor_error" "error caught" "$out"

# 4. read from invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bad = [1, 2, 3]
    dyn data = i2c.read(bad, 2)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "read_bad_cursor_error" || fail "read_bad_cursor_error" "error caught" "$out"

# 5. write_reg to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bad = [1, 2, 3]
    i2c.write_reg(bad, 16, 255)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "write_reg_bad_cursor_error" || fail "write_reg_bad_cursor_error" "error caught" "$out"

# 6. read_reg from invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bad = [1, 2, 3]
    int v = i2c.read_reg(bad, 16)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "read_reg_bad_cursor_error" || fail "read_reg_bad_cursor_error" "error caught" "$out"

# 7. read_reg16 from invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn bad = [1, 2, 3]
    int v = i2c.read_reg16(bad, 16)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "read_reg16_bad_cursor_error" || fail "read_reg16_bad_cursor_error" "error caught" "$out"

# 8. scan non-existent device → error
toml
cat > "$P/main.flx" << 'FLX'
import std i2c
danger {
    dyn found = i2c.scan("/dev/i2c-99")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "scan_bad_device_error" || fail "scan_bad_device_error" "error caught" "$out"

# 9. prst dyn bus cursor pattern
out=$(run << 'FLX'
import std i2c
prst dyn bus_handle = [0]
danger {
    dyn b = i2c.open("/dev/i2c-0", 72)
}
print("pattern ok")
FLX
)
echo "$out" | grep -q "pattern ok" && pass "prst_cursor_pattern_compiles" || fail "prst_cursor_pattern_compiles" "pattern ok" "$out"

# 10. physical device test — only if FLUXA_TEST_I2C=1 and /dev/i2c-1 exists
if [ "${FLUXA_TEST_I2C:-0}" = "1" ] && [ -e /dev/i2c-1 ]; then
    out=$(run << 'FLX'
import std i2c
danger {
    dyn found = i2c.scan("/dev/i2c-1")
    print(len(found))
}
FLX
)
    echo "$out" | grep -qE "^[0-9]+$" && pass "scan_real_bus_returns_list" || fail "scan_real_bus_returns_list" "integer" "$out"
else
    skip "scan_real_bus_returns_list" "set FLUXA_TEST_I2C=1 with /dev/i2c-1 to enable"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.i2c: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.i2c: PASS"
[ "$FAILS" -eq 0 ] && exit 0 || exit 1
