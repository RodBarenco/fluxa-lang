#!/usr/bin/env bash
# =============================================================================
# tests/integration/run_all.sh — Runner mestre de integration tests
# =============================================================================
#
# Executa todos os cenários de simulação em sequência.
# Saída compatível com o formato do run_tests.sh da suite de unit tests.
#
# Uso:
#   ./run_all.sh [--fluxa <path>] [--nvs <dir>] [--verbose] [--scenario <1|2>]
#
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

FLUXA="${PROJECT_ROOT}/fluxa"
NVS_DIR="/tmp/fluxa-nvs"
VERBOSE=0
ONLY_SCENARIO=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)    FLUXA="$2";          shift 2 ;;
        --nvs)      NVS_DIR="$2";        shift 2 ;;
        --verbose)  VERBOSE=1;           shift   ;;
        --scenario) ONLY_SCENARIO="$2";  shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ ! -x "$FLUXA" ]]; then
    echo "ERRO: binário não encontrado: $FLUXA"
    echo "      Execute 'make' antes de rodar integration tests."
    exit 1
fi

VERBOSE_FLAG=""
[[ $VERBOSE -eq 1 ]] && VERBOSE_FLAG="--verbose"

TOTAL_PASS=0
TOTAL_FAIL=0
SCENARIO_RESULTS=()

run_scenario() {
    local num="$1" script="$2"
    shift 2
    local extra_args=("$@")

    echo ""
    if bash "$script" --fluxa "$FLUXA" $VERBOSE_FLAG "${extra_args[@]}" 2>&1; then
        SCENARIO_RESULTS+=("  PASS  Cenário $num")
        TOTAL_PASS=$((TOTAL_PASS+1))
    else
        SCENARIO_RESULTS+=("  FAIL  Cenário $num")
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    fi
}

echo "══════════════════════════════════════════════════════════════════"
echo "  Fluxa Integration Tests — Atomic Handover Validation"
echo "  binary  : $FLUXA"
echo "  workdir : $NVS_DIR"
echo "══════════════════════════════════════════════════════════════════"

if [[ -z "$ONLY_SCENARIO" ]] || [[ "$ONLY_SCENARIO" == "1" ]]; then
    run_scenario 1 "$SCRIPT_DIR/scenario1/run.sh" --nvs "$NVS_DIR"
fi

if [[ -z "$ONLY_SCENARIO" ]] || [[ "$ONLY_SCENARIO" == "2" ]]; then
    run_scenario 2 "$SCRIPT_DIR/scenario2/run.sh"
fi

echo ""
echo "══════════════════════════════════════════════════════════════════"
echo "  Resumo:"
for r in "${SCENARIO_RESULTS[@]}"; do
    echo "$r"
done
echo ""
echo "  Total: $TOTAL_PASS passaram, $TOTAL_FAIL falharam"
echo "══════════════════════════════════════════════════════════════════"

if [[ $TOTAL_FAIL -gt 0 ]]; then
    echo ""
    echo "  ⚠  CRITÉRIO DE ACEITE NÃO ATINGIDO"
    echo "  Sprint 8 não está pronta para produção."
    exit 1
fi

echo ""
echo "  ✓  TODOS OS CRITÉRIOS DE ACEITE ATINGIDOS"
echo "  Sprint 8 — Handover Atômico: APROVADO"
exit 0
