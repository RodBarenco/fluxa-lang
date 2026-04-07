#!/usr/bin/env bash
# tests/libs/csv.sh — std.csv test suite
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.csv/%s\n" "$1"; }
fail() { printf "  FAIL  std.csv/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.csv = "1.0"
TOML
}

# Create a sample CSV file used across tests
CSV_FILE="$WORK_DIR/sensors.csv"
cat > "$CSV_FILE" << 'CSV'
sensor_id,temp,humidity,active
s001,23.5,60,true
s002,31.2,45,false
s003,19.8,72,true
CSV

echo "── std.csv: CSV library ─────────────────────────────────────────────"

# CASE 1: import std csv without [libs] → clear error
cat > "$WORK_DIR/no_toml.flx" << 'FLX'
import std csv
dyn d = csv.load("x.csv")
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/no_toml.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|toml\|libs"; then
    pass "import_without_toml_error"
else
    fail "import_without_toml_error" "error: not declared in [libs]" "$out"
fi

# CASE 2: csv.load — load all lines
P="$WORK_DIR/p2"; setup "$P" "load"
cat > "$P/main.flx" << FLX
import std csv
danger {
    dyn d = csv.load("$CSV_FILE")
    print(len(d))
    print(d[0])
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "sensor_id"; then
    pass "csv_load_all_lines"
else
    fail "csv_load_all_lines" "4 lines, first is header" "$out"
fi

# CASE 3: csv.field — extract field by index
P="$WORK_DIR/p3"; setup "$P" "field"
cat > "$P/main.flx" << FLX
import std csv
str row = "s001,23.5,60,true"
str id   = csv.field(row, 0)
str temp = csv.field(row, 1)
str last = csv.field(row, 3)
print(id)
print(temp)
print(last)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^s001$" && echo "$out" | grep -q "^23.5$" \
    && echo "$out" | grep -q "^true$"; then
    pass "csv_field_extract"
else
    fail "csv_field_extract" "s001, 23.5, true" "$out"
fi

# CASE 4: csv.field_count
P="$WORK_DIR/p4"; setup "$P" "field_count"
cat > "$P/main.flx" << 'FLX'
import std csv
str row = "a,b,c,d,e"
int n = csv.field_count(row)
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^5$"; then
    pass "csv_field_count"
else
    fail "csv_field_count" "5" "$out"
fi

# CASE 5: csv.skip — skip header row
P="$WORK_DIR/p5"; setup "$P" "skip"
cat > "$P/main.flx" << FLX
import std csv
danger {
    dyn all  = csv.load("$CSV_FILE")
    dyn data = csv.skip(all, 1)
    print(len(data))
    print(csv.field(data[0], 0))
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^s001$"; then
    pass "csv_skip_header"
else
    fail "csv_skip_header" "3 rows, first id=s001" "$out"
fi

# CASE 6: csv.chunk — load in chunks
P="$WORK_DIR/p6"; setup "$P" "chunk"
cat > "$P/main.flx" << FLX
import std csv
danger {
    dyn c1 = csv.chunk("$CSV_FILE", 2)
    dyn c2 = csv.chunk("$CSV_FILE", 2)
    print(len(c1))
    print(len(c2))
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
# Both calls load from offset 0 (simple mode) — each returns 2 lines
if echo "$out" | grep -q "^2$"; then
    pass "csv_chunk_simple"
else
    fail "csv_chunk_simple" "2 lines per chunk" "$out"
fi

# CASE 7: csv.open / csv.next / csv.close — cursor mode
P="$WORK_DIR/p7"; setup "$P" "cursor"
cat > "$P/main.flx" << FLX
import std csv
prst dyn cur = csv.open("$CSV_FILE")
danger {
    dyn chunk = csv.next(cur, 2)
    print(len(chunk))
    dyn chunk2 = csv.next(cur, 10)
    print(len(chunk2))
    bool done = csv.is_eof(cur)
    print(done)
    csv.close(cur)
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^2$" \
    && echo "$out" | grep -q "true"; then
    pass "csv_cursor_open_next_close"
else
    fail "csv_cursor_open_next_close" "2 first, 2 remaining, eof=true" "$out"
fi

# CASE 8: csv.save — write dyn to file
P="$WORK_DIR/p8"; setup "$P" "save"
OUTCSV="$WORK_DIR/out.csv"
cat > "$P/main.flx" << FLX
import std csv
danger {
    dyn rows = ["a,1,true", "b,2,false", "c,3,true"]
    csv.save(rows, "$OUTCSV")
    dyn back = csv.load("$OUTCSV")
    print(len(back))
    print(csv.field(back[0], 0))
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^a$"; then
    pass "csv_save_and_reload"
else
    fail "csv_save_and_reload" "3 lines, first field=a" "$out"
fi

# CASE 9: csv.field on nonexistent file → error in danger
P="$WORK_DIR/p9"; setup "$P" "missing_file"
cat > "$P/main.flx" << 'FLX'
import std csv
danger {
    dyn d = csv.load("/nonexistent/path/data.csv")
    print(len(d))
}
print(42)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "csv_missing_file_captured_in_danger"
else
    fail "csv_missing_file_captured_in_danger" "42 (error captured)" "$out"
fi

# CASE 10: cursor pattern with prst — iterate full file
P="$WORK_DIR/p10"; setup "$P" "full_iterate"
BIGCSV="$WORK_DIR/big.csv"
python3 -c "
print('id,val')
for i in range(50):
    print(f'{i},{i*2}')
" > "$BIGCSV"
cat > "$P/main.flx" << FLX
import std csv
prst dyn cur    = csv.open("$BIGCSV")
prst int total  = 0
danger {
    dyn chunk = csv.next(cur, 20)
    while len(chunk) > 0 {
        total = total + len(chunk)
        chunk = csv.next(cur, 20)
    }
    csv.close(cur)
}
print(total)
FLX
out=$(timeout 10s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
# 51 lines total (1 header + 50 data)
if echo "$out" | grep -q "^51$"; then
    pass "csv_cursor_iterate_full_file"
else
    fail "csv_cursor_iterate_full_file" "51 (all lines)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.csv: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
