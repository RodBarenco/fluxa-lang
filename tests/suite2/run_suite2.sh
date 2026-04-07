#!/usr/bin/env bash
# tests/suite2/run_suite2.sh
# Suite 2 master runner — Edge Cases & Integration (Fluxa v0.10)
#
# Usage:
#   bash tests/suite2/run_suite2.sh
#   bash tests/suite2/run_suite2.sh --fluxa /path/to/fluxa
#   bash tests/suite2/run_suite2.sh --section prst
#   bash tests/suite2/run_suite2.sh --section handover,gc,dyn
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
SECTION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa)   FLUXA="$2"; shift 2 ;;
        --section) SECTION="$2"; shift 2 ;;
        *) shift ;;
    esac
done

if [ ! -x "$FLUXA" ]; then
    echo "[suite2] ERROR: fluxa binary not found at $FLUXA"
    echo "         Run 'make' first, or pass --fluxa <path>"
    exit 1
fi

PASS=0
FAIL=0
ERRORS=""

run_section() {
    local name="$1"
    local script="$SCRIPT_DIR/s2_${name}.sh"
    if [ ! -f "$script" ]; then
        echo "  [suite2] section not found: $name ($script)"
        return
    fi
    out=$(bash "$script" --fluxa "$FLUXA" 2>&1)
    printf "  %-56s" "suite2/${name}"
    if echo "$out" | grep -q "${name}: PASS"; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        echo "FAIL"
        FAIL=$((FAIL+1))
        ERRORS="${ERRORS}\n  suite2/${name}:\n$(echo "$out" | grep -E "FAIL" | sed 's/^/    /')"
    fi
}

echo ""
echo "════════════════════════════════════════════════════════════════════"
echo "  Fluxa Suite 2 — Edge Cases & Integration (v0.10)"
echo "  binary: $FLUXA"
echo "════════════════════════════════════════════════════════════════════"

ALL_SECTIONS="prst handover gc dyn block types_danger embedded"

if [ -n "$SECTION" ]; then
    # Run only requested sections (comma-separated)
    IFS=',' read -ra REQUESTED <<< "$SECTION"
    for s in "${REQUESTED[@]}"; do
        run_section "$s"
    done
else
    for s in $ALL_SECTIONS; do
        run_section "$s"
    done
fi

echo "════════════════════════════════════════════════════════════════════"
total=$((PASS + FAIL))
echo "  Results: ${PASS}/${total} sections passed"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "  Failures:"
    echo -e "$ERRORS"
    echo ""
    echo "  Re-run a single section:"
    echo "    bash tests/suite2/run_suite2.sh --section <name>"
    echo "  Run individual script:"
    echo "    bash tests/suite2/s2_<name>.sh"
    exit 1
else
    echo ""
    echo "  All sections passed."
    exit 0
fi
