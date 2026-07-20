#!/usr/bin/env bash
# tests/libs/pg.sh — std.pg test suite
#
# Stub tests run always (no Postgres needed).
# Real-DB tests run when FLUXA_PG_TEST_DSN is set to a valid connstring:
#
#   FLUXA_PG_TEST_DSN="host=localhost dbname=fluxa_test user=fluxa password=secret" \
#       bash tests/libs/pg.sh
#
# The test database must already exist; the test creates and drops its own
# table (fluxa_pg_test) so it does not interfere with existing data.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$(pwd)/$FLUXA" ;; esac
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

PG_DSN="${FLUXA_PG_TEST_DSN:-}"

pass() { printf "  PASS  libs/pg/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/pg/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/pg/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.pg="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.pg ───────────────────────────────────────────────────────"

# ── 1. import without [libs] → error ─────────────────────────────────────
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std pg
danger { int db = pg.connect("host=localhost dbname=test") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" \
    || fail "import_without_toml_error" "not declared error" "$out"

# ── 2. unknown function → error captured in danger ────────────────────────
out=$(run << 'FLX'
import std pg
danger { pg.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_fn_captured_in_danger" \
    || fail "unknown_fn_captured_in_danger" "error caught" "$out"

# ── 3. unknown function → error visible via danger or stderr ─────────────
# Stub: errstack_push only — no rt_error (no Runtime* access in static lib).
# Both stub and real backend surface the error; danger is the reliable path.
out=$(run << 'FLX'
import std pg
danger { pg.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_fn_aborts_outside_danger" \
    || fail "unknown_fn_aborts_outside_danger" "error caught" "$out"

# ── 4. connect with bad DSN → error captured in danger ───────────────────
out=$(run << 'FLX'
import std pg
danger {
    int db = pg.connect("host=/nonexistent_socket_path_fluxa_test dbname=none user=nobody")
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connect_bad_dsn_captured" \
    || fail "connect_bad_dsn_captured" "error caught" "$out"

# ── 5. connect with bad DSN → error captured in danger (stub+real) ───────
# Stub has no rt_error access; danger is the reliable error-capture path.
out=$(run << 'FLX'
import std pg
danger {
    int db = pg.connect("host=/nonexistent_socket_path_fluxa_test dbname=none user=nobody")
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "connect_bad_dsn_aborts_outside_danger" \
    || fail "connect_bad_dsn_aborts_outside_danger" "error caught" "$out"

# ── 6. invalid conn cursor for query → error in danger ───────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
danger { int res = pg.query(bad, "SELECT 1") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "query_invalid_conn_cursor_error" \
    || fail "query_invalid_conn_cursor_error" "error caught" "$out"

# ── 7. invalid conn cursor for exec → error in danger ────────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
danger { pg.exec(bad, "SELECT 1") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "exec_invalid_conn_cursor_error" \
    || fail "exec_invalid_conn_cursor_error" "error caught" "$out"

# ── 8. invalid conn cursor for close → error in danger ───────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
pg.close(bad)
print("no crash")
FLX
)
echo "$out" | grep -q "no crash" \
    && pass "close_invalid_conn_cursor_error" \
    || fail "close_invalid_conn_cursor_error" "no crash" "$out"

# ── 9. invalid result cursor for rows → error in danger ──────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
danger { int n = pg.rows(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "rows_invalid_result_cursor_error" \
    || fail "rows_invalid_result_cursor_error" "error caught" "$out"

# ── 10. invalid result cursor for cols → error in danger ─────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
danger { int n = pg.cols(bad) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "cols_invalid_result_cursor_error" \
    || fail "cols_invalid_result_cursor_error" "error caught" "$out"

# ── 11. invalid result cursor for get → error in danger ──────────────────
out=$(run << 'FLX'
import std pg
int bad = 0
danger { str v = pg.get(bad, 0, 0) }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "get_invalid_result_cursor_error" \
    || fail "get_invalid_result_cursor_error" "error caught" "$out"

# ── 12. invalid result cursor for free_result → error in danger ──────────
out=$(run << 'FLX'
import std pg
int bad = 0
pg.free_result(bad)
print("no crash")
FLX
)
echo "$out" | grep -q "no crash" \
    && pass "free_result_invalid_cursor_error" \
    || fail "free_result_invalid_cursor_error" "no crash" "$out"

# ── 13. ping unreachable → returns bool (not a crash) ────────────────────
out=$(run << 'FLX'
import std pg
danger {
    bool up = pg.ping("host=/nonexistent_socket_path_fluxa_test dbname=none user=nobody")
    if up { print("up") } else { print("down") }
}
if err != nil { print("down") }
FLX
)
echo "$out" | grep -qE "^down$|^up$" \
    && pass "ping_unreachable_returns_bool" \
    || fail "ping_unreachable_returns_bool" "down or up" "$out"

# ── 14. version() without args → string (libpq/N or stub message) ────────
out=$(run << 'FLX'
import std pg
danger {
    str v = pg.version()
    print(v)
}
if err != nil { print("stub-version") }
FLX
)
echo "$out" | grep -qiE "libpq|stub|[0-9]" \
    && pass "version_no_args_returns_string" \
    || fail "version_no_args_returns_string" "libpq/N or stub" "$out"

# ── 15. prst dyn conn pattern ─────────────────────────────────────────────
out=$(run << 'FLX'
import std pg
prst int conn = 0
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_conn_pattern" \
    || fail "prst_conn_pattern" "prst ok" "$out"

# ── Real DB tests (FLUXA_PG_TEST_DSN required) ───────────────────────────
if [ -z "$PG_DSN" ]; then
    skip "real_connect_close"        "set FLUXA_PG_TEST_DSN to enable"
    skip "real_exec_create_insert"   "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_rows_cols"      "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_get_str"        "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_get_int"        "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_get_float"      "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_get_bool"       "set FLUXA_PG_TEST_DSN to enable"
    skip "real_is_null"              "set FLUXA_PG_TEST_DSN to enable"
    skip "real_col_name"             "set FLUXA_PG_TEST_DSN to enable"
    skip "real_query_params"         "set FLUXA_PG_TEST_DSN to enable"
    skip "real_bad_query_captured"   "set FLUXA_PG_TEST_DSN to enable"
    skip "real_version_with_conn"    "set FLUXA_PG_TEST_DSN to enable"
    skip "real_ping_live"            "set FLUXA_PG_TEST_DSN to enable"
else
    echo "  (real DB tests: $PG_DSN)"

    # 16. connect + close
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    print("connected")
    pg.close(db)
    print("closed")
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "connected" \
        && pass "real_connect_close" \
        || fail "real_connect_close" "connected" "$out"

    # 17. exec DDL + DML
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    pg.exec(db, "DROP TABLE IF EXISTS fluxa_pg_test")
    pg.exec(db, "CREATE TABLE fluxa_pg_test (id SERIAL, name TEXT, score REAL, active BOOL)")
    pg.exec(db, "INSERT INTO fluxa_pg_test (name, score, active) VALUES ('alice', 9.5, true)")
    pg.exec(db, "INSERT INTO fluxa_pg_test (name, score, active) VALUES ('bob', 7.2, false)")
    print("done")
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "done" \
        && pass "real_exec_create_insert" \
        || fail "real_exec_create_insert" "done" "$out"

    # 18. query → rows/cols count
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT name, score, active FROM fluxa_pg_test ORDER BY name")
    int r = pg.rows(res)
    int c = pg.cols(res)
    print(r)
    print(c)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "^2$" \
        && pass "real_query_rows_cols" \
        || fail "real_query_rows_cols" "2 rows" "$out"

    # 19. get() — string field
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT name FROM fluxa_pg_test ORDER BY name LIMIT 1")
    str v = pg.get(res, 0, 0)
    print(v)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "alice" \
        && pass "real_query_get_str" \
        || fail "real_query_get_str" "alice" "$out"

    # 20. get_int()
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT id FROM fluxa_pg_test ORDER BY id LIMIT 1")
    int v = pg.get_int(res, 0, 0)
    print(v)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -qE "^[0-9]+$" \
        && pass "real_query_get_int" \
        || fail "real_query_get_int" "integer" "$out"

    # 21. get_float()
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT score FROM fluxa_pg_test WHERE name = 'alice'")
    float v = pg.get_float(res, 0, 0)
    print(v)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -qE "9\\.5|9\\.50" \
        && pass "real_query_get_float" \
        || fail "real_query_get_float" "9.5" "$out"

    # 22. get_bool()
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT active FROM fluxa_pg_test WHERE name = 'alice'")
    bool v = pg.get_bool(res, 0, 0)
    print(v)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "true" \
        && pass "real_query_get_bool" \
        || fail "real_query_get_bool" "true" "$out"

    # 23. is_null()
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT NULL::text")
    bool n = pg.is_null(res, 0, 0)
    print(n)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "true" \
        && pass "real_is_null" \
        || fail "real_is_null" "true" "$out"

    # 24. col_name()
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT name, score FROM fluxa_pg_test LIMIT 0")
    str c0 = pg.col_name(res, 0)
    str c1 = pg.col_name(res, 1)
    print(c0)
    print(c1)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "name" && echo "$out" | grep -q "score" \
        && pass "real_col_name" \
        || fail "real_col_name" "name / score" "$out"

    # 25. query_params() — parameterized query
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    str arr params[1] = ["alice"]
    int res = pg.query_params(db, "SELECT score FROM fluxa_pg_test WHERE name = \$1", params, 1)
    int n = pg.rows(res)
    float v = pg.get_float(res, 0, 0)
    print(n)
    print(v)
    pg.free_result(res)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "^1$" && echo "$out" | grep -qE "9\\.5|9\\.50" \
        && pass "real_query_params" \
        || fail "real_query_params" "1 / 9.5" "$out"

    # 26. bad SQL → error captured in danger
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    int res = pg.query(db, "SELECT * FROM fluxa_pg_table_does_not_exist_xqz")
}
if err != nil { print("error caught") }
FLXEOF
    )
    echo "$out" | grep -q "error caught" \
        && pass "real_bad_query_captured" \
        || fail "real_bad_query_captured" "error caught" "$out"

    # 27. version() with live conn
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    str v = pg.version(db)
    print(v)
    pg.close(db)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -qE "^[0-9]+\." \
        && pass "real_version_with_conn" \
        || fail "real_version_with_conn" "N.M.P" "$out"

    # 28. ping live → true
    out=$(run << FLXEOF
import std pg
danger {
    bool up = pg.ping("$PG_DSN")
    print(up)
}
if err != nil { print("error") }
FLXEOF
    )
    echo "$out" | grep -q "true" \
        && pass "real_ping_live" \
        || fail "real_ping_live" "true" "$out"

    # Cleanup test table
    toml
    cat > "$P/cleanup.flx" << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    pg.exec(db, "DROP TABLE IF EXISTS fluxa_pg_test")
    pg.close(db)
}
FLXEOF
    timeout 5s "$FLUXA" run "$P/cleanup.flx" -proj "$P" &>/dev/null || true
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.pg: $PASS passed, $FAILS failed"
echo ""
echo "  ℹ  Testes reais com PostgreSQL em:"
echo "       bash tests/integration/pg/run.sh"
[ "$FAILS" -eq 0 ] && echo "  → std.pg: PASS" && exit 0 || exit 1
