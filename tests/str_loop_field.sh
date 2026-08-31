#!/usr/bin/env bash
# Regression: a string built by a compiled loop must remain owned by a field.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")"

cat > "$WORK_DIR/main.flx" <<'FLX'
import std strings

Block Builder {
    str from_nil = ""
    str from_ret = ""

    fn build_nil(int n) nil {
        str name = ""
        int i = 0
        while i < n {
            str part = "m"
            if i == 1 { part = "a" }
            if i == 2 { part = "p" }
            str joined = strings.concat(name, part)
            name = joined
            i = i + 1
        }
        from_nil = name
    }

    fn build_ret(int n) str {
        str name = ""
        int i = 0
        while i < n {
            str part = "m"
            if i == 1 { part = "a" }
            if i == 2 { part = "p" }
            str joined = strings.concat(name, part)
            name = joined
            i = i + 1
        }
        from_ret = name
        return from_ret
    }
}

Block builder typeof Builder
int round = 0
while round < 1000 {
    builder.build_nil(3)
    round = round + 1
}
str result = builder.build_ret(3)
print("RESULT", builder.from_nil, builder.from_ret)
FLX

cat > "$WORK_DIR/fluxa.toml" <<'TOML'
[project]
name = "str-loop-field-regression"
main = "main.flx"

[libs]
std.strings = "1.0"
TOML

out=$(cd "$WORK_DIR" && timeout 15s "$FLUXA" run main.flx 2>&1 || true)
expected="RESULT map map"
if [[ "$out" != "$expected" ]]; then
    echo "  str_loop_field: FAIL"
    echo "    expected: $expected"
    echo "    got:      $out"
    exit 1
fi

echo "  str_loop_field: PASS"
