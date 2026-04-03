#!/usr/bin/env bash
# tests/sprint9c_dyn_indexed_member.sh
# Bug fix: acesso a campos e métodos de Block dentro de dyn via dyn[i].campo
#
# Padrão problemático antes do fix:
#   dyn petshop = [caramelo, persa]
#   int n = petshop[1].olhos   ← parser gerava NODE_ARR_ACCESS + token .olhos perdido
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  dyn_indexed/%s\n" "$1"; }
fail() { printf "  FAIL  dyn_indexed/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint9c: dyn[i].campo e dyn[i].metodo() ─────────────────────────"

# ── CASO 1: o exemplo exato do bug report ────────────────────────────────────
cat > "$WORK_DIR/bug_report.flx" << 'FLX'
int novo = 5
Block cachorro {
    int patas = 4
    int olhos = 2
    int orelhas = 2
    int rabo = 1
    int boca = 1
}
Block gato {
    int patas = 4
    int olhos = 2
    int orelhas = 2
    int rabo = 1
    int boca = 1
}
Block caramelo typeof cachorro
Block persa typeof gato
dyn petshop = [caramelo, persa]
int n = petshop[1].olhos
print(n)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/bug_report.flx" 2>&1 || true)
if echo "$out" | grep -q "^2$"; then
    pass "bug_report_exact"
else
    fail "bug_report_exact" "2" "$out"
fi

# ── CASO 2: leitura de campos em múltiplos índices ───────────────────────────
cat > "$WORK_DIR/multi_field.flx" << 'FLX'
Block Animal {
    int patas = 4
    int olhos = 2
    str nome = "generico"
}
Block cobra typeof Animal
Block aranha typeof Animal
cobra.patas = 0
aranha.patas = 8
aranha.nome = "aranha"
dyn zoo = [cobra, aranha]
print(zoo[0].patas)
print(zoo[1].patas)
print(zoo[0].olhos)
print(zoo[1].nome)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/multi_field.flx" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^8$" \
    && echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^aranha$"; then
    pass "multi_field_read"
else
    fail "multi_field_read" "0 8 2 aranha" "$out"
fi

# ── CASO 3: chamada de método via dyn[i].metodo() ────────────────────────────
cat > "$WORK_DIR/method_call.flx" << 'FLX'
Block Contador {
    prst int total = 0
    fn inc() nil { total = total + 1 }
    fn get() int { return total }
}
Block c1 typeof Contador
Block c2 typeof Contador
dyn pool = [c1, c2]
pool[0].inc()
pool[0].inc()
pool[1].inc()
print(pool[0].get())
print(pool[1].get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/method_call.flx" 2>&1 || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^1$"; then
    pass "method_call_via_index"
else
    fail "method_call_via_index" "2 1" "$out"
fi

# ── CASO 4: método com argumento via dyn[i].metodo(arg) ──────────────────────
cat > "$WORK_DIR/method_with_arg.flx" << 'FLX'
Block Sensor {
    prst float leitura = 0.0
    fn registrar(float v) nil { leitura = v }
    fn ler() float { return leitura }
}
Block s1 typeof Sensor
Block s2 typeof Sensor
dyn sensores = [s1, s2]
sensores[0].registrar(1.5)
sensores[1].registrar(9.9)
print(sensores[0].ler())
print(sensores[1].ler())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/method_with_arg.flx" 2>&1 || true)
if echo "$out" | grep -q "1.5" && echo "$out" | grep -q "9.9"; then
    pass "method_with_arg"
else
    fail "method_with_arg" "1.5 9.9" "$out"
fi

# ── CASO 5: isolamento — alterar pool[0] não afeta pool[1] ───────────────────
cat > "$WORK_DIR/isolation.flx" << 'FLX'
Block Item {
    prst int val = 10
    fn set_val(int n) nil { val = n }
    fn get_val() int { return val }
}
Block a typeof Item
Block b typeof Item
dyn lista = [a, b]
lista[0].set_val(99)
print(lista[0].get_val())
print(lista[1].get_val())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/isolation.flx" 2>&1 || true)
if echo "$out" | grep -q "^99$" && echo "$out" | grep -q "^10$"; then
    pass "isolation"
else
    fail "isolation" "99 10" "$out"
fi

# ── CASO 6: atribuição a variável tipada a partir de dyn[i].campo ─────────────
cat > "$WORK_DIR/assign_from_field.flx" << 'FLX'
Block Ponto {
    int x = 3
    int y = 7
}
Block p typeof Ponto
dyn pts = [p]
int cx = pts[0].x
int cy = pts[0].y
print(cx)
print(cy)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/assign_from_field.flx" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^7$"; then
    pass "assign_from_indexed_field"
else
    fail "assign_from_indexed_field" "3 7" "$out"
fi

# ── CASO 7: dyn[i].campo em expressão aritmética ─────────────────────────────
cat > "$WORK_DIR/field_in_expr.flx" << 'FLX'
Block Caixa {
    int largura = 5
    int altura = 3
}
Block c typeof Caixa
dyn caixas = [c]
int area = caixas[0].largura * caixas[0].altura
print(area)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/field_in_expr.flx" 2>&1 || true)
if echo "$out" | grep -q "^15$"; then
    pass "field_in_arithmetic"
else
    fail "field_in_arithmetic" "15" "$out"
fi

# ── CASO 8: índice out-of-bounds gera erro claro ─────────────────────────────
cat > "$WORK_DIR/oob.flx" << 'FLX'
Block X { int v = 1 }
Block x typeof X
dyn d = [x]
print(d[5].v)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/oob.flx" 2>&1 || true)
if echo "$out" | grep -qi "error\|out of bounds"; then
    pass "oob_error"
else
    fail "oob_error" "runtime error on oob" "$out"
fi

# ── CASO 9: elemento não-Block no índice gera erro claro ─────────────────────
cat > "$WORK_DIR/not_block.flx" << 'FLX'
dyn d = [42, "texto"]
print(d[0].campo)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/not_block.flx" 2>&1 || true)
if echo "$out" | grep -qi "error\|not a Block\|block instance"; then
    pass "not_block_error"
else
    fail "not_block_error" "error: not a Block instance" "$out"
fi

# ── CASO 10: dyn com 3 types diferentes de Block, cada um com campo próprio ──
cat > "$WORK_DIR/three_types.flx" << 'FLX'
Block Cao {
    int patas = 4
    str tipo = "cao"
}
Block Peixe {
    int patas = 0
    str tipo = "peixe"
}
Block Ave {
    int patas = 2
    str tipo = "ave"
}
Block rex    typeof Cao
Block nemo   typeof Peixe
Block tweety typeof Ave
dyn ark = [rex, nemo, tweety]
print(ark[0].patas)
print(ark[1].patas)
print(ark[2].patas)
print(ark[0].tipo)
print(ark[2].tipo)
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/three_types.flx" 2>&1 || true)
if echo "$out" | grep -q "^4$" && echo "$out" | grep -q "^0$" \
    && echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^cao$" \
    && echo "$out" | grep -q "^ave$"; then
    pass "three_block_types_in_dyn"
else
    fail "three_block_types_in_dyn" "4 0 2 cao ave" "$out"
fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → dyn_indexed: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
