#!/usr/bin/env bash
# tests/libs/sqlite.sh — std.sqlite test suite
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0
DB="$P/test.db"

pass() { printf "  PASS  libs/sqlite/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/sqlite/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.sqlite="1.0"\n' > "$P/fluxa.toml"; }
run() {
    toml
    cat > "$P/main.flx"
    timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

echo "── std.sqlite ───────────────────────────────────────────────────"

# Check sqlite3 is available in this build
if ! echo "" | grep -q "FLUXA_STD_SQLITE" <<< "$(make -n build 2>/dev/null)"; then
    # Just try to use it — if not compiled in, skip
    :
fi

# 1. version() returns a string
out=$(run << 'FLX'
import std sqlite
danger {
    str v = sqlite.version()
    print(v)
}
FLX
)
echo "$out" | grep -qE "^3\.[0-9]" && pass "version_returns_string" || fail "version_returns_string" "3.x.x" "$out"

# 2. open() creates a db file
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    print("opened")
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -q "opened" && pass "open_and_close" || fail "open_and_close" "opened" "$out"

# 3. exec() — DDL: create table
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    sqlite.exec(db, "CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, name TEXT, val REAL)")
    print("created")
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -q "created" && pass "exec_create_table" || fail "exec_create_table" "created" "$out"

# 4. exec() — DML: insert rows
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    sqlite.exec(db, "INSERT INTO t (name, val) VALUES ('alpha', 1.5)")
    sqlite.exec(db, "INSERT INTO t (name, val) VALUES ('beta',  2.5)")
    print("inserted")
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -q "inserted" && pass "exec_insert" || fail "exec_insert" "inserted" "$out"

# 5. last_insert_id()
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    sqlite.exec(db, "INSERT INTO t (name, val) VALUES ('gamma', 3.5)")
    int lid = sqlite.last_insert_id(db)
    print(lid)
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -qE "^[0-9]+$" && pass "last_insert_id" || fail "last_insert_id" "integer" "$out"

# 6. changes()
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    sqlite.exec(db, "UPDATE t SET val = val + 1 WHERE val < 3.0")
    int ch = sqlite.changes(db)
    print(ch)
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -qE "^[0-9]+$" && pass "changes_returns_int" || fail "changes_returns_int" "integer" "$out"

# 7. query() — returns dyn of rows
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    dyn rows = sqlite.query(db, "SELECT name, val FROM t ORDER BY id LIMIT 2")
    print(len(rows))
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "query_returns_rows" || fail "query_returns_rows" ">=1 rows" "$out"

# 8. query() — row fields accessible
out=$(run << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    dyn rows = sqlite.query(db, "SELECT name FROM t WHERE id = 1")
    if len(rows) > 0 {
        dyn row = rows[0]
        print(row[0])
    }
    sqlite.close(db)
}
FLX
)
echo "$out" | grep -q "alpha" && pass "query_row_field_accessible" || fail "query_row_field_accessible" "alpha" "$out"

# 9. query() — bad SQL → error captured in danger
printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.sqlite="1.0"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << FLX
import std sqlite
danger {
    dyn db = sqlite.open("$DB")
    dyn rows = sqlite.query(db, "SELECT * FROM nonexistent_table")
    sqlite.close(db)
}
if err != nil { print(err[0]) }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "no such table|error" && pass "bad_query_error_captured" || fail "bad_query_error_captured" "error" "$out"

# 10. open() non-existent dir → error
printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.sqlite="1.0"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std sqlite
danger {
    dyn db = sqlite.open("/nonexistent/path/db.sqlite")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "open_bad_path_error" || fail "open_bad_path_error" "error caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.sqlite: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.sqlite: PASS"
[ "$FAILS" -eq 0 ] && exit 0 || exit 1
