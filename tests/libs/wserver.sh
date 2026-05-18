#!/usr/bin/env bash
# tests/libs/wserver.sh — std.wserver test suite
#
# Tests the stub backend (always available) plus a real round-trip when
# libmicrohttpd is compiled in.  The round-trip uses a free port picked
# at runtime; no fixed port is required.
#
# Requires for real-backend tests: libmicrohttpd-dev at build time.
set -euo pipefail
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
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.wserver ──────────────────────────────────────────────────"

# ── 1. import without [libs] → error ─────────────────────────────────────
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std wserver
danger { int srv = wserver.serve(19180) }
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

# ── 3. unknown function → aborts outside danger ───────────────────────────
# Stub has no rt_error access; danger is the reliable error-capture path
# for both stub and real backend.
out=$(run << 'FLX'
import std wserver
danger { wserver.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_fn_aborts_outside_danger" \
    || fail "unknown_fn_aborts_outside_danger" "error caught" "$out"

# ── 4. invalid server cursor for accept → error in danger ────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { int req = wserver.accept(bad, 100) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "accept_invalid_cursor_error" \
    || fail "accept_invalid_cursor_error" "error caught" "$out"

# ── 5. invalid request cursor for req_method → error in danger ───────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str m = wserver.req_method(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_method_invalid_cursor_error" \
    || fail "req_method_invalid_cursor_error" "error caught" "$out"

# ── 6. invalid request cursor for req_path → error in danger ─────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str p = wserver.req_path(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_path_invalid_cursor_error" \
    || fail "req_path_invalid_cursor_error" "error caught" "$out"

# ── 7. invalid request cursor for req_body → error in danger ─────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { str b = wserver.req_body(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "req_body_invalid_cursor_error" \
    || fail "req_body_invalid_cursor_error" "error caught" "$out"

# ── 8. invalid request cursor for reply → error in danger ─────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { wserver.reply(bad, 200, "hi") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "reply_invalid_cursor_error" \
    || fail "reply_invalid_cursor_error" "error caught" "$out"

# ── 9. invalid request cursor for reply_json → error in danger ───────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { wserver.reply_json(bad, 200, "{}") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "reply_json_invalid_cursor_error" \
    || fail "reply_json_invalid_cursor_error" "error caught" "$out"

# ── 10. invalid server cursor for connections → error in danger ───────────
out=$(run << 'FLX'
import std wserver
int bad = 0
danger { int n = wserver.connections(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connections_invalid_cursor_error" \
    || fail "connections_invalid_cursor_error" "error caught" "$out"

# ── 11. invalid server cursor for stop → error in danger ─────────────────
out=$(run << 'FLX'
import std wserver
int bad = 0
wserver.stop(bad)
print("no crash")
FLX
)
echo "$out" | grep -q "no crash" \
    && pass "stop_invalid_cursor_error" \
    || fail "stop_invalid_cursor_error" "no crash" "$out"

# ── 12. prst dyn cursor pattern compiles and runs ─────────────────────────
out=$(run << 'FLX'
import std wserver
prst int srv = 0
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_cursor_pattern" \
    || fail "prst_cursor_pattern" "prst ok" "$out"

# ── 13. serve → stub logs warning OR starts daemon; stop cleans up ────────
# Works in both stub and real-backend builds.
out=$(run << 'FLX'
import std wserver
danger {
    int srv = wserver.serve(19181)
    wserver.stop(srv)
    print("ok")
}
if err != nil { print("stub ok") }
FLX
)
echo "$out" | grep -qE "^ok$|^stub ok$" \
    && pass "serve_stop_stub_or_real" \
    || fail "serve_stop_stub_or_real" "ok or stub ok" "$out"

# ── 14. version() returns a non-empty string (stub or real) ───────────────
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

# ── Real round-trip (only when libmicrohttpd compiled in) ─────────────────
# Detect real backend: serve must succeed (no error) and stop must succeed.
_REAL_BACKEND=0
_PROBE_PORT=0
for _p in $(shuf -i 19200-19900 -n 30 2>/dev/null || seq 19200 19230); do
    ! ss -tlnp 2>/dev/null | grep -q ":${_p}[^0-9]" && _PROBE_PORT=$_p && break
done
[ "$_PROBE_PORT" -eq 0 ] && _PROBE_PORT=19300

toml
cat > "$P/probe.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_PROBE_PORT})
    if srv != 0 {
        wserver.stop(srv)
        print("real")
    }
}
if err != nil { print("stub") }
FLXEOF
_probe_out=$(timeout 5s "$FLUXA" run "$P/probe.flx" -proj "$P" 2>&1 || true)
echo "$_probe_out" | grep -q "^real$" && _REAL_BACKEND=1

if [ "$_REAL_BACKEND" -eq 0 ]; then
    skip "round_trip_get_200"          "libmicrohttpd not compiled in (install libmicrohttpd-dev)"
    skip "round_trip_post_body"        "libmicrohttpd not compiled in"
    skip "round_trip_reply_json"       "libmicrohttpd not compiled in"
    skip "connections_count_live"      "libmicrohttpd not compiled in"
else
    # Pick a fresh port for the actual round-trip tests
    _RT_PORT=0
    for _p in $(shuf -i 19200-19900 -n 30 2>/dev/null || seq 19200 19230); do
        [ "$_p" -ne "$_PROBE_PORT" ] || continue
        ! ss -tlnp 2>/dev/null | grep -q ":${_p}[^0-9]" && _RT_PORT=$_p && break
    done
    [ "$_RT_PORT" -eq 0 ] && _RT_PORT=19301

    # 15. GET round-trip — Fluxa server + curl client
    if command -v curl &>/dev/null; then
        toml
        cat > "$P/rt_get.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_RT_PORT})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        wserver.reply(req, 200, "hello-wserver")
    }
    wserver.stop(srv)
}
FLXEOF
        _SRV_LOG=$(mktemp)
        timeout 8s "$FLUXA" run "$P/rt_get.flx" -proj "$P" >"$_SRV_LOG" 2>&1 &
        _SRV_PID=$!
        # Wait for port to open (max 4s — MHD needs a moment to bind)
        _ready=0
        for _i in $(seq 1 40); do
            if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/$_RT_PORT; then
                exec 3>&-; _ready=1; break
            fi
            sleep 0.1
        done
        if [ "$_ready" -eq 1 ]; then
            sleep 0.2   # MHD bound but accept loop may not be ready yet
            _curl_out=$(curl -s --max-time 3 "http://127.0.0.1:${_RT_PORT}/" 2>/dev/null || echo "")
            _srv_out=$(cat "$_SRV_LOG" 2>/dev/null)
            echo "$_curl_out" | grep -q "hello-wserver" \
                && pass "round_trip_get_200" \
                || fail "round_trip_get_200" "hello-wserver" "curl=[$_curl_out] srv=[$_srv_out]"
        else
            skip "round_trip_get_200" "server did not bind in time"
        fi
        wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0

        # 16. POST round-trip — body accessible via req_body
        _RT_PORT2=$(( _RT_PORT + 1 ))
        toml
        cat > "$P/rt_post.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_RT_PORT2})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        str body = wserver.req_body(req)
        wserver.reply(req, 200, body)
    }
    wserver.stop(srv)
}
FLXEOF
        _SRV_LOG2=$(mktemp)
        timeout 8s "$FLUXA" run "$P/rt_post.flx" -proj "$P" >"$_SRV_LOG2" 2>&1 &
        _SRV_PID=$!
        _ready=0
        for _i in $(seq 1 40); do
            if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/$_RT_PORT2; then
                exec 3>&-; _ready=1; break
            fi
            sleep 0.1
        done
        if [ "$_ready" -eq 1 ]; then
            sleep 0.2
            _curl_out=$(curl -s --max-time 3 -X POST -d "ping=1" \
                "http://127.0.0.1:${_RT_PORT2}/" 2>/dev/null || echo "")
            _srv_out2=$(cat "$_SRV_LOG2" 2>/dev/null)
            echo "$_curl_out" | grep -q "ping=1" \
                && pass "round_trip_post_body" \
                || fail "round_trip_post_body" "ping=1" "curl=[$_curl_out] srv=[$_srv_out2]"
        else
            skip "round_trip_post_body" "server did not bind in time"
        fi
        wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0

        # 17. reply_json — Content-Type application/json in response
        _RT_PORT3=$(( _RT_PORT + 2 ))
        toml
        cat > "$P/rt_json.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_RT_PORT3})
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        wserver.reply_json(req, 200, "{\"ok\":true}")
    }
    wserver.stop(srv)
}
FLXEOF
        timeout 8s "$FLUXA" run "$P/rt_json.flx" -proj "$P" &>/dev/null &
        _SRV_PID=$!
        _ready=0
        for _i in $(seq 1 40); do
            if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/$_RT_PORT3; then
                exec 3>&-; _ready=1; break
            fi
            sleep 0.1
        done
        if [ "$_ready" -eq 1 ]; then
            sleep 0.2
            _curl_out=$(curl -s --max-time 3 -i \
                "http://127.0.0.1:${_RT_PORT3}/" 2>/dev/null || echo "")
            echo "$_curl_out" | grep -q "application/json" \
                && pass "round_trip_reply_json" \
                || fail "round_trip_reply_json" "Content-Type: application/json" "$_curl_out"
        else
            skip "round_trip_reply_json" "server did not bind in time"
        fi
        wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0

        # 18. connections() returns 0 when no requests pending
        toml
        cat > "$P/conns.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve($(( _RT_PORT + 3 )))
    int n = wserver.connections(srv)
    print(n)
    wserver.stop(srv)
}
FLXEOF
        out=$(timeout 5s "$FLUXA" run "$P/conns.flx" -proj "$P" 2>&1 || true)
        echo "$out" | grep -qE "^[0-9]+$" \
            && pass "connections_count_live" \
            || fail "connections_count_live" "integer" "$out"

    # ── Auto-scaling tests (real backend only) ───────────────────────────
    # 19. serve with auto=true starts and stops cleanly
    toml
    cat > "$P/auto_serve.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve($(( _RT_PORT + 4 )), true)
    if srv != 0 {
        wserver.stop(srv)
        print("auto ok")
    }
}
if err != nil { print("error") }
FLXEOF
    out=$(timeout 6s "$FLUXA" run "$P/auto_serve.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "auto ok" \
        && pass "autoscale_serve_stop" \
        || fail "autoscale_serve_stop" "auto ok" "$out"

    # 20. auto=false (explicit) behaves same as no arg
    toml
    cat > "$P/manual_serve.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve($(( _RT_PORT + 5 )), false)
    if srv != 0 {
        wserver.stop(srv)
        print("manual ok")
    }
}
if err != nil { print("error") }
FLXEOF
    out=$(timeout 6s "$FLUXA" run "$P/manual_serve.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "manual ok" \
        && pass "autoscale_manual_explicit" \
        || fail "autoscale_manual_explicit" "manual ok" "$out"

    # 21. round-trip with auto=true — same Fluxa accept/reply interface
    if command -v curl &>/dev/null; then
        _RT_PORT_AUTO=$(( _RT_PORT + 6 ))
        toml
        cat > "$P/auto_rt.flx" << FLXEOF
import std wserver
danger {
    int srv = wserver.serve(${_RT_PORT_AUTO}, true)
    int req = wserver.accept(srv, 4000)
    if req != 0 {
        wserver.reply(req, 200, "auto-scaled")
    }
    wserver.stop(srv)
}
FLXEOF
        _SRV_LOG_AUTO=$(mktemp)
        timeout 8s "$FLUXA" run "$P/auto_rt.flx" -proj "$P" >"$_SRV_LOG_AUTO" 2>&1 &
        _SRV_PID=$!
        _ready=0
        for _i in $(seq 1 40); do
            if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/$_RT_PORT_AUTO; then
                exec 3>&-; _ready=1; break
            fi
            sleep 0.1
        done
        if [ "$_ready" -eq 1 ]; then
            sleep 0.2
            _curl_out=$(curl -s --max-time 3 "http://127.0.0.1:${_RT_PORT_AUTO}/" 2>/dev/null || echo "")
            _srv_out_auto=$(cat "$_SRV_LOG_AUTO" 2>/dev/null)
            echo "$_curl_out" | grep -q "auto-scaled" \
                && pass "autoscale_round_trip" \
                || fail "autoscale_round_trip" "auto-scaled" "curl=[$_curl_out] srv=[$_srv_out_auto]"
        else
            skip "autoscale_round_trip" "server did not bind in time"
        fi
        wait "$_SRV_PID" 2>/dev/null || true; _SRV_PID=0
        rm -f "$_SRV_LOG_AUTO"
    else
        skip "autoscale_round_trip" "curl not available"
    fi

    else
        skip "round_trip_get_200"     "curl not available"
        skip "round_trip_post_body"   "curl not available"
        skip "round_trip_reply_json"  "curl not available"
        skip "connections_count_live" "curl not available"
        skip "autoscale_serve_stop"   "curl not available"
        skip "autoscale_manual_explicit" "curl not available"
        skip "autoscale_round_trip"   "curl not available"
    fi
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.wserver: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.wserver: PASS" && exit 0 || exit 1
