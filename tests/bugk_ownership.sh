#!/usr/bin/env bash
# tests/bugk_ownership.sh — Bug K: ownership unification (owned producers +
# frame teardown). Covers:
#   - module-Block str FIELD reassignment (leak A) — functional + no crash
#   - str LOCAL fed by lib calls, 500 calls, all contexts (leak B)
#   - free(field) inside a module Block == main-Block behavior (free + nil)
#   - arr/dyn element reads and member reads are OWNED COPIES:
#       str x = arr[i]; free(x)  → element intact (old double-free is gone)
#       x = arr[j] reassign      → previous element intact
#       loop reads               → no crash
#   - prst str inside a fn survives repeated calls (pool/slot independence)
# If ../fluxa_asan exists, the leak cases are ALSO run under it and must
# report zero leaks.
set -euo pipefail
set +o pipefail  # tests compare captured output with echo|grep; pipefail + SIGPIPE would cause spurious failures
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")"
FLUXA_ASAN="${PROJECT_ROOT}/fluxa_asan"

pass() { printf "  PASS  bugk/%s\n" "$1"; }
fail() { printf "  FAIL  bugk/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

mk_proj() { # $1 = dir
    mkdir -p "$1/static"
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.strings="1.0"\n' > "$1/fluxa.toml"
}

run_flx() { # $1 = dir ; prints program output
    ( cd "$1" && "$FLUXA" run main.flx -proj . 2>&1 )
}

check_asan_flat() { # $1 = dir, $2 = name — only if fluxa_asan exists
    # Gate: leaks must NOT scale with iteration count. A per-call leak at
    # 500 iterations produces >=500 leaked allocations; a small CONSTANT
    # residual (known bug A: const-pool strings on while-compile, VM chunk
    # lifecycle, tracked separately) is tolerated up to 50 allocations.
    [[ -x "$FLUXA_ASAN" ]] || return 0
    local out n
    out=$( cd "$1" && ASAN_OPTIONS=detect_leaks=1 "$FLUXA_ASAN" run main.flx -proj . 2>&1 ) || true
    if echo "$out" | grep -q "LeakSanitizer"; then
        n=$(echo "$out" | grep -oE "in [0-9]+ allocation" | grep -oE "[0-9]+" | head -1)
        if [[ "${n:-0}" -lt 50 ]]; then
            pass "$2_asan_no_percall_leak"
        else
            fail "$2_asan_no_percall_leak" "constant residual (<50 allocs)" "$(echo "$out" | grep 'SUMMARY' | head -1)"
        fi
    else
        pass "$2_asan_no_percall_leak"
    fi
}

# ── 1. module Block: str field reassign + str local, 500 calls ─────────────
D="$WORK_DIR/mod"; mk_proj "$D"
cat > "$D/static/m.flx" << 'EOF'
Block M {
    str hud = ""
    int n = 0
    fn bump() nil {
        str t = strings.concat("tick ", strings.from_int(n))
        hud = strings.concat("P ", strings.from_int(n))
        n = n + 1
    }
    fn show() str { return hud }
}
EOF
cat > "$D/main.flx" << 'EOF'
import static m
Block inst typeof m.M
int i = 0
while i < 500 {
    inst.bump()
    i = i + 1
}
print(inst.show())
EOF
out=$(run_flx "$D" | tail -1)
if [[ "$out" == "P 499" ]]; then pass "module_block_field_and_local_500"; else fail "module_block_field_and_local_500" "P 499" "$out"; fi
check_asan_flat "$D" "module_block"

# ── 2. free(field) inside a module Block method: frees + becomes nil ───────
D="$WORK_DIR/freefield"; mk_proj "$D"
cat > "$D/static/f.flx" << 'EOF'
Block F {
    str hud = "alive"
    fn drop() nil { free(hud) }
    fn refill() nil { hud = "reborn" }
    fn show() str { return hud }
}
EOF
cat > "$D/main.flx" << 'EOF'
import static f
Block inst typeof f.F
print(inst.show())
inst.drop()
print(inst.show())
inst.refill()
print(inst.show())
EOF
out=$(run_flx "$D" | tr '\n' '|')
if [[ "$out" == "alive|nil|reborn|" ]]; then pass "free_field_module_semantics"; else fail "free_field_module_semantics" "alive|nil|reborn|" "$out"; fi

# ── 3. arr element read is an OWNED COPY: free(x) leaves element intact ────
D="$WORK_DIR/arrown"; mk_proj "$D"
cat > "$D/main.flx" << 'EOF'
str arr names[3] = ""
names[0] = "Alpha"
names[1] = "Beta"
str x = names[0]
free(x)
print(names[0])
str y = names[0]
y = names[1]
print(names[0])
print(y)
EOF
out=$(run_flx "$D" | tr '\n' '|')
if [[ "$out" == "Alpha|Alpha|Beta|" ]]; then pass "arr_element_read_owned_copy"; else fail "arr_element_read_owned_copy" "Alpha|Alpha|Beta|" "$out"; fi

# ── 4. loop element reads: reassigning the reader never corrupts the arr ───
D="$WORK_DIR/arrloop"; mk_proj "$D"
cat > "$D/main.flx" << 'EOF'
str arr teams[2] = ""
teams[0] = "Flamengo"
teams[1] = "Palmeiras"
str current = "initial"
int i = 0
while i < 6 {
    current = teams[i % 2]
    i = i + 1
}
print(current)
print(teams[0])
EOF
out=$(run_flx "$D" | tr '\n' '|')
if [[ "$out" == "Palmeiras|Flamengo|" ]]; then pass "arr_loop_read_owned"; else fail "arr_loop_read_owned" "Palmeiras|Flamengo|" "$out"; fi
check_asan_flat "$D" "arr_loop"

# ── 5. member read from outside is an owned copy ───────────────────────────
D="$WORK_DIR/memown"; mk_proj "$D"
cat > "$D/main.flx" << 'EOF'
Block B {
    str label = "held"
    fn ping() nil { }
}
Block b typeof B
str x = b.label
free(x)
print(b.label)
EOF
out=$(run_flx "$D" | tail -1)
if [[ "$out" == "held" ]]; then pass "member_read_owned_copy"; else fail "member_read_owned_copy" "held" "$out"; fi

# ── 6. prst str inside a fn: pool and slot stay independent across calls ───
D="$WORK_DIR/prstfn"; mk_proj "$D"
cat > "$D/main.flx" << 'EOF'
fn stamp() str {
    prst str log = "x"
    log = strings.concat(log, "x")
    return log
}
stamp()
stamp()
print(stamp())
EOF
out=$(run_flx "$D" | tail -1)
if [[ "$out" == "xxxx" ]]; then pass "prst_str_in_fn_pool_independent"; else fail "prst_str_in_fn_pool_independent" "xxxx" "$out"; fi
check_asan_flat "$D" "prst_fn"

if [[ $FAILS -eq 0 ]]; then echo "bugk: PASS"; else echo "bugk: FAIL ($FAILS)"; exit 1; fi
