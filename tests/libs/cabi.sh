#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FLUXA="$ROOT/fluxa"
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
pass_n=0; fail_n=0
pass(){ echo "  PASS  libs/cabi/$1"; pass_n=$((pass_n+1)); }
fail(){ echo "  FAIL  libs/cabi/$1"; fail_n=$((fail_n+1)); }

toml(){ cat > "$P/fluxa.toml" <<'TOML'
[project]
name="cabi-test"
entry="main.flx"
[libs]
std.cabi="1.0"
TOML
}
run(){ toml; cat > "$P/main.flx"; "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1; }

echo "── std.cabi ─────────────────────────────────────────────────────"

out=$(cat > "$P/fluxa.toml" <<'TOML'
[project]
name="cabi-test"
entry="main.flx"
TOML
cat > "$P/main.flx" <<'FLX'
import std cabi
print(cabi.version())
FLX
"$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "library not declared" && pass import_without_toml_error || fail import_without_toml_error

out=$(run <<'FLX'
import std cabi
print(cabi.version())
FLX
); echo "$out" | grep -q '1.0.0' && pass version || fail version

out=$(run <<'FLX'
import std cabi
danger { int n = cabi.count() }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass no_context_error || fail no_context_error

for fn in read_int read_float read_bool read_str; do
out=$(run <<FLX
import std cabi
danger { cabi.$fn(0) }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass "${fn}_without_context" || fail "${fn}_without_context"
done

out=$(run <<'FLX'
import std cabi
int arr a[1] = 0
danger { cabi.read_int_arr(0, a) }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass read_int_arr_without_context || fail read_int_arr_without_context
out=$(run <<'FLX'
import std cabi
float arr a[1] = 0.0
danger { cabi.read_float_arr(0, a) }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass read_float_arr_without_context || fail read_float_arr_without_context
out=$(run <<'FLX'
import std cabi
bool arr a[1] = false
danger { cabi.read_bool_arr(0, a) }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass read_bool_arr_without_context || fail read_bool_arr_without_context
out=$(run <<'FLX'
import std cabi
str arr a[1] = ""
danger { cabi.read_str_arr(0, a) }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass read_str_arr_without_context || fail read_str_arr_without_context

out=$(run <<'FLX'
import std cabi
fn cabi_dispatch() int {
    int i = cabi.read_int(0)
    float f = cabi.read_float(1)
    bool b = cabi.read_bool(2)
    str s = cabi.read_str(3)
    int arr ai[3] = 0
    float arr af[3] = 0.0
    bool arr ab[3] = false
    str arr astr[3] = ""
    cabi.read_int_arr(4, ai)
    cabi.read_float_arr(5, af)
    cabi.read_bool_arr(6, ab)
    cabi.read_str_arr(7, astr)
    cabi.response_reset()
    cabi.write_int(i + 1)
    cabi.write_float(f + 0.5)
    cabi.write_bool(!b)
    cabi.write_str(s)
    cabi.write_int_arr(ai)
    cabi.write_float_arr(af)
    cabi.write_bool_arr(ab)
    cabi.write_str_arr(astr)
    return 0
}
print("ok")
FLX
); echo "$out" | grep -q ok && pass dispatcher_program || fail dispatcher_program

out=$(run <<'FLX'
import std cabi
danger { cabi.no_such_function() }
if err != nil { print("caught") }
FLX
); echo "$out" | grep -q caught && pass unknown_function_error || fail unknown_function_error

echo "────────────────────────────────────────────────────────────────"
echo "  → std.cabi: $pass_n passed, $fail_n failed"
if [ "$fail_n" -eq 0 ]; then echo "  → std.cabi: PASS"; exit 0; fi
exit 1
