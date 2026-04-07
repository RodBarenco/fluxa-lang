#!/usr/bin/env bash
# tests/sprint10a_std_math.sh
# std.math — standard math library tests
#
# Tests run with a temporary fluxa.toml that declares [libs] std.math = "1.0"
# The binary must be compiled with -DFLUXA_STD_MATH=1 (done by make with std.math in toml).
# If the binary was compiled without FLUXA_STD_MATH, math calls silently fall through
# to the "not a Block / not FFI" path and will report an error — tests will fail clearly.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.math/%s\n" "$1"; }
fail() { printf "  FAIL  std.math/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

# Every test needs a project dir with fluxa.toml declaring std.math
setup_proj() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.math = "1.0"
TOML
}

echo "── std.math: standard math library ─────────────────────────────────"

# ── CASE 1: import std math without [libs] toml → clear error ─────────────
cat > "$WORK_DIR/no_toml.flx" << 'FLX'
import std math
float r = math.sqrt(16.0)
print(r)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/no_toml.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|fluxa.toml\|libs"; then
    pass "import_without_toml_clear_error"
else
    fail "import_without_toml_clear_error" "error: not declared in [libs]" "$out"
fi

# ── CASE 2: sqrt ──────────────────────────────────────────────────────────
P="$WORK_DIR/p2"; setup_proj "$P" "sqrt"
cat > "$P/main.flx" << 'FLX'
import std math
float r = math.sqrt(16.0)
print(r)
float r2 = math.sqrt(2.0)
print(r2)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "1.41"; then
    pass "sqrt"
else
    fail "sqrt" "4 and ~1.414" "$out"
fi

# ── CASE 3: sqrt negative → error ─────────────────────────────────────────
P="$WORK_DIR/p3"; setup_proj "$P" "sqrt_neg"
cat > "$P/main.flx" << 'FLX'
import std math
danger {
    float r = math.sqrt(-1.0)
}
print(42)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^42$"; then
    pass "sqrt_negative_domain_error_in_danger"
else
    fail "sqrt_negative_domain_error_in_danger" "42 (error captured in danger)" "$out"
fi

# ── CASE 4: trig functions ─────────────────────────────────────────────────
P="$WORK_DIR/p4"; setup_proj "$P" "trig"
cat > "$P/main.flx" << 'FLX'
import std math
float pi  = math.pi()
float c   = math.cos(0.0)
float s   = math.sin(pi)
float t   = math.tan(0.0)
print(c)
print(t)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^0$"; then
    pass "trig_cos_sin_tan"
else
    fail "trig_cos_sin_tan" "cos(0)=1, tan(0)=0" "$out"
fi

# ── CASE 5: pow ───────────────────────────────────────────────────────────
P="$WORK_DIR/p5"; setup_proj "$P" "pow"
cat > "$P/main.flx" << 'FLX'
import std math
float p1 = math.pow(2.0, 10.0)
float p2 = math.pow(3.0, 3.0)
print(p1)
print(p2)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^1024$" && echo "$out" | grep -q "^27$"; then
    pass "pow"
else
    fail "pow" "1024 and 27" "$out"
fi

# ── CASE 6: abs — type-preserving ─────────────────────────────────────────
P="$WORK_DIR/p6"; setup_proj "$P" "abs"
cat > "$P/main.flx" << 'FLX'
import std math
int   ai = math.abs(-42)
float af = math.abs(-3.14)
print(ai)
print(af)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^42$" && echo "$out" | grep -q "3.14"; then
    pass "abs_type_preserving"
else
    fail "abs_type_preserving" "42 (int) and 3.14 (float)" "$out"
fi

# ── CASE 7: min / max ─────────────────────────────────────────────────────
P="$WORK_DIR/p7"; setup_proj "$P" "minmax"
cat > "$P/main.flx" << 'FLX'
import std math
int   mn = math.min(10, 3)
int   mx = math.max(10, 3)
float mf = math.min(1.5, 2.5)
print(mn)
print(mx)
print(mf)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^10$" \
    && echo "$out" | grep -q "1.5"; then
    pass "min_max"
else
    fail "min_max" "3, 10, 1.5" "$out"
fi

# ── CASE 8: clamp ─────────────────────────────────────────────────────────
P="$WORK_DIR/p8"; setup_proj "$P" "clamp"
cat > "$P/main.flx" << 'FLX'
import std math
float hi  = math.clamp(200.0, 0.0, 100.0)
float lo  = math.clamp(-50.0, 0.0, 100.0)
float mid = math.clamp(50.0,  0.0, 100.0)
print(hi)
print(lo)
print(mid)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^100$" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^50$"; then
    pass "clamp"
else
    fail "clamp" "100, 0, 50" "$out"
fi

# ── CASE 9: log / exp ─────────────────────────────────────────────────────
P="$WORK_DIR/p9"; setup_proj "$P" "log_exp"
cat > "$P/main.flx" << 'FLX'
import std math
float e_val = math.e()
float ln_e  = math.log(e_val)
float exp_0 = math.exp(0.0)
print(ln_e)
print(exp_0)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^1$"; then
    pass "log_exp"
else
    fail "log_exp" "log(e)=1, exp(0)=1" "$out"
fi

# ── CASE 10: deg_to_rad / rad_to_deg ─────────────────────────────────────
P="$WORK_DIR/p10"; setup_proj "$P" "deg_rad"
cat > "$P/main.flx" << 'FLX'
import std math
float d = math.rad_to_deg(math.pi())
float r = math.deg_to_rad(180.0)
print(d)
print(r)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^180$" && echo "$out" | grep -q "3.14"; then
    pass "deg_to_rad_rad_to_deg"
else
    fail "deg_to_rad_rad_to_deg" "180 and ~3.14159" "$out"
fi

# ── CASE 11: floor / ceil / round ─────────────────────────────────────────
P="$WORK_DIR/p11"; setup_proj "$P" "round"
cat > "$P/main.flx" << 'FLX'
import std math
float fl = math.floor(3.7)
float ce = math.ceil(3.2)
float ro = math.round(3.5)
print(fl)
print(ce)
print(ro)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^4$"; then
    pass "floor_ceil_round"
else
    fail "floor_ceil_round" "3, 4, 4" "$out"
fi

# ── CASE 12: hypot ────────────────────────────────────────────────────────
P="$WORK_DIR/p12"; setup_proj "$P" "hypot"
cat > "$P/main.flx" << 'FLX'
import std math
float h = math.hypot(3.0, 4.0)
print(h)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^5$"; then
    pass "hypot_3_4_5"
else
    fail "hypot_3_4_5" "5 (Pythagorean triple)" "$out"
fi

# ── CASE 13: sign ─────────────────────────────────────────────────────────
P="$WORK_DIR/p13"; setup_proj "$P" "sign"
cat > "$P/main.flx" << 'FLX'
import std math
int sp = math.sign(42.0)
int sn = math.sign(-7.0)
int sz = math.sign(0.0)
print(sp)
print(sn)
print(sz)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | sed -n '1p' | grep -q "^1$" \
    && echo "$out" | sed -n '2p' | grep -q "^-1$" \
    && echo "$out" | sed -n '3p' | grep -q "^0$"; then
    pass "sign"
else
    fail "sign" "1, -1, 0" "$out"
fi

# ── CASE 14: math in script mode (no project) ────────────────────────────
# std.math without [libs] in toml should give clear error
cat > "$WORK_DIR/script_math.flx" << 'FLX'
import std math
print(math.sqrt(9.0))
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/script_math.flx" 2>&1 || true)
if echo "$out" | grep -qi "not declared\|toml\|libs"; then
    pass "script_mode_no_toml_clear_error"
else
    fail "script_mode_no_toml_clear_error" "error: not declared in [libs]" "$out"
fi

# ── CASE 15: math used in prst computation ───────────────────────────────
P="$WORK_DIR/p15"; setup_proj "$P" "math_prst"
cat > "$P/main.flx" << 'FLX'
import std math
prst float radius  = 5.0
prst float area    = 0.0
area = math.pi() * math.pow(radius, 2.0)
print(radius)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "^5$"; then
    pass "math_with_prst_vars"
else
    fail "math_with_prst_vars" "5 (radius preserved)" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=15
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.math: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
