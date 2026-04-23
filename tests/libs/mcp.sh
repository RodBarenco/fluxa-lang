#!/usr/bin/env bash
# tests/libs/mcp.sh — std.mcp test suite
#
# std.mcp = Fluxa AS an MCP server (JSON-RPC 2.0 over HTTP, mongoose backend)
# API: mcp.serve(port), mcp.poll(server, ms), mcp.stop(server), mcp.version()
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; _SRV_PID=0
trap 'rm -rf "$P"; [ "$_SRV_PID" -gt 0 ] && kill "$_SRV_PID" 2>/dev/null || true' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/mcp/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/mcp/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/mcp/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.mcp="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.mcp ──────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std mcp
danger { dyn s = mcp.serve(19300) }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version is nonempty
out=$(run << 'FLX'
import std mcp
str v = mcp.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. version mentions backend
out=$(run << 'FLX'
import std mcp
str v = mcp.version()
print(v)
FLX
)
echo "$out" | grep -qiE "mongoose|fluxa-mcp" && pass "version_contains_backend" || fail "version_contains_backend" "mongoose" "$out"

# 4. serve returns cursor
out=$(run << 'FLX'
import std mcp
danger {
    dyn s = mcp.serve(19301)
    bool ok = s != nil
    print(ok)
    mcp.stop(s)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "true" && pass "serve_returns_cursor" || fail "serve_returns_cursor" "true" "$out"

# 5. poll returns nil on timeout (no request)
out=$(run << 'FLX'
import std mcp
danger {
    dyn s = mcp.serve(19303)
    dyn req = mcp.poll(s, 100)
    bool is_nil = req == nil
    print(is_nil)
    mcp.stop(s)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "true" && pass "poll_no_request_nil" || fail "poll_no_request_nil" "true" "$out"

# 6. stop bad cursor → error
out=$(run << 'FLX'
import std mcp
danger {
    dyn bad = [1, 2, 3]
    mcp.stop(bad)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "stop_bad_cursor_error" || fail "stop_bad_cursor_error" "error caught" "$out"

# 7. poll bad cursor → error
out=$(run << 'FLX'
import std mcp
danger {
    dyn bad = [1, 2, 3]
    mcp.poll(bad, 100)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "poll_bad_cursor_error" || fail "poll_bad_cursor_error" "error caught" "$out"

# 8. unknown function → error
out=$(run << 'FLX'
import std mcp
danger { mcp.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 9. serve+poll loop pattern
out=$(run << 'FLX'
import std mcp
danger {
    dyn srv = mcp.serve(19304)
    int tick = 0
    while tick < 3 {
        mcp.poll(srv, 50)
        tick = tick + 1
    }
    print("loop ok")
    mcp.stop(srv)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "loop ok" && pass "serve_poll_loop_pattern" || fail "serve_poll_loop_pattern" "loop ok" "$out"

# 10. live: tools/list via JSON-RPC POST (opt-in: FLUXA_TEST_MCP=1)
if [ "${FLUXA_TEST_MCP:-0}" = "1" ]; then
    toml
    cat > "$P/main.flx" << 'FLX'
import std mcp
danger {
    dyn srv = mcp.serve(19305)
    int tick = 0
    while tick < 50 {
        mcp.poll(srv, 100)
        tick = tick + 1
    }
    mcp.stop(srv)
}
FLX
    timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" >/dev/null 2>&1 &
    _SRV_PID=$!; sleep 0.5
    resp=$(curl -s --max-time 3 -X POST \
        -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' \
        http://127.0.0.1:19305/ 2>/dev/null || echo "")
    kill "$_SRV_PID" 2>/dev/null; _SRV_PID=0
    echo "$resp" | grep -q '"tools"' \
        && pass "live_tools_list" || fail "live_tools_list" '"tools" in JSON' "$resp"
else
    skip "live_tools_list" "set FLUXA_TEST_MCP=1 to enable"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.mcp: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.mcp: PASS" && exit 0 || exit 1
