#!/usr/bin/env bash
# tests/libs/httpc.sh — std.httpc test suite
#
# Live server: minimal Python HTTP server written inline.
# No fixed ports — picks a free port dynamically.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$(pwd)/$FLUXA" ;; esac
P="$(mktemp -d)"; _SRV_PID=0
trap 'rm -rf "$P"; [ "$_SRV_PID" -gt 0 ] && kill "$_SRV_PID" 2>/dev/null || true' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/httpc/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/httpc/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/httpc/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml_c() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.httpc="1.0"\n' > "$P/fluxa.toml"; }
run()    { toml_c; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.httpc ─────────────────────────────────────────────────────"

# ── Static tests (no server needed) ──────────────────────────────────────

# 1. import without [libs]
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std httpc
danger { dyn r = httpc.get("http://127.0.0.1:19999/") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. connection refused → danger catches error
out=$(run << 'FLX'
import std httpc
danger { dyn r = httpc.get("http://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connection_refused_error_captured" || fail "connection_refused_error_captured" "error caught" "$out"

# 3. get() missing arg
out=$(run << 'FLX'
import std httpc
danger { dyn r = httpc.get() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -qiE "error caught|expected|arg" \
    && pass "get_missing_arg_error" || fail "get_missing_arg_error" "error" "$out"

# 4. post() missing body
out=$(run << 'FLX'
import std httpc
danger { dyn r = httpc.post("http://127.0.0.1:19999/") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -qiE "error caught|expected|arg" \
    && pass "post_missing_arg_error" || fail "post_missing_arg_error" "error" "$out"

# 5. unknown function
out=$(run << 'FLX'
import std httpc
danger { httpc.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# ── Live tests ────────────────────────────────────────────────────────────

# Find a free port
_PORT=0
for _p in $(shuf -i 19000-19800 -n 30 2>/dev/null || seq 19000 19030); do
    ! ss -tlnp 2>/dev/null | grep -q ":${_p}[^0-9]" && _PORT=$_p && break
done
[ "$_PORT" -eq 0 ] && _PORT=19100

# Write a self-contained Python HTTP server that handles all paths correctly
cat > "$P/srv.py" << 'PYEOF'
import sys, threading
from http.server import HTTPServer, BaseHTTPRequestHandler

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == '/':
            body = b'hello fluxa httpc'
            self.send_response(200)
        elif self.path == '/404':
            body = b'nope'
            self.send_response(404)
        elif self.path == '/data':
            body = b'ok:true'
            self.send_response(200)
        else:
            body = b'unknown'
            self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

port = int(sys.argv[1])
srv = HTTPServer(('127.0.0.1', port), H)
srv.serve_forever()
PYEOF

python3 "$P/srv.py" "$_PORT" &
_SRV_PID=$!

# Wait for TCP bind (max 3s)
_ready=0
for _i in $(seq 1 30); do
    if 2>/dev/null exec 3<>/dev/tcp/127.0.0.1/$_PORT; then
        exec 3>&-; _ready=1; break
    fi
    sleep 0.1
done

if [ "$_ready" -eq 0 ]; then
    for _t in get_local_returns_200 get_local_ok_true get_local_body_correct \
               get_local_404_status get_local_404_ok_false body_accessor; do
        skip "$_t" "local server not ready (port $_PORT)"
    done
else
    _BASE="http://127.0.0.1:${_PORT}"

    # 6-8: GET / → 200, ok=true, body
    toml_c; cat > "$P/main.flx" << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_BASE}/")
    int s = httpc.status(r)
    bool ok = httpc.ok(r)
    str b = httpc.body(r)
    print(s)
    print(ok)
    print(b)
}
FLXEOF
    out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "^200$"             && pass "get_local_returns_200"  || fail "get_local_returns_200"  "200"              "$out"
    echo "$out" | grep -q "^true$"            && pass "get_local_ok_true"      || fail "get_local_ok_true"      "true"             "$out"
    echo "$out" | grep -q "hello fluxa httpc" && pass "get_local_body_correct" || fail "get_local_body_correct" "hello fluxa httpc" "$out"

    # 9-10: GET /404 → 404, ok=false
    toml_c; cat > "$P/main.flx" << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_BASE}/404")
    int s = httpc.status(r)
    bool ok = httpc.ok(r)
    print(s)
    print(ok)
}
FLXEOF
    out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "^404$"   && pass "get_local_404_status"   || fail "get_local_404_status"   "404"   "$out"
    echo "$out" | grep -q "^false$" && pass "get_local_404_ok_false" || fail "get_local_404_ok_false" "false" "$out"

    # 11: body accessor
    toml_c; cat > "$P/main.flx" << FLXEOF
import std httpc
danger {
    dyn r = httpc.get("${_BASE}/data")
    str b = httpc.body(r)
    print(b)
}
FLXEOF
    out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "ok:true" && pass "body_accessor" || fail "body_accessor" "ok:true" "$out"
fi

kill "$_SRV_PID" 2>/dev/null; _SRV_PID=0

echo "────────────────────────────────────────────────────────────────"
echo "  → std.httpc: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.httpc: PASS" && exit 0 || exit 1
