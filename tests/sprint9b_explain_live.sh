#!/usr/bin/env bash
# tests/sprint9b_explain_live.sh — Sprint 9.b Issue #96: fluxa explain via IPC
#
# Valida os dois modos do comando explain:
#   Modo 1: fluxa explain (sem arquivo) — conecta ao runtime via IPC
#   Modo 2: fluxa explain <file>        — executa o arquivo (comportamento antigo)
#
# Também valida que -prod sobe o servidor IPC corretamente (root cause do bug).
#
# Uso: ./tests/sprint9b_explain_live.sh [--fluxa <path>] [--verbose]
# Exit: 0 = passou, 1 = falhou

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
VERBOSE=0
WORK_DIR="/tmp/fluxa-9b-explain-$$"
RT_PID=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)   FLUXA="$2"; shift 2 ;;
        --verbose) VERBOSE=1;  shift   ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

if [[ ! -x "$FLUXA" ]]; then
    echo "ERRO: binário não encontrado: $FLUXA"
    exit 1
fi

PASS=0; FAIL=0; ERRORS=""
mkdir -p "$WORK_DIR"

cleanup() {
    [[ -n "$RT_PID" ]] && kill -9 "$RT_PID" 2>/dev/null || true
    rm -f /tmp/fluxa-${RT_PID:-0}.sock /tmp/fluxa-${RT_PID:-0}.lock 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); ERRORS="${ERRORS}\n  $1: $2"; }
vlog() { [[ $VERBOSE -eq 1 ]] && echo "        $*" || true; }

wait_for_socket() {
    local pid="$1"
    for i in $(seq 1 30); do
        [[ -S "/tmp/fluxa-${pid}.sock" ]] && return 0
        sleep 0.1
    done
    return 1
}

echo "── Sprint 9.b Issue #96: fluxa explain via IPC ─────────────────────"
echo "   binary : $FLUXA"
echo "────────────────────────────────────────────────────────────────────"

# =============================================================================
# CASO 1 — -prod sobe IPC server (root cause fix)
# =============================================================================
echo ""
echo "  ── Caso 1: -prod sobe servidor IPC ──────────────────────────────"

PROG1="$WORK_DIR/prod_loop.flx"
cat > "$PROG1" << 'FLX'
prst int counter = 0
int i = 0
while i >= 0 {
    counter = counter + 1
    i = 0
}
FLX

"$FLUXA" run "$PROG1" -prod \
    >"$WORK_DIR/c1_out.log" 2>"$WORK_DIR/c1_err.log" &
RT_PID=$!
vlog "runtime -prod pid=$RT_PID"

if ! wait_for_socket "$RT_PID"; then
    fail "caso1/-prod_ipc_socket" "socket IPC não apareceu — -prod não subiu o servidor"
else
    vlog "socket /tmp/fluxa-${RT_PID}.sock existe"
    pass "caso1/-prod_ipc_socket"
fi

kill -9 "$RT_PID" 2>/dev/null || true
wait "$RT_PID" 2>/dev/null || true
RT_PID=""
sleep 0.2

# =============================================================================
# CASO 2 — fluxa set funciona com -prod (o bug original)
# =============================================================================
echo ""
echo "  ── Caso 2: fluxa set aplica valor em loop com -prod ─────────────"

PROG2="$WORK_DIR/set_prod.flx"
cat > "$PROG2" << 'FLX'
prst int number = 12
bool key = true
while key == true {
    number = number
}
print(number)
FLX

"$FLUXA" run "$PROG2" -prod \
    >"$WORK_DIR/c2_out.log" 2>"$WORK_DIR/c2_err.log" &
RT_PID=$!

if ! wait_for_socket "$RT_PID"; then
    fail "caso2/set_in_prod_loop" "socket não apareceu"
else
    sleep 0.3

    set_out=$("$FLUXA" set number 99 2>&1 || true)
    vlog "set output: $set_out"

    sleep 0.4

    obs_out=$(timeout 1s "$FLUXA" observe number 2>/dev/null || true)
    vlog "observe: $obs_out"

    if echo "$obs_out" | grep -q "= 99"; then
        pass "caso2/set_applied_in_prod_loop"
    else
        fail "caso2/set_applied_in_prod_loop" \
            "esperado 99, got: '$obs_out' (set: '$set_out')"
    fi
fi

kill -9 "$RT_PID" 2>/dev/null || true
wait "$RT_PID" 2>/dev/null || true
RT_PID=""
sleep 0.2

# =============================================================================
# CASO 3 — fluxa explain (modo IPC ao vivo)
# =============================================================================
echo ""
echo "  ── Caso 3: fluxa explain (modo IPC — sem arquivo) ───────────────"

PROG3="$WORK_DIR/explain_live.flx"
cat > "$PROG3" << 'FLX'
prst int score = 100
prst float rate = 2.5
prst bool active = true
int i = 0
while i >= 0 {
    score = score + 1
    i = 0
}
FLX

"$FLUXA" run "$PROG3" -prod \
    >"$WORK_DIR/c3_out.log" 2>"$WORK_DIR/c3_err.log" &
RT_PID=$!

if ! wait_for_socket "$RT_PID"; then
    fail "caso3/explain_live_socket" "socket não apareceu"
else
    sleep 0.4

    explain_out=$("$FLUXA" explain 2>&1 || true)
    vlog "explain output: $explain_out"

    c3_ok=1

    # Deve mostrar as prst vars
    if ! echo "$explain_out" | grep -q "score"; then
        vlog "FAIL: 'score' não encontrado no explain"
        c3_ok=0
    fi
    if ! echo "$explain_out" | grep -q "rate"; then
        vlog "FAIL: 'rate' não encontrado"
        c3_ok=0
    fi
    if ! echo "$explain_out" | grep -q "active"; then
        vlog "FAIL: 'active' não encontrado"
        c3_ok=0
    fi

    # Deve mostrar a seção Runtime com pid
    if ! echo "$explain_out" | grep -q "pid"; then
        vlog "FAIL: seção Runtime sem pid"
        c3_ok=0
    fi

    # Valores devem estar presentes
    if ! echo "$explain_out" | grep -q "2.5\|float"; then
        vlog "FAIL: valor float não encontrado"
        c3_ok=0
    fi
    if ! echo "$explain_out" | grep -q "true"; then
        vlog "FAIL: bool true não encontrado"
        c3_ok=0
    fi

    if [[ $c3_ok -eq 1 ]]; then
        pass "caso3/explain_live_output"
    else
        fail "caso3/explain_live_output" \
            "saída incompleta. Got: $(echo "$explain_out" | tr '\n' '|')"
    fi
fi

kill -9 "$RT_PID" 2>/dev/null || true
wait "$RT_PID" 2>/dev/null || true
RT_PID=""
sleep 0.2

# =============================================================================
# CASO 4 — fluxa explain <file> (modo 2 — comportamento original mantido)
# =============================================================================
echo ""
echo "  ── Caso 4: fluxa explain <file> (modo arquivo) ──────────────────"

PROG4="$WORK_DIR/explain_file.flx"
cat > "$PROG4" << 'FLX'
prst int x = 42
prst bool flag = false
print(x)
FLX

explain_file_out=$("$FLUXA" explain "$PROG4" 2>&1 || true)
vlog "explain file output: $explain_file_out"

c4_ok=1
if ! echo "$explain_file_out" | grep -q "42\|x"; then
    c4_ok=0; vlog "FAIL: var x=42 não encontrada"
fi
if ! echo "$explain_file_out" | grep -q "prst\|sobrevivem"; then
    c4_ok=0; vlog "FAIL: seção prst não encontrada"
fi
# O output do arquivo também imprime o valor (42)
if ! echo "$explain_file_out" | grep -q "42"; then
    c4_ok=0; vlog "FAIL: valor 42 não encontrado"
fi

if [[ $c4_ok -eq 1 ]]; then
    pass "caso4/explain_file_mode"
else
    fail "caso4/explain_file_mode" \
        "saída do modo arquivo incorreta: $(echo "$explain_file_out" | tr '\n' '|')"
fi

# =============================================================================
# CASO 5 — fluxa explain sem runtime rodando imprime erro claro
# =============================================================================
echo ""
echo "  ── Caso 5: fluxa explain sem runtime → erro claro ───────────────"

no_rt_out=$("$FLUXA" explain 2>&1 || true)
vlog "no runtime: $no_rt_out"
if echo "$no_rt_out" | grep -qi "no running runtime\|cannot connect"; then
    pass "caso5/explain_no_runtime_error"
else
    fail "caso5/explain_no_runtime_error" \
        "esperado mensagem de erro, got: $no_rt_out"
fi

# =============================================================================
# Resultado final
# =============================================================================
echo ""
echo "────────────────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "  Failures:"
    printf '%b\n' "$ERRORS"
    exit 1
fi

echo "  → Issue #96 (fluxa explain via IPC + -prod IPC): PASS"
exit 0
