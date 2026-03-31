#!/usr/bin/env bash
# tests/sprint9b_set_in_loop.sh — Sprint 9.b Issue #95
#
# Valida que fluxa set funciona dentro de loops while infinitos.
# Valor aplicado via live_rt->stack[offset] — sem restart, sem perda de
# variáveis não-persistentes.
#
# Uso: ./tests/sprint9b_set_in_loop.sh [--fluxa <path>] [--verbose]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
VERBOSE=0
WORK_DIR="/tmp/fluxa-9b-$$"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)   FLUXA="$2"; shift 2 ;;
        --verbose) VERBOSE=1;  shift   ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

[[ -x "$FLUXA" ]] || { echo "ERRO: binário não encontrado: $FLUXA"; exit 1; }

PASS=0; FAIL=0; ERRORS=""
mkdir -p "$WORK_DIR"

cleanup() {
    kill -9 $(pgrep -f "fluxa run" 2>/dev/null) 2>/dev/null || true
    rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); ERRORS="${ERRORS}\n  $1: $2"; }
vlog() { [[ $VERBOSE -eq 1 ]] && echo "        $*" || true; }

wait_for_socket() {
    for i in $(seq 1 30); do
        ls /tmp/fluxa-*.sock 2>/dev/null | head -1 | grep -q . && return 0
        sleep 0.1
    done
    return 1
}

# observe com timeout — nunca trava; espera até var=valor aparecer
observe_once() {
    local var="$1" tmpf
    tmpf=$(mktemp)
    timeout 2s "$FLUXA" observe "$var" > "$tmpf" 2>&1 &
    local obs_pid=$!
    local t=0
    while [[ $t -lt 15 ]]; do
        if grep -q "${var} = " "$tmpf" 2>/dev/null; then
            kill $obs_pid 2>/dev/null || true
            wait $obs_pid 2>/dev/null || true
            grep "${var} = " "$tmpf" | head -1
            rm -f "$tmpf"
            return 0
        fi
        sleep 0.1
        t=$((t+1))
    done
    kill $obs_pid 2>/dev/null || true
    wait $obs_pid 2>/dev/null || true
    rm -f "$tmpf"
    return 1
}

echo "── Sprint 9.b: fluxa set dentro de loops while ─────────────────────"
echo "   binary : $FLUXA"
echo "───────────────────────────────────────────────────────────────────"

# =============================================================================
# CASO 1 — bytecode loop (sem print): set muda stack, observe lê novo valor
# =============================================================================
echo ""
echo "  ── Caso 1: bytecode loop — set prst lida pelo VM ───────────────"

cat > "$WORK_DIR/loop_bc.flx" << 'FLX'
prst int x = 5
int i = 0
while i >= 0 {
    i = i + 0
}
FLX

kill -9 $(pgrep -f "fluxa run" 2>/dev/null) 2>/dev/null || true
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true; sleep 0.15

"$FLUXA" run "$WORK_DIR/loop_bc.flx" -dev >"$WORK_DIR/c1_out.log" 2>"$WORK_DIR/c1_err.log" &

if ! wait_for_socket; then
    fail "caso1/socket" "IPC socket não apareceu em 3s"
else
    sleep 0.3
    set_out=$("$FLUXA" set x 99 2>&1 || true)
    vlog "set: $set_out"
    sleep 0.15
    new_val=$(observe_once x || echo "not_found")
    vlog "observe: $new_val"

    if echo "$set_out" | grep -q "applied\|queued" && \
       echo "$new_val"  | grep -q "x = 99"; then
        pass "caso1/set_em_bytecode_loop"
    else
        fail "caso1/set_em_bytecode_loop" "set='$set_out' observe='$new_val'"
    fi
fi

kill -9 $(pgrep -f "fluxa run" 2>/dev/null) 2>/dev/null || true
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true; sleep 0.15

# =============================================================================
# CASO 2 — loop interpretado com print: output muda após set
# =============================================================================
echo ""
echo "  ── Caso 2: loop interpretado — output muda após set ─────────────"

cat > "$WORK_DIR/loop_print.flx" << 'FLX'
prst int number = 5
bool key = true
while key == true {
    print(number)
}
FLX

"$FLUXA" run "$WORK_DIR/loop_print.flx" -dev >"$WORK_DIR/c2_out.log" 2>"$WORK_DIR/c2_err.log" &

if ! wait_for_socket; then
    fail "caso2/socket" "IPC socket não apareceu em 3s"
else
    sleep 0.3
    c5=$(grep -c "^5$" "$WORK_DIR/c2_out.log" 2>/dev/null || echo 0)
    vlog "prints de 5 antes: $c5"

    set_out=$("$FLUXA" set number 99 2>&1 || true)
    vlog "set: $set_out"
    sleep 0.3

    c99=$(grep -c "^99$" "$WORK_DIR/c2_out.log" 2>/dev/null || echo 0)
    obs=$(observe_once number || echo "not_found")
    vlog "prints de 99: $c99  observe: $obs"

    if [[ "$c5" -gt 0 ]] && [[ "$c99" -gt 0 ]] && \
       echo "$obs" | grep -q "number = 99"; then
        pass "caso2/output_muda_apos_set"
    else
        fail "caso2/output_muda_apos_set" "c5=$c5 c99=$c99 obs='$obs'"
    fi
fi

kill -9 $(pgrep -f "fluxa run" 2>/dev/null) 2>/dev/null || true
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true; sleep 0.15

# =============================================================================
# CASO 3 — status durante loop infinito
# =============================================================================
echo ""
echo "  ── Caso 3: status durante loop infinito ─────────────────────────"

cat > "$WORK_DIR/loop_status.flx" << 'FLX'
prst int x = 10
int i = 0
while i >= 0 {
    i = i + 0
}
FLX

"$FLUXA" run "$WORK_DIR/loop_status.flx" -dev >"$WORK_DIR/c3_out.log" 2>"$WORK_DIR/c3_err.log" &

if ! wait_for_socket; then
    fail "caso3/socket" "IPC socket não apareceu em 3s"
else
    sleep 0.3
    status_out=$("$FLUXA" status 2>&1 || true)
    vlog "status: $status_out"
    if echo "$status_out" | grep -q "pid\|cycle\|prst"; then
        pass "caso3/status_responsivo"
    else
        fail "caso3/status_responsivo" "$status_out"
    fi
fi

kill -9 $(pgrep -f "fluxa run" 2>/dev/null) 2>/dev/null || true
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true

# =============================================================================
echo ""
echo "───────────────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "  Failures:"; printf '%b\n' "$ERRORS"
    exit 1
fi
echo "  → Issue #95: fluxa set em loops while: PASS"
exit 0
