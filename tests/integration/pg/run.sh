#!/usr/bin/env bash
# tests/integration/pg/run.sh
#
# Wrapper externo: builda e executa o container de teste PostgreSQL.
# Pode ser chamado diretamente ou pelo run_all.sh de integração.
#
# Uso:
#   bash tests/integration/pg/run.sh
#   bash tests/integration/pg/run.sh --verbose
#   bash tests/integration/pg/run.sh --build    # força rebuild da imagem
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERBOSE=0
FORCE_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verbose)   VERBOSE=1;     shift ;;
        --build)     FORCE_BUILD=1; shift ;;
        *) shift ;;
    esac
done

# Verificar pré-requisitos
if ! command -v docker >/dev/null 2>&1; then
    echo "  SKIP  pg/integration — docker não encontrado"
    echo "         Instale docker: https://docs.docker.com/engine/install/"
    exit 0
fi

if ! docker info >/dev/null 2>&1; then
    echo "  SKIP  pg/integration — daemon Docker não está rodando"
    echo "         Execute: sudo systemctl start docker"
    exit 0
fi

echo "══════════════════════════════════════════════════════════════════"
echo "  Fluxa PostgreSQL Integration Tests"
echo "  Usando container com PostgreSQL local"
echo "══════════════════════════════════════════════════════════════════"

cd "$PROJECT_ROOT"

BUILD_ARGS=""
[ "$FORCE_BUILD" -eq 1 ] && BUILD_ARGS="--no-cache"
echo "  [pg] Building Docker image..."
docker build $BUILD_ARGS \
    -f tests/integration/pg/Dockerfile \
    -t fluxa-pg-test \
    . 2>&1 | grep -E "Step|Successfully|Successfully built|error|ERROR" || true

echo "  [pg] Running tests..."
VERBOSE_FLAG=""
[ "$VERBOSE" -eq 1 ] && VERBOSE_FLAG="--verbose"

docker run --rm \
    fluxa-pg-test \
    --fluxa /fluxa/fluxa $VERBOSE_FLAG
