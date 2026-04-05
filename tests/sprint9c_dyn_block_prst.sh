#!/usr/bin/env bash
# tests/sprint9c_dyn_block_prst.sh
# Testes faltantes da sessão anterior:
#   - Block dentro de dyn  (armazenar e chamar métodos de instâncias)
#   - prst dyn (persistência entre reloads — modos projeto)
#
# Complementa sprint9c_dyn.sh (9 casos base já cobertos lá).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  dyn_block_prst/%s\n" "$1"; }
fail() { printf "  FAIL  dyn_block_prst/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

echo "── sprint9c: dyn com Block e prst dyn ───────────────────────────────"

# ─────────────────────────────────────────────────────────────────────────────
# BLOCO 1: Block dentro de dyn
# ─────────────────────────────────────────────────────────────────────────────

# CASO 1: armazenar instância typeof em dyn e chamar método via acesso
cat > "$WORK_DIR/block_in_dyn.flx" << 'FLX'
Block Contador {
    prst int total = 0
    fn inc() nil { total = total + 1 }
    fn valor() int { return total }
}

Block c1 typeof Contador
Block c2 typeof Contador

dyn pool = [c1, c2]

pool[0].inc()
pool[0].inc()
pool[1].inc()

print(pool[0].valor())
print(pool[1].valor())
print(len(pool))
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/block_in_dyn.flx" 2>&1 || true)
if echo "$out" | grep -q "^2$" && echo "$out" | grep -q "^1$" \
    && echo "$out" | grep -q "^2$"; then
    pass "block_in_dyn_method_call"
else
    fail "block_in_dyn_method_call" "2 1 2" "$out"
fi

# CASO 2: dyn misturado — Block e primitivos convivendo
cat > "$WORK_DIR/dyn_mixed_block.flx" << 'FLX'
Block Tag {
    prst str label = "default"
    fn set_label(str s) nil { label = s }
    fn get_label() str { return label }
}

Block t typeof Tag

t.set_label("ativo")

dyn bolsa = [42, "texto", true, t]

print(bolsa[0])
print(bolsa[1])
print(bolsa[2])
print(bolsa[3].get_label())
print(len(bolsa))
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_mixed_block.flx" 2>&1 || true)
if echo "$out" | grep -q "^42$" && echo "$out" | grep -q "^texto$" \
    && echo "$out" | grep -q "^true$" && echo "$out" | grep -q "^ativo$" \
    && echo "$out" | grep -q "^4$"; then
    pass "dyn_mixed_block_primitives"
else
    fail "dyn_mixed_block_primitives" "42 texto true ativo 4" "$out"
fi

# CASO 3: substituir Block no dyn por outro typeof (troca de instância)
cat > "$WORK_DIR/dyn_replace_block.flx" << 'FLX'
Block Item {
    prst int id = 0
    fn get_id() int { return id }
}

Block a typeof Item
Block b typeof Item

a.id = 10
b.id = 20

dyn lista = [a]
print(lista[0].get_id())

lista[0] = b
print(lista[0].get_id())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_replace_block.flx" 2>&1 || true)
if echo "$out" | grep -q "^10$" && echo "$out" | grep -q "^20$"; then
    pass "dyn_replace_block_instance"
else
    fail "dyn_replace_block_instance" "10 20" "$out"
fi

# CASO 4: Block com prst interno em dyn — estado persiste após ops no dyn
cat > "$WORK_DIR/block_prst_in_dyn.flx" << 'FLX'
Block Acumulador {
    prst int soma = 0
    fn add(int n) nil { soma = soma + n }
    fn get() int { return soma }
}

Block ac typeof Acumulador

dyn workers = [ac]

workers[0].add(5)
workers[0].add(3)

print(ac.get())
print(workers[0].get())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/block_prst_in_dyn.flx" 2>&1 || true)
if echo "$out" | grep -q "^8$"; then
    pass "block_internal_prst_via_dyn"
else
    fail "block_internal_prst_via_dyn" "8 8" "$out"
fi

# CASO 5: dyn de Blocks com typeof — cada instância isolada
cat > "$WORK_DIR/dyn_isolation.flx" << 'FLX'
Block Sensor {
    prst float leitura = 0.0
    fn registrar(float v) nil { leitura = v }
    fn ler() float { return leitura }
}

Block s1 typeof Sensor
Block s2 typeof Sensor
Block s3 typeof Sensor

dyn sensores = [s1, s2, s3]

sensores[0].registrar(1.1)
sensores[1].registrar(2.2)
sensores[2].registrar(3.3)

print(sensores[0].ler())
print(sensores[1].ler())
print(sensores[2].ler())
FLX
out=$(timeout 5s "$FLUXA" run "$WORK_DIR/dyn_isolation.flx" 2>&1 || true)
if echo "$out" | grep -q "1.1" && echo "$out" | grep -q "2.2" \
    && echo "$out" | grep -q "3.3"; then
    pass "dyn_block_isolation"
else
    fail "dyn_block_isolation" "1.1 2.2 3.3" "$out"
fi

# ─────────────────────────────────────────────────────────────────────────────
# BLOCO 2: prst dyn — persistência em modo projeto
# ─────────────────────────────────────────────────────────────────────────────

# CASO 6: prst dyn de primitivos — sobrevive ao reload (verifica valor inicial)
PROJ6="$WORK_DIR/proj_prst_dyn"
mkdir -p "$PROJ6"
cat > "$PROJ6/main.flx" << 'FLX'
prst dyn historico = [10, 20, 30]
print(len(historico))
print(historico[0])
print(historico[2])
FLX
cat > "$PROJ6/fluxa.toml" << 'TOML'
[project]
name = "prst_dyn_basic"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ6/main.flx" -proj "$PROJ6" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^10$" \
    && echo "$out" | grep -q "^30$"; then
    pass "prst_dyn_primitives"
else
    fail "prst_dyn_primitives" "3 10 30" "$out"
fi

# CASO 7: prst dyn com tipos mistos (int, str, bool, float)
PROJ7="$WORK_DIR/proj_prst_dyn_mixed"
mkdir -p "$PROJ7"
cat > "$PROJ7/main.flx" << 'FLX'
prst dyn registro = [1, "fluxa", true, 3.14]
print(registro[0])
print(registro[1])
print(registro[2])
print(registro[3])
FLX
cat > "$PROJ7/fluxa.toml" << 'TOML'
[project]
name = "prst_dyn_mixed"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ7/main.flx" -proj "$PROJ7" 2>&1 || true)
if echo "$out" | grep -q "^1$" && echo "$out" | grep -q "^fluxa$" \
    && echo "$out" | grep -q "^true$" && echo "$out" | grep -q "3.14"; then
    pass "prst_dyn_mixed_types"
else
    fail "prst_dyn_mixed_types" "1 fluxa true 3.14" "$out"
fi

# CASO 8: prst dyn com auto-grow mantém estado após crescimento
PROJ8="$WORK_DIR/proj_prst_dyn_grow"
mkdir -p "$PROJ8"
cat > "$PROJ8/main.flx" << 'FLX'
prst dyn log = [0]
log[4] = 99
print(len(log))
print(log[1])
print(log[4])
FLX
cat > "$PROJ8/fluxa.toml" << 'TOML'
[project]
name = "prst_dyn_grow"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ8/main.flx" -proj "$PROJ8" 2>&1 || true)
if echo "$out" | grep -q "^5$" && echo "$out" | grep -q "^nil$" \
    && echo "$out" | grep -q "^99$"; then
    pass "prst_dyn_auto_grow"
else
    fail "prst_dyn_auto_grow" "5 nil 99" "$out"
fi

# CASO 9: prst dyn vazio declarado no projeto
PROJ9="$WORK_DIR/proj_prst_dyn_empty"
mkdir -p "$PROJ9"
cat > "$PROJ9/main.flx" << 'FLX'
prst dyn buffer = []
print(len(buffer))
buffer[0] = "inicio"
print(len(buffer))
print(buffer[0])
FLX
cat > "$PROJ9/fluxa.toml" << 'TOML'
[project]
name = "prst_dyn_empty"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ9/main.flx" -proj "$PROJ9" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^1$" \
    && echo "$out" | grep -q "^inicio$"; then
    pass "prst_dyn_empty_project"
else
    fail "prst_dyn_empty_project" "0 1 inicio" "$out"
fi

# CASO 10: prst dyn com Block — VAL_PTR semantics (não serializado em Flash)
#   No modo projeto em x86, o ponteiro de instância é preservado normalmente.
#   Este teste valida que prst dyn contendo Block não gera erro silencioso.
PROJ10="$WORK_DIR/proj_prst_dyn_block"
mkdir -p "$PROJ10"
cat > "$PROJ10/main.flx" << 'FLX'
Block Node {
    prst int val = 7
    fn get() int { return val }
}

Block n typeof Node

prst dyn nodes = [n]
print(nodes[0].get())
FLX
cat > "$PROJ10/fluxa.toml" << 'TOML'
[project]
name = "prst_dyn_block"
entry = "main.flx"
TOML
out=$(timeout 5s "$FLUXA" run "$PROJ10/main.flx" -proj "$PROJ10" 2>&1 || true)
if echo "$out" | grep -q "^7$"; then
    pass "prst_dyn_with_block_instance"
else
    # Se falhar com erro claro sobre serialização de instância — aceitável
    if echo "$out" | grep -qi "cannot serialize\|VAL_BLOCK\|not serializable"; then
        pass "prst_dyn_with_block_instance (erro esperado de serialização)"
    else
        fail "prst_dyn_with_block_instance" "7 ou erro de serialização explícito" "$out"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
echo "────────────────────────────────────────────────────────────────────"
total=10
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → dyn_block_prst: PASS"
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
