#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="$(mktemp -d)"
trap 'rm -rf "$P"' EXIT

cat > "$P/fluxa.toml" <<'TOML'
[project]
name="cabi-bench"
entry="main.flx"

[libs]
std.cabi="1.0"
TOML

cat > "$P/main.flx" <<'FLX'
import std cabi

fn cabi_dispatch() int {
    int mode = cabi.read_int(0)

    if mode == 1 {
        int i = cabi.read_int(1)
        float f = cabi.read_float(2)
        bool b = cabi.read_bool(3)
        str s = cabi.read_str(4)
        int arr ai[3] = 0
        float arr af[3] = 0.0
        bool arr ab[3] = false
        str arr astr[3] = ""
        cabi.read_int_arr(5, ai)
        cabi.read_float_arr(6, af)
        cabi.read_bool_arr(7, ab)
        cabi.read_str_arr(8, astr)

        // Touch every decoded value so the work cannot become semantically dead.
        bool ok = i == 41
        if f != 2.5 { ok = false }
        if !b { ok = false }
        if s != "hello" { ok = false }
        if ai[1] != -20 { ok = false }
        if af[2] != 3.75 { ok = false }
        if ab[1] { ok = false }
        if astr[1] != "Fluxa" { ok = false }

        cabi.response_reset()
        cabi.write_bool(ok)
        return 0
    }

    if mode == 2 {
        int arr ai[3] = [10, -20, 30]
        float arr af[3] = [1.25, -2.5, 3.75]
        bool arr ab[3] = [true, false, true]
        str arr astr[3] = ["a", "Fluxa", ""]

        cabi.response_reset()
        cabi.write_int(42)
        cabi.write_float(3.0)
        cabi.write_bool(false)
        cabi.write_str("hello")
        cabi.write_int_arr(ai)
        cabi.write_float_arr(af)
        cabi.write_bool_arr(ab)
        cabi.write_str_arr(astr)
        return 0
    }

    cabi.response_reset()
    cabi.write_bool(false)
    return 0
}
FLX

case "$(uname -s)" in
  Darwin) CABI_LIB="libfluxa_cabi.dylib" ;;
  *)      CABI_LIB="libfluxa_cabi.so" ;;
esac

if [ ! -f "$ROOT/$CABI_LIB" ]; then
    echo "missing $ROOT/$CABI_LIB — run: make build"
    exit 1
fi

cp "$ROOT/$CABI_LIB" "$P/$CABI_LIB"

${CC:-cc} -std=c99 -Wall -Wextra -O2 \
    -I"$ROOT/src/cabi" \
    "$ROOT/tests/cabi/bench.c" \
    -L"$P" -lfluxa_cabi -Wl,-rpath,'$ORIGIN' \
    -o "$P/cabi_bench"

"$P/cabi_bench" "$P/main.flx" "$P/fluxa.toml"
