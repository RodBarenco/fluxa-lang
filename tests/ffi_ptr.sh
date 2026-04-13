#!/usr/bin/env bash
# tests/ffi_ptr.sh — Issue 9.c-3: automatic FFI pointer type mapping
#
# The runtime automatically marshals Fluxa values to C pointer arguments
# and writes results back into the original variables — the user writes
# plain Fluxa types and the runtime handles everything invisibly.
#
# Pointer mapping (from spec):
#   int*           → int   variable  : pass &var, write back int32 after call
#   double*/float* → float variable  : pass &var, write back double after call
#   bool*          → bool  variable  : pass &var, write back bool after call
#   char*          → str   variable  : writable buffer, copy result back to str
#   uint8_t*/void* → int arr variable: flatten arr→bytes, scatter back after call
#   dyn            → opaque void*    : VAL_PTR extracted from dyn[0]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  ffi_ptr/%s\n" "$1"; }
fail() { printf "  FAIL  ffi_ptr/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── ffi_ptr: Issue 9.c-3 — automatic pointer type mapping ──────────"

# ── CASE 1: int* writeback — scanf reads int from stdin ──────────────────────
# scanf(char* fmt, int* out) -> int  (returns number of items matched)
# User writes: libc.scanf("%d", val)
# Runtime sees int* in sig → passes &val automatically → writes result back.
P="$WORK_DIR/p1"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
scanf = "(char*, int*) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
int val = 0
int matched = 0
danger {
    matched = libc.scanf("%d", val)
}
print(matched)
print(val)
FLX
out=$(echo "123" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^123$"; then
    pass "int_ptr_scanf_writeback"
else
    fail "int_ptr_scanf_writeback" "1 then 123" "$out"
fi

# ── CASE 2: double* writeback — scanf reads float from stdin ─────────────────
# scanf(char* fmt, double* out) -> int
# User writes: libc.scanf("%lf", val) with float val.
P="$WORK_DIR/p2"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
scanf = "(char*, double*) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
float val = 0.0
int matched = 0
danger {
    matched = libc.scanf("%lf", val)
}
print(matched)
print(val)
FLX
out=$(echo "3.14" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "3\.14"; then
    pass "double_ptr_scanf_writeback"
else
    fail "double_ptr_scanf_writeback" "1 then 3.14" "$out"
fi

# ── CASE 3: two int* in one call — both written back independently ────────────
# scanf reads two ints from one format string using two int* parameters.
P="$WORK_DIR/p3"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
scanf = "(char*, int*, int*) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
int a = 0
int b = 0
int matched = 0
danger {
    matched = libc.scanf("%d %d", a, b)
}
print(matched)
print(a)
print(b)
FLX
out=$(echo "10 20" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^10$" && echo "$out" | grep -q "^20$"; then
    pass "two_int_ptr_same_call"
else
    fail "two_int_ptr_same_call" "2 then 10 then 20" "$out"
fi

# ── CASE 4: int* no match — var stays unchanged when scanf fails ──────────────
# scanf("%d") on "abc" returns 0 (no items matched). val must not change.
P="$WORK_DIR/p4"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
scanf = "(char*, int*) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
int val = 99
int matched = 0
danger {
    matched = libc.scanf("%d", val)
}
print(matched)
print(val)
FLX
out=$(echo "abc" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^99$"; then
    pass "int_ptr_no_match_unchanged"
else
    fail "int_ptr_no_match_unchanged" "0 then 99" "$out"
fi

# ── CASE 5: char* writable — fgets reads a line from stdin into str ───────────
# fgets(char* buf, int size, FILE* stream) -> char*
# The runtime allocates a write buffer, passes it to fgets, copies the result
# back into the str variable. User writes: libc.fgets(buf, 64, fp)
P="$WORK_DIR/p5"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
fgets = "(char*, int, dyn) -> dyn"
fopen = "(char*, char*) -> dyn"
TOML
cat > "$P/main.flx" << 'FLX'
str buf = ""
danger {
    dyn fp = libc.fopen("/dev/stdin", "r")
    dyn ret = libc.fgets(buf, 64, fp)
}
print(buf)
FLX
out=$(echo "hello from fluxa" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "hello from fluxa"; then
    pass "char_ptr_fgets_writeback"
else
    fail "char_ptr_fgets_writeback" "hello from fluxa" "$out"
fi

# ── CASE 6: char* read-only — str passed as input, not modified ───────────────
# puts(char*) only reads the string. The str variable must be unchanged.
P="$WORK_DIR/p6"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
puts = "(char*) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
str msg = "hello from fluxa"
danger {
    int r = libc.puts(msg)
}
print(msg)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
# puts prints the string + newline; print(msg) prints it again
lines=$(echo "$out" | grep -c "hello from fluxa" || true)
if [ "$lines" -ge 2 ]; then
    pass "char_ptr_readonly_str_unchanged"
else
    fail "char_ptr_readonly_str_unchanged" "hello from fluxa twice" "$out"
fi

# ── CASE 7: uint8_t* arr writeback — fread fills arr with raw bytes ───────────
# fread(void* buf, size_t size, size_t count, FILE* fp) -> size_t
# The runtime flattens the int arr to a byte buffer, passes it to fread,
# then scatters the bytes back into the arr elements as integers.
# "ABC" → buf[0]=65, buf[1]=66, buf[2]=67
P="$WORK_DIR/p7"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
fread = "(uint8_t*, int, int, dyn) -> int"
fopen = "(char*, char*) -> dyn"
TOML
cat > "$P/main.flx" << 'FLX'
int arr buf[8] = [0,0,0,0,0,0,0,0]
int n = 0
danger {
    dyn fp = libc.fopen("/dev/stdin", "r")
    n = libc.fread(buf, 1, 8, fp)
}
print(n)
print(buf[0])
print(buf[1])
print(buf[2])
FLX
out=$(printf "ABC" | timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^3$" && \
   echo "$out" | grep -q "^65$" && \
   echo "$out" | grep -q "^66$" && \
   echo "$out" | grep -q "^67$"; then
    pass "uint8_arr_fread_writeback"
else
    fail "uint8_arr_fread_writeback" "3 then 65 66 67" "$out"
fi

# ── CASE 8: dyn opaque pointer — fopen/fclose round-trip ─────────────────────
# fopen returns void* → stored as VAL_PTR inside dyn[0].
# fclose receives that dyn, extracts the pointer, closes the file.
# Return value 0 = success.
P="$WORK_DIR/p8"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libc = "auto"

[ffi.libc.signatures]
fopen  = "(char*, char*) -> dyn"
fclose = "(dyn) -> int"
TOML
cat > "$P/main.flx" << 'FLX'
int result = -1
danger {
    dyn fp = libc.fopen("/etc/hostname", "r")
    result = libc.fclose(fp)
}
print(result)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^0$"; then
    pass "dyn_opaque_fopen_fclose"
else
    fail "dyn_opaque_fopen_fclose" "0 (fclose success)" "$out"
fi

# ── CASE 9: int* with libm frexp — pure math, no stdin ───────────────────────
# frexp(double x, int* exp) -> double
# frexp(8.0): mantissa=0.5, exp=4  because 8 == 0.5 * 2^4
P="$WORK_DIR/p9"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"

[ffi.libm.signatures]
frexp = "(double, int*) -> double"
TOML
cat > "$P/main.flx" << 'FLX'
int exp = 0
float mantissa = 0.0
danger {
    mantissa = libm.frexp(8.0, exp)
}
print(exp)
print(mantissa)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "0\.5"; then
    pass "int_ptr_frexp_libm"
else
    fail "int_ptr_frexp_libm" "4 then 0.5" "$out"
fi

# ── CASE 10: double* with libm modf — splits integer and fractional parts ─────
# modf(double x, double* iptr) -> double (fractional part)
# modf(3.7): fractional=0.7, whole=3.0
P="$WORK_DIR/p10"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"

[ffi.libm.signatures]
modf = "(double, double*) -> double"
TOML
cat > "$P/main.flx" << 'FLX'
float whole = 0.0
float frac = 0.0
danger {
    frac = libm.modf(3.7, whole)
}
print(whole)
print(frac)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^3$\|^3\.0" && echo "$out" | grep -q "0\.7"; then
    pass "double_ptr_modf_libm"
else
    fail "double_ptr_modf_libm" "3 then 0.7" "$out"
fi

# ── CASE 11: prst int — pointer writeback into persistent variable ────────────
# frexp(16.0): exp=5 because 16 == 0.5 * 2^5. The prst int must be updated.
P="$WORK_DIR/p11"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"

[ffi.libm.signatures]
frexp = "(double, int*) -> double"
TOML
cat > "$P/main.flx" << 'FLX'
prst int exp = 0
float m = 0.0
danger {
    m = libm.frexp(16.0, exp)
}
print(exp)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^5$"; then
    pass "prst_int_ptr_writeback"
else
    fail "prst_int_ptr_writeback" "5" "$out"
fi

# ── CASE 12: no sig — legacy value mode still works, no crash ────────────────
# Without a signature the runtime falls back to passing by value.
# sqrt(25.0) must return 5.0 correctly.
P="$WORK_DIR/p12"; mkdir -p "$P"
cat > "$P/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"
TOML
cat > "$P/main.flx" << 'FLX'
float r = 0.0
danger {
    r = libm.sqrt(25.0)
}
print(r)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>/dev/null || true)
if echo "$out" | grep -q "^5$\|^5\.0"; then
    pass "no_sig_value_call_no_crash"
else
    fail "no_sig_value_call_no_crash" "5 or 5.0" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=12
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → ffi_ptr: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
