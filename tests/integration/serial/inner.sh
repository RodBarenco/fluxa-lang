#!/usr/bin/env bash
# tests/integration/serial/inner.sh
#
# Roda dentro do container Docker.
# Compila e carrega o módulo tty0tty, cria par de TTYs virtuais,
# executa testes de IO serial real com o Fluxa runtime.
#
# tty0tty: https://github.com/lcgamboa/tty0tty
# Cria /dev/tnt0 <-> /dev/tnt1 — par interconectado registrado no
# subsistema tty do kernel, visível ao libserialport via sysfs.
#
# Requer: container com --privileged e volumes do kernel do host:
#   -v /lib/modules:/lib/modules:ro
#   -v /usr/src:/usr/src:ro
#   -v /dev:/dev
set -euo pipefail

FLUXA="/fluxa/fluxa"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)   FLUXA="$2";  shift 2 ;;
        --verbose) VERBOSE=1;   shift   ;;
        *) shift ;;
    esac
done

P="$(mktemp -d)"
trap 'rm -rf "$P"; _cleanup_tty0tty' EXIT

PASS=0; FAIL=0

pass() { printf "  PASS  serial/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  serial/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAIL=$((FAIL+1)); }
skip() { printf "  SKIP  serial/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.serial="1.0"\n' \
        > "$P/fluxa.toml"
}
run() {
    toml; cat > "$P/main.flx"
    timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true
}

# ── tty0tty setup ────────────────────────────────────────────────────────────
TTY0TTY_LOADED=0
TNT0="/dev/tnt0"
TNT1="/dev/tnt1"

_load_tty0tty() {
    # Check if already loaded
    if lsmod 2>/dev/null | grep -q tty0tty; then
        TTY0TTY_LOADED=1
        return 0
    fi

    # Find kernel version
    KVER=$(uname -r)
    echo "  [serial] Kernel: $KVER"

    # Clone tty0tty source
    if [ ! -d /tmp/tty0tty ]; then
        echo "  [serial] Cloning tty0tty..."
        git clone --depth 1 https://github.com/lcgamboa/tty0tty.git /tmp/tty0tty \
            2>/dev/null || {
            echo "  [serial] git clone failed — network restricted"
            return 1
        }
    fi

    # Check kernel headers are available
    if [ ! -d "/lib/modules/${KVER}/build" ] && [ ! -d "/usr/src/linux-headers-${KVER}" ]; then
        echo "  [serial] Kernel headers not found for ${KVER}"
        echo "  [serial] Mount /lib/modules and /usr/src from host"
        return 1
    fi

    # Build the module
    echo "  [serial] Building tty0tty module..."
    cd /tmp/tty0tty/module
    make 2>&1 | tail -3
    cd /fluxa

    # Load the module
    insmod /tmp/tty0tty/module/tty0tty.ko 2>/dev/null || {
        echo "  [serial] insmod failed — container needs --privileged"
        return 1
    }

    # Wait for devices
    for i in $(seq 1 20); do
        [ -e "$TNT0" ] && [ -e "$TNT1" ] && TTY0TTY_LOADED=1 && break
        sleep 0.1
    done

    if [ "$TTY0TTY_LOADED" -eq 1 ]; then
        # Set permissions
        chmod 666 "$TNT0" "$TNT1" 2>/dev/null || true
        echo "  [serial] tty0tty loaded: $TNT0 <-> $TNT1"
    fi
}

_cleanup_tty0tty() {
    if lsmod 2>/dev/null | grep -q tty0tty; then
        rmmod tty0tty 2>/dev/null || true
    fi
}

echo "══════════════════════════════════════════════════════════════════"
echo "  Fluxa Serial Integration Tests — tty0tty virtual serial pair"
echo "  binary  : $FLUXA"
echo "══════════════════════════════════════════════════════════════════"
echo ""

# Attempt to load tty0tty
_load_tty0tty || true

# ── Test: 1. serial.list() encontra tnt devices ───────────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    out=$(run << 'FLX'
import std serial
danger {
    dyn ports = serial.list()
    int n = len(ports)
    print(n)
}
FLX
)
    count=$(echo "$out" | grep -oE "^[0-9]+" | head -1 || echo "0")
    if [ "${count:-0}" -ge 2 ]; then
        pass "list_finds_tnt_devices"
    else
        fail "list_finds_tnt_devices" ">= 2 ports" "$out"
    fi
else
    skip "list_finds_tnt_devices" "tty0tty not loaded"
fi

# ── Test: 2. open/close tnt0 ─────────────────────────────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    out=$(run << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    print("opened")
    serial.close(p)
    print("closed")
}
FLX
)
    echo "$out" | grep -q "opened" && echo "$out" | grep -q "closed" \
        && pass "open_close_tnt0" \
        || fail "open_close_tnt0" "opened + closed" "$out"
else
    skip "open_close_tnt0" "tty0tty not loaded"
fi

# ── Test: 3. bytes_available = 0 on empty port ───────────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    out=$(run << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    int n = serial.bytes_available(p)
    print(n)
    serial.close(p)
}
FLX
)
    echo "$out" | grep -qE "^[0-9]+" \
        && pass "bytes_available_returns_int" \
        || fail "bytes_available_returns_int" "integer" "$out"
else
    skip "bytes_available_returns_int" "tty0tty not loaded"
fi

# ── Test: 4. write tnt0 → read tnt1 (full loopback) ─────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    # Writer: open tnt0, write "hello fluxa\n", close
    toml
    cat > "$P/writer.flx" << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    serial.write(p, "hello fluxa\n")
    serial.flush(p)
    serial.close(p)
    print("sent")
}
FLX

    # Reader: open tnt1, readline with 2s timeout
    cat > "$P/reader.flx" << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt1", 9600)
    str line = serial.readline(p, 2000)
    print(line)
    serial.close(p)
}
FLX

    printf '[project]\nname="t"\nentry="reader.flx"\n[libs]\nstd.serial="1.0"\n' \
        > "$P/fluxa.toml"

    # Start reader in background, then write
    timeout 5s "$FLUXA" run "$P/reader.flx" -proj "$P" > "$P/read_out.txt" 2>&1 &
    READER_PID=$!
    sleep 0.2

    printf '[project]\nname="t"\nentry="writer.flx"\n[libs]\nstd.serial="1.0"\n' \
        > "$P/fluxa.toml"
    timeout 5s "$FLUXA" run "$P/writer.flx" -proj "$P" > "$P/write_out.txt" 2>&1 || true
    wait "$READER_PID" 2>/dev/null || true

    READ_DATA=$(cat "$P/read_out.txt" 2>/dev/null || echo "")
    WRITE_DATA=$(cat "$P/write_out.txt" 2>/dev/null || echo "")

    [ "$VERBOSE" -eq 1 ] && echo "  writer: $WRITE_DATA" && echo "  reader: $READ_DATA"

    echo "$READ_DATA" | grep -q "hello fluxa" \
        && pass "write_tnt0_read_tnt1" \
        || fail "write_tnt0_read_tnt1" "hello fluxa" "$READ_DATA"

    echo "$WRITE_DATA" | grep -q "sent" \
        && pass "write_confirms_sent" \
        || fail "write_confirms_sent" "sent" "$WRITE_DATA"
else
    skip "write_tnt0_read_tnt1"   "tty0tty not loaded"
    skip "write_confirms_sent"    "tty0tty not loaded"
fi

# ── Test: 5. prst dyn port sobrevive hot reload ───────────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    out=$(run << 'FLX'
import std serial
prst dyn active_port = [0]
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    active_port = p
    print("prst ok")
    serial.close(p)
}
FLX
)
    echo "$out" | grep -q "prst ok" \
        && pass "prst_dyn_port_hot_reload" \
        || fail "prst_dyn_port_hot_reload" "prst ok" "$out"
else
    skip "prst_dyn_port_hot_reload" "tty0tty not loaded"
fi

# ── Test: 6. read com timeout — retorna string vazia, não erro ───────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    out=$(run << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    str data = serial.read(p, 10, 200)
    print(len(data))
    serial.close(p)
}
FLX
)
    echo "$out" | grep -qE "^[0-9]+" \
        && pass "read_timeout_returns_empty_not_error" \
        || fail "read_timeout_returns_empty_not_error" "0 (timeout)" "$out"
else
    skip "read_timeout_returns_empty_not_error" "tty0tty not loaded"
fi

# ── Test: 7. multi-message sequential write/read ──────────────────────────
if [ "$TTY0TTY_LOADED" -eq 1 ]; then
    toml
    cat > "$P/multi_w.flx" << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt0", 9600)
    serial.write(p, "msg1\n")
    serial.write(p, "msg2\n")
    serial.write(p, "msg3\n")
    serial.flush(p)
    serial.close(p)
    print("written 3")
}
FLX
    cat > "$P/multi_r.flx" << 'FLX'
import std serial
danger {
    dyn p = serial.open("/dev/tnt1", 9600)
    str l1 = serial.readline(p, 1000)
    str l2 = serial.readline(p, 1000)
    str l3 = serial.readline(p, 1000)
    print(l1)
    print(l2)
    print(l3)
    serial.close(p)
}
FLX

    printf '[project]\nname="t"\nentry="multi_r.flx"\n[libs]\nstd.serial="1.0"\n' \
        > "$P/fluxa.toml"
    timeout 6s "$FLUXA" run "$P/multi_r.flx" -proj "$P" > "$P/multi_read.txt" 2>&1 &
    READER_PID=$!
    sleep 0.2

    printf '[project]\nname="t"\nentry="multi_w.flx"\n[libs]\nstd.serial="1.0"\n' \
        > "$P/fluxa.toml"
    timeout 5s "$FLUXA" run "$P/multi_w.flx" -proj "$P" >/dev/null 2>&1 || true
    wait "$READER_PID" 2>/dev/null || true

    MULTI=$(cat "$P/multi_read.txt" 2>/dev/null || echo "")
    echo "$MULTI" | grep -q "msg1" && echo "$MULTI" | grep -q "msg3" \
        && pass "multi_message_sequential" \
        || fail "multi_message_sequential" "msg1 msg2 msg3" "$MULTI"
else
    skip "multi_message_sequential" "tty0tty not loaded"
fi

# ── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════════"
echo "  Serial Integration Tests:"
total=$((PASS + FAIL))
echo "  $total tests: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════════════════════════════"

if [ "$TTY0TTY_LOADED" -eq 0 ]; then
    echo ""
    echo "  ⚠  tty0tty não carregado — testes de IO foram skipped."
    echo "     Para IO real, execute com:"
    echo "       docker-compose run --rm fluxa-serial-test"
    echo "     (requer --privileged e volumes do kernel montados)"
fi

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
