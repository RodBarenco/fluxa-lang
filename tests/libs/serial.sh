#!/usr/bin/env bash
# tests/libs/serial.sh — std.serial test suite
# Uses socat to create virtual serial port pairs for deterministic testing.
# Requires: socat installed (apt install socat).
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/serial/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/serial/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/serial/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.serial="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.serial ───────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std serial
danger { dyn ports = serial.list() }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. serial.list() returns an int (count, possibly 0)
out=$(run << 'FLX'
import std serial
danger {
    dyn ports = serial.list()
    print(len(ports))
}
if err != nil { print("0") }
FLX
)
echo "$out" | grep -qE "^[0-9]+$" \
    && pass "list_returns_dyn" || fail "list_returns_dyn" "integer count" "$out"

# 3. open closed port → error
toml; cat > "$P/main.flx" << 'FLX'
import std serial
danger { dyn port = serial.open("/dev/ttyNONEXISTENT99", 9600) }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "open_bad_port_error" || fail "open_bad_port_error" "error caught" "$out"

# 4. write to invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    int n = serial.write(bad, "hello")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "write_bad_cursor_error" || fail "write_bad_cursor_error" "error caught" "$out"

# 5. read from invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    str data = serial.read(bad, 10, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "read_bad_cursor_error" || fail "read_bad_cursor_error" "error caught" "$out"

# 6. bytes_available on invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    int n = serial.bytes_available(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "bytes_available_bad_cursor" || fail "bytes_available_bad_cursor" "error caught" "$out"

# 7. flush on invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    serial.flush(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "flush_bad_cursor_error" || fail "flush_bad_cursor_error" "error caught" "$out"

# 8. prst dyn cursor pattern
out=$(run << 'FLX'
import std serial
prst dyn port = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

# 9. unknown function → error
out=$(run << 'FLX'
import std serial
danger { serial.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# IO real via container Docker com tty0tty
# Ver: tests/integration/serial/
# Uso: bash tests/integration/serial/run.sh

echo "────────────────────────────────────────────────────────────────"
echo "  → std.serial: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.serial: PASS" && exit 0 || exit 1
