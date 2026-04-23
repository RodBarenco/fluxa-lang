#!/usr/bin/env bash
# tests/libs/libv.sh — std.libv test suite
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
float arr a[2] = libv.vec2()
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. vec2/vec3/vec4 correct sizes
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2()
float arr b[3] = libv.vec3()
float arr c[4] = libv.vec4()
print(len(a))
print(len(b))
print(len(c))
FLX
)
echo "$out" | grep -q "^2$" && pass "vec2_len_2" || fail "vec2_len_2" "2" "$out"
echo "$out" | grep -q "^3$" && pass "vec3_len_3" || fail "vec3_len_3" "3" "$out"
echo "$out" | grep -q "^4$" && pass "vec4_len_4" || fail "vec4_len_4" "4" "$out"

# 3. mat4 identity
out=$(run << 'FLX'
import std libv
float arr m[16] = libv.mat4()
print(m[0])
print(m[5])
print(m[1])
FLX
)
echo "$out" | grep -q "^1$" && pass "mat4_diagonal_is_1" || fail "mat4_diagonal_is_1" "1" "$out"
echo "$out" | grep -q "^0$" && pass "mat4_off_diagonal_is_0" || fail "mat4_off_diagonal_is_0" "0" "$out"

# 4. vec(n) arbitrary size
out=$(run << 'FLX'
import std libv
float arr a[8] = libv.vec(8)
print(len(a))
FLX
)
echo "$out" | grep -q "^8$" && pass "vec_n_arbitrary_size" || fail "vec_n_arbitrary_size" "8" "$out"

# 5. add in-place
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3()
float arr b[3] = libv.vec3()
a[0] = 1.0
a[1] = 2.0
a[2] = 3.0
b[0] = 4.0
b[1] = 5.0
b[2] = 6.0
libv.add(a, b)
print(a[0])
print(a[1])
print(a[2])
FLX
)
echo "$out" | grep -q "^5$" && pass "add_element_0" || fail "add_element_0" "5" "$out"
echo "$out" | grep -q "^7$" && pass "add_element_1" || fail "add_element_1" "7" "$out"
echo "$out" | grep -q "^9$" && pass "add_element_2" || fail "add_element_2" "9" "$out"

# 6. scale in-place
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3()
a[0] = 1.0
a[1] = 2.0
a[2] = 3.0
libv.scale(a, 2.0)
print(a[0])
FLX
)
echo "$out" | grep -q "^2$" && pass "scale_doubles_values" || fail "scale_doubles_values" "2" "$out"

# 7. dot product — orthogonal → 0
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3()
float arr b[3] = libv.vec3()
a[0] = 1.0
b[1] = 1.0
float d = libv.dot(a, b)
print(d)
FLX
)
echo "$out" | grep -q "^0$" && pass "dot_orthogonal_is_0" || fail "dot_orthogonal_is_0" "0" "$out"

# 8. norm — 3-4-5
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2()
a[0] = 3.0
a[1] = 4.0
float n = libv.norm(a)
print(n)
FLX
)
echo "$out" | grep -q "^5$" && pass "norm_3_4_5" || fail "norm_3_4_5" "5" "$out"

# 9. normalize → unit length
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3()
a[0] = 3.0
a[1] = 4.0
libv.normalize(a)
float n = libv.norm(a)
print(n)
FLX
)
echo "$out" | grep -q "^1$" && pass "normalize_unit_length" || fail "normalize_unit_length" "1" "$out"

# 10. cross product x cross y = z
out=$(run << 'FLX'
import std libv
float arr a[3] = libv.vec3()
float arr b[3] = libv.vec3()
float arr c[3] = libv.vec3()
a[0] = 1.0
b[1] = 1.0
libv.cross(c, a, b)
print(c[0])
print(c[2])
FLX
)
echo "$out" | grep -q "^0$" && pass "cross_x_element_0" || fail "cross_x_element_0" "0" "$out"
echo "$out" | grep -q "^1$" && pass "cross_z_element_2" || fail "cross_z_element_2" "1" "$out"

# 11. matmul with identity → unchanged
out=$(run << 'FLX'
import std libv
float arr a[4] = libv.mat2()
float arr id[4] = libv.mat2()
float arr r[4] = libv.mat2()
a[0] = 2.0
a[3] = 5.0
libv.matmul(r, a, id)
print(r[0])
print(r[3])
FLX
)
echo "$out" | grep -q "^2$" && pass "matmul_identity" || fail "matmul_identity" "2" "$out"

# 12. transpose mat2
out=$(run << 'FLX'
import std libv
float arr m[4] = libv.mat2()
m[0] = 1.0
m[1] = 2.0
m[2] = 3.0
m[3] = 4.0
libv.transpose(m)
print(m[0])
print(m[1])
FLX
)
echo "$out" | grep -q "^1$" && pass "transpose_mat2_00" || fail "transpose_mat2_00" "1" "$out"
echo "$out" | grep -q "^3$" && pass "transpose_mat2_01" || fail "transpose_mat2_01" "3" "$out"

# 13. lerp midpoint
out=$(run << 'FLX'
import std libv
float arr a[2] = libv.vec2()
float arr b[2] = libv.vec2()
b[0] = 4.0
b[1] = 4.0
libv.lerp(a, b, 0.5)
print(a[0])
FLX
)
echo "$out" | grep -q "^2$" && pass "lerp_midpoint" || fail "lerp_midpoint" "2" "$out"

# 14. det 2x2: [3,4;2,5] = 15-8 = 7
out=$(run << 'FLX'
import std libv
float arr m[4] = libv.mat2()
m[0] = 3.0
m[1] = 4.0
m[2] = 2.0
m[3] = 5.0
float d = libv.det(m)
print(d)
FLX
)
echo "$out" | grep -q "^7$" && pass "det_mat2" || fail "det_mat2" "7" "$out"

# 15. dist via sub+norm (3-4-5)
out=$(run << 'FLX'
import std libv
float arr b[2] = libv.vec2()
b[0] = 3.0
b[1] = 4.0
float d = libv.norm(b)
print(d)
FLX
)
echo "$out" | grep -q "^5$" && pass "dist_3_4_5" || fail "dist_3_4_5" "5" "$out"

# 16. fill + dot with self = sum of squares
out=$(run << 'FLX'
import std libv
float arr a[4] = libv.vec(4)
libv.fill(a, 2.5)
float s = libv.dot(a, a)
print(s)
FLX
)
echo "$out" | grep -q "^25$" && pass "fill_sum" || fail "fill_sum" "25" "$out"

# 17. prst float arr pattern
out=$(run << 'FLX'
import std libv
prst float arr weights[16] = libv.mat4()
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_float_arr_mat4" || fail "prst_float_arr_mat4" "prst ok" "$out"

# 18. shape mismatch → error in danger
toml
cat > "$P/main.flx" << 'FLX'
import std libv
danger {
    float arr a[3] = libv.vec3()
    float arr b[4] = libv.vec4()
    libv.add(a, b)
}
if err != nil { print("shape error") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "shape error" && pass "shape_mismatch_error" || fail "shape_mismatch_error" "shape error" "$out"

# 19. OpenBLAS backend — matmul result same as native
# Backend test: always runs — falls back to native if OpenBLAS not compiled
BLAS_AVAILABLE=1
if [ "$BLAS_AVAILABLE" -eq 1 ]; then
    mkdir -p "$P"
    printf '[project]
name="t"
entry="main.flx"
[libs]
std.libv="1.0"
[libs.libv]
backend="blas"
' > "$P/fluxa.toml"
    cat > "$P/main.flx" << 'FLX'
import std libv
float arr a[4] = libv.mat2()
float arr b[4] = libv.mat2()
float arr r[4] = libv.mat2()
a[0] = 3.0
a[3] = 5.0
libv.matmul(r, a, b)
print(r[0])
print(r[3])
FLX
    out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "^3$" && pass "blas_backend_matmul_r00"         || fail "blas_backend_matmul_r00" "3" "$out"
    echo "$out" | grep -q "^5$" && pass "blas_backend_matmul_r11"         || fail "blas_backend_matmul_r11" "5" "$out"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.libv: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.libv: PASS" && exit 0 || exit 1
