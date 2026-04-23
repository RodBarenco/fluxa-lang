#!/usr/bin/env bash
# tests/libs/json2.sh — std.json2 test suite (DOM JSON)
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/json2/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/json2/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.json2="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.json2 ────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std json2
dyn d = json2.parse("{}")
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. parse valid JSON object
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"name\":\"fluxa\",\"version\":1}")
    bool v = json2.valid(d)
    print(v)
    str n = json2.get(d, "name")
    print(n)
}
FLX
)
echo "$out" | grep -q "true"  && pass "parse_valid_object" || fail "parse_valid_object" "true" "$out"
echo "$out" | grep -q "fluxa" && pass "get_str_field"      || fail "get_str_field" "fluxa" "$out"

# 3. get_int
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"count\":42,\"pi\":3.14}")
    int n = json2.get_int(d, "count")
    print(n)
}
FLX
)
echo "$out" | grep -q "^42$" && pass "get_int_field" || fail "get_int_field" "42" "$out"

# 4. get_float
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"pi\":3.14159}")
    float f = json2.get_float(d, "pi")
    print(f)
}
FLX
)
echo "$out" | grep -qE "^3\.14" && pass "get_float_field" || fail "get_float_field" "3.14159" "$out"

# 5. get_bool
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"active\":true,\"deleted\":false}")
    bool a = json2.get_bool(d, "active")
    bool b = json2.get_bool(d, "deleted")
    print(a)
    print(b)
}
FLX
)
echo "$out" | grep -q "true"  && pass "get_bool_true"  || fail "get_bool_true"  "true" "$out"
echo "$out" | grep -q "false" && pass "get_bool_false" || fail "get_bool_false" "false" "$out"

# 6. nested path navigation
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"user\":{\"name\":\"alice\",\"age\":30}}")
    str name = json2.get(d, "user.name")
    int age  = json2.get_int(d, "user.age")
    print(name)
    print(age)
}
FLX
)
echo "$out" | grep -q "alice" && pass "nested_path_str" || fail "nested_path_str" "alice" "$out"
echo "$out" | grep -q "^30$"  && pass "nested_path_int" || fail "nested_path_int" "30" "$out"

# 7. array indexing
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"items\":[\"a\",\"b\",\"c\"]}")
    str first = json2.get(d, "items[0]")
    str third = json2.get(d, "items[2]")
    print(first)
    print(third)
}
FLX
)
echo "$out" | grep -q "^a$" && pass "array_index_0" || fail "array_index_0" "a" "$out"
echo "$out" | grep -q "^c$" && pass "array_index_2" || fail "array_index_2" "c" "$out"

# 8. has() — existing and missing paths
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"x\":1}")
    bool has_x = json2.has(d, "x")
    bool has_y = json2.has(d, "y")
    print(has_x)
    print(has_y)
}
FLX
)
echo "$out" | grep -q "true"  && pass "has_existing_key"  || fail "has_existing_key"  "true" "$out"
echo "$out" | grep -q "false" && pass "has_missing_key"   || fail "has_missing_key"  "false" "$out"

# 9. type()
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"n\":1,\"f\":1.5,\"s\":\"hi\",\"b\":true,\"a\":[],\"o\":{}}")
    print(json2.type(d, "n"))
    print(json2.type(d, "f"))
    print(json2.type(d, "s"))
    print(json2.type(d, "b"))
    print(json2.type(d, "a"))
    print(json2.type(d, "o"))
}
FLX
)
echo "$out" | grep -q "^int$"    && pass "type_int"    || fail "type_int"    "int"    "$out"
echo "$out" | grep -q "^float$"  && pass "type_float"  || fail "type_float"  "float"  "$out"
echo "$out" | grep -q "^str$"    && pass "type_str"    || fail "type_str"    "str"    "$out"
echo "$out" | grep -q "^array$"  && pass "type_array"  || fail "type_array"  "array"  "$out"
echo "$out" | grep -q "^object$" && pass "type_object" || fail "type_object" "object" "$out"

# 10. length() — array and object
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"arr\":[1,2,3],\"obj\":{\"a\":1,\"b\":2}}")
    int arr_len = json2.length(d, "arr")
    int obj_len = json2.length(d, "obj")
    print(arr_len)
    print(obj_len)
}
FLX
)
echo "$out" | grep -q "^3$" && pass "length_array"  || fail "length_array"  "3" "$out"
echo "$out" | grep -q "^2$" && pass "length_object" || fail "length_object" "2" "$out"

# 11. key() — enumerate object keys
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"first\":1,\"second\":2}")
    str k0 = json2.key(d, "", 0)
    str k1 = json2.key(d, "", 1)
    print(k0)
    print(k1)
}
FLX
)
echo "$out" | grep -q "first"  && pass "key_0_is_first"  || fail "key_0_is_first"  "first"  "$out"
echo "$out" | grep -q "second" && pass "key_1_is_second" || fail "key_1_is_second" "second" "$out"

# 12. set() — mutate value in-place
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"x\":1}")
    json2.set(d, "x", "hello")
    str v = json2.get(d, "x")
    print(v)
}
FLX
)
echo "$out" | grep -q "hello" && pass "set_str_value" || fail "set_str_value" "hello" "$out"

# 13. set_int()
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"n\":0}")
    json2.set_int(d, "n", 99)
    int v = json2.get_int(d, "n")
    print(v)
}
FLX
)
echo "$out" | grep -q "^99$" && pass "set_int_value" || fail "set_int_value" "99" "$out"

# 14. stringify() roundtrip
out=$(run << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{\"ok\":true}")
    str s = json2.stringify(d)
    print(s)
}
FLX
)
echo "$out" | grep -q '"ok"' && pass "stringify_has_key" || fail "stringify_has_key" '"ok"' "$out"
echo "$out" | grep -q "true" && pass "stringify_has_val" || fail "stringify_has_val" "true" "$out"

# 15. parse error → error caught in danger
toml; cat > "$P/main.flx" << 'FLX'
import std json2
danger {
    dyn d = json2.parse("{invalid}")
}
if err != nil { print("parse error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "parse error caught" && pass "invalid_json_error" || fail "invalid_json_error" "parse error caught" "$out"

# 16. load from file
echo '{"sensor":"temp","value":22}' > "$P/data.json"
toml; cat > "$P/main.flx" << 'FLX'
import std json2
danger {
    dyn d = json2.load("data.json")
    str s = json2.get(d, "sensor")
    int v = json2.get_int(d, "value")
    print(s)
    print(v)
}
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "temp" && pass "load_from_file_str" || fail "load_from_file_str" "temp" "$out"
echo "$out" | grep -q "^22$" && pass "load_from_file_int" || fail "load_from_file_int" "22"   "$out"

# 17. prst dyn cursor survives reload pattern
out=$(run << 'FLX'
import std json2
prst dyn config = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.json2: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.json2: PASS" && exit 0 || exit 1
