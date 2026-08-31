#!/usr/bin/env bash
# Regression: return inside any while depth must leave the owning function.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

cat > "$WORK_DIR/main.flx" << 'FLX'
fn direct() int {
    int i = 0
    while i < 4 { return 77 }
    return 0 - 1
}

fn conditional() int {
    int i = 0
    while i < 4 {
        if i == 0 { return 99 }
        i = i + 1
    }
    return 0 - 1
}

fn nested() int {
    int outer = 0
    while outer < 2 {
        int inner = 0
        while inner < 2 {
            if inner == 0 { return 123 }
            inner = inner + 1
        }
        outer = outer + 1
    }
    return 0 - 1
}

fn array_exposed() int {
    int arr values[2] = [41, 42]
    int i = 0
    while i < 2 {
        if values[i] == 42 { return values[i] }
        i = i + 1
    }
    return 0 - 1
}

fn outside_control() int {
    int i = 0
    while i < 4 { i = i + 1 }
    return i
}

fn if_control() int {
    if true { return 55 }
    return 0 - 1
}

fn ping(int n) int {
    while n > 0 { return pong(n - 1) }
    return 7
}

fn pong(int n) int {
    while n > 0 { return ping(n - 1) }
    return 7
}

fn string_result() str {
    int i = 0
    while i < 1 { return "owned" }
    return "wrong"
}

Block Marker {
    int value = 0
    fn stop(int unused) nil {
        int i = 0
        while i < 1 { return }
        value = 1
    }
}
Block marker typeof Marker
marker.stop(0)

print("RESULT", direct(), conditional(), nested(), array_exposed(), outside_control(), if_control(), ping(2000), string_result(), marker.value)
FLX

out=$(timeout 10s "$FLUXA" run "$WORK_DIR/main.flx" 2>&1 || true)
expected="RESULT 77 99 123 42 4 55 7 owned 0"
if [[ "$out" != "$expected" ]]; then
    echo "  return_in_while: FAIL"
    echo "    expected: $expected"
    echo "    got:      $out"
    exit 1
fi

echo "  return_in_while: PASS"
