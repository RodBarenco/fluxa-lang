#!/usr/bin/env bash
# Leak growth per iteration, under LeakSanitizer.
#
# A leak that scales with the iteration count is the one that ends a long run;
# a bounded residue is a fixed cost paid once. Neither is visible to the rest
# of the suite, because a leaking program still prints the right answer — the
# defect this guards against was found only by varying the iteration count by
# hand and watching the allocation count follow.
#
# Each case runs at N and at 10N. The check is on the *allocation count*, not
# the byte total: a bounded residue keeps the same count at both sizes, while
# anything per-iteration multiplies. ASan and UBSan errors fail immediately.
#
#   bash tests/leak_scaling.sh --fluxa ./fluxa_asan
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa_asan"
while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
[[ -x "$FLUXA" ]] || { echo "  leak_scaling: no sanitizer build at $FLUXA"; exit 1; }
FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "leak_scaling"
version = "0.1.0"

[libs]
std.strings = "1.0"
TOML

PASS=0; FAIL=0
N_SMALL=100
N_BIG=1000

# run <name> — reads the .flx body on stdin with NNN as the iteration count
run_case() {
    local name="$1" body; body="$(cat)"
    local small big out sa ba sb bb
    for n in "$N_SMALL" "$N_BIG"; do
        printf '%s\n' "${body//NNN/$n}" > "$WORK_DIR/case.flx"
        out=$(cd "$WORK_DIR" && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
              timeout 900s "$FLUXA" run case.flx 2>&1)
        if grep -q "ERROR: AddressSanitizer" <<<"$out" ||
           grep -q "runtime error:" <<<"$out"; then
            echo "  FAIL  $name — sanitizer error at $n iterations"
            grep -m3 -E "ERROR: AddressSanitizer|runtime error:" <<<"$out" | sed 's/^/        /'
            FAIL=$((FAIL+1)); return
        fi
        local allocs bytes
        allocs=$(grep -oE 'leaked in [0-9]+ allocation' <<<"$out" | grep -oE '[0-9]+' | tail -1)
        bytes=$(grep -oE 'AddressSanitizer: [0-9]+ byte' <<<"$out" | grep -oE '[0-9]+' | tail -1)
        [[ -n "$allocs" ]] || allocs=0
        [[ -n "$bytes"  ]] || bytes=0
        if [[ "$n" == "$N_SMALL" ]]; then sa=$allocs; ba=$bytes; else sb=$allocs; bb=$bytes; fi
    done
    if (( sb > sa )); then
        echo "  FAIL  $name — leak grows with iterations:"
        echo "        ${N_SMALL}x: $ba B / $sa alloc      ${N_BIG}x: $bb B / $sb alloc"
        FAIL=$((FAIL+1))
    else
        printf '  PASS  %-38s bounded (%s B / %s alloc at both sizes)\n' "$name" "$bb" "$sb"
        PASS=$((PASS+1))
    fi
}

echo "── leak growth per iteration ───────────────────────────────────────"

run_case "method inlining a str literal return" << 'FLX'
Block B { str tag = "campo"  fn get() str { return "literal" } }
Block b typeof B
int arr a[4] = 1
int i = 0
str s = ""
while i < NNN { s = b.get()  i = i + a[0] }
print("R", s)
FLX

run_case "str field read and write in a loop" << 'FLX'
import std strings
Block B { str name = "x"  str tag = "t" }
Block b typeof B
int arr a[4] = 1
int i = 0
str s = ""
while i < NNN { b.name = strings.concat("n", b.tag)  s = b.name  i = i + a[0] }
print("R", s)
FLX

run_case "str field self and cross assignment" << 'FLX'
Block B { str x = "aa"  str y = "bb" }
Block b typeof B
int arr a[4] = 1
int i = 0
while i < NNN { b.x = b.x  b.y = b.x  b.x = b.y  i = i + a[0] }
print("R", b.x, b.y)
FLX

run_case "array read and write in a loop" << 'FLX'
int arr fb[256] = 0
float arr fv[64] = 0.5
int i = 0
int acc = 0
while i < NNN { int p = i % 256  fb[p] = fb[p] + 1  acc = acc + fb[p]  i = i + 1 }
print("R", acc, fv[0])
FLX

run_case "Block field scalars in a loop" << 'FLX'
Block B {
    int total = 0
    float scale = 1.5
    bool on = true
    fn work(int n) int {
        int arr a[4] = 1
        int i = 0
        while i < n { total = total + a[0]  if on { total = total + 1 }  i = i + a[0] }
        return total
    }
}
Block b typeof B
print("R", b.work(NNN), b.scale)
FLX

run_case "logical operators over fields and arrays" << 'FLX'
Block B { bool on = false  int hits = 0 }
Block b typeof B
int arr a[8] = 1
bool t = true
int i = 0
while i < NNN {
    if !b.on { b.hits = b.hits + a[0] }
    if t && !b.on { b.hits = b.hits + a[1] }
    if b.on || t { b.hits = b.hits + a[2] }
    i = i + a[0]
}
print("R", b.hits)
FLX

run_case "method call with literal argument in a loop" << 'FLX'
Block B { int total = 0  fn add(int v) nil { total = total + v } }
Block b typeof B
int arr a[4] = 1
int i = 0
while i < NNN { b.add(1)  i = i + a[0] }
print("R", b.total)
FLX

run_case "str built in a loop and stored to a field" << 'FLX'
import std strings
Block B { str name = ""  fn build(int n) nil {
    int arr a[4] = 1
    int i = 0
    str nm = ""
    while i < n { nm = strings.concat("ma", "p")  i = i + a[0] }
    name = nm
} }
Block b typeof B
b.build(NNN)
print("R", b.name)
FLX

echo "────────────────────────────────────────────────────────────────────"
echo "  leak_scaling: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]] || exit 1
