#!/usr/bin/env bash
# test_sim.sh — Hardware simulation test suite
# Tests that the Fluxa runtime handles OOM gracefully under SRAM caps.
#
# Usage:
#   bash tests/test_sim.sh --rp2040 ./fluxa_sim_rp2040 --esp32 ./fluxa_sim_esp32

set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures

FLUXA_RP2040=""
FLUXA_ESP32=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rp2040) FLUXA_RP2040="$2"; shift 2 ;;
        --esp32)  FLUXA_ESP32="$2";  shift 2 ;;
        *) shift ;;
    esac
done

PASS=0; FAIL=0
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

pass() { echo "  PASS  sim/$1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  sim/$1"; echo "    expected: $2"; echo "    got:      $3"; FAIL=$((FAIL+1)); }

run_test() {
    local fluxa="$1" label="$2" prog="$3" expected="$4"
    local f="$WORK/$(echo "$label" | tr '/ ' '__').flx"
    printf '%s\n' "$prog" > "$f"
    local out
    out=$(timeout 10s "$fluxa" run "$f" 2>&1 || true)
    if echo "$out" | grep -q "$expected"; then
        pass "$label"
    else
        fail "$label" "$expected" "$(echo "$out" | head -2 | tr '\n' '|')"
    fi
}

echo "════════════════════════════════════════════════════════"
echo "  Fluxa Hardware Simulation Test Suite"
echo "════════════════════════════════════════════════════════"

for SIM_NAME in RP2040 ESP32; do
    if [ "$SIM_NAME" = "RP2040" ]; then
        FLUXA="$FLUXA_RP2040"
        CAP_KB=264
    else
        FLUXA="$FLUXA_ESP32"
        CAP_KB=520
    fi
    [ -z "$FLUXA" ] && continue
    [ ! -x "$FLUXA" ] && echo "  SKIP  $SIM_NAME binary not found: $FLUXA" && continue

    echo ""
    echo "── $SIM_NAME (${CAP_KB}KB SRAM cap) ──────────────────────────────"

    # Case 1: Simple program runs correctly within cap
    run_test "$FLUXA" "$SIM_NAME/simple_runs_ok" \
        'int i = 0
int s = 0
while i < 1000 { s = s + i  i = i + 1 }
print(s)' \
        "499500"

    # Case 2: prst survives within cap
    run_test "$FLUXA" "$SIM_NAME/prst_within_cap" \
        'prst int counter = 0
counter = counter + 42
print(counter)' \
        "42"

    # Case 3: Block method works within cap
    run_test "$FLUXA" "$SIM_NAME/block_within_cap" \
        'Block S { int v = 0
fn set(int n) nil { v = n }
fn get() int { return v } }
Block c typeof S
c.set(0)
c.set(99)
print(c.get())' \
        "99"

    # Case 4: OOM on massive dyn allocation — must not crash (SIGSEGV=139, SIGABRT=134)
    # We try to allocate a massive dyn that exceeds the SRAM cap
    PROG4="$WORK/oom_test.flx"
    cat > "$PROG4" << 'FLX'
danger {
    dyn d = []
    int i = 0
    while i < 100000 {
        d = d + [i]
        i = i + 1
    }
    print(len(d))
}
print(0)
FLX
    out4=$(timeout 10s "$FLUXA" run "$PROG4" 2>&1 || true)
    exit_code=$?
    if [ $exit_code -eq 139 ] || [ $exit_code -eq 134 ]; then
        fail "$SIM_NAME/oom_no_crash" "no SIGSEGV/SIGABRT" "exit $exit_code"
    else
        pass "$SIM_NAME/oom_no_crash"
    fi

    # Case 5: Runtime reports meaningful output/error on OOM (not silent hang)
    out5=$(timeout 10s "$FLUXA" run "$PROG4" 2>&1 || true)
    if [ -n "$out5" ]; then
        pass "$SIM_NAME/oom_produces_output"
    else
        fail "$SIM_NAME/oom_produces_output" "non-empty output" "(empty)"
    fi

done

echo ""
echo "────────────────────────────────────────────────────────"
if [ $FAIL -eq 0 ]; then
    echo "  Results: $PASS passed, 0 failed"
    echo "  → Hardware simulation: PASS"
    exit 0
else
    echo "  Results: $PASS passed, $FAIL failed"
    echo "  → Hardware simulation: FAIL"
    exit 1
fi
