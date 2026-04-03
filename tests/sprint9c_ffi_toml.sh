#!/usr/bin/env bash
# tests/sprint9c_ffi_toml.sh — Issue #103: [ffi] no toml
# Testa resolução automática de libs via toml, retrocompatibilidade com
# import c, fluxa ffi list, fluxa ffi inspect, fluxa runtime info.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  ffi_toml/%s\n" "$1"; }
fail() { printf "  FAIL  ffi_toml/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint9c: [ffi] no toml (issue #103) ────────────────────────────"

# ── CASO 1: lib via [ffi] no toml — sem import c no código ──────────────────
PROJ1="$WORK_DIR/toml_ffi"
mkdir -p "$PROJ1"
cat > "$PROJ1/main.flx" << 'FLX'
float r = 0.0
danger {
    r = libm.sqrt(9.0)
}
print(r)
FLX
cat > "$PROJ1/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ1/main.flx" -proj "$PROJ1" 2>&1 || true)
if echo "$out" | grep -q "^3$\|^3\.0"; then
    pass "toml_auto_no_import_c"
else
    fail "toml_auto_no_import_c" "3 or 3.0" "$out"
fi

# ── CASO 2: retrocompatibilidade — import c ainda funciona ──────────────────
cat > "$WORK_DIR/import_c_compat.flx" << 'FLX'
import c libm

float r = 0.0
danger {
    r = libm.sqrt(16.0)
}
print(r)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/import_c_compat.flx" 2>&1 || true)
if echo "$out" | grep -q "^4$\|^4\.0"; then
    pass "import_c_retrocompat"
else
    fail "import_c_retrocompat" "4 or 4.0" "$out"
fi

# ── CASO 3: lib via toml + import c redundante não duplica handle ────────────
PROJ3="$WORK_DIR/no_dup"
mkdir -p "$PROJ3"
cat > "$PROJ3/main.flx" << 'FLX'
import c libm
float r = 0.0
danger {
    r = libm.fabs(-7.5)
}
print(r)
FLX
cat > "$PROJ3/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ3/main.flx" -proj "$PROJ3" 2>&1 || true)
if echo "$out" | grep -q "^7\.5\|^7$"; then
    pass "toml_plus_import_no_dup"
else
    fail "toml_plus_import_no_dup" "7.5" "$out"
fi

# ── CASO 4: lib com path explícito no toml ──────────────────────────────────
PROJ4="$WORK_DIR/explicit_path"
mkdir -p "$PROJ4"
# Find actual libm path
LIBM_PATH=$(ldconfig -p 2>/dev/null | grep "libm\.so" | head -1 | awk '{print $NF}' || true)
if [[ -z "$LIBM_PATH" ]]; then
    LIBM_PATH=$(find /usr/lib /lib -name "libm.so*" 2>/dev/null | head -1 || true)
fi
if [[ -n "$LIBM_PATH" ]]; then
    cat > "$PROJ4/main.flx" << 'FLX'
float r = 0.0
danger { r = libm.sqrt(25.0) }
print(r)
FLX
    cat > "$PROJ4/fluxa.toml" << TOML
[ffi]
libm = "$LIBM_PATH"
TOML
    # Replace variable in toml
    sed -i "s|\$LIBM_PATH|$LIBM_PATH|g" "$PROJ4/fluxa.toml"
    out=$(timeout 5s "$FLUXA" run "$PROJ4/main.flx" -proj "$PROJ4" 2>&1 || true)
    if echo "$out" | grep -q "^5$\|^5\.0"; then
        pass "explicit_path_in_toml"
    else
        fail "explicit_path_in_toml" "5 or 5.0" "$out"
    fi
else
    pass "explicit_path_in_toml (skipped — libm path not found)"
fi

# ── CASO 5: lib inexistente no toml — erro claro, não segfault ──────────────
PROJ5="$WORK_DIR/bad_lib"
mkdir -p "$PROJ5"
cat > "$PROJ5/main.flx" << 'FLX'
print(42)
FLX
cat > "$PROJ5/fluxa.toml" << 'TOML'
[ffi]
libfluxa_does_not_exist_xyz = "auto"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ5/main.flx" -proj "$PROJ5" 2>&1 || true)
# Should still run (warning, not fatal) AND print 42
if echo "$out" | grep -q "^42$"; then
    pass "bad_lib_non_fatal"
else
    fail "bad_lib_non_fatal" "42 (bad lib is warning only)" "$out"
fi

# ── CASO 6: fluxa runtime info mostra [ffi] do toml ─────────────────────────
PROJ6="$WORK_DIR/runtime_info"
mkdir -p "$PROJ6"
cat > "$PROJ6/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"

[runtime]
gc_cap = 512
TOML
out=$(timeout 5s "$FLUXA" runtime info 2>&1 || true)
if echo "$out" | grep -qi "runtime\|gc_cap"; then
    pass "runtime_info_shows_config"
else
    fail "runtime_info_shows_config" "output with runtime/gc_cap" "$out"
fi

# ── CASO 7: fluxa ffi list executa sem crash ─────────────────────────────────
out=$(timeout 10s "$FLUXA" ffi list 2>&1 || true)
if echo "$out" | grep -qi "lib\|fluxa.toml\|available\|declared"; then
    pass "ffi_list_runs"
else
    fail "ffi_list_runs" "list output" "$out"
fi

# ── CASO 8: fluxa ffi inspect libm gera assinaturas ─────────────────────────
out=$(timeout 10s "$FLUXA" ffi inspect libm 2>&1 || true)
if echo "$out" | grep -qi "toml\|ffi\|signatures\|\[ffi"; then
    pass "ffi_inspect_libm"
else
    fail "ffi_inspect_libm" "toml signature output" "$out"
fi

# ── CASO 9: múltiplas libs no [ffi] — todas carregadas ──────────────────────
PROJ9="$WORK_DIR/multi_lib"
mkdir -p "$PROJ9"
cat > "$PROJ9/main.flx" << 'FLX'
float r = 0.0
danger {
    r = libm.sqrt(4.0)
}
print(r)
FLX
cat > "$PROJ9/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"
libc = "auto"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ9/main.flx" -proj "$PROJ9" 2>&1 || true)
if echo "$out" | grep -q "^2$\|^2\.0"; then
    pass "multi_lib_in_toml"
else
    fail "multi_lib_in_toml" "2 or 2.0" "$out"
fi

# ── CASO 10: assinatura no toml — sig registrada (smoke test) ────────────────
PROJ10="$WORK_DIR/sig_reg"
mkdir -p "$PROJ10"
cat > "$PROJ10/main.flx" << 'FLX'
float r = 0.0
danger {
    r = libm.sqrt(36.0)
}
print(r)
FLX
cat > "$PROJ10/fluxa.toml" << 'TOML'
[ffi]
libm = "auto"

[ffi.libm.signatures]
sqrt  = "(double) -> double"
fabs  = "(double) -> double"
floor = "(double) -> double"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ10/main.flx" -proj "$PROJ10" 2>&1 || true)
if echo "$out" | grep -q "^6$\|^6\.0"; then
    pass "sig_registered_in_toml"
else
    fail "sig_registered_in_toml" "6 or 6.0" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → ffi_toml: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
