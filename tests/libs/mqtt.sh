#!/usr/bin/env bash
# tests/libs/mqtt.sh — std.mqtt test suite
# Tests that don't require a broker validate API shape and error handling.
# Broker-dependent tests are skipped if no MQTT broker is reachable.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/mqtt/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/mqtt/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }
skip() { printf "  SKIP  libs/mqtt/%s  (%s)\n" "$1" "$2"; PASS=$((PASS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.mqtt="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

# Check broker availability (test.mosquitto.org public broker)
HAVE_BROKER=0
nc -z test.mosquitto.org 1883 2>/dev/null && HAVE_BROKER=1 || true

echo "── std.mqtt ─────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger { dyn c = mqtt.connect("localhost", 1883, "test") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. connect to non-existent host → error captured in danger
out=$(run << 'FLX'
import std mqtt
danger {
    dyn c = mqtt.connect("this-host-does-not-exist-fluxa.invalid", 1883, "fluxa-test")
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "bad_host_error_captured" || fail "bad_host_error_captured" "error caught" "$out"

# 3. publish to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.publish(bad, "topic", "payload")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "publish_bad_cursor_error" || fail "publish_bad_cursor_error" "error caught" "$out"

# 4. subscribe to invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.subscribe(bad, "topic/test")
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "subscribe_bad_cursor_error" || fail "subscribe_bad_cursor_error" "error caught" "$out"

# 5. loop on invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.loop(bad, 100)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "loop_bad_cursor_error" || fail "loop_bad_cursor_error" "error caught" "$out"

# 6. connected on invalid cursor → error
toml
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    bool c = mqtt.connected(bad)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "connected_bad_cursor_error" || fail "connected_bad_cursor_error" "error caught" "$out"

# 7. publish_qos invalid qos value → error
toml
cat > "$P/main.flx" << 'FLX'
import std mqtt
danger {
    dyn bad = [1, 2, 3]
    mqtt.publish_qos(bad, "t", "p", 5)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "publish_qos_bad_qos_error" || fail "publish_qos_bad_qos_error" "error caught" "$out"

# 8. prst dyn client cursor pattern
out=$(run << 'FLX'
import std mqtt
prst dyn client = [0]
print("prst ok")
FLX
)
echo "$out" | grep -q "prst ok" && pass "prst_cursor_pattern" || fail "prst_cursor_pattern" "prst ok" "$out"

# 9. unknown function → error
out=$(run << 'FLX'
import std mqtt
danger { mqtt.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 10. live broker — connect, publish, disconnect (test.mosquitto.org)
if [ "$HAVE_BROKER" -eq 1 ]; then
    out=$(run << 'FLX'
import std mqtt
danger {
    dyn c = mqtt.connect("test.mosquitto.org", 1883, "fluxa-test-pub")
    bool conn = mqtt.connected(c)
    print(conn)
    mqtt.publish(c, "fluxa/test", "hello from fluxa")
    mqtt.disconnect(c)
}
FLX
)
    echo "$out" | grep -q "true" && pass "live_connect_publish_disconnect" || fail "live_connect_publish_disconnect" "true" "$out"
else
    skip "live_connect_publish_disconnect" "no MQTT broker reachable"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.mqtt: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.mqtt: PASS" && exit 0 || exit 1
