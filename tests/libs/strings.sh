#!/usr/bin/env bash
# tests/libs/str.sh — std.str test suite
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.strings/%s\n" "$1"; }
fail() { printf "  FAIL  std.strings/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.strings = "1.0"
TOML
}

echo "── std.str: string library ──────────────────────────────────────────"

# CASE 1: split
P="$WORK_DIR/p1"; setup "$P" "split"
cat > "$P/main.flx" << 'FLX'
import std strings
dyn parts = strings.split("a,b,c,d", ",")
print(len(parts))
print(parts[0])
print(parts[3])
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "^a$" \
    && echo "$out" | grep -q "^d$"; then
    pass "split"
else
    fail "split" "4, a, d" "$out"
fi

# CASE 2: join
P="$WORK_DIR/p2"; setup "$P" "join"
cat > "$P/main.flx" << 'FLX'
import std strings
dyn parts = ["hello", "world", "fluxa"]
str result = strings.join(parts, " ")
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "hello world fluxa"; then
    pass "join"
else
    fail "join" "hello world fluxa" "$out"
fi

# CASE 3: split then join roundtrip
P="$WORK_DIR/p3"; setup "$P" "roundtrip"
cat > "$P/main.flx" << 'FLX'
import std strings
str original = "one:two:three"
dyn parts    = strings.split(original, ":")
str rejoined = strings.join(parts, ":")
print(rejoined)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "one:two:three"; then
    pass "split_join_roundtrip"
else
    fail "split_join_roundtrip" "one:two:three" "$out"
fi

# CASE 4: trim
P="$WORK_DIR/p4"; setup "$P" "trim"
cat > "$P/main.flx" << 'FLX'
import std strings
str t = strings.trim("  hello world  ")
print(t)
str t2 = strings.trim("noSpaces")
print(t2)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^hello world$" && echo "$out" | grep -q "^noSpaces$"; then
    pass "trim"
else
    fail "trim" "hello world, noSpaces" "$out"
fi

# CASE 5: find
P="$WORK_DIR/p5"; setup "$P" "find"
cat > "$P/main.flx" << 'FLX'
import std strings
int pos  = strings.find("hello world", "world")
int none = strings.find("hello world", "xyz")
print(pos)
print(none)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^6$" && echo "$out" | grep -q "^-1$"; then
    pass "find"
else
    fail "find" "6, -1" "$out"
fi

# CASE 6: replace
P="$WORK_DIR/p6"; setup "$P" "replace"
cat > "$P/main.flx" << 'FLX'
import std strings
str s = strings.replace("aabbaa", "aa", "X")
print(s)
str s2 = strings.replace("hello", "xyz", "!")
print(s2)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^XbbX$" && echo "$out" | grep -q "^hello$"; then
    pass "replace"
else
    fail "replace" "XbbX, hello" "$out"
fi

# CASE 7: starts_with / ends_with / contains
P="$WORK_DIR/p7"; setup "$P" "predicates"
cat > "$P/main.flx" << 'FLX'
import std strings
bool sw = strings.starts_with("fluxa-lang", "fluxa")
bool ew = strings.ends_with("fluxa-lang", "lang")
bool cn = strings.contains("hello world", "world")
bool no = strings.contains("hello", "xyz")
print(sw)
print(ew)
print(cn)
print(no)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "true" \
    && echo "$out" | sed -n '3p' | grep -q "true" \
    && echo "$out" | sed -n '4p' | grep -q "false"; then
    pass "starts_with_ends_with_contains"
else
    fail "starts_with_ends_with_contains" "true, true, true, false" "$out"
fi

# CASE 8: count
P="$WORK_DIR/p8"; setup "$P" "count"
cat > "$P/main.flx" << 'FLX'
import std strings
int n1 = strings.count("banana", "an")
int n2 = strings.count("hello", "xyz")
int n3 = strings.count("aaa", "a")
print(n1)
print(n2)
print(n3)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^3$"; then
    pass "count"
else
    fail "count" "2, 0, 3" "$out"
fi

# CASE 9: lower / upper
P="$WORK_DIR/p9"; setup "$P" "case"
cat > "$P/main.flx" << 'FLX'
import std strings
str lo = strings.lower("Hello World")
str up = strings.upper("hello world")
print(lo)
print(up)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "hello world" && echo "$out" | grep -q "HELLO WORLD"; then
    pass "lower_upper"
else
    fail "lower_upper" "hello world, HELLO WORLD" "$out"
fi

# CASE 10: slice
P="$WORK_DIR/p10"; setup "$P" "slice"
cat > "$P/main.flx" << 'FLX'
import std strings
str s1 = strings.slice("hello world", 6, 11)
str s2 = strings.slice("fluxa", 0, 3)
print(s1)
print(s2)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^world$" && echo "$out" | grep -q "^flu$"; then
    pass "slice"
else
    fail "slice" "world, flu" "$out"
fi

# CASE 11: repeat
P="$WORK_DIR/p11"; setup "$P" "repeat"
cat > "$P/main.flx" << 'FLX'
import std strings
str s = strings.repeat("ab", 3)
str z = strings.repeat("x", 0)
print(s)
print(z)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
line1=$(echo "$out" | sed -n '1p')
line2=$(echo "$out" | sed -n '2p')
if [ "$line1" = "ababab" ] && [ -z "$line2" ]; then
    pass "repeat"
else
    fail "repeat" "ababab then empty string" "$out"
fi

# CASE 12: math.approx
P="$WORK_DIR/p12"
mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "approx"
entry = "main.flx"
[libs]
std.math = "1.0"
TOML
cat > "$P/main.flx" << 'FLX'
import std math
bool ok1 = math.approx(0.1 + 0.2, 0.3)
bool ok2 = math.approx(1.0, 1.001, 0.01)
bool bad = math.approx(1.0, 2.0)
print(ok1)
print(ok2)
print(bad)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "true" \
    && echo "$out" | sed -n '3p' | grep -q "false"; then
    pass "math_approx"
else
    fail "math_approx" "true, true, false" "$out"
fi

# CASE 13: CSV with custom delimiter (TSV)
P="$WORK_DIR/p13"
mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "csv_delim"
entry = "main.flx"
[libs]
std.csv = "1.0"
TOML
TSV_FILE="$WORK_DIR/data.tsv"
printf "id\tname\tval\n1\talice\t42\n2\tbob\t99\n" > "$TSV_FILE"
cat > "$P/main.flx" << FLX
import std csv
danger {
    dyn rows = csv.load("$TSV_FILE")
    str row1 = rows[1]
    str name = csv.field(row1, 1, "\t")
    str val  = csv.field(row1, 2, "\t")
    print(name)
    print(val)
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^alice$" && echo "$out" | grep -q "^42$"; then
    pass "csv_custom_delimiter_tsv"
else
    fail "csv_custom_delimiter_tsv" "alice, 42" "$out"
fi

# CASE 14: CSV quoted fields with FSM
P="$WORK_DIR/p14"
mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[project]
name = "csv_quoted"
entry = "main.flx"
[libs]
std.csv = "1.0"
TOML
cat > "$P/main.flx" << 'FLX'
import std csv
str row = "alice,\"hello, world\",42"
str f0  = csv.field(row, 0)
str f1  = csv.field(row, 1)
str f2  = csv.field(row, 2)
int n   = csv.field_count(row)
print(f0)
print(f1)
print(f2)
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^alice$" && echo "$out" | grep -q "hello, world" \
    && echo "$out" | grep -q "^42$" && echo "$out" | grep -q "^3$"; then
    pass "csv_quoted_fields_fsm"
else
    fail "csv_quoted_fields_fsm" "alice, hello world, 42, 3 fields" "$out"
fi

# CASE 15: str with prst
P="$WORK_DIR/p15"; setup "$P" "str_prst"
cat > "$P/main.flx" << 'FLX'
import std strings
prst str label = "sensor_01"
bool starts = strings.starts_with(label, "sensor")
str  upper  = strings.upper(label)
print(starts)
print(upper)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "true" && echo "$out" | grep -q "SENSOR_01"; then
    pass "str_with_prst_vars"
else
    fail "str_with_prst_vars" "true, SENSOR_01" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=15
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.strings: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
