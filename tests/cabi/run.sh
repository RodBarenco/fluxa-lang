#!/usr/bin/env bash
set -euo pipefail
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cat > "$P/fluxa.toml" <<'TOML'
[project]
name="cabi-host-test"
entry="main.flx"

[libs]
std.cabi="1.0"
TOML

cat > "$P/main.flx" <<'FLX'
import std cabi

fn cabi_dispatch() int {
    int i = cabi.read_int(0)
    float f = cabi.read_float(1)
    bool b = cabi.read_bool(2)
    str s = cabi.read_str(3)
    int arr ai[3] = 0
    float arr af[3] = 0.0
    bool arr ab[3] = false
    str arr astr[3] = ""
    cabi.read_int_arr(4, ai)
    cabi.read_float_arr(5, af)
    cabi.read_bool_arr(6, ab)
    cabi.read_str_arr(7, astr)

    cabi.response_reset()
    cabi.write_int(i + 1)
    cabi.write_float(f + 0.5)
    cabi.write_bool(!b)
    cabi.write_str(s)
    cabi.write_int_arr(ai)
    cabi.write_float_arr(af)
    cabi.write_bool_arr(ab)
    cabi.write_str_arr(astr)
    return 0
}
FLX

case "$(uname -s)" in Darwin) CABI_LIB="libfluxa_cabi.dylib" ;; *) CABI_LIB="libfluxa_cabi.so" ;; esac
cp "$ROOT/$CABI_LIB" "$P/$CABI_LIB"
${CC:-cc} -std=c99 -Wall -Wextra -O2 -I"$ROOT/src/cabi" "$ROOT/tests/cabi/cabi_host.c" -L"$P" -lfluxa_cabi -Wl,-rpath,'$ORIGIN' -lm -o "$P/cabi_host"
"$P/cabi_host" "$P/main.flx" "$P/fluxa.toml" | grep -q CABI_HOST_PASS
echo "→ fluxa-cabi typed host integration: PASS"
