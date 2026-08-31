#!/usr/bin/env bash
# Regression: nested calls must not overwrite an enclosing call's arguments.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

cat > "$WORK_DIR/main.flx" <<'FLX'
fn add1(int value) int { return value + 1 }
fn add2(int value) int { return add1(add1(value)) }

Block Mapper {
    fn map(int value) int {
        // Keep this callee on the tree-walking path, like the production
        // method that exposed the caller-frame corruption.
        danger { int marker = 0 }
        return value - 3
    }
}

int i = 0
while i < 2 {
    print("F in a loop ", i, " -> ", add1(56))
    print("M in a loop ", i, " -> ", Mapper.map(60))
    print("middle ", add2(5), " tail ", add1(8))
    print("many ", add1(0), " ", add1(1), " ", add1(2), " ", add1(3), " ", add1(4), " ", add1(5), " ", add1(6), " ", add1(7), " ", add1(8), " ", add1(9), " ", add1(10))
    i = i + 1
}
FLX

out=$(timeout 10s "$FLUXA" run "$WORK_DIR/main.flx" 2>&1 || true)
expected='F in a loop  0  ->  57
M in a loop  0  ->  57
middle  7  tail  9
many  1   2   3   4   5   6   7   8   9   10   11
F in a loop  1  ->  57
M in a loop  1  ->  57
middle  7  tail  9
many  1   2   3   4   5   6   7   8   9   10   11'

if [[ "$out" != "$expected" ]]; then
    echo "  call_arg_in_loop: FAIL"
    echo "    expected: $expected"
    echo "    got:      $out"
    exit 1
fi

echo "  call_arg_in_loop: PASS"
