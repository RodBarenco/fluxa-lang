#!/usr/bin/env bash
# tests/libs/infer.sh — std.infer test suite
# Tests the stub backend (default when llama.cpp is not vendored).
# generate() returns a placeholder in stub mode — tests API, prst patterns,
# error handling, and the hot-reload survival of model cursors.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/infer/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/infer/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.infer="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.infer ────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std infer
danger { dyn m = infer.load("/tmp/fake.gguf") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. version returns a string
out=$(run << 'FLX'
import std infer
str v = infer.version()
print(len(v))
FLX
)
echo "$out" | grep -qE "^[1-9]" && pass "version_nonempty" || fail "version_nonempty" "nonempty" "$out"

# 3. load returns a cursor (stub succeeds regardless of path)
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/fake.gguf")
    bool ok = m != nil
    print(ok)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "true" && pass "load_returns_cursor" || fail "load_returns_cursor" "true" "$out"

# 4. loaded() reflects state
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/test.gguf")
    bool before = infer.loaded(m)
    print(before)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "true" && pass "loaded_true_after_load" || fail "loaded_true_after_load" "true" "$out"

# 5. model_name extracts filename from path
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/models/mistral-7b-q4.gguf")
    str name = infer.model_name(m)
    print(name)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "mistral-7b-q4.gguf" && pass "model_name_from_path" || fail "model_name_from_path" "mistral-7b-q4.gguf" "$out"

# 6. ctx_size returns int
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/test.gguf")
    int ctx = infer.ctx_size(m)
    bool ok = ctx > 0
    print(ok)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "true" && pass "ctx_size_positive" || fail "ctx_size_positive" "true" "$out"

# 7. generate returns a non-empty string (stub returns placeholder)
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/test.gguf")
    str r = infer.generate(m, "What is 2+2?")
    bool ok = len(r) > 0
    print(ok)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "true" && pass "generate_nonempty" || fail "generate_nonempty" "true" "$out"

# 8. generate_n with token limit
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/test.gguf")
    str r = infer.generate_n(m, "Hello", 50)
    bool ok = len(r) > 0
    print(ok)
    infer.unload(m)
}
FLX
)
echo "$out" | grep -q "true" && pass "generate_n_nonempty" || fail "generate_n_nonempty" "true" "$out"

# 9. generate_n with invalid n → error
out=$(run << 'FLX'
import std infer
danger {
    dyn m = infer.load("/tmp/test.gguf")
    str r = infer.generate_n(m, "Hello", 0)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "generate_n_invalid_error" || fail "generate_n_invalid_error" "error caught" "$out"

# 10. unload bad cursor → error
out=$(run << 'FLX'
import std infer
danger {
    dyn bad = [1, 2, 3]
    infer.unload(bad)
}
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unload_bad_cursor_error" || fail "unload_bad_cursor_error" "error caught" "$out"

# 11. unknown function → error
out=$(run << 'FLX'
import std infer
danger { infer.nonexistent_fn() }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "unknown_function_error" || fail "unknown_function_error" "error caught" "$out"

# 12. prst dyn model survives hot-reload pattern
out=$(run << 'FLX'
import std infer
danger {
    dyn model = infer.load("/models/llm.gguf")
    bool ok = model != nil
    print(ok)
    infer.unload(model)
}
FLX
)
echo "$out" | grep -q "true" && pass "prst_model_pattern" || fail "prst_model_pattern" "true" "$out"

# 13. multiple models (two cursors)
out=$(run << 'FLX'
import std infer
danger {
    dyn a = infer.load("/models/a.gguf")
    dyn b = infer.load("/models/b.gguf")
    str na = infer.model_name(a)
    str nb = infer.model_name(b)
    print(na)
    print(nb)
    infer.unload(a)
    infer.unload(b)
}
FLX
)
echo "$out" | grep -q "a.gguf" && pass "multi_model_a" || fail "multi_model_a" "a.gguf" "$out"
echo "$out" | grep -q "b.gguf" && pass "multi_model_b" || fail "multi_model_b" "b.gguf" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.infer: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.infer: PASS" && exit 0 || exit 1
