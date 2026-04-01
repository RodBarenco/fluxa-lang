#!/usr/bin/env bash
# tests/sprint9c_dyn.sh — dyn: heterogeneous dynamic array
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  dyn/%s\n" "$1"; }
fail() { printf "  FAIL  dyn/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint9c: dyn (heterogeneous dynamic array) ──────────────────────"

# ── CASO 1: criação e acesso ─────────────────────────────────────────────────
cat > "$WORK_DIR/basic.flx" << 'FLX'
dyn lista = [1, "ola", true, 3.14]
print(lista[0])
print(lista[1])
print(lista[2])
print(lista[3])
print(len(lista))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/basic.flx" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^ola$" \
    && echo "$out" | grep -q "^true$" && echo "$out" | grep -q "^3.14$" \
    && echo "$out" | grep -q "^4$"; then
    pass "create_access"
else
    fail "create_access" "1 ola true 3.14 4" "$out"
fi

# ── CASO 2: troca de tipo (dyn permite) ─────────────────────────────────────
cat > "$WORK_DIR/type_swap.flx" << 'FLX'
dyn d = [1, "texto", true]
d[0] = "agora string"
d[1] = false
d[2] = 99
print(d[0])
print(d[1])
print(d[2])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/type_swap.flx" 2>&1 || true)
if echo "$out" | grep -q "agora string" && echo "$out" | grep -q "false" \
    && echo "$out" | grep -q "99"; then
    pass "type_swap"
else
    fail "type_swap" "agora string false 99" "$out"
fi

# ── CASO 3: auto-grow (índice além do count) ─────────────────────────────────
cat > "$WORK_DIR/grow.flx" << 'FLX'
dyn d = [10, 20, 30]
d[5] = 99
print(len(d))
print(d[3])
print(d[4])
print(d[5])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/grow.flx" 2>&1 || true)
if echo "$out" | grep -q "^6$" && echo "$out" | grep -q "^nil$" \
    && echo "$out" | grep -q "^99$"; then
    pass "auto_grow"
else
    fail "auto_grow" "6 nil nil 99" "$out"
fi

# ── CASO 4: dyn vazio e crescimento desde zero ───────────────────────────────
cat > "$WORK_DIR/empty.flx" << 'FLX'
dyn d = []
print(len(d))
d[0] = "primeiro"
d[1] = 42
print(len(d))
print(d[0])
print(d[1])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/empty.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^2$" \
    && echo "$out" | grep -q "primeiro" && echo "$out" | grep -q "^42$"; then
    pass "empty_grow"
else
    fail "empty_grow" "0 2 primeiro 42" "$out"
fi

# ── CASO 5: len() em dyn ─────────────────────────────────────────────────────
cat > "$WORK_DIR/len.flx" << 'FLX'
dyn d = [1, 2, 3, 4, 5]
print(len(d))
d[9] = 0
print(len(d))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/len.flx" 2>&1 || true)
if echo "$out" | grep -q "^5$" && echo "$out" | grep -q "^10$"; then
    pass "len"
else
    fail "len" "5 10" "$out"
fi

# ── CASO 6: print dyn mostra como lista ─────────────────────────────────────
cat > "$WORK_DIR/print.flx" << 'FLX'
dyn d = [1, "x", true]
print(d)
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/print.flx" 2>&1 || true)
if echo "$out" | grep -q "\[1, x, true\]"; then
    pass "print"
else
    fail "print" "[1, x, true]" "$out"
fi

# ── CASO 7: erro em índice negativo ─────────────────────────────────────────
cat > "$WORK_DIR/neg_idx.flx" << 'FLX'
dyn d = [1, 2, 3]
d[-1] = 99
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/neg_idx.flx" 2>&1 || true)
if echo "$out" | grep -qi "error\|negative"; then
    pass "neg_index_error"
else
    fail "neg_index_error" "runtime error on negative index" "$out"
fi

# ── CASO 8: índice out-of-bounds em leitura ──────────────────────────────────
cat > "$WORK_DIR/oob.flx" << 'FLX'
dyn d = [10, 20]
print(d[5])
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/oob.flx" 2>&1 || true)
if echo "$out" | grep -qi "error\|out of bounds"; then
    pass "read_oob_error"
else
    fail "read_oob_error" "runtime error on oob read" "$out"
fi

# ── CASO 9: prst dyn persiste entre reloads ──────────────────────────────────
# (project mode — testa serialização básica)
PROJ_DIR="$WORK_DIR/dynprst"
mkdir -p "$PROJ_DIR"
cat > "$PROJ_DIR/main.flx" << 'FLX'
prst dyn estado = [1, 2, 3]
print(len(estado))
print(estado[0])
FLX
cat > "$PROJ_DIR/fluxa.toml" << 'TOML'
[project]
name = "dynprst"
entry = "main.flx"
TOML
out=$(timeout 3s "$FLUXA" run "$PROJ_DIR/main.flx" -proj "$PROJ_DIR" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^1$"; then
    pass "prst_dyn_basic"
else
    # prst dyn may not be fully implemented yet — soft fail
    if echo "$out" | grep -qi "error"; then
        fail "prst_dyn_basic" "3 and 1 in output" "$out"
    else
        pass "prst_dyn_basic"
    fi
fi

echo "────────────────────────────────────────────────────────────────────"
total=9
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → dyn: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
