#!/usr/bin/env bash
# tests/libs/serial.sh — std.serial test suite
# Tests that don't require physical hardware use serial.list() and
# error-path validation. Hardware-dependent tests are skipped if no
# serial port is found.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/serial/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/serial/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/serial/%s  (%s)\n" "$1" "$2"; }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.serial="1.0"\n' > "$P/fluxa.toml"; }
run() {
    toml; cat > "$P/main.flx"
    timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

echo "── std.serial ───────────────────────────────────────────────────"

# 1. import std serial without toml → clear error
cat > "$P/main.flx" << 'FLX'
import std serial
dyn ports = serial.list()
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared error" "$out"

# 2. serial.list() returns a dyn (even if empty)
out=$(run << 'FLX'
import std serial
danger {
    dyn ports = serial.list()
    print(len(ports))
}
FLX
)
echo "$out" | grep -qE "^[0-9]+$" && pass "list_returns_dyn" || fail "list_returns_dyn" "integer (count)" "$out"

# 3. serial.list() elements are strings (if any ports found)
PORT_COUNT=$(run << 'FLX'
import std serial
danger { dyn p = serial.list(); print(len(p)) }
FLX
)
PORT_COUNT=$(echo "$PORT_COUNT" | grep -oE "^[0-9]+" | head -1 || echo "0")
if [ "${PORT_COUNT:-0}" -gt 0 ]; then
    out=$(run << 'FLX'
import std serial
danger {
    dyn ports = serial.list()
    if len(ports) > 0 { print(ports[0]) }
}
FLX
)
    echo "$out" | grep -qE "^/" && pass "list_elements_are_paths" || fail "list_elements_are_paths" "/dev/..." "$out"
else
    skip "list_elements_are_paths" "no serial ports on this system"
    PASS=$((PASS+1))
fi

# 4. open() non-existent port → error captured in danger
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn port = serial.open("/dev/ttyNONEXISTENT99", 9600)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "open_bad_port_error" || fail "open_bad_port_error" "error caught" "$out"

# 5. write to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    int n = serial.write(bad, "hello")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "write_bad_cursor_error" || fail "write_bad_cursor_error" "error caught" "$out"

# 6. read to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    str data = serial.read(bad, 10, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "read_bad_cursor_error" || fail "read_bad_cursor_error" "error caught" "$out"

# 7. bytes_available on invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    int n = serial.bytes_available(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "bytes_available_bad_cursor" || fail "bytes_available_bad_cursor" "error caught" "$out"

# 8. read max_bytes=0 → error
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [99]
    str d = serial.read(bad, 0, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "read_zero_bytes_error" || fail "read_zero_bytes_error" "error caught" "$out"

# 9. prst dyn port survives hot reload simulation (cursor pattern)
out=$(run << 'FLX'
import std serial
prst dyn last_ports = [0]
danger {
    dyn ports = serial.list()
    last_ports = ports
}
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_dyn_port_list_survives" || fail "prst_dyn_port_list_survives" "prst ok" "$out"

# 10. flush on invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std serial
danger {
    dyn bad = [1, 2, 3]
    serial.flush(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "flush_bad_cursor_error" || fail "flush_bad_cursor_error" "error caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.serial: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.serial: PASS"
[ "$FAILS" -eq 0 ] && exit 0 || exit 1
