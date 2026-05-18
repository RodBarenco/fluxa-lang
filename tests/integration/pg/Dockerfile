# tests/integration/pg/Dockerfile
#
# Compila o Fluxa runtime com libpq e sobe um servidor PostgreSQL local
# para os testes de integração do std.pg.
#
# Uso:
#   cd tests/integration/pg
#   docker build -t fluxa-pg-test ../../../
#   docker run --rm fluxa-pg-test

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    gcc make python3 bash curl pkg-config \
    libffi-dev libffi8 \
    libsodium-dev libsodium23 \
    libsqlite3-dev libsqlite3-0 \
    libmosquitto-dev libmosquitto1 \
    libcurl4-openssl-dev libcurl4 \
    libserialport-dev libserialport0 \
    libpq-dev libpq5 \
    postgresql postgresql-client \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /fluxa

# Copy full project source and build Fluxa with libpq
COPY . .
RUN make build

ENTRYPOINT ["bash", "tests/integration/pg/inner.sh"]
CMD ["--fluxa", "/fluxa/fluxa"]
