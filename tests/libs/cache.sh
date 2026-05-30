#!/usr/bin/env bash
# tests/libs/cache.sh — std.cache: thread-safe k/v cache.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  cache/%s\n" "$1"; }
fail() { printf "  FAIL  cache/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

mk_proj() {
    cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "cachetest"
entry = "main.flx"
[libs]
std.cache    = "1.0"
std.strings  = "1.0"
std.flxthread = "1.0"
TOML
}
mk_proj

echo "── std.cache: thread-safe k/v cache ────────────────────────────────"

# ── 1: basic put/get ─────────────────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.put("k1", "value1")
print(cache.get("k1"))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^value1$"; then pass "put_get"
else fail "put_get" "value1" "$out"; fi

# ── 2: get miss returns "" ───────────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
str x = cache.get("absent")
if x == "" { print("miss") }
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^miss$"; then pass "miss_returns_empty"
else fail "miss_returns_empty" "miss" "$out"; fi

# ── 3: overwrite — second put replaces first value ───────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.put("k", "first")
cache.put("k", "second")
print(cache.get("k"))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^second$"; then pass "overwrite"
else fail "overwrite" "second" "$out"; fi

# ── 4: del removes entry ─────────────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.put("k", "v")
cache.del("k")
str x = cache.get("k")
if x == "" { print("gone") }
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^gone$"; then pass "del_removes"
else fail "del_removes" "gone" "$out"; fi

# ── 5: size tracks populated slots ───────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.clear()
cache.put("a", "1")
cache.put("b", "2")
cache.put("c", "3")
print(cache.size())
cache.del("b")
print(cache.size())
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^3$" && echo "$out" | grep -q "^2$"; then pass "size_tracks"
else fail "size_tracks" "3 then 2" "$out"; fi

# ── 6: clear resets cache ────────────────────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.put("a", "1")
cache.put("b", "2")
cache.clear()
print(cache.size())
str x = cache.get("a")
if x == "" { print("empty") }
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^0$" && echo "$out" | grep -q "^empty$"; then pass "clear_resets"
else fail "clear_resets" "0 then empty" "$out"; fi

# ── 7: get returns owned copy — caller free does not corrupt cache ───────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.put("k", "stored_value")
str v = cache.get("k")
free(v)
print(cache.get("k"))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^stored_value$" && ! echo "$out" | grep -qi "double free\|corrupt"; then
    pass "get_returns_owned_copy"
else fail "get_returns_owned_copy" "stored_value (cache intact)" "$out"; fi

# ── 8: put copies caller's strings — caller free does not corrupt cache ──────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
import std strings
str k = strings.concat("my", "_key")
str v = strings.concat("my", "_value")
cache.put(k, v)
free(k)
free(v)
print(cache.get("my_key"))
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^my_value$" && ! echo "$out" | grep -qi "double free\|corrupt"; then
    pass "put_copies_caller_strings"
else fail "put_copies_caller_strings" "my_value (cache intact)" "$out"; fi

# ── 9: concurrent put/get/del across 8 workers is race-free ──────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
import std strings
import std flxthread as ft

fn worker(int id) nil {
    int i = 0
    while i < 20000 {
        int op = i % 4
        str key = strings.concat("user", "42")
        if op == 0 {
            str cached = cache.get(key)
            if cached == "" {
                str body = strings.concat("response_", key)
                cache.put(key, body)
                free(body)
            }
            free(cached)
        }
        if op == 1 { cache.del(key) }
        if op == 2 {
            str j = strings.concat("payload_", key)
            free(j)
        }
        if op == 3 {
            str cached = cache.get(key)
            free(cached)
        }
        free(key)
        i = i + 1
    }
}

ft.new("w1", "worker", 1)
ft.new("w2", "worker", 2)
ft.new("w3", "worker", 3)
ft.new("w4", "worker", 4)
ft.new("w5", "worker", 5)
ft.new("w6", "worker", 6)
ft.new("w7", "worker", 7)
ft.new("w8", "worker", 8)
ft.resolve_all()
print("done")
FLX
out=$(timeout 30s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^done$" && ! echo "$out" | grep -qi "double free\|corrupt\|abort\|unaligned"; then
    pass "concurrent_8_workers_race_free"
else fail "concurrent_8_workers_race_free" "done (no crash)" "$out"; fi

# ── 10: del on absent key is a safe no-op ────────────────────────────────────
cat > "$WORK_DIR/main.flx" << 'FLX'
import std cache
cache.del("never_set")
cache.del("never_set")
print("ok")
FLX
out=$(timeout 3s "$FLUXA" run "$WORK_DIR/main.flx" -proj "$WORK_DIR" 2>&1 || true)
if echo "$out" | grep -q "^ok$" && ! echo "$out" | grep -qi "double free\|corrupt"; then
    pass "del_absent_safe"
else fail "del_absent_safe" "ok" "$out"; fi

echo "────────────────────────────────────────────────────────────────────"
total=10
if [[ $FAILS -eq 0 ]]; then
    echo "  Results: $total passed, 0 failed"
    echo "  → std.cache: PASS"
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    echo "  → std.cache: FAIL"
fi
