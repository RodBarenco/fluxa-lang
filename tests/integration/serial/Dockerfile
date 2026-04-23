# tests/integration/serial/Dockerfile
#
# Compila o Fluxa runtime e empacota o script de teste.
# O módulo tty0tty é compilado em runtime pelo inner.sh
# usando os kernel headers montados do host via volume.
#
# Uso:
#   cd tests/integration/serial
#   docker-compose build
#   docker-compose run --rm fluxa-serial-test

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    gcc make python3 bash kmod git curl \
    libffi-dev libffi8 \
    libsodium-dev libsodium23 \
    libsqlite3-dev libsqlite3-0 \
    libserialport-dev libserialport0 \
    libmosquitto-dev libmosquitto1 \
    libcurl4-openssl-dev libcurl4 \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /fluxa

# Copy full project source and build Fluxa
COPY . .
RUN make build

ENTRYPOINT ["bash", "tests/integration/serial/inner.sh"]
CMD ["--fluxa", "/fluxa/fluxa"]
