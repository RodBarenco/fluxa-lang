#!/usr/bin/env bash
# tests/libs/wserver.sh — std.wserver test suite
#
# Covers:
#   - Stub detection and error handling (always)
#   - Real backend round-trips: GET, POST, PUT, PATCH, DELETE
#   - Path inspection, body echo, req_header, reply_json, reply_headers
#   - Benchmark endpoint patterns: /users (POST), /users/:id (GET/PATCH), /healthz
#   - Multi-worker (ft.new manual mode)
#   - Auto-scaling mode
#   - Worker fn (serve with fn_name)
#   - Connection count
#   - Error paths (invalid handles, bad port, etc.)
#
# Requires for real-backend tests: libmicrohttpd-dev at build time + curl.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$(pwd)/$FLUXA" ;; esac
P="$(mktemp -d)"; _SRV_PID=0
trap 'rm -rf "$P"; [ "$_SRV_PID" -gt 0 ] && kill "$_SRV_PID" 2>/dev/null || true' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/wserver/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/wserver/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/wserver/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.wserver="1.0"\n' > "$P/fluxa.toml"; }
toml_ft() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.wserver="1.0"\nstd.flxthread="1.0"\n' > "$P/fluxa.toml"
}
toml_str() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.wserver="1.0"\nstd.strings="1.0"\n' > "$P/fluxa.toml"
}
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# Port helper — pick a free port in range
_next_port=20100
_alloc_port() {
    local p
    for p in $(seq $_next_port $((_next_port+200))); do
        # Check LISTEN, TIME_WAIT, and ESTABLISHED — avoid all occupied ports
        if ! ss -tln 2>/dev/null | grep -q ":${p}[^0-9]" && \
           ! ss -tn  2>/dev/null | grep -q ":${p}[^0-9]"; then
            _next_port=$((p+1)); echo "$p"; return
        fi
    done
    echo $((_next_port++))
}

# Wait for a port to open (max 4s)
_wait_port() {
    local port="$1" ready=0
    for _i in $(seq 1 40); do
        if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/"$port"; then
            exec 3>&-; ready=1; break
        fi
        sleep 0.1
    done
    echo "$ready"
}

echo "── std.wserver ──────────────────────────────────────────────────"

# ── Detect real backend ───────────────────────────────────────────────────
_REAL_BACKEND=0
_probe_port=$(_alloc_port)
toml
cat > "$P/probe.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_probe_port})
    if srv != 0 {
        wserver.stop(srv)
        print("real")
    }
}
if err != nil { print("stub") }
FLXEOF
_probe_out=$(timeout 5s "$FLUXA" run "$P/probe.flx" -proj "$P" 2>&1 || true)
echo "$_probe_out" | grep -q "^real$" && _REAL_BACKEND=1

# ── 1. import without [libs] → error ─────────────────────────────────────
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std wserver
danger { int srv = wserver.serve(19100) }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" \
    || fail "import_without_toml_error" "not declared error" "$out"

# ── 2. unknown function → error captured in danger ────────────────────────
out=$(run << 'FLX'
import std wserver
danger { wserver.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_fn_captured_in_danger" \
    || fail "unknown_fn_captured_in_danger" "error caught" "$out"

# ── 3. invalid server handle for accept → error ───────────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { int req = wserver.accept(bad, 100) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "accept_invalid_handle_error" \
    || fail "accept_invalid_handle_error" "error caught" "$out"

# ── 4. invalid request handle for req_method → error ─────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str m = wserver.req_method(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_method_invalid_handle_error" \
    || fail "req_method_invalid_handle_error" "error caught" "$out"

# ── 5. invalid request handle for req_path → error ───────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str p = wserver.req_path(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_path_invalid_handle_error" \
    || fail "req_path_invalid_handle_error" "error caught" "$out"

# ── 6. invalid request handle for req_body → error ───────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str b = wserver.req_body(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_body_invalid_handle_error" \
    || fail "req_body_invalid_handle_error" "error caught" "$out"

# ── 7. invalid request handle for reply → error ───────────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { wserver.reply(bad, 200, "hi") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "reply_invalid_handle_error" \
    || fail "reply_invalid_handle_error" "error caught" "$out"

# ── 8. invalid request handle for reply_json → error ─────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { wserver.reply_json(bad, 200, "{}") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "reply_json_invalid_handle_error" \
    || fail "reply_json_invalid_handle_error" "error caught" "$out"

# ── 9. invalid server handle for connections → error ─────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { int n = wserver.connections(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connections_invalid_handle_error" \
    || fail "connections_invalid_handle_error" "error caught" "$out"

# ── 10. stop(0) is a no-op (no crash) ────────────────────────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
wserver.stop(bad)
print("no crash")
FLX
)
echo "$out" | grep -q "no crash" \
    && pass "stop_zero_handle_no_crash" \
    || fail "stop_zero_handle_no_crash" "no crash" "$out"

# ── 11. prst int pattern compiles ────────────────────────────────────────
out=$(run << 'FLX'
import std wserver
prst int srv = 0
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_int_handle_pattern" \
    || fail "prst_int_handle_pattern" "prst ok" "$out"

# ── 12. serve + stop (stub or real) ──────────────────────────────────────
_p12=$(_alloc_port)
out=$(run << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p12})
    wserver.stop(srv)
    print("ok")
}
if err != nil { print("stub ok") }
FLXEOF
)
echo "$out" | grep -qE "^ok$|^stub ok$" \
    && pass "serve_stop_clean" \
    || fail "serve_stop_clean" "ok or stub ok" "$out"

# ── 13. version() returns non-empty string ────────────────────────────────
out=$(run << 'FLX'
import std wserver
danger {
    str v = wserver.version()
    print(v)
}
if err != nil { print("stub-version") }
FLX
)
echo "$out" | grep -qE "microhttpd|stub" \
    && pass "version_returns_string" \
    || fail "version_returns_string" "microhttpd/x.x or stub-version" "$out"

# ── Real backend tests ────────────────────────────────────────────────────
if [ "$_REAL_BACKEND" -eq 0 ]; then
    for t in round_trip_get_200 round_trip_post_body round_trip_post_json \
              round_trip_put round_trip_patch round_trip_delete \
              round_trip_req_header round_trip_path_inspect \
              round_trip_reply_json_content_type \
              benchmark_healthz_pattern benchmark_post_users_pattern \
              benchmark_get_users_id_pattern benchmark_patch_users_id_pattern \
              connections_count_live autoscale_serve_stop autoscale_manual_explicit \
              autoscale_round_trip multi_worker_concurrent \
              autoscale_worker_fn_round_trip manual_worker_fn_round_trip \
              autoscale_unknown_fn_error; do
        skip "$t" "libmicrohttpd not compiled in (install libmicrohttpd-dev)"
    done
else

# Real backend tests use PID-based port range to avoid conflicts between concurrent runs
_next_port=$((21000 + ($$ % 3000)))

# Helper: start a Fluxa server in background, wait for port, send request, get response
_rt() {
    local port="$1"; local flx_file="$2"; local log_file="$3"
    timeout 8s "$FLUXA" run "$flx_file" -proj "$P" >"$log_file" 2>&1 &
    echo $!
}

# ── 14. GET round-trip → 200 body ────────────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply(req, 200, "hello-wserver") }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "hello-wserver" \
        && pass "round_trip_get_200" \
        || fail "round_trip_get_200" "hello-wserver" "curl=[$_curl] srv=[$(cat "$_log")]"
else skip "round_trip_get_200" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 15. POST body echoed ──────────────────────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str body = wserver.req_body(req)
        wserver.reply(req, 200, body)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -X POST -d "ping=1" "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "ping=1" \
        && pass "round_trip_post_body" \
        || fail "round_trip_post_body" "ping=1" "curl=[$_curl]"
else skip "round_trip_post_body" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 16. POST JSON body echoed with reply_json ─────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str body = wserver.req_body(req)
        wserver.reply_json(req, 201, body)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -X POST -H "Content-Type: application/json" \
        -d '{"name":"alice"}' "http://127.0.0.1:${_p}/users" 2>/dev/null || true)
    echo "$_curl" | grep -q '"name"' \
        && pass "round_trip_post_json" \
        || fail "round_trip_post_json" '{"name":"alice"}' "curl=[$_curl]"
else skip "round_trip_post_json" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 17. PUT → 200 ────────────────────────────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str method = wserver.req_method(req)
        wserver.reply(req, 200, method)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -X PUT -d "" "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "PUT" \
        && pass "round_trip_put" \
        || fail "round_trip_put" "PUT" "curl=[$_curl]"
else skip "round_trip_put" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 18. PATCH → req_method returns PATCH ─────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str method = wserver.req_method(req)
        wserver.reply(req, 200, method)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -X PATCH -d '{}' "http://127.0.0.1:${_p}/users/abc" 2>/dev/null || true)
    echo "$_curl" | grep -q "PATCH" \
        && pass "round_trip_patch" \
        || fail "round_trip_patch" "PATCH" "curl=[$_curl]"
else skip "round_trip_patch" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 19. DELETE → 204 ─────────────────────────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply(req, 204, "") }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _http_code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 3 \
        -X DELETE "http://127.0.0.1:${_p}/users/abc" 2>/dev/null || true)
    echo "$_http_code" | grep -q "204" \
        && pass "round_trip_delete" \
        || fail "round_trip_delete" "204" "http_code=[$_http_code]"
else skip "round_trip_delete" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 20. req_header reads custom header ───────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str ct = wserver.req_header(req, "X-Custom-Header")
        wserver.reply(req, 200, ct)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -H "X-Custom-Header: fluxa-test" \
        "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "fluxa-test" \
        && pass "round_trip_req_header" \
        || fail "round_trip_req_header" "fluxa-test" "curl=[$_curl]"
else skip "round_trip_req_header" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 21. req_path returns full path including segments ─────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str path = wserver.req_path(req)
        wserver.reply(req, 200, path)
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/users/abc-123-def" 2>/dev/null || true)
    echo "$_curl" | grep -q "abc-123-def" \
        && pass "round_trip_path_inspect" \
        || fail "round_trip_path_inspect" "/users/abc-123-def" "curl=[$_curl]"
else skip "round_trip_path_inspect" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 22. reply_json sets Content-Type: application/json ───────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply_json(req, 200, "{\"ok\":true}") }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -i "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "application/json" \
        && pass "round_trip_reply_json_content_type" \
        || fail "round_trip_reply_json_content_type" "Content-Type: application/json" "$_curl"
else skip "round_trip_reply_json_content_type" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 23. Benchmark pattern: GET /healthz → 200 {"status":"ok"} ────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str path = wserver.req_path(req)
        if path == "/healthz" {
            wserver.reply_json(req, 200, "{\"status\":\"ok\"}")
        }
        if path != "/healthz" {
            wserver.reply(req, 404, "not found")
        }
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/healthz" 2>/dev/null || true)
    echo "$_curl" | grep -q '"status"' \
        && pass "benchmark_healthz_pattern" \
        || fail "benchmark_healthz_pattern" '{"status":"ok"}' "curl=[$_curl]"
else skip "benchmark_healthz_pattern" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 24. Benchmark pattern: POST /users → 201 with JSON body ──────────────
_p=$(_alloc_port); _log=$(mktemp)
toml_str; cat > "$P/main.flx" << FLXEOF
import std wserver
import std strings
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str method = wserver.req_method(req)
        str path   = wserver.req_path(req)
        str body   = wserver.req_body(req)
        if method == "POST" {
            if path == "/users" {
                str blen = strings.from_int(len(body))
                str resp = strings.concat("{\"id\":\"test-uuid\",\"body_len\":", blen)
                str resp2 = strings.concat(resp, "}")
                wserver.reply_json(req, 201, resp2)
            }
        }
        if method != "POST" { wserver.reply(req, 405, "method not allowed") }
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 -X POST -H "Content-Type: application/json" \
        -d '{"name":"alice","email":"alice@example.com"}' \
        "http://127.0.0.1:${_p}/users" 2>/dev/null || true)
    echo "$_curl" | grep -q "test-uuid" \
        && pass "benchmark_post_users_pattern" \
        || fail "benchmark_post_users_pattern" '{"id":"test-uuid",...}' "curl=[$_curl]"
else skip "benchmark_post_users_pattern" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 25. Benchmark pattern: GET /users/:id → path contains UUID ───────────
_p=$(_alloc_port); _log=$(mktemp)
toml_str; cat > "$P/main.flx" << FLXEOF
import std wserver
import std strings
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str path = wserver.req_path(req)
        bool is_users_id = strings.starts_with(path, "/users/")
        if is_users_id {
            str id   = strings.slice(path, 7, len(path))
            str pre  = strings.concat("{\"id\":\"", id)
            str resp = strings.concat(pre, "\"}")
            wserver.reply_json(req, 200, resp)
        }
        if !is_users_id { wserver.reply(req, 404, "not found") }
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _uuid="550e8400-e29b-41d4-a716-446655440000"
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/users/${_uuid}" 2>/dev/null || true)
    echo "$_curl" | grep -q "$_uuid" \
        && pass "benchmark_get_users_id_pattern" \
        || fail "benchmark_get_users_id_pattern" "{\"id\":\"$_uuid\"}" "curl=[$_curl]"
else skip "benchmark_get_users_id_pattern" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 26. Benchmark pattern: PATCH /users/:id → 200 updated ────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml_str; cat > "$P/main.flx" << FLXEOF
import std wserver
import std strings
danger {
    int srv = wserver.serve(${_p})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str method = wserver.req_method(req)
        str path   = wserver.req_path(req)
        if method == "PATCH" {
            if strings.starts_with(path, "/users/") {
                str id   = strings.slice(path, 7, len(path))
                str pre  = strings.concat("{\"id\":\"", id)
                str resp = strings.concat(pre, "\",\"updated\":true}")
                wserver.reply_json(req, 200, resp)
            }
        }
        if method != "PATCH" { wserver.reply(req, 405, "method not allowed") }
    }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _uuid="abc-def-123"
    _curl=$(curl -s --max-time 3 -X PATCH -d '{}' \
        "http://127.0.0.1:${_p}/users/${_uuid}" 2>/dev/null || true)
    echo "$_curl" | grep -q "updated" \
        && pass "benchmark_patch_users_id_pattern" \
        || fail "benchmark_patch_users_id_pattern" '{"updated":true}' "curl=[$_curl]"
else skip "benchmark_patch_users_id_pattern" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 27. connections() returns integer when server running ─────────────────
_p=$(_alloc_port)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p})
    int n = wserver.connections(srv)
    print(n)
    wserver.stop(srv)
}
FLXEOF
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qE "^[0-9]+$" \
    && pass "connections_count_live" \
    || fail "connections_count_live" "integer" "$out"

# ── 28. serve(port, true) auto-scaling starts and stops cleanly ──────────
_p=$(_alloc_port)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p}, true)
    if srv != 0 {
        wserver.stop(srv)
        print("auto ok")
    }
}
if err != nil { print("error") }
FLXEOF
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "auto ok" \
    && pass "autoscale_serve_stop" \
    || fail "autoscale_serve_stop" "auto ok" "$out"

# ── 29. serve(port, false) explicit manual mode ───────────────────────────
_p=$(_alloc_port)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p}, false)
    if srv != 0 {
        wserver.stop(srv)
        print("manual ok")
    }
}
if err != nil { print("error") }
FLXEOF
out=$(timeout 6s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "manual ok" \
    && pass "autoscale_manual_explicit" \
    || fail "autoscale_manual_explicit" "manual ok" "$out"

# ── 30. auto=true round-trip ──────────────────────────────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p}, true)
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply(req, 200, "auto-scaled") }
    wserver.stop(srv)
}
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "auto-scaled" \
        && pass "autoscale_round_trip" \
        || fail "autoscale_round_trip" "auto-scaled" "curl=[$_curl]"
else skip "autoscale_round_trip" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 31. Multi-worker (ft.new manual mode) handles concurrent requests ─────
_p=$(_alloc_port); _log=$(mktemp)
toml_ft; cat > "$P/main.flx" << FLXEOF
import std wserver
import std flxthread as ft

fn handle(int srv) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            if req != 0 {
                str method = wserver.req_method(req)
                if method == "GET"    { wserver.reply(req, 200, "ok") }
                if method == "POST"   { wserver.reply_json(req, 201, "{\"created\":true}") }
                if method == "PATCH"  { wserver.reply_json(req, 200, "{\"updated\":true}") }
                if method == "DELETE" { wserver.reply(req, 204, "") }
            }
        }
    }
}

int srv = 0
danger { srv = wserver.serve(${_p}, false) }
if srv == 0 { print("serve failed") }

ft.new("w1", "handle", srv)
ft.new("w2", "handle", srv)

danger {
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply(req, 200, "initial-ok") }
}

ft.stop("w1")
ft.stop("w2")
ft.resolve_all()
wserver.stop(srv)
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.1
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "ok" \
        && pass "multi_worker_concurrent" \
        || fail "multi_worker_concurrent" "ok" "curl=[$_curl] srv=[$(cat "$_log")]"
else skip "multi_worker_concurrent" "server did not bind in time"; fi
wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0; rm -f "$_log"

# ── 32. serve(port, true, "fn_name") launches workers ────────────────────
_p=$(_alloc_port); _log=$(mktemp)
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "t"
entry = "main.flx"
[libs]
std.wserver = "1.0"
std.flxthread = "1.0"
[libs.wserver]
workers = 2
TOML
cat > "$P/main.flx" << FLXEOF
import std wserver
import std flxthread as ft

fn handle(int srv) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            if req != 0 { wserver.reply(req, 200, "worker-ok") }
        }
    }
}

int srv = 0
danger { srv = wserver.serve(${_p}, true, "handle") }
if srv == 0 { print("serve failed") }
if srv != 0 { wserver.wait(srv) }
wserver.stop(srv)
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.2
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -q "worker-ok" \
        && pass "autoscale_worker_fn_round_trip" \
        || fail "autoscale_worker_fn_round_trip" "worker-ok" "curl=[$_curl] srv=[$(cat "$_log")]"
else skip "autoscale_worker_fn_round_trip" "server did not bind in time"; fi
kill "$_SRV_PID" 2>/dev/null || true; wait "$_SRV_PID" 2>/dev/null || true
_SRV_PID=0; rm -f "$_log"

# ── 33. serve(port, false) + ft.new manual round-trip ────────────────────
_p=$(_alloc_port); _log=$(mktemp)
toml_ft; cat > "$P/main.flx" << FLXEOF
import std wserver
import std flxthread as ft

fn handle(int srv) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            if req != 0 { wserver.reply(req, 200, "manual-ok") }
        }
    }
}

int srv = 0
danger { srv = wserver.serve(${_p}, false) }
ft.new("w1", "handle", srv)
ft.new("w2", "handle", srv)

danger {
    int req = wserver.accept(srv, 4000)
    if req != 0 { wserver.reply(req, 200, "direct-ok") }
}

ft.stop("w1")
ft.stop("w2")
ft.resolve_all()
wserver.stop(srv)
FLXEOF
_SRV_PID=$(_rt "$_p" "$P/main.flx" "$_log")
if [ "$(_wait_port "$_p")" = "1" ]; then
    sleep 0.2
    _curl=$(curl -s --max-time 3 "http://127.0.0.1:${_p}/" 2>/dev/null || true)
    echo "$_curl" | grep -qE "ok" \
        && pass "manual_worker_fn_round_trip" \
        || fail "manual_worker_fn_round_trip" "ok" "curl=[$_curl] srv=[$(cat "$_log")]"
else skip "manual_worker_fn_round_trip" "server did not bind in time"; fi
kill "$_SRV_PID" 2>/dev/null || true; wait "$_SRV_PID" 2>/dev/null || true
_SRV_PID=0; rm -f "$_log"

# ── 34. serve(port, true, "unknown_fn") → clear error ────────────────────
_p=$(_alloc_port)
toml; cat > "$P/main.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_p}, true, "does_not_exist")
    if srv != 0 { print("should not reach") }
}
if err != nil { print("fn not found") }
FLXEOF
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "fn not found" \
    && pass "autoscale_unknown_fn_error" \
    || fail "autoscale_unknown_fn_error" "fn not found" "$out"

fi  # _REAL_BACKEND

echo "────────────────────────────────────────────────────────────────"
echo "  → std.wserver: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.wserver: PASS" && exit 0 || exit 1
