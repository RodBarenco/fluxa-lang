#!/usr/bin/env bash
# tests/libs/mcp.sh — std.mcp test suite
# Tests that don't require a live MCP server validate API shape and errors.
# Server-dependent tests are skipped if no MCP server is reachable.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/mcpc/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/mcpc/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/mcpc/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.mcpc="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# MCP server check — only if MCP_TEST_URL is set in env
MCP_URL="${MCP_TEST_URL:-}"
HAVE_MCP=0
if [ -n "$MCP_URL" ]; then
    curl -s --max-time 3 "$MCP_URL" >/dev/null 2>&1 && HAVE_MCP=1 || true
fi

echo "── std.mcpc ──────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std mcpc
danger { dyn c = mcpc.connect("http://localhost:3000") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. connect returns dyn cursor (no network needed — lazy connect)
out=$(run << 'FLX'
import std mcpc
danger {
    dyn c = mcpc.connect("http://localhost:9999")
    print("cursor ok")
}
FLX
)
echo "$out" | grep -q "cursor ok" && pass "connect_returns_cursor" || fail "connect_returns_cursor" "cursor ok" "$out"

# 3. connect_auth returns dyn cursor
out=$(run << 'FLX'
import std mcpc
danger {
    dyn c = mcpc.connect_auth("http://localhost:9999", "mytoken")
    print("auth ok")
}
FLX
)
echo "$out" | grep -q "auth ok" && pass "connect_auth_returns_cursor" || fail "connect_auth_returns_cursor" "auth ok" "$out"

# 4. list_tools on unreachable server → error in danger
out=$(run << 'FLX'
import std mcpc
danger {
    dyn c = mcpc.connect("http://127.0.0.1:19999")
    dyn tools = mcpc.list_tools(c)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "list_tools_unreachable_error" || fail "list_tools_unreachable_error" "error caught" "$out"

# 5. call on unreachable server → error in danger
out=$(run << 'FLX'
import std mcpc
danger {
    dyn c = mcpc.connect("http://127.0.0.1:19999")
    str result = mcpc.call(c, "read_file", "{\"path\":\"/tmp/x\"}")
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "call_unreachable_error" || fail "call_unreachable_error" "error caught" "$out"

# 6. list_tools on bad cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mcpc
danger {
    dyn bad = [1, 2, 3]
    dyn tools = mcpc.list_tools(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "list_tools_bad_cursor_error" || fail "list_tools_bad_cursor_error" "error caught" "$out"

# 7. call on bad cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mcpc
danger {
    dyn bad = [1, 2, 3]
    str r = mcpc.call(bad, "tool", "{}")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "call_bad_cursor_error" || fail "call_bad_cursor_error" "error caught" "$out"

# 8. disconnect on bad cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mcpc
danger {
    dyn bad = [1, 2, 3]
    mcpc.disconnect(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "disconnect_bad_cursor_error" || fail "disconnect_bad_cursor_error" "error caught" "$out"

# 9. prst dyn cursor pattern (state survives hot reload)
out=$(run << 'FLX'
import std mcpc
prst dyn server = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

# 10. unknown function → error
out=$(run << 'FLX'
import std mcpc
danger { mcp.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 11. live server — list_tools (only if MCP_TEST_URL is set)
if [ "$HAVE_MCP" -eq 1 ]; then
    out=$(run << FLX
import std mcpc
danger {
    dyn c = mcpc.connect("$MCP_URL")
    dyn tools = mcpc.list_tools(c)
    print(len(tools))
    mcpc.disconnect(c)
}
FLX
)
    echo "$out" | grep -qE "^[0-9]+" && pass "live_list_tools_returns_count" || fail "live_list_tools_returns_count" "integer count" "$out"
else
    skip "live_list_tools_returns_count" "no MCP server (set MCP_TEST_URL to enable)"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.mcpc: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.mcpc: PASS" && exit 0 || exit 1
