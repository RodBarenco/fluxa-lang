#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
${CC:-cc} -std=c99 -Wall -Wextra -pedantic -I"$ROOT/src/cabi" \
  "$ROOT/src/cabi/fluxa_cabi_wire.c" "$ROOT/tests/cabi/wire_smoke.c" -o "$P/wire_smoke"
"$P/wire_smoke"
echo "→ fluxa-cabi deterministic wire: PASS"
