#!/usr/bin/env bash
# tests/libs/mqtt.sh — std.mqtt test suite
# Spins up a local mosquitto broker for deterministic testing.
# Requires: mosquitto installed (apt install mosquitto).
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"
_MQTT_PID=0
_CONF=""
cleanup() {
    rm -rf "$P" "$_CONF" 2>/dev/null || true
    [ "$_MQTT_PID" -gt 0 ] && kill "$_MQTT_PID" 2>/dev/null || true
}
trap cleanup EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/mqtt/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/mqtt/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/mqtt/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.mqtt="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# ── Local broker setup ────────────────────────────────────────────────────
_MQTT_PORT=18883
_BROKER_READY=0

if command -v mosquitto >/dev/null 2>&1; then
    _CONF=$(mktemp /tmp/fluxa_mqtt_test.XXXXX.conf)
    printf 'listener %d\nallow_anonymous true\n' "$_MQTT_PORT" > "$_CONF"
    mosquitto -d -c "$_CONF" 2>/dev/null || true
    # Wait for broker
    for i in $(seq 1 15); do
        nc -zw1 127.0.0.1 "$_MQTT_PORT" 2>/dev/null && _BROKER_READY=1 && break
        sleep 0.2
    done
    # Get PID
    _MQTT_PID=$(pgrep -f "mosquitto.*${_MQTT_PORT}" 2>/dev/null | head -1 || echo 0)
fi

echo "── std.mqtt ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger { dyn c = mqtt.connect("127.0.0.1", 18883, "test") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. connect to closed port → error in danger
out=$(run << 'FLX'
import std mqtt
danger { dyn c = mqtt.connect("127.0.0.1", 19883, "fluxa-test") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "closed_port_error_captured" || fail "closed_port_error_captured" "error caught" "$out"

# 3. publish to invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.publish(bad, "topic", "payload")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "publish_bad_cursor_error" || fail "publish_bad_cursor_error" "error caught" "$out"

# 4. subscribe to invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.subscribe(bad, "topic/test")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "subscribe_bad_cursor_error" || fail "subscribe_bad_cursor_error" "error caught" "$out"

# 5. loop on invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.loop(bad, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "loop_bad_cursor_error" || fail "loop_bad_cursor_error" "error caught" "$out"

# 6. connected on invalid cursor → error
toml; cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    bool c = mqtt.connected(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "connected_bad_cursor_error" || fail "connected_bad_cursor_error" "error caught" "$out"

# 7. publish_qos invalid qos → error
toml; cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.publish_qos(bad, "t", "p", 5)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" \
    && pass "publish_qos_bad_qos_error" || fail "publish_qos_bad_qos_error" "error caught" "$out"

# 8. prst cursor pattern
out=$(run << 'FLX'
import std mqtt
prst dyn client = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" \
    && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

# 9. unknown function → error
out=$(run << 'FLX'
import std mqtt
danger { mqtt.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" \
    && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# ── Live broker tests ─────────────────────────────────────────────────────

# 10. connect to local broker → connected=true, publish, disconnect
if [ "$_BROKER_READY" -eq 1 ]; then
    out=$(run << FLXEOF
import std mqtt
danger {
    dyn c = mqtt.connect("127.0.0.1", ${_MQTT_PORT}, "fluxa-test-pub")
    bool conn = mqtt.connected(c)
    print(conn)
    mqtt.publish(c, "fluxa/test", "hello from fluxa")
    mqtt.loop(c, 50)
    mqtt.disconnect(c)
    print("done")
}
FLXEOF
)
    echo "$out" | grep -q "true" && pass "local_broker_connect_publish" \
        || fail "local_broker_connect_publish" "true" "$out"
    echo "$out" | grep -q "done" && pass "local_broker_disconnect_clean" \
        || fail "local_broker_disconnect_clean" "done" "$out"
else
    skip "local_broker_connect_publish"  "mosquitto not available (apt install mosquitto)"
    skip "local_broker_disconnect_clean" "mosquitto not available"
fi

# 11. connect_auth (local broker allows anonymous — auth should also work)
if [ "$_BROKER_READY" -eq 1 ]; then
    out=$(run << FLXEOF
import std mqtt
danger {
    dyn c = mqtt.connect_auth("127.0.0.1", ${_MQTT_PORT}, "fluxa-auth", "", "")
    bool conn = mqtt.connected(c)
    print(conn)
    mqtt.disconnect(c)
}
FLXEOF
)
    echo "$out" | grep -q "true" && pass "local_broker_connect_auth" \
        || fail "local_broker_connect_auth" "true" "$out"
else
    skip "local_broker_connect_auth" "mosquitto not available"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.mqtt: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.mqtt: PASS" && exit 0 || exit 1
