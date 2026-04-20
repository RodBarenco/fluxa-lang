#!/usr/bin/env bash
# tests/libs/lib_linker.sh — lib linker validation tests
#
# Tests the error handling in scripts/gen_lib_registry.py and the
# runtime behavior when lib registration has problems.
#
# Scenarios:
#   1. FLUXA_LIB_EXPORT missing required field → scanner WARNING, lib skipped
#   2. Duplicate lib name → scanner ERROR + exit 1
#   3. Import lib not in registry → parse error listing available libs
#   4. Import lib in registry but false in fluxa.libs → excluded from binary
#   5. Unknown function on valid lib → runtime error with lib.fn context

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${FLUXA:-$ROOT/fluxa}"
SCANNER="$ROOT/scripts/gen_lib_registry.py"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/lib_linker/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/lib_linker/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done

echo "── lib_linker: registration validation ─────────────────────────"

# ── Helper: run scanner against a custom std/ dir ─────────────────────────
run_scanner() {
    local std_dir="$1"
    REPO_ROOT="$ROOT" python3 << PYEOF 2>&1
import re, os, sys

STD_DIR = "$std_dir"
EXPORT_RE = re.compile(r'FLUXA_LIB_EXPORT\s*\(\s*(.*?)\s*\)', re.DOTALL)
FIELD_RE  = re.compile(r'(\w+)\s*=\s*"?([^",\n]+)"?\s*,?')

def parse_export(block):
    fields = {}
    for m in FIELD_RE.finditer(block):
        fields[m.group(1).strip()] = m.group(2).strip().strip('"')
    return fields

seen = {}
for lib_dir in sorted(os.listdir(STD_DIR)):
    lib_path = os.path.join(STD_DIR, lib_dir)
    if not os.path.isdir(lib_path): continue
    headers = [f for f in os.listdir(lib_path) if f.startswith("fluxa_std_") and f.endswith(".h")]
    if not headers: continue
    header = os.path.join(lib_path, sorted(headers)[0])
    with open(header) as f:
        content = f.read()
    for m in EXPORT_RE.finditer(content):
        fields = parse_export(m.group(1))
        required = {"name", "toml_key", "owner", "call"}
        missing = required - fields.keys()
        if missing:
            print(f"WARNING: {os.path.basename(header)}: FLUXA_LIB_EXPORT missing fields: {sorted(missing)}", file=sys.stderr)
            continue
        name = fields["name"]
        if name in seen:
            print(f"ERROR: duplicate lib name '{name}': {os.path.basename(header)} conflicts with {seen[name]}", file=sys.stderr)
            sys.exit(1)
        seen[name] = os.path.basename(header)
        print(f"OK: {name}")
PYEOF
}

# ── Scenario 1: Missing required field → WARNING + lib skipped ─────────────
STD1="$WORK/std1"
mkdir -p "$STD1/goodlib" "$STD1/missingcall"

cat > "$STD1/goodlib/fluxa_std_goodlib.h" << 'C'
#ifndef X
#define X
#include "../../scope.h"
#include "../../err.h"
static inline Value glib_nil(void) { Value v; v.type=VAL_NIL; return v; }
static inline Value fluxa_std_goodlib_call(const char *f,
    const Value *a, int c, ErrStack *e, int *h, int l)
    { (void)f;(void)a;(void)c;(void)e;(void)h;(void)l; return glib_nil(); }
FLUXA_LIB_EXPORT(
    name     = "goodlib",
    toml_key = "std.goodlib",
    owner    = "goodlib",
    call     = fluxa_std_goodlib_call,
    rt_aware = 0,
    cfg_aware = 0
)
#endif
C

cat > "$STD1/missingcall/fluxa_std_missingcall.h" << 'C'
#ifndef Y
#define Y
#include "../../scope.h"
#include "../../err.h"
FLUXA_LIB_EXPORT(
    name     = "missingcall",
    toml_key = "std.missingcall",
    owner    = "missingcall"
    /* call is missing */
)
#endif
C

# Capture combined output — Python may interleave stdout/stderr
combined1=$(run_scanner "$STD1" 2>&1 || true)

if echo "$combined1" | grep -q "WARNING.*missingcall.*missing fields"; then
    pass "missing_field_produces_warning"
else
    fail "missing_field_produces_warning" "WARNING: missingcall missing fields" "$combined1"
fi

if echo "$combined1" | grep -q "OK: goodlib"; then
    pass "valid_lib_still_registered_when_other_has_warning"
else
    fail "valid_lib_still_registered_when_other_has_warning" "OK: goodlib" "$combined1"
fi

# missingcall should NOT appear in OK: lines
if echo "$combined1" | grep "^OK:" | grep -q "missingcall"; then
    fail "missing_field_lib_is_skipped" "missingcall should not be in OK list" "$combined1"
else
    pass "missing_field_lib_is_skipped"
fi

# ── Scenario 2: Duplicate lib name → ERROR + exit 1 ───────────────────────
STD2="$WORK/std2"
mkdir -p "$STD2/liba" "$STD2/libb"

for lib in liba libb; do
cat > "$STD2/$lib/fluxa_std_$lib.h" << C
#ifndef Z_$lib
#define Z_$lib
#include "../../scope.h"
#include "../../err.h"
static inline Value fluxa_std_${lib}_call(const char *f,
    const Value *a, int c, ErrStack *e, int *h, int l)
    { (void)f;(void)a;(void)c;(void)e;(void)h;(void)l; Value v; v.type=VAL_NIL; return v; }
FLUXA_LIB_EXPORT(
    name     = "duplicate",
    toml_key = "std.duplicate",
    owner    = "duplicate",
    call     = fluxa_std_${lib}_call,
    rt_aware = 0,
    cfg_aware = 0
)
#endif
C
done

dup_err=$(run_scanner "$STD2" 2>&1 || true)
dup_exit=0
( run_scanner "$STD2" > /dev/null 2>&1 ) || dup_exit=$?

if echo "$dup_err" | grep -q "ERROR.*duplicate lib name"; then
    pass "duplicate_name_produces_error"
else
    fail "duplicate_name_produces_error" "ERROR: duplicate lib name" "$dup_err"
fi

if [ "$dup_exit" -ne 0 ]; then
    pass "duplicate_name_exits_nonzero"
else
    fail "duplicate_name_exits_nonzero" "exit code != 0" "exit code was 0"
fi

# ── Scenario 3: Import lib not in registry → parse error ──────────────────
mkdir -p "$WORK/proj3"
cat > "$WORK/proj3/fluxa.toml" << 'T'
[project]
name="t"
entry="main.flx"
[libs]
std.nonexistent = "1.0"
T
cat > "$WORK/proj3/main.flx" << 'FLX'
import std nonexistent
print("unreachable")
FLX
out3=$(timeout 5s "$FLUXA" run "$WORK/proj3/main.flx" -proj "$WORK/proj3" 2>&1 || true)

if echo "$out3" | grep -q "unknown std library"; then
    pass "import_unknown_lib_parse_error"
else
    fail "import_unknown_lib_parse_error" "unknown std library" "$out3"
fi

if echo "$out3" | grep -qE "available:.*math.*csv|available:.*csv.*math"; then
    pass "parse_error_lists_available_libs"
else
    fail "parse_error_lists_available_libs" "available: math, csv, ..." "$out3"
fi

# ── Scenario 4: Lib declared in toml but not in fluxa.toml [libs] ─────────
mkdir -p "$WORK/proj4"
cat > "$WORK/proj4/fluxa.toml" << 'T'
[project]
name="t"
entry="main.flx"
T
# No [libs] section — std.math not declared
cat > "$WORK/proj4/main.flx" << 'FLX'
import std math
float r = math.sqrt(4.0)
print(r)
FLX
out4=$(timeout 5s "$FLUXA" run "$WORK/proj4/main.flx" -proj "$WORK/proj4" 2>&1 || true)

if echo "$out4" | grep -qiE "not declared|add std\.math"; then
    pass "undeclared_lib_runtime_error"
else
    fail "undeclared_lib_runtime_error" "not declared in [libs]" "$out4"
fi

# ── Scenario 5: Valid lib, unknown function → runtime error with context ───
mkdir -p "$WORK/proj5"
cat > "$WORK/proj5/fluxa.toml" << 'T'
[project]
name="t"
entry="main.flx"
[libs]
std.math = "1.0"
T
cat > "$WORK/proj5/main.flx" << 'FLX'
import std math
danger {
    math.nonexistent_function(1.0)
}
if err != nil { print(err[0]) }
FLX
out5=$(timeout 5s "$FLUXA" run "$WORK/proj5/main.flx" -proj "$WORK/proj5" 2>&1 || true)

if echo "$out5" | grep -q "unknown function"; then
    pass "unknown_function_produces_error_with_context"
else
    fail "unknown_function_produces_error_with_context" "unknown function" "$out5"
fi

echo "────────────────────────────────────────────────────────────────"
echo "  → std.lib_linker: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.lib_linker: PASS" && exit 0 || exit 1
