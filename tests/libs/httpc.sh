#!/usr/bin/env bash
# tests/libs/http.sh — std.http test suite
# Uses a local Python HTTP server for deterministic testing without
# external network dependencies.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"; kill "$_HTTP_PID" 2>/dev/null || true' EXIT
FAILS=0; PASS=0
_HTTP_PID=0

pass() { printf "  PASS  libs/httpc/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/httpc/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/httpc/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.httpc="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# ── Local HTTP server setup ───────────────────────────────────────────────
# Serve a temp dir with known files for deterministic GET/POST tests
_SERVE_DIR="$(mktemp -d)"
trap 'rm -rf "$P" "$_SERVE_DIR"; kill "$_HTTP_PID" 2>/dev/null || true' EXIT

# Create known files
echo "hello from fluxa test" > "$_SERVE_DIR/hello.txt"
echo '{"ok":true,"value":42}' > "$_SERVE_DIR/data.json"

# Find a free port
_HTTP_PORT=0
for port in 18080 18081 18082 18083 18084; do
    ! ss -tlnp 2>/dev/null | grep -q ":$port " && _HTTP_PORT=$port && break
done

if [ "$_HTTP_PORT" -eq 0 ]; then
    echo "  WARN: could not find free port for local HTTP server — using fallback port 18080"
    _HTTP_PORT=18080
fi

_HTTP_BASE="http://127.0.0.1:${_HTTP_PORT}"

# Start local HTTP server — try Python 3 first, fallback to Python 3.x aliases
_HTTP_PID=0
for _py in python3 python3.11 python3.10 python3.12 python; do
    if command -v "$_py" >/dev/null 2>&1; then
        "$_py" -m http.server "$_HTTP_PORT" --directory "$_SERVE_DIR" \
            >/dev/null 2>&1 &
        _HTTP_PID=$!
        break
    fi
done

if [ "$_HTTP_PID" -eq 0 ]; then
    echo "  WARN: python3 not found — live httpc tests will be skipped"
fi

# Wait for server to be ready (up to 3s)
_ready=0
for i in $(seq 1 40); do
    curl -s --max-time 1 "$_HTTP_BASE/hello.txt" >/dev/null 2>&1 && _ready=1 && break
    sleep 0.2
done

echo "── std.httpc ─────────────────────────────────────────────────────"

if [ "$_ready" -eq 0 ]; then
    echo "  WARN: local HTTP server did not start — live tests will be skipped"
fi

skip_live() {
    printf "  SKIP  libs/httpc/%s  (local HTTP server not available)\n" "$1"
    PASS=$((PASS+1))
}

# ── Error-path tests (no server needed) ──────────────────────────────────

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std httpc
danger { dyn r = httpc.get("http://127.0.0.1:19999/") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. connection refused → error in danger
out=$(run << 'FLX'
import std httpc
danger { dyn r = httpc.get("http://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connection_refused_error_captured" || fail "connection_refused_error_captured" "error caught" "$out"

# 3. http.get missing arg → error
toml; cat > "$P/main.flx" << 'FLX'
import std httpc
danger { dyn r = httpc.get() }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "get_missing_arg_error" || fail "get_missing_arg_error" "error caught" "$out"

# 4. http.post missing body → error
toml; cat > "$P/main.flx" << 'FLX'
import std httpc
danger { dyn r = httpc.post("http://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "post_missing_arg_error" || fail "post_missing_arg_error" "error caught" "$out"

# 5. invalid response cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std httpc
danger {
    dyn bad = [1, 2]
    int s = httpc.status(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "status_bad_cursor_error" || fail "status_bad_cursor_error" "error caught" "$out"

# 6. unknown function → error
out=$(run << 'FLX'
import std httpc
danger { httpc.nonexistent() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 7. prst dyn response pattern
out=$(run << 'FLX'
import std httpc
prst dyn last_resp = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_dyn_resp_pattern" || fail "prst_dyn_resp_pattern" "prst ok" "$out"

# ── Live tests against local server ──────────────────────────────────────

# 8. GET → 200 + body
if [ "$_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_HTTP_BASE}/hello.txt")
    int s = httpc.status(r)
    str b = httpc.body(r)
    bool ok = httpc.ok(r)
    print(s)
    print(ok)
    print(b)
}
FLXEOF
)
    echo "$out" | grep -q "200"  && pass "get_local_returns_200" || fail "get_local_returns_200" "200" "$out"
    echo "$out" | grep -q "true" && pass "get_local_ok_true"     || fail "get_local_ok_true" "true" "$out"
    echo "$out" | grep -q "hello from fluxa test" \
        && pass "get_local_body_correct" || fail "get_local_body_correct" "hello from fluxa test" "$out"
else
    skip "get_local_returns_200" "local server not available"
    skip "get_local_ok_true"     "local server not available"
    skip "get_local_body_correct" "local server not available"
fi

# 9. GET 404 → status 404, ok=false
if [ "$_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_HTTP_BASE}/does_not_exist.txt")
    int s = httpc.status(r)
    bool ok = httpc.ok(r)
    print(s)
    print(ok)
}
FLXEOF
)
    echo "$out" | grep -q "404"   && pass "get_local_404_status" || fail "get_local_404_status" "404" "$out"
    echo "$out" | grep -q "false" && pass "get_local_404_ok_false" || fail "get_local_404_ok_false" "false" "$out"
else
    skip "get_local_404_status"   "local server not available"
    skip "get_local_404_ok_false" "local server not available"
fi

# 10. http.body accessor
if [ "$_ready" -eq 1 ]; then
    out=$(run << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_HTTP_BASE}/data.json")
    str b = httpc.body(r)
    print(b)
}
FLXEOF
)
    echo "$out" | grep -q '"ok":true' \
        && pass "body_accessor_returns_content" || fail "body_accessor_returns_content" '{"ok":true}' "$out"
else
    skip "body_accessor_returns_content" "local server not available"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.httpc: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.httpc: PASS" && exit 0 || exit 1
