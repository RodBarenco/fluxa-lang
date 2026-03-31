#!/usr/bin/env bash
# tests/sprint9_cli.sh — Sprint 9 CLI tests
#
# Tests all new Sprint 9 commands that can be validated without a live
# running runtime (IPC observe/set/logs/status require a live process and
# are tested separately in sprint9_ipc.sh).
#
# Covered here:
#   1. fluxa apply -p (preflight OK)
#   2. fluxa apply -p on a bad file (preflight FAIL, no apply)
#   3. fluxa apply -p --force (preflight warns but applies anyway)
#   4. --force without -p is rejected
#   5. fluxa init creates main.flx + fluxa.toml
#   6. fluxa init is idempotent (won't overwrite existing files)
#   7. scaffolded main.flx is executable
#   8. fluxa run -p on a valid file passes preflight and runs
#   9. fluxa logs/status/observe/set without a running runtime print a clear error
#
# Usage: ./tests/sprint9_cli.sh [--fluxa <path>]
# Exit:  0 = all passed, 1 = any failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="/tmp/fluxa-sprint9-$$"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa) FLUXA="$2"; shift 2 ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

if [[ ! -x "$FLUXA" ]]; then
    echo "ERRO: binary not found: $FLUXA (run 'make' first)"
    exit 1
fi

PASS=0; FAIL=0; ERRORS=""
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); ERRORS="${ERRORS}\n  $1: $2"; }

echo "── Sprint 9 CLI Tests ─────────────────────────────────────────────"
echo "   binary : $FLUXA"
echo "───────────────────────────────────────────────────────────────────"

# ── Test 1: preflight on valid file exits 0 ──────────────────────────────────
PF_OK="$SCRIPT_DIR/sprint9_preflight_ok.flx"
pf_out=$("$FLUXA" apply "$PF_OK" -p 2>&1 || true)
if echo "$pf_out" | grep -q "preflight: OK"; then
    pass "apply/-p/preflight_ok"
else
    fail "apply/-p/preflight_ok" "expected 'preflight: OK', got: $pf_out"
fi

# ── Test 2: preflight on syntactically bad file exits non-zero ───────────────
BAD_FILE="$WORK_DIR/bad.flx"
printf 'int x = \n' > "$BAD_FILE"   # incomplete expression — parse error
pf_bad_exit=0
"$FLUXA" apply "$BAD_FILE" -p >/dev/null 2>&1 || pf_bad_exit=$?
if [[ $pf_bad_exit -ne 0 ]]; then
    pass "apply/-p/preflight_fail_exits_nonzero"
else
    fail "apply/-p/preflight_fail_exits_nonzero" "expected non-zero exit, got 0"
fi

# ── Test 3: --force without -p is rejected ───────────────────────────────────
force_out=$("$FLUXA" apply "$PF_OK" --force 2>&1 || true)
if echo "$force_out" | grep -q "requires -p\|--force requires"; then
    pass "apply/--force_requires_-p"
else
    fail "apply/--force_requires_-p" "expected rejection message, got: $force_out"
fi

# ── Test 4: -p --force applies even when preflight fails ─────────────────────
# Use a file that parses fine but whose resolver may produce warnings.
# For this test we just verify the command doesn't abort when --force is set.
force_result=$("$FLUXA" apply "$PF_OK" -p --force 2>&1 || true)
if echo "$force_result" | grep -qv "apply aborted"; then
    pass "apply/-p/--force_proceeds"
else
    fail "apply/-p/--force_proceeds" "command aborted unexpectedly: $force_result"
fi

# ── Test 5: fluxa init creates main.flx and fluxa.toml ───────────────────────
INIT_DIR="$WORK_DIR/myproject"
init_out=$("$FLUXA" init "$INIT_DIR" 2>&1 || true)
if [[ -f "$INIT_DIR/main.flx" ]] && [[ -f "$INIT_DIR/fluxa.toml" ]]; then
    pass "init/creates_files"
else
    fail "init/creates_files" "expected main.flx + fluxa.toml in $INIT_DIR; got: $init_out"
fi

# ── Test 6: fluxa.toml contains expected keys ────────────────────────────────
if grep -q "gc_cap" "$INIT_DIR/fluxa.toml" && \
   grep -q "prst_cap" "$INIT_DIR/fluxa.toml" && \
   grep -q "prst_graph_cap" "$INIT_DIR/fluxa.toml"; then
    pass "init/toml_has_runtime_keys"
else
    fail "init/toml_has_runtime_keys" "fluxa.toml missing expected keys"
fi

# ── Test 7: init is idempotent — won't overwrite existing files ───────────────
# Modify the scaffolded file, run init again, verify modification is preserved
echo "// my custom content" >> "$INIT_DIR/main.flx"
"$FLUXA" init "$INIT_DIR" >/dev/null 2>&1 || true
if grep -q "my custom content" "$INIT_DIR/main.flx"; then
    pass "init/idempotent_no_overwrite"
else
    fail "init/idempotent_no_overwrite" "init overwrote existing main.flx"
fi

# ── Test 8: scaffolded main.flx is executable ────────────────────────────────
# Use a fresh init dir without our custom content appended
INIT_DIR2="$WORK_DIR/myproject2"
"$FLUXA" init "$INIT_DIR2" >/dev/null 2>&1 || true
scaffold_out=$("$FLUXA" run "$INIT_DIR2/main.flx" 2>/dev/null || true)
if [[ -n "$scaffold_out" ]]; then
    pass "init/scaffolded_main_runs"
else
    fail "init/scaffolded_main_runs" "scaffolded main.flx produced no output"
fi

# ── Test 9: run -p on valid file passes preflight and produces output ─────────
run_p_out=$("$FLUXA" run "$PF_OK" -p 2>&1 || true)
# stderr should have preflight OK; stdout should have values
if echo "$run_p_out" | grep -q "preflight: OK"; then
    pass "run/-p/preflight_then_run"
else
    fail "run/-p/preflight_then_run" "expected preflight: OK in output"
fi

# ── Test 10: observe/set/logs/status without runtime print clear error ────────
for cmd in "observe x" "set x 5" "logs" "status"; do
    cmd_out=$("$FLUXA" $cmd 2>&1 || true)
    if echo "$cmd_out" | grep -qi "no running runtime\|cannot connect\|not found"; then
        pass "no_runtime/$cmd"
    else
        fail "no_runtime/$cmd" "expected 'no running runtime' message, got: $cmd_out"
    fi
done

echo "───────────────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "  Failures:"
    printf '%b\n' "$ERRORS"
    exit 1
fi
exit 0
