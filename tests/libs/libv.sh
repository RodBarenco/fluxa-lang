#!/usr/bin/env bash
# tests/libs/liblibv.sh — std.libv test suite
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/libv/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/libv/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.libv="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.libv ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std libv
float arr a[3] = libv.vec3
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. vec2/vec3/vec4 initializers
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2
float arr b[3] = libv.vec3
float arr c[4] = libv.vec4
print(libv.len(a))
print(libv.len(b))
print(libv.len(c))
FLX
)
echo "$out" | grep -q "^2" && pass "vec2_len_2" || fail "vec2_len_2" "2" "$out"
echo "$out" | grep -q "^3" && pass "vec3_len_3" || fail "vec3_len_3" "3" "$out"
echo "$out" | grep -q "^4" && pass "vec4_len_4" || fail "vec4_len_4" "4" "$out"

# 3. mat4 is identity
out=$(run << 'FLX'
import std libv
float arr m[16] = libv.mat4
print(libv.get(m, 0))
print(libv.get(m, 5))
print(libv.get(m, 10))
print(libv.get(m, 15))
print(libv.get(m, 1))
FLX
)
echo "$out" | grep -q "^1$" && pass "mat4_diagonal_is_1" || fail "mat4_diagonal_is_1" "1" "$out"
echo "$out" | grep -q "^0$" && pass "mat4_off_diagonal_is_0" || fail "mat4_off_diagonal_is_0" "0" "$out"

# 4. vec(n) — arbitrary size
out=$(run << 'FLX'
import std libv
float arr a[8] = libv.vec(8)
print(libv.len(a))
FLX
)
echo "$out" | grep -q "^8$" && pass "vec_n_arbitrary_size" || fail "vec_n_arbitrary_size" "8" "$out"

# 5. add in-place
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3
float arr b[3] = libv.vec3
libv.set(a, 0, 1.0)
libv.set(a, 1, 2.0)
libv.set(a, 2, 3.0)
libv.set(b, 0, 4.0)
libv.set(b, 1, 5.0)
libv.set(b, 2, 6.0)
libv.add(a, b)
print(libv.get(a, 0))
print(libv.get(a, 1))
print(libv.get(a, 2))
FLX
)
echo "$out" | grep -q "^5$"  && pass "add_element_0" || fail "add_element_0" "5" "$out"
echo "$out" | grep -q "^7$"  && pass "add_element_1" || fail "add_element_1" "7" "$out"
echo "$out" | grep -q "^9$"  && pass "add_element_2" || fail "add_element_2" "9" "$out"

# 6. scale
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3
libv.set(a, 0, 3.0)
libv.set(a, 1, 4.0)
libv.set(a, 2, 0.0)
libv.scale(a, 2.0)
print(libv.get(a, 0))
FLX
)
echo "$out" | grep -q "^6$" && pass "scale_doubles_values" || fail "scale_doubles_values" "6" "$out"

# 7. dot product
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3
float arr b[3] = libv.vec3
libv.set(a, 0, 1.0)
libv.set(a, 1, 0.0); libv.set(a, 2, 0.0)
libv.set(b, 0, 0.0)
libv.set(b, 1, 1.0); libv.set(b, 2, 0.0)
float d = libv.dot(a, b)
print(d)
FLX
)
echo "$out" | grep -q "^0$" && pass "dot_orthogonal_is_0" || fail "dot_orthogonal_is_0" "0" "$out"

# 8. norm — 3-4-5 triangle
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2
libv.set(a, 0, 3.0)
libv.set(a, 1, 4.0)
float n = libv.norm(a)
print(n)
FLX
)
echo "$out" | grep -q "^5$" && pass "norm_3_4_5" || fail "norm_3_4_5" "5" "$out"

# 9. normalize
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3
libv.set(a, 0, 0.0)
libv.set(a, 1, 5.0)
libv.set(a, 2, 0.0)
libv.normalize(a)
float n = libv.norm(a)
print(n)
FLX
)
echo "$out" | grep -q "^1$" && pass "normalize_unit_length" || fail "normalize_unit_length" "1" "$out"

# 10. cross product — X×Y = Z
out=$(run << 'FLX'
import std libv
float arr x[3] = libv.vec3
float arr y[3] = libv.vec3
float arr z[3] = libv.vec3
libv.set(x, 0, 1.0)
libv.set(x, 1, 0.0); libv.set(x, 2, 0.0)
libv.set(y, 0, 0.0)
libv.set(y, 1, 1.0); libv.set(y, 2, 0.0)
libv.cross(z, x, y)
print(libv.get(z, 0))
print(libv.get(z, 1))
print(libv.get(z, 2))
FLX
)
echo "$out" | grep -q "^0$" && pass "cross_x_element_0" || fail "cross_x_element_0" "0" "$out"
echo "$out" | grep -q "^1$" && pass "cross_z_element_2" || fail "cross_z_element_2" "1" "$out"

# 11. matmul identity
out=$(run << 'FLX'
import std libv
float arr m[16] = libv.mat4
float arr r[16] = libv.mat4
libv.matmul(r, m, m)
print(libv.get(r, 0))
print(libv.get(r, 5))
FLX
)
echo "$out" | grep -q "^1$" && pass "matmul_identity" || fail "matmul_identity" "1" "$out"

# 12. transpose mat2
out=$(run << 'FLX'
import std libv
float arr m[4] = libv.mat(2,2)
libv.set(m, 0, 1.0)
libv.set(m, 1, 2.0)
libv.set(m, 2, 3.0)
libv.set(m, 3, 4.0)
libv.transpose(m)
print(libv.get(m, 0))
print(libv.get(m, 1))
print(libv.get(m, 2))
FLX
)
echo "$out" | grep -q "^1$" && pass "transpose_mat2_00" || fail "transpose_mat2_00" "1" "$out"
echo "$out" | grep -q "^3$" && pass "transpose_mat2_01" || fail "transpose_mat2_01" "3" "$out"

# 13. shape mismatch → error in danger
toml
cat > "$P/main.flx" << 'FLX'
import std libv
danger {
    float arr a[3] = libv.vec3
    float arr b[4] = libv.vec4
    libv.add(a, b)
}
if err != nil { print("shape error") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "shape error" && pass "shape_mismatch_error" || fail "shape_mismatch_error" "shape error" "$out"

# 14. lerp
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2
float arr b[2] = libv.vec2
libv.set(a, 0, 0.0)
libv.set(a, 1, 0.0)
libv.set(b, 0, 10.0)
libv.set(b, 1, 10.0)
libv.lerp(a, b, 0.5)
print(libv.get(a, 0))
FLX
)
echo "$out" | grep -q "^5$" && pass "lerp_midpoint" || fail "lerp_midpoint" "5" "$out"

# 15. det mat2
out=$(run << 'FLX'
import std libv
float arr m[4] = libv.mat2
libv.set(m, 0, 3.0)
libv.set(m, 1, 4.0)
libv.set(m, 2, 2.0)
libv.set(m, 3, 1.0)
float d = libv.det(m)
print(d)
FLX
)
echo "$out" | grep -qE "^-5|^-5\.0" && pass "det_mat2" || fail "det_mat2" "-5" "$out"

# 16. dist
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2
float arr b[2] = libv.vec2
libv.set(a, 0, 0.0)
libv.set(a, 1, 0.0)
libv.set(b, 0, 3.0)
libv.set(b, 1, 4.0)
float d = libv.dist(a, b)
print(d)
FLX
)
echo "$out" | grep -q "^5$" && pass "dist_3_4_5" || fail "dist_3_4_5" "5" "$out"

# 17. fill + sum
out=$(run << 'FLX'
import std libv
float arr a[4] = libv.vec4
libv.fill(a, 2.0)
float s = libv.sum(a)
print(s)
FLX
)
echo "$out" | grep -q "^8$" && pass "fill_sum" || fail "fill_sum" "8" "$out"

# 18. prst float arr survives pattern
out=$(run << 'FLX'
import std libv
prst float arr weights[16] = libv.mat4
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_float_arr_mat4" || fail "prst_float_arr_mat4" "prst ok" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.libv: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.libv: PASS" && exit 0 || exit 1
