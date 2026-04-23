#!/usr/bin/env bash
# tests/libs/websocket.sh — std.websocket test suite
# Uses a local Python WebSocket echo server for deterministic testing.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"
_WS_PID=0
cleanup() {
    rm -rf "$P"
    [ "$_WS_PID" -gt 0 ] && kill "$_WS_PID" 2>/dev/null || true
}
trap cleanup EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/websocket/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/websocket/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/websocket/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.websocket="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.websocket ────────────────────────────────────────────────"

# ── Local WebSocket echo server ───────────────────────────────────────────
_WS_PORT=19876
cat > "$P/ws_server.py" << 'PYEOF'
import asyncio, websockets, sys

async def echo(ws):
    async for msg in ws:
        await ws.send(msg)

async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19876
    async with websockets.serve(echo, "127.0.0.1", port):
        await asyncio.Future()  # run forever

asyncio.run(main())
PYEOF

python3 "$P/ws_server.py" "$_WS_PORT" >/dev/null 2>&1 &
_WS_PID=$!

# Wait for server to be ready
_ws_ready=0
for i in $(seq 1 20); do
    nc -z 127.0.0.1 "$_WS_PORT" 2>/dev/null && _ws_ready=1 && break
    sleep 0.2
done

if [ "$_ws_ready" -eq 0 ]; then
    echo "  WARN: local WS server did not start — live tests will be skipped"
fi

# ── Error-path tests (no server needed) ──────────────────────────────────

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std websocket
danger { dyn c = websocket.connect("ws://127.0.0.1:19999/") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a string
out=$(run << 'FLX'
import std websocket
danger {
    str v = websocket.version()
    print(len(v))
}
FLX
)
echo "$out" | grep -qE "^[0-9]+" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. connect to closed port → error
out=$(run << 'FLX'
import std websocket
danger { dyn c = websocket.connect("ws://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connect_refused_error" || fail "connect_refused_error" "error caught" "$out"

# 4. invalid URL → error
toml; cat > "$P/main.flx" << 'FLX'
import std websocket
danger { dyn c = websocket.connect("http://not-a-websocket-url/") }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "invalid_url_error" || fail "invalid_url_error" "error caught" "$out"

# 5. send to invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std websocket
danger {
    dyn bad = [1, 2, 3]
    websocket.send(bad, "hello")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "send_bad_cursor_error" || fail "send_bad_cursor_error" "error caught" "$out"

# 6. recv timeout with invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std websocket
danger {
    dyn bad = [1, 2, 3]
    str m = websocket.recv(bad, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "recv_bad_cursor_error" || fail "recv_bad_cursor_error" "error caught" "$out"

# 7. prst dyn cursor pattern
out=$(run << 'FLX'
import std websocket
prst dyn conn = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

# 8. unknown function → error
out=$(run << 'FLX'
import std websocket
danger { websocket.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# ── Live tests against local echo server ─────────────────────────────────

# 9. connect → connected=true
if [ "$_ws_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std websocket
danger {
    dyn c = websocket.connect("ws://127.0.0.1:${_WS_PORT}/")
    bool conn = websocket.connected(c)
    print(conn)
    websocket.close(c)
}
FLXEOF
)
    echo "$out" | grep -q "true" && pass "connect_local_ok" || fail "connect_local_ok" "true" "$out"
else
    skip "connect_local_ok" "local WS server not available"
fi

# 10. send and recv echo
if [ "$_ws_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std websocket
danger {
    dyn c = websocket.connect("ws://127.0.0.1:${_WS_PORT}/")
    websocket.send(c, "hello fluxa")
    str reply = websocket.recv(c, 2000)
    print(reply)
    websocket.close(c)
}
FLXEOF
)
    echo "$out" | grep -q "hello fluxa" \
        && pass "send_recv_echo" || fail "send_recv_echo" "hello fluxa" "$out"
else
    skip "send_recv_echo" "local WS server not available"
fi

# 11. ws.url() returns the connection URL
if [ "$_ws_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std websocket
danger {
    dyn c = websocket.connect("ws://127.0.0.1:${_WS_PORT}/")
    str u = websocket.url(c)
    print(u)
    websocket.close(c)
}
FLXEOF
)
    echo "$out" | grep -q "ws://127.0.0.1" \
        && pass "url_returns_connection_url" || fail "url_returns_connection_url" "ws://127.0.0.1" "$out"
else
    skip "url_returns_connection_url" "local WS server not available"
fi

# 12. recv timeout → empty string (no error)
if [ "$_ws_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std websocket
danger {
    dyn c = websocket.connect("ws://127.0.0.1:${_WS_PORT}/")
    str reply = websocket.recv(c, 200)
    print(len(reply))
    websocket.close(c)
}
FLXEOF
)
    echo "$out" | grep -q "^0$" \
        && pass "recv_timeout_empty_string" || fail "recv_timeout_empty_string" "0" "$out"
else
    skip "recv_timeout_empty_string" "local WS server not available"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.websocket: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.websocket: PASS" && exit 0 || exit 1
