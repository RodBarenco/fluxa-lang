#!/usr/bin/env bash
# tests/libs/json.sh — std.json test suite
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.json/%s\n" "$1"; }
fail() { printf "  FAIL  std.json/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.json = "1.0"
TOML
}

echo "── std.json: JSON library ───────────────────────────────────────────"

# CASE 1: import std json without [libs] → clear error
cat > "$WORK_DIR/no_toml.flx" << 'FLX'
import std json
str s = json.object()
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/no_toml.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|toml\|libs"; then
    pass "import_without_toml_error"
else
    fail "import_without_toml_error" "error: not declared in [libs]" "$out"
fi

# CASE 2: json.object + json.set + json.get_*
P="$WORK_DIR/p2"; setup "$P" "build_object"
cat > "$P/main.flx" << 'FLX'
import std json
str obj = json.object()
obj = json.set(obj, "temp",   json.from_float(23.5))
obj = json.set(obj, "unit",   json.from_str("celsius"))
obj = json.set(obj, "active", json.from_bool(true))
float t  = json.get_float(obj, "temp")
str   u  = json.get_str(obj,   "unit")
bool  a  = json.get_bool(obj,  "active")
print(t)
print(u)
print(a)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "23.5" && echo "$out" | grep -q "celsius" \
    && echo "$out" | grep -q "true"; then
    pass "json_build_and_extract"
else
    fail "json_build_and_extract" "23.5, celsius, true" "$out"
fi

# CASE 3: json.get_int
P="$WORK_DIR/p3"; setup "$P" "get_int"
cat > "$P/main.flx" << 'FLX'
import std json
str obj = json.object()
obj = json.set(obj, "count", json.from_int(42))
int n = json.get_int(obj, "count")
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "json_get_int"
else
    fail "json_get_int" "42" "$out"
fi

# CASE 4: json.has
P="$WORK_DIR/p4"; setup "$P" "has"
cat > "$P/main.flx" << 'FLX'
import std json
str obj = json.object()
obj = json.set(obj, "x", json.from_int(1))
bool yes = json.has(obj, "x")
bool no  = json.has(obj, "y")
print(yes)
print(no)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "false"; then
    pass "json_has_key"
else
    fail "json_has_key" "true, false" "$out"
fi

# CASE 5: json.valid
P="$WORK_DIR/p5"; setup "$P" "valid"
cat > "$P/main.flx" << 'FLX'
import std json
bool ok  = json.valid("{\"a\":1}")
bool bad = json.valid("not json")
print(ok)
print(bad)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "true" \
    && echo "$out" | sed -n '2p' | grep -q "false"; then
    pass "json_valid"
else
    fail "json_valid" "true, false" "$out"
fi

# CASE 6: json.from_* conversions
P="$WORK_DIR/p6"; setup "$P" "from"
cat > "$P/main.flx" << 'FLX'
import std json
str si = json.from_int(99)
str sf = json.from_float(3.14)
str sb = json.from_bool(false)
str ss = json.from_str("hello")
print(si)
print(sf)
print(sb)
print(ss)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^99$" && echo "$out" | grep -q "3.14" \
    && echo "$out" | grep -q "false" && echo "$out" | grep -q '"hello"'; then
    pass "json_from_conversions"
else
    fail "json_from_conversions" '99, 3.14, false, "hello"' "$out"
fi

# CASE 7: json.stringify — dyn → JSON array str
P="$WORK_DIR/p7"; setup "$P" "stringify"
cat > "$P/main.flx" << 'FLX'
import std json
str a = json.from_int(1)
str b = json.from_str("hello")
str c = json.from_bool(true)
dyn d = [a, b, c]
str out_str = json.stringify(d)
print(out_str)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "1" && echo "$out" | grep -q "hello" \
    && echo "$out" | grep -q "true"; then
    pass "json_stringify_dyn"
else
    fail "json_stringify_dyn" '[1,"hello",true]' "$out"
fi

# CASE 8: json.load — read file as str
P="$WORK_DIR/p8"; setup "$P" "load_file"
JSON_FILE="$WORK_DIR/config.json"
echo '{"temp":23.5,"unit":"celsius","active":true}' > "$JSON_FILE"
cat > "$P/main.flx" << FLX
import std json
danger {
    str raw  = json.load("$JSON_FILE")
    float t  = json.get_float(raw, "temp")
    bool  ok = json.get_bool(raw, "active")
    print(t)
    print(ok)
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "23.5" && echo "$out" | grep -q "true"; then
    pass "json_load_file"
else
    fail "json_load_file" "23.5, true" "$out"
fi

# CASE 9: json.parse_array — JSON array string → dyn of str
P="$WORK_DIR/p9"; setup "$P" "parse_array"
cat > "$P/main.flx" << 'FLX'
import std json
str raw = "[{\"id\":1},{\"id\":2},{\"id\":3}]"
dyn items = json.parse_array(raw)
print(len(items))
int n = json.get_int(items[0], "id")
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^1$"; then
    pass "json_parse_array"
else
    fail "json_parse_array" "3 elements, first id=1" "$out"
fi

# CASE 10: json.get_* missing key → error captured in danger
P="$WORK_DIR/p10"; setup "$P" "missing_key"
cat > "$P/main.flx" << 'FLX'
import std json
str obj = "{\"a\":1}"
danger {
    float v = json.get_float(obj, "missing_key")
    print(v)
}
print(42)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "json_missing_key_captured_in_danger"
else
    fail "json_missing_key_captured_in_danger" "42 (error captured)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.json: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
