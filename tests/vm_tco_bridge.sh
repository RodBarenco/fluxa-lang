#!/usr/bin/env bash
# Regression: VM-to-interpreter calls must preserve tail-call returns.
# Initial VM parameters are borrowed; tail-call arguments are owned by the
# reused frame. Neither may leak into or be confused with another scope.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

cat > "$WORK_DIR/main.flx" <<'FLX'
fn leaf(int x) int { return x + 1 }
fn plain_tail(int x) int {
    int arr force_fallback[1] = [0]
    int y = force_fallback[0]
    return leaf(x + y)
}
fn plain_nontail(int x) int {
    int arr force_fallback[1] = [0]
    int y = force_fallback[0]
    int result = leaf(x + y)
    return result
}
fn hop_a(int n) int {
    int arr force_fallback[1] = [0]
    int y = force_fallback[0]
    if n <= 0 { return y }
    return hop_b(n - 1)
}
fn hop_b(int n) int {
    int arr force_fallback[1] = [0]
    int y = force_fallback[0]
    if n <= 0 { return y }
    return hop_a(n - 1)
}
fn string_leaf(str s) str { return s }
fn string_tail(str s) str {
    int arr force_fallback[1] = [0]
    int y = force_fallback[0]
    if y == 0 { return string_leaf(s) }
    return "wrong"
}

Block Box {
    int arr force_fallback[1] = [0]
    fn leaf(int x) int { return x + 1 }
    fn tail(int x) int {
        int y = force_fallback[0]
        return leaf(x + y)
    }
    fn nontail(int x) int {
        int y = force_fallback[0]
        int result = leaf(x + y)
        return result
    }
    fn internal(int unused) int {
        int i = 0
        int total = 0
        while i < 3 {
            int value = tail(i)
            total = total + value
            i = i + 1
        }
        return total
    }
}

Block box typeof Box
int i = 0
int plain = 0
int plain_control = 0
int external_method = 0
int external_control = 0
str text = ""
while i < 3 {
    plain = plain + plain_tail(i)
    plain_control = plain_control + plain_nontail(i)
    external_method = external_method + box.tail(i)
    external_control = external_control + box.nontail(i)
    text = string_tail("safe")
    i = i + 1
}

int deep = 1
int once = 0
while once < 1 {
    deep = hop_a(2000)
    once = once + 1
}
print("RESULT", plain, plain_control, external_method, external_control,
      box.internal(0), deep, text)
FLX

out=$(timeout 10s "$FLUXA" run "$WORK_DIR/main.flx" 2>&1 || true)
if echo "$out" | grep -q '^RESULT 6 6 6 6 6 0 safe$'; then
    echo "  vm_tco_bridge: PASS"
else
    echo "  vm_tco_bridge: FAIL"
    echo "    expected: RESULT 6 6 6 6 6 0 safe"
    echo "    got:      $out"
    exit 1
fi
