#!/usr/bin/env bash
# Regression: content equality for separately owned strings in bytecode loops.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

cat > "$WORK_DIR/main.flx" << 'FLX'
int local_eq = 0
int i = 0
while i < 3 {
    str a = "X"
    str b = "X"
    if a == b { local_eq = local_eq + 1 }
    i = i + 1
}

int literal_eq = 0
int j = 0
while j < 3 {
    if "X" == "X" { literal_eq = literal_eq + 1 }
    j = j + 1
}

int local_neq = 0
int k = 0
while k < 3 {
    str a = "X"
    str b = "Y"
    if a != b { local_neq = local_neq + 1 }
    k = k + 1
}

fn function_loop(int unused) int {
    int hits = 0
    int n = 0
    while n < 3 {
        str a = "ABCDE"
        str b = "ABCDE"
        if a == b { hits = hits + 1 }
        n = n + 1
    }
    return hits
}

print("RESULT", local_eq, literal_eq, local_neq, function_loop(0))
FLX

out=$(timeout 10s "$FLUXA" run "$WORK_DIR/main.flx" 2>&1 || true)
if echo "$out" | grep -q '^RESULT 3 3 3 3$'; then
    echo "  str_eq_in_loop: PASS"
else
    echo "  str_eq_in_loop: FAIL"
    echo "    expected: RESULT 3 3 3 3"
    echo "    got:      $out"
    exit 1
fi
