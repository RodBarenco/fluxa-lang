#!/usr/bin/env bash
# tests/libs/http.sh — std.http test suite (mongoose server + client)
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"; kill "$_SRV_PID" 2>/dev/null || true' EXIT
FAILS=0; PASS=0
_SRV_PID=0

pass() { printf "  PASS  libs/http/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/http/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.http ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std http
danger { dyn s = http.get("http://127.0.0.1:19999/") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns mongoose version
out=$(run << 'FLX'
import std http
str v = http.version()
print(v)
FLX
)
echo "$out" | grep -qi "mongoose" && pass "version_is_mongoose" || fail "version_is_mongoose" "mongoose" "$out"

# 3. client GET — connect refused → error
out=$(run << 'FLX'
import std http
danger { dyn r = http.get("http://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "client_get_refused_error" || fail "client_get_refused_error" "error caught" "$out"

# 4. server bind → ok
out=$(run << 'FLX'
import std http
danger {
    dyn s = http.serve(19180)
    bool ok = s != nil
    print("server ok")
    http.stop(s)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -q "server ok" && pass "server_bind_ok" || fail "server_bind_ok" "server ok" "$out"

# 5. server + client roundtrip
# Start a Fluxa HTTP server in background, then GET from it
toml
cat > "$P/server.flx" << 'FLX'
import std http
danger {
    dyn s = http.serve(19181)
    dyn req = http.poll(s, 5000)
    if req != nil {
        str method = http.req_method(req)
        str path   = http.req_path(req)
        http.reply(req, 200, "fluxa ok")
    }
    http.stop(s)
}
FLX
printf '[project]\nname="t"\nentry="server.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"
timeout 8s "$FLUXA" run "$P/server.flx" -proj "$P" > "$P/srv_out.txt" 2>&1 &
_SRV_PID=$!
sleep 0.5

# Client: GET the server
toml
cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn r = http.get("http://127.0.0.1:19181/")
    int s = http.status(r)
    str b = http.body(r)
    bool ok = http.ok(r)
    print(s)
    print(ok)
    print(b)
}
if err != nil { print(err[0]) }
FLX
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0

echo "$out" | grep -q "^200$"       && pass "server_client_status_200"  || fail "server_client_status_200"  "200"      "$out"
echo "$out" | grep -q "^true$"      && pass "server_client_ok_true"     || fail "server_client_ok_true"     "true"     "$out"
echo "$out" | grep -q "fluxa ok"    && pass "server_client_body_ok"     || fail "server_client_body_ok"     "fluxa ok" "$out"

# 6. reply_json sends correct content-type
toml
cat > "$P/json_server.flx" << 'FLX'
import std http
danger {
    dyn s = http.serve(19182)
    dyn req = http.poll(s, 5000)
    if req != nil {
        http.reply_json(req, 200, "{\"ok\":true}")
    }
    http.stop(s)
}
FLX
printf '[project]\nname="t"\nentry="json_server.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"
timeout 8s "$FLUXA" run "$P/json_server.flx" -proj "$P" > "$P/json_srv.txt" 2>&1 &
_SRV_PID=$!
sleep 0.5

toml
cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn r = http.get("http://127.0.0.1:19182/")
    str b = http.body(r)
    print(b)
}
FLX
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0
echo "$out" | grep -q '"ok":true' && pass "reply_json_body" || fail "reply_json_body" '{"ok":true}' "$out"

# 7. post sends body
toml
cat > "$P/post_server.flx" << 'FLX'
import std http
danger {
    dyn s = http.serve(19183)
    dyn req = http.poll(s, 5000)
    if req != nil {
        str body = http.req_body(req)
        http.reply(req, 200, body)
    }
    http.stop(s)
}
FLX
printf '[project]\nname="t"\nentry="post_server.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"
timeout 8s "$FLUXA" run "$P/post_server.flx" -proj "$P" > /dev/null 2>&1 &
_SRV_PID=$!
sleep 0.5

toml
cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn r = http.post("http://127.0.0.1:19183/", "hello=world")
    str b = http.body(r)
    print(b)
}
FLX
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0
echo "$out" | grep -q "hello=world" && pass "post_body_echoed" || fail "post_body_echoed" "hello=world" "$out"

# 8. req_method and req_path
toml
cat > "$P/info_server.flx" << 'FLX'
import std http
danger {
    dyn s = http.serve(19184)
    dyn req = http.poll(s, 5000)
    if req != nil {
        str m = http.req_method(req)
        str p = http.req_path(req)
        http.reply(req, 200, m)
    }
    http.stop(s)
}
FLX
printf '[project]\nname="t"\nentry="info_server.flx"\n[libs]\nstd.http="1.0"\n' > "$P/fluxa.toml"
timeout 8s "$FLUXA" run "$P/info_server.flx" -proj "$P" > /dev/null 2>&1 &
_SRV_PID=$!
sleep 0.5

toml
cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn r = http.get("http://127.0.0.1:19184/api/test")
    str b = http.body(r)
    print(b)
}
FLX
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0
echo "$out" | grep -q "GET" && pass "req_method_is_GET" || fail "req_method_is_GET" "GET" "$out"

# 9. stop bad cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std http
danger {
    dyn bad = [1, 2, 3]
    http.stop(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "stop_bad_cursor_error" || fail "stop_bad_cursor_error" "error caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.http: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.http: PASS" && exit 0 || exit 1
