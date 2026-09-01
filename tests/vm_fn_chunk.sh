#!/usr/bin/env bash
# Indexing inside a compiled function body, and the limits that come with it.
#
# A function body compiles to a chunk cached on its AST node and reused for
# every later call, from any instance. Nothing instance-specific may be baked
# into it, so eligibility is decided from *declarations* only — never from
# whatever array happened to be live when the chunk was built. These are the
# limits that decision creates; each case states one, and every case checks the
# answer against what the evaluator produces for the same expression.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
fail() { echo "  vm_fn_chunk: FAIL ($1)"; echo "    expected: $2"; echo "    got: $3"; exit 1; }
run() { timeout 10s "$FLUXA" run "$WORK_DIR/$1" 2>&1 || true; }

# ── Eligible: declared int/float/bool arrays reachable from the declaration ──
# Parameters, the function's own array locals, and the array fields of the
# Block the function is a method of.
cat > "$WORK_DIR/ok.flx" << 'FLX'
fn from_param(int arr src, int n) int {
    int i = 0  int acc = 0
    while i < n { acc = acc + src[i]  i = i + 1 }
    return acc
}
fn from_local(int n) int {
    int arr own[8] = 2
    int i = 0  int acc = 0
    while i < n { own[i] = own[i] + 1  acc = acc + own[i]  i = i + 1 }
    return acc
}
Block N {
    int arr fi[8] = 3
    float arr ff[4] = 1.5
    bool arr fb[4] = true
    fn from_field(int n) int {
        int i = 0  int acc = 0
        while i < n { fi[i] = fi[i] + 1  acc = acc + fi[i]  i = i + 1 }
        return acc
    }
    fn floats(int n) float {
        int i = 0  float acc = 0.0
        while i < n { acc = acc + ff[i]  i = i + 1 }
        return acc
    }
    fn bools(int n) int {
        int i = 0  int acc = 0
        while i < n { if fb[i] { acc = acc + 1 }  i = i + 1 }
        return acc
    }
    fn drive(int n) int { return from_field(n) }
}
Block nn typeof N
int arr data[8] = 5
print("OK", from_param(data, 4), from_local(4), nn.drive(4), nn.floats(4), nn.bools(4))
FLX
out=$(run ok.flx)
[[ "$out" == "OK 20 12 16 6 4" ]] || fail "eligible arrays" "OK 20 12 16 6 4" "$out"

# ── The caching limit itself ─────────────────────────────────────────────────
# One chunk serves every instance, so it must resolve the array per call. Two
# instances of the same Block must not see each other's elements.
cat > "$WORK_DIR/two.flx" << 'FLX'
Block N {
    int arr fi[4] = 0
    fn bump(int n) int {
        int i = 0  int acc = 0
        while i < n { fi[i] = fi[i] + 1  acc = acc + fi[i]  i = i + 1 }
        return acc
    }
    fn drive(int n) int { return bump(n) }
    fn peek(int k) int { return fi[k] }
}
Block a typeof N
Block b typeof N
int r1 = a.drive(4)
int r2 = a.drive(4)
int r3 = b.drive(4)
print("TWO", r1, r2, r3, a.peek(0), b.peek(0))
FLX
out=$(run two.flx)
[[ "$out" == "TWO 4 8 4 2 1" ]] || fail "one chunk, two instances" "TWO 4 8 4 2 1" "$out"

# ── Excluded, and still correct ──────────────────────────────────────────────
# Each of these keeps the evaluator's path. The point is that they still give
# the right answer, not that they are refused.
# The tier is only reached by a call made from already-compiled bytecode, so
# every case here is driven from inside a while loop — a call from the
# evaluator would never exercise it and the case would prove nothing.
cat > "$WORK_DIR/excluded.flx" << 'FLX'
Block N {
    int arr guard[4] = 1
    str arr names[4] = "n"
    prst int arr counts[4] = 0
    fn one_str(int k) str { names[k] = "x"  return names[k] }
    fn one_prst(int k) int { counts[k] = counts[k] + 1  return counts[k] }
    fn drive_s(int n) str {
        int i = 0  str last = ""
        while i < n { last = one_str(i)  i = i + guard[0] }
        return last
    }
    fn drive_p(int n) int {
        int i = 0  int acc = 0
        while i < n { acc = acc + one_prst(i)  i = i + guard[0] }
        return acc
    }
}
Block nn typeof N
fn one_untyped(arr any, int k) int { return any[k] }
fn drive_u(arr any, int n) int {
    int arr g[4] = 1
    int i = 0  int acc = 0
    while i < n { acc = acc + one_untyped(any, i)  i = i + g[0] }
    return acc
}
int arr data[4] = 7
print("EXC", nn.drive_s(3), nn.drive_p(3), drive_u(data, 3))
FLX
out=$(run excluded.flx)
[[ "$out" == "EXC x 3 21" ]] || fail "excluded kinds stay correct" "EXC x 3 21" "$out"

# ── The runtime keeps the guarantee, not the compiler ────────────────────────
# Eligibility is decided once per function; the element type is rechecked on
# every read and write, so bounds and type errors still report.
cat > "$WORK_DIR/bounds.flx" << 'FLX'
fn over(int arr src, int at) int { return src[at] }
int arr data[4] = 1
print("B", over(data, 9))
FLX
out=$(run bounds.flx)
grep -q "array index out of bounds: src\[9\] (size 4)" <<<"$out" ||
    fail "bounds inside a function chunk" "out-of-bounds error" "$out"

cat > "$WORK_DIR/type.flx" << 'FLX'
fn put(int arr dst, int at) nil { dst[at] = "texto" }
int arr data[4] = 1
put(data, 0)
print("never")
FLX
out=$(run type.flx)
grep -q "type error: dst\[0\] is int, cannot assign str" <<<"$out" ||
    fail "element type inside a function chunk" "type error" "$out"

echo "  vm_fn_chunk: PASS"
