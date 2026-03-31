# Dockerfile — Fluxa IoT Simulation Container
# Simula ambiente de dispositivo embedded (ESP32/RP2040-like) com:
#   - Runtime Fluxa compilado estaticamente para o container
#   - Volume NVS montado em /nvs (simula memória não-volátil)
#   - Script de orquestração para os cenários de handover

FROM ubuntu:24.04 AS builder

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    gcc make libffi-dev python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
# Compila o binário fluxa
RUN make clean && make

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    libffi8 python3 bash \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /fluxa

# Copia binário e artefatos de teste
COPY --from=builder /build/fluxa               /fluxa/fluxa
COPY --from=builder /build/tests/integration   /fluxa/tests/integration
COPY --from=builder /build/tests/integration/fixtures /fluxa/tests/integration/fixtures

# Volume NVS — montado externamente para simular Flash persistente
VOLUME ["/nvs"]

# Ponto de entrada: roda os cenários de integração
ENTRYPOINT ["/fluxa/tests/integration/run_all.sh", "--fluxa", "/fluxa/fluxa", "--nvs", "/nvs"]
