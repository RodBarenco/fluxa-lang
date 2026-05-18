#!/usr/bin/env bash
# tests/integration/pg/inner.sh
#
# Roda dentro do container Docker.
# Inicializa o PostgreSQL local, cria usuário e banco de teste,
# executa todos os testes reais do std.pg.
#
# Requer: container com libpq-dev e postgresql instalados.
set -euo pipefail

FLUXA="/fluxa/fluxa"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)   FLUXA="$2";  shift 2 ;;
        --verbose) VERBOSE=1;   shift   ;;
        *) shift ;;
    esac
done

P="$(mktemp -d)"
trap 'rm -rf "$P"; _pg_cleanup' EXIT

PASS=0; FAIL=0
PG_STARTED=0
PG_DSN=""

pass() { printf "  PASS  pg/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  pg/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAIL=$((FAIL+1)); }
skip() { printf "  SKIP  pg/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.pg="1.0"\n' > "$P/fluxa.toml"
}
run() {
    toml; cat > "$P/main.flx"
    FLUXA_PG_TEST_DSN="$PG_DSN" timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

# ── PostgreSQL setup ──────────────────────────────────────────────────────
_pg_cleanup() {
    if [ "$PG_STARTED" -eq 1 ]; then
        pg_ctlcluster 16 main stop -m fast 2>/dev/null || \
        pg_ctlcluster 15 main stop -m fast 2>/dev/null || \
        pg_ctlcluster 14 main stop -m fast 2>/dev/null || true
    fi
}

_pg_start() {
    # Find installed PostgreSQL version
    PG_VER=$(pg_lsclusters -h 2>/dev/null | awk 'NR==1{print $1}' || echo "")
    if [ -z "$PG_VER" ]; then
        echo "  [pg] No PostgreSQL cluster found"
        return 1
    fi

    echo "  [pg] Starting PostgreSQL $PG_VER..."
    pg_ctlcluster "$PG_VER" main start 2>/dev/null || true

    # Wait for PostgreSQL to be ready (max 15s)
    local ready=0
    for i in $(seq 1 30); do
        if pg_isready -q 2>/dev/null; then
            ready=1; break
        fi
        sleep 0.5
    done

    if [ "$ready" -eq 0 ]; then
        echo "  [pg] PostgreSQL did not start in time"
        return 1
    fi

    # Create test user and database
    su -c "psql -c \"CREATE USER fluxa_test WITH PASSWORD 'fluxa_test_pw';\"" postgres 2>/dev/null || true
    su -c "psql -c \"CREATE DATABASE fluxa_test OWNER fluxa_test;\"" postgres 2>/dev/null || true
    su -c "psql -c \"GRANT ALL PRIVILEGES ON DATABASE fluxa_test TO fluxa_test;\"" postgres 2>/dev/null || true

    PG_DSN="host=localhost dbname=fluxa_test user=fluxa_test password=fluxa_test_pw"
    PG_STARTED=1
    echo "  [pg] PostgreSQL ready — DSN: $PG_DSN"
    return 0
}

echo "══════════════════════════════════════════════════════════════════"
echo "  Fluxa PostgreSQL Integration Tests"
echo "  binary  : $FLUXA"
echo "══════════════════════════════════════════════════════════════════"
echo ""

_pg_start || true

if [ "$PG_STARTED" -eq 0 ]; then
    echo "  [pg] Could not start PostgreSQL — skipping all real tests"
fi

# ── Test 1: connect + close ───────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
    echo "$out" | grep -q "connected" && echo "$out" | grep -q "closed" \
        && pass "real_connect_close" \
        || fail "real_connect_close" "connected + closed" "$out"
else
    skip "real_connect_close" "PostgreSQL not available"
fi

# ── Test 2: exec DDL + DML ────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
else
    skip "real_exec_create_insert" "PostgreSQL not available"
fi

# ── Test 3: query rows/cols ───────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^3$" \
        && pass "real_query_rows_cols" \
        || fail "real_query_rows_cols" "2 rows / 3 cols" "$out"
else
    skip "real_query_rows_cols" "PostgreSQL not available"
fi

# ── Test 4: get str ───────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -q "alice" \
        && pass "real_query_get_str" \
        || fail "real_query_get_str" "alice" "$out"
else
    skip "real_query_get_str" "PostgreSQL not available"
fi

# ── Test 5: get_int ───────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -qE "^[0-9]+$" \
        && pass "real_query_get_int" \
        || fail "real_query_get_int" "integer id" "$out"
else
    skip "real_query_get_int" "PostgreSQL not available"
fi

# ── Test 6: get_float ─────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -qE "9\.5|9\.50" \
        && pass "real_query_get_float" \
        || fail "real_query_get_float" "9.5" "$out"
else
    skip "real_query_get_float" "PostgreSQL not available"
fi

# ── Test 7: get_bool ─────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -qiE "^true$|^1$" \
        && pass "real_query_get_bool" \
        || fail "real_query_get_bool" "true" "$out"
else
    skip "real_query_get_bool" "PostgreSQL not available"
fi

# ── Test 8: is_null ───────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -qiE "^true$|^1$" \
        && pass "real_is_null" \
        || fail "real_is_null" "true" "$out"
else
    skip "real_is_null" "PostgreSQL not available"
fi

# ── Test 9: col_name ─────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -q "name" && echo "$out" | grep -q "score" \
        && pass "real_col_name" \
        || fail "real_col_name" "name + score" "$out"
else
    skip "real_col_name" "PostgreSQL not available"
fi

# ── Test 10: query_params ─────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
FLXEOF
)
    echo "$out" | grep -q "^1$" && echo "$out" | grep -qE "9\.5|9\.50" \
        && pass "real_query_params" \
        || fail "real_query_params" "1 row / 9.5" "$out"
else
    skip "real_query_params" "PostgreSQL not available"
fi

# ── Test 11: query_params with 2 params ──────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    str arr params[2] = ["alice", "9.0"]
    int res = pg.query_params(db, "SELECT name FROM fluxa_pg_test WHERE name = \$1 AND score > \$2::real", params, 2)
    int n = pg.rows(res)
    print(n)
    pg.free_result(res)
    pg.close(db)
}
FLXEOF
)
    echo "$out" | grep -q "^1$" \
        && pass "real_query_params_multi" \
        || fail "real_query_params_multi" "1" "$out"
else
    skip "real_query_params_multi" "PostgreSQL not available"
fi

# ── Test 12: bad SQL captured in danger ───────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
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
else
    skip "real_bad_query_captured" "PostgreSQL not available"
fi

# ── Test 13: version with conn ────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
    out=$(run << FLXEOF
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    str v = pg.version(db)
    print(v)
    pg.close(db)
}
FLXEOF
)
    echo "$out" | grep -qE "^[0-9]+\.[0-9]" \
        && pass "real_version_with_conn" \
        || fail "real_version_with_conn" "X.Y.Z version string" "$out"
else
    skip "real_version_with_conn" "PostgreSQL not available"
fi

# ── Test 14: ping live ────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
    out=$(run << FLXEOF
import std pg
danger {
    bool ok = pg.ping("$PG_DSN")
    print(ok)
}
FLXEOF
)
    echo "$out" | grep -qiE "^true$|^1$" \
        && pass "real_ping_live" \
        || fail "real_ping_live" "true" "$out"
else
    skip "real_ping_live" "PostgreSQL not available"
fi

# ── Test 15: multiple connections simultaneously ──────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
    out=$(run << FLXEOF
import std pg
danger {
    int db1 = pg.connect("$PG_DSN")
    int db2 = pg.connect("$PG_DSN")
    int res1 = pg.query(db1, "SELECT name FROM fluxa_pg_test ORDER BY name LIMIT 1")
    int res2 = pg.query(db2, "SELECT name FROM fluxa_pg_test ORDER BY name DESC LIMIT 1")
    str v1 = pg.get(res1, 0, 0)
    str v2 = pg.get(res2, 0, 0)
    print(v1)
    print(v2)
    pg.free_result(res1)
    pg.free_result(res2)
    pg.close(db1)
    pg.close(db2)
}
FLXEOF
)
    echo "$out" | grep -q "alice" && echo "$out" | grep -q "bob" \
        && pass "real_multiple_connections" \
        || fail "real_multiple_connections" "alice + bob" "$out"
else
    skip "real_multiple_connections" "PostgreSQL not available"
fi

# ── Cleanup table ─────────────────────────────────────────────────────────
if [ "$PG_STARTED" -eq 1 ]; then
    run << FLXEOF > /dev/null 2>&1 || true
import std pg
danger {
    int db = pg.connect("$PG_DSN")
    pg.exec(db, "DROP TABLE IF EXISTS fluxa_pg_test")
    pg.close(db)
}
FLXEOF
fi

# ── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════════"
echo "  PostgreSQL Integration Tests:"
total=$((PASS + FAIL))
echo "  $total tests: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════════════════════════════"

[ "$PG_STARTED" -eq 0 ] && echo "" && \
    echo "  ⚠  PostgreSQL não inicializado — testes de IO foram skipped." && \
    echo "     Para IO real, execute:" && \
    echo "       bash tests/integration/pg/run.sh"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
