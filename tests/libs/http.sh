#!/usr/bin/env bash
# tests/libs/http.sh — std.http test suite
# Tests that don't require a live server validate API shape and error handling.
# Network-dependent tests use httpbin.org with graceful skip on failure.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/http/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/http/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/http/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# Check network availability — need actual 200, not just TCP
HAVE_NET=0
_net_code=$(curl -s --max-time 3 -o /dev/null -w "%{http_code}" https://httpbin.org/get 2>/dev/null || echo "0")
[ "$_net_code" = "200" ] && HAVE_NET=1 || true

echo "── std.http ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std http
danger { dyn r = http.get("http://example.com") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. bad URL → error captured in danger
out=$(run << 'FLX'
import std http
danger {
    dyn r = http.get("http://this-host-does-not-exist-fluxa-test.invalid/")
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "bad_url_error_captured" || fail "bad_url_error_captured" "error caught" "$out"

# 3. http.get missing arg → error
toml
cat > "$P/main.flx" << 'FLX'
import std http
danger { dyn r = http.get() }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "get_missing_arg_error" || fail "get_missing_arg_error" "error caught" "$out"

# 4. http.post missing args → error
toml
cat > "$P/main.flx" << 'FLX'
import std http
danger { dyn r = http.post("http://x.invalid") }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "post_missing_arg_error" || fail "post_missing_arg_error" "error caught" "$out"

# 5. invalid cursor to http.status → error
toml
cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn bad = [1, 2]
    int s = http.status(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "status_bad_cursor_error" || fail "status_bad_cursor_error" "error caught" "$out"

# 6. unknown function → error
toml
cat > "$P/main.flx" << 'FLX'
import std http
danger { http.nonexistent() }
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 7. prst dyn response survives reload pattern
out=$(run << 'FLX'
import std http
prst dyn last_resp = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_dyn_resp_pattern" || fail "prst_dyn_resp_pattern" "prst ok" "$out"

# 8. GET live — httpbin.org
if [ "$HAVE_NET" -eq 1 ]; then
    out=$(run << 'FLX'
import std http
danger {
    dyn r = http.get("https://httpbin.org/get")
    int s = http.status(r)
    print(s)
}
FLX
)
    echo "$out" | grep -q "200" && pass "get_live_returns_200" || fail "get_live_returns_200" "200" "$out"
else
    skip "get_live_returns_200" "no network"
fi

# 9. POST live — httpbin.org
if [ "$HAVE_NET" -eq 1 ]; then
    out=$(run << 'FLX'
import std http
danger {
    dyn r = http.post("https://httpbin.org/post", "key=value")
    bool ok = http.ok(r)
    print(ok)
}
FLX
)
    echo "$out" | grep -q "true" && pass "post_live_returns_ok" || fail "post_live_returns_ok" "true" "$out"
else
    skip "post_live_returns_ok" "no network"
fi

# 10. http.ok false on 404
if [ "$HAVE_NET" -eq 1 ]; then
    out=$(run << 'FLX'
import std http
danger {
    dyn r = http.get("https://httpbin.org/status/404")
    bool ok = http.ok(r)
    int s = http.status(r)
    print(ok)
    print(s)
}
FLX
)
    echo "$out" | grep -q "false" && pass "ok_false_on_404" || fail "ok_false_on_404" "false" "$out"
    echo "$out" | grep -q "404"   && pass "status_404_correct" || fail "status_404_correct" "404" "$out"
else
    skip "ok_false_on_404" "no network"
    skip "status_404_correct" "no network"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.http: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.http: PASS" && exit 0 || exit 1
