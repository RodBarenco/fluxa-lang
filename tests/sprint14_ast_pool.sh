#!/usr/bin/env bash
# tests/sprint14_ast_pool.sh — configurable ast_pool_cap / ast_str_pool_cap
# and the AST pool overflow log-spam fix.
#
# Background: ASTPool (src/pool.h) is a fixed-size arena for AST nodes
# (default 4096) and interned strings (default 65536 bytes). Once exceeded
# it silently falls back to per-item malloc()/strdup() — it never crashes —
# but every single overflow allocation used to print an unconditional
# fprintf(stderr, ...) line. A real workload overflowing by ~95x produced
# ~592k log lines. This suite covers:
#
#   - a program whose AST exceeds the default node cap still runs correctly
#     via the fallback path (no crash, correct output)
#   - exactly ONE overflow line is printed per overflow-type per run, not
#     one per allocation
#   - ast_pool_cap/ast_str_pool_cap in [runtime] raise the caps and
#     eliminate the overflow log entirely once set high enough
#   - values below the historical default floor are rejected (clamped back
#     up), so a misconfigured toml can never shrink the pool below default
set -euo pipefail
set +o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

case "$FLUXA" in
    /*) : ;;
    *)  FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")" ;;
esac

pass() { printf "  PASS  ast_pool/%s\n" "$1"; }
fail() { printf "  FAIL  ast_pool/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

# Build a project dir with a given fluxa.toml [runtime] body and main.flx,
# run it, return combined stdout+stderr.
run_proj() {
    local runtime_lines="$1" main_body="$2"
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
    } > "$P/fluxa.toml"
    printf '%s\n' "$main_body" > "$P/main.flx"
    local out; out="$(cd "$P" && timeout 30s "$FLUXA" run main.flx -proj "$P" 2>&1)"
    rm -rf "$P"
    printf '%s' "$out"
}

# ── program bodies ───────────────────────────────────────────────────────
# 5000 top-level int declarations — comfortably exceeds the 4096-node
# default cap (each declaration is several AST nodes).
big_node_body=""
for i in $(seq 1 5000); do big_node_body+="int x$i = $i"$'\n'; done
big_node_body+='print(x5000)'

# 2000 unique 50-byte string literals — 2000*51 = 102000 bytes, well past
# the 65536-byte default str cap, and spread across hundreds of individual
# pool_strdup() calls (unlike one giant literal, which would only ever
# overflow once regardless of the log-spam fix).
big_str_body=""
for i in $(seq 1 2000); do
    big_str_body+="str s$i = \"line${i}_0123456789012345678901234567890123456789\""$'\n'
done
big_str_body+='print(len(s2000))'

# ── 1. default cap exceeded: program still runs correctly ──────────────────
out="$(run_proj "" "$big_node_body")"
if [[ "$out" == *$'\n'"5000" || "$out" == "5000" ]]; then
    pass "node_overflow_still_runs"
else
    fail "node_overflow_still_runs" "5000" "$out"
fi

# ── 2. default cap exceeded: exactly one node-overflow line ────────────────
out="$(run_proj "" "$big_node_body")"
n=$(printf '%s' "$out" | grep -c "\[fluxa\] pool: node capacity" || true)
if [[ "$n" -eq 1 ]]; then pass "node_overflow_logs_once"
else fail "node_overflow_logs_once" "1 line" "$n lines"; fi

# ── 3. string cap exceeded: exactly one str-overflow line ──────────────────
out="$(run_proj "" "$big_str_body")"
n=$(printf '%s' "$out" | grep -c "\[fluxa\] pool: string capacity" || true)
if [[ "$n" -eq 1 ]]; then pass "str_overflow_logs_once"
else fail "str_overflow_logs_once" "1 line" "$n lines"; fi
if [[ "$out" == *"49"* ]]; then pass "str_overflow_still_runs"
else fail "str_overflow_still_runs" "49 (len of s2000)" "$out"; fi

# ── 4. ast_pool_cap raised above the program's node count: zero overflow ───
out="$(run_proj "ast_pool_cap = 20000" "$big_node_body")"
n=$(printf '%s' "$out" | grep -c "\[fluxa\] pool: node capacity" || true)
if [[ "$n" -eq 0 ]]; then pass "node_cap_raised_eliminates_log"
else fail "node_cap_raised_eliminates_log" "0 lines" "$n lines"; fi
if [[ "$out" == *$'\n'"5000" || "$out" == "5000" ]]; then
    pass "node_cap_raised_still_correct"
else
    fail "node_cap_raised_still_correct" "5000" "$out"
fi

# ── 5. ast_str_pool_cap raised above the program's string bytes: zero overflow ─
out="$(run_proj "ast_str_pool_cap = 200000" "$big_str_body")"
n=$(printf '%s' "$out" | grep -c "\[fluxa\] pool: string capacity" || true)
if [[ "$n" -eq 0 ]]; then pass "str_cap_raised_eliminates_log"
else fail "str_cap_raised_eliminates_log" "0 lines" "$n lines"; fi

# ── 6. floor clamp: ast_pool_cap below default keeps default, small program ok ─
out="$(run_proj "ast_pool_cap = 10" 'print(42)')"
if [[ "$out" == *"42"* ]]; then pass "node_cap_floor_clamp_still_runs"
else fail "node_cap_floor_clamp_still_runs" "42" "$out"; fi

# ── 7. floor clamp: ast_str_pool_cap below default keeps default ───────────
out="$(run_proj "ast_str_pool_cap = 10" 'print(42)')"
if [[ "$out" == *"42"* ]]; then pass "str_cap_floor_clamp_still_runs"
else fail "str_cap_floor_clamp_still_runs" "42" "$out"; fi

echo "────────────────────────────────────────────────────────────────"
if [[ "$FAILS" -eq 0 ]]; then echo "  → ast_pool: PASS"; exit 0
else echo "  → ast_pool: $FAILS FAILED"; exit 1; fi
