#!/usr/bin/env bash
# Regression and semantic parity for primitive fixed-array bytecode indexing.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
cat > "$WORK_DIR/main.flx" << 'FLX'
int arr nums[3] = [1, 2, 3]
float arr reals[2] = [1.5, 2.5]
bool arr flags[2] = [true, false]
Block Store {
    int arr nums[2] = [10, 20]
    fn run(int unused) int {
        int i = 0  int total = 0
        while i < 2 { total = total + nums[i]  nums[i] = nums[i] + 1  i = i + 1 }
        return total + nums[0] + nums[1]
    }
}
Block store typeof Store
fn sum_arr(int arr values, int count) int {
    int i = 0  int total = 0
    while i < count { total = total + values[i]  i = i + 1 }
    return total
}
int i = 0  int total = 0
while i < 3 { total = total + nums[i]  nums[i] = nums[i] * 2  i = i + 1 }
int j = 0
while j < 2 {
    reals[j] = reals[j] + 0.5
    if flags[j] { flags[j] = false } else { flags[j] = true }
    j = j + 1
}
print("RESULT", total, nums[0], nums[1], nums[2], reals[0], reals[1], flags[0], flags[1], store.run(0), sum_arr(nums, 3))
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/main.flx" 2>&1 || true)
expected="RESULT 6 2 4 6 2 3 false true 62 12"
if [[ "$out" != "$expected" ]]; then
    echo "  vm_array_index: FAIL"; echo "    expected: $expected"; echo "    got: $out"; exit 1
fi
cat > "$WORK_DIR/bounds.flx" << 'FLX'
Block Side { int calls = 0  fn bump(int unused) int { calls = calls + 1  return 7 } }
Block side typeof Side
int arr values[1] = [1]
int i = 0
while i < 1 { values[2] = side.bump(0)  i = i + 1 }
print("CALLS", side.calls)
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/bounds.flx" 2>&1 || true)
if ! grep -q "array index out of bounds: values\[2\] (size 1)" <<<"$out" || grep -q "CALLS 1" <<<"$out"; then
    echo "  vm_array_index: FAIL (bounds/RHS ordering)"; echo "    got: $out"; exit 1
fi
cat > "$WORK_DIR/type.flx" << 'FLX'
int arr values[1] = [1]
int i = 0
while i < 1 { values[0] = "wrong"  i = i + 1 }
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/type.flx" 2>&1 || true)
if ! grep -q "type error: values\[0\] is int, cannot assign str" <<<"$out"; then
    echo "  vm_array_index: FAIL (element type enforcement)"; echo "    got: $out"; exit 1
fi
cat > "$WORK_DIR/fallback.flx" << 'FLX'
str arr words[2] = ["A", "B"]
dyn items = [1, 2]
int i = 0
while i < 2 { words[i] = words[i]  items[i] = items[i] + 1  i = i + 1 }
print("FALLBACK", words[0], words[1], items[0], items[1])
FLX
out=$(timeout 10s "$FLUXA" run "$WORK_DIR/fallback.flx" 2>&1 || true)
if [[ "$out" != "FALLBACK A B 2 3" ]]; then
    echo "  vm_array_index: FAIL (fallback parity)"; echo "    got: $out"; exit 1
fi
cat > "$WORK_DIR/large_field.flx" << 'FLX'
// Regression: bytecode eligibility used to rescan every element of every
// field array each time this while was compiled.  With a framebuffer-sized
// array, composing many short calls dominated the useful work.
Block Frame {
    int arr pixels[196608] = 0
    fn touch(int at) int {
        int i = 0
        int value = 0
        while i < 1 {
            value = pixels[at]
            pixels[at] = value + 1
            i = i + 1
        }
        return value
    }
    fn get(int at) int { return pixels[at] }
}
Block frame typeof Frame
int n = 0
int sum = 0
while n < 1000 {
    sum = sum + frame.touch(n % 64)
    n = n + 1
}
print("LARGE", sum, frame.get(0), frame.get(63))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/large_field.flx" 2>&1 || true)
if [[ "$out" != "LARGE 7320 16 15" ]]; then
    echo "  vm_array_index: FAIL (large field eligibility cache)"; echo "    got: $out"; exit 1
fi
echo "  vm_array_index: PASS"
