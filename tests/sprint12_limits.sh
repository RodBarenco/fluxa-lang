#!/usr/bin/env bash
# tests/sprint12_limits.sh — configurable str_concat_cap and module_cap.
#
# Covers the two [runtime] limits added alongside the strings.concat rewrite:
#
#   strings.concat
#     - large concat no longer truncates (the old 512/4096 fixed buffers cut
#       a base64-sized string; it now allocates to fit)
#     - str_concat_cap bounds a single concat; over the cap errors by default
#     - str_autogrow lets an over-cap concat grow instead of erroring
#     - values below the 4096 floor are rejected, keeping the default
#
#   module_cap
#     - default 32: importing a 33rd module aborts cleanly with a clear message
#     - raising module_cap in [runtime] lets more modules load
#     - exactly at the cap still loads (boundary)
#     - the loader allocates exactly module_cap slots (checked indirectly: a
#       high cap with few modules still runs)
set -euo pipefail
set +o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

# Resolve FLUXA to an absolute path: run_proj cd's into a temp project dir, so a
# relative binary path (e.g. ./fluxa) would no longer resolve there.
case "$FLUXA" in
    /*) : ;;                                   # already absolute
    *)  FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")" ;;
esac

pass() { printf "  PASS  limits/%s\n" "$1"; }
fail() { printf "  FAIL  limits/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

# Build a project dir with a given fluxa.toml [runtime] body and main.flx, run it.
# Usage: run_proj "<toml runtime lines>" "<main.flx body>" [extra_setup_fn]
run_proj() {
    local runtime_lines="$1" main_body="$2" setup="${3:-}"
    local P; P="$(mktemp -d)"
    mkdir -p "$P/static"
    {
        echo '[project]'
        echo 'name = "t"'
        echo 'entry = "main.flx"'
        if [[ -n "$runtime_lines" ]]; then
            echo '[runtime]'
            echo "$runtime_lines"
        fi
        echo '[libs]'
        echo 'std.strings = "1.0"'
    } > "$P/fluxa.toml"
    printf '%s\n' "$main_body" > "$P/main.flx"
    if [[ -n "$setup" ]]; then "$setup" "$P"; fi
    local out; out="$(cd "$P" && timeout 20s "$FLUXA" run main.flx -proj "$P" 2>&1)"
    rm -rf "$P"
    printf '%s' "$out"
}

# ── strings.concat: large concat does not truncate ──────────────────────────
# Build a string well past the old 512/4096 buffers by concatenating in a loop.
big_body='import std strings
fn build(int n) str {
    str s = ""
    int i = 0
    while i < n {
        s = strings.concat(s, "0123456789")
        i = i + 1
    }
    return s
}
str r = build(500)
print(len(r))'
out="$(run_proj "" "$big_body")"
# 500 * 10 = 5000 chars — impossible under the old 4096 cap.
if [[ "$out" == "5000" ]]; then pass "concat_no_truncate_5000"
else fail "concat_no_truncate_5000" "5000" "$out"; fi

# ── str_concat_cap: over-cap concat errors (default str_autogrow off) ────────
cap_body='import std strings
fn build(int n) str {
    str s = ""
    int i = 0
    while i < n {
        s = strings.concat(s, "0123456789")
        i = i + 1
    }
    return s
}
danger {
    str r = build(2000)
    print(len(r))
}
if err != nil { print("capped") }'
# cap 8192, result would be 20000 → must error.
out="$(run_proj "str_concat_cap = 8192" "$cap_body")"
if [[ "$out" == *"capped"* ]]; then pass "concat_cap_blocks_over"
else fail "concat_cap_blocks_over" "capped" "$out"; fi

# ── str_autogrow: over-cap concat grows instead of erroring ──────────────────
out="$(run_proj "$(printf 'str_concat_cap = 8192\nstr_autogrow = yes')" "$cap_body")"
if [[ "$out" == "20000" ]]; then pass "concat_autogrow_allows"
else fail "concat_autogrow_allows" "20000" "$out"; fi

# ── str_concat_cap below the floor is rejected (keeps default) ───────────────
# cap 100 is below the 4096 floor → a warning is printed and the default 8 MiB
# is kept, so the big concat still succeeds. We check the result is present (the
# warning line precedes it on stderr).
out="$(run_proj "str_concat_cap = 100" "$big_body")"
if [[ "$out" == *"5000"* ]]; then pass "concat_cap_floor_rejected"
else fail "concat_cap_floor_rejected" "5000 (with floor warning)" "$out"; fi

# ── module_cap: 33 modules with default 32 aborts cleanly ────────────────────
setup_33() {
    local P="$1"; local i
    for i in $(seq 1 33); do echo "fn f$i() int { return $i }" > "$P/static/mod$i.flx"; done
}
main_33=""
for i in $(seq 1 33); do main_33+="import static mod$i"$'\n'; done
main_33+='print("loaded33")'
out="$(run_proj "" "$main_33" setup_33)"
if [[ "$out" == *"too many modules"* ]]; then pass "module_cap_default_blocks_33"
else fail "module_cap_default_blocks_33" "too many modules" "$out"; fi

# ── module_cap raised: the 33 modules load ───────────────────────────────────
out="$(run_proj "module_cap = 64" "$main_33" setup_33)"
if [[ "$out" == *"loaded33"* ]]; then pass "module_cap_raised_allows_33"
else fail "module_cap_raised_allows_33" "loaded33" "$out"; fi

# ── module_cap boundary: exactly 32 loads on the default ─────────────────────
setup_32() {
    local P="$1"; local i
    for i in $(seq 1 32); do echo "fn f$i() int { return $i }" > "$P/static/mod$i.flx"; done
}
main_32=""
for i in $(seq 1 32); do main_32+="import static mod$i"$'\n'; done
main_32+='print("loaded32")'
out="$(run_proj "" "$main_32" setup_32)"
if [[ "$out" == *"loaded32"* ]]; then pass "module_cap_exactly_32_ok"
else fail "module_cap_exactly_32_ok" "loaded32" "$out"; fi

echo "────────────────────────────────────────────────────────────────"
if [[ "$FAILS" -eq 0 ]]; then echo "  → limits: PASS"; exit 0
else echo "  → limits: $FAILS FAILED"; exit 1; fi
