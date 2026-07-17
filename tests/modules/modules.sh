#!/usr/bin/env bash
# tests/modules/modules.sh — v0.15 module system tests
#
# Covers:
#   - import static: pure functions (all args/return, no state)
#   - import live:   Block with prst state
#   - Multiple simultaneous modules
#   - Namespace isolation
#   - prst in module Block survives -prod
#   - module_root in fluxa.toml
#   - import is textual (position-independent)
#   - Watcher detects module file change and reloads
#   - Block declared in module: typeof ns.Block, method calls
#   - Two independent instances of Block from module
#   - Mixed: Block from live + pure fns from static
#   - arr inside Block in module
#   - dyn inside Block in module (top-level prst dyn, not in Block member)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
FIXTURES="$SCRIPT_DIR/fixtures"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fluxa) FLUXA="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        *) shift ;;
    esac
done

pass() { printf "  PASS  modules/%s\n" "$1"; }
fail() {
    printf "  FAIL  modules/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"
    FAILS=$((FAILS+1))
}

setup_proj() {
    local dir="$1"; mkdir -p "$dir/live" "$dir/static"
    printf '[project]\nentry = "main.flx"\n' > "$dir/fluxa.toml"
}
use_module() { cp "$FIXTURES/$2/$3.flx" "$1/$2/$3.flx"; }

echo "── modules: v0.15 module system ─────────────────────────────────────"

# ── 1: static — int function ──────────────────────────────────────────────
P="$WORK_DIR/c01"; setup_proj "$P"; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
import static math_utils
print(math_utils.double(7))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "14" ] && pass "static_fn_int" || fail "static_fn_int" "14" "$out"

# ── 2: static — bool return ───────────────────────────────────────────────
P="$WORK_DIR/c02"; setup_proj "$P"; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
import static math_utils
print(math_utils.is_even(4))
print(math_utils.is_even(3))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "true
false" ] && pass "static_fn_bool" || fail "static_fn_bool" "true/false" "$out"

# ── 3: static — multiple args ─────────────────────────────────────────────
P="$WORK_DIR/c03"; setup_proj "$P"; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
import static math_utils
print(math_utils.clamp(15, 0, 10))
print(math_utils.clamp(-5, 0, 10))
print(math_utils.max(3, 7))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "10
0
7" ] && pass "static_fn_multi_args" || fail "static_fn_multi_args" "10/0/7" "$out"

# ── 4: static — str arg and return ───────────────────────────────────────
P="$WORK_DIR/c04"; setup_proj "$P"; use_module "$P" static greeter
cat > "$P/main.flx" << 'FLX'
import static greeter
print(greeter.greet("fluxa"))
print(greeter.is_long("hi"))
print(greeter.is_long("fluxa-lang"))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "fluxa
false
true" ] && pass "static_fn_str" || fail "static_fn_str" "fluxa/false/true" "$out"

# ── 5: live — Block with prst int ────────────────────────────────────────
P="$WORK_DIR/c05"; setup_proj "$P"; use_module "$P" live counter
cat > "$P/main.flx" << 'FLX'
import live counter
Block c typeof counter.Counter
c.increment()
c.increment()
c.add(10)
print(c.get())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "12" ] && pass "live_block_prst_int" || fail "live_block_prst_int" "12" "$out"

# ── 6: live — Block reset ────────────────────────────────────────────────
P="$WORK_DIR/c06"; setup_proj "$P"; use_module "$P" live counter
cat > "$P/main.flx" << 'FLX'
import live counter
Block c typeof counter.Counter
c.add(5)
c.reset()
print(c.get())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "0" ] && pass "live_block_reset" || fail "live_block_reset" "0" "$out"

# ── 7: live — Block with prst float + str ────────────────────────────────
P="$WORK_DIR/c07"; setup_proj "$P"; use_module "$P" live sensor
cat > "$P/main.flx" << 'FLX'
import live sensor
Block s typeof sensor.Sensor
s.set(3.14)
print(s.get())
print(s.label())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "3.14
sensor-v1" ] && pass "live_block_float_str" || fail "live_block_float_str" "3.14/sensor-v1" "$out"

# ── 8: live — Block with bool return ─────────────────────────────────────
P="$WORK_DIR/c08"; setup_proj "$P"; use_module "$P" live devices
cat > "$P/main.flx" << 'FLX'
import live devices
Block m typeof devices.Motor
m.set_rpm(1200)
print(m.get_rpm())
print(m.is_running())
Block m2 typeof devices.Motor
print(m2.is_running())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "1200
true
false" ] && pass "live_block_bool_return" || fail "live_block_bool_return" "1200/true/false" "$out"

# ── 9: live — two independent Block instances ────────────────────────────
P="$WORK_DIR/c09"; setup_proj "$P"; use_module "$P" live devices
cat > "$P/main.flx" << 'FLX'
import live devices
Block m1 typeof devices.Motor
Block m2 typeof devices.Motor
m1.set_rpm(800)
m2.set_rpm(1600)
print(m1.get_rpm())
print(m2.get_rpm())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "800
1600" ] && pass "live_block_two_instances_isolated" || fail "live_block_two_instances_isolated" "800/1600" "$out"

# ── 10: live — Block with multiple prst fields ───────────────────────────
P="$WORK_DIR/c10"; setup_proj "$P"; use_module "$P" live devices
cat > "$P/main.flx" << 'FLX'
import live devices
Block t typeof devices.Thermometer
t.set(35.5)
t.check(30.0)
print(t.get())
print(t.alarmed())
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "35.5
true" ] && pass "live_block_multi_prst_fields" || fail "live_block_multi_prst_fields" "35.5/true" "$out"

# ── 11: two modules simultaneously ───────────────────────────────────────
P="$WORK_DIR/c11"; setup_proj "$P"
use_module "$P" live counter; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
import live counter
import static math_utils
Block c typeof counter.Counter
c.add(math_utils.double(5))
print(c.get())
print(math_utils.square(3))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx -prod 2>/dev/null || true)
[ "$out" = "10
9" ] && pass "two_modules_simultaneous" || fail "two_modules_simultaneous" "10/9" "$out"

# ── 12: namespace isolation ───────────────────────────────────────────────
P="$WORK_DIR/c12"; setup_proj "$P"; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
import static math_utils
int math_utils__double = 99
print(math_utils.double(3))
print(math_utils__double)
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "6
99" ] && pass "namespace_isolation" || fail "namespace_isolation" "6/99" "$out"

# ── 13: module_root in fluxa.toml ────────────────────────────────────────
P="$WORK_DIR/c13"; mkdir -p "$P/src/static"
cp "$FIXTURES/static/math_utils.flx" "$P/src/static/"
cat > "$P/fluxa.toml" << 'TOML'
[project]
entry = "main.flx"
module_root = "src"
TOML
cat > "$P/main.flx" << 'FLX'
import static math_utils
print(math_utils.double(6))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "12" ] && pass "module_root_toml" || fail "module_root_toml" "12" "$out"

# ── 14: import is textual — position in file does not matter ─────────────
P="$WORK_DIR/c14"; setup_proj "$P"; use_module "$P" static math_utils
cat > "$P/main.flx" << 'FLX'
fn setup() nil {
    import static math_utils
}
print(math_utils.double(3))
FLX
out=$(cd "$P" && timeout 5s "$FLUXA" run main.flx 2>/dev/null || true)
[ "$out" = "6" ] && pass "import_textual_position_independent" \
                 || fail "import_textual_position_independent" "6" "$out"

# ── 15: -dev watcher detects module file change ───────────────────────────
P="$WORK_DIR/c15"; setup_proj "$P"; use_module "$P" live counter
cat > "$P/main.flx" << 'FLX'
import live counter
Block c typeof counter.Counter
c.increment()
print(c.get())
FLX
LOG="$WORK_DIR/c15.log"
cd "$P" && timeout 7s "$FLUXA" run main.flx -dev >> "$LOG" 2>&1 &
DEV_PID=$!
sleep 1.5
echo "// modified" >> "$P/live/counter.flx"
sleep 2
kill "$DEV_PID" 2>/dev/null; wait "$DEV_PID" 2>/dev/null || true
reload_count=$(grep -c "reload done" "$LOG" 2>/dev/null || echo 0)
[ "$reload_count" -gt 0 ] && pass "dev_module_file_change_triggers_reload" \
    || fail "dev_module_file_change_triggers_reload" "reload done" \
            "$(cat "$LOG" | tr '\n' '|' | head -c 200)"


# ── v0.22: mod.Block.metodo — singleton namespaced ────────────────────────
P="$WORK_DIR/c22a"; setup_proj "$P"; use_module "$P" static vault
cat > "$P/main.flx" << 'FLX'
import static vault
vault.Vault.bump(55)
vault.Vault.bump(30)
print(vault.Vault.get_best())
FLX
OUT="$("$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 | tail -1)"
[[ "$OUT" == "55" ]] && pass "c22a mod.Block.metodo statement+expr" || fail "c22a" "55" "$OUT"

# ── v0.22: fachada — fn do módulo chama o singleton do próprio módulo ─────
P="$WORK_DIR/c22b"; setup_proj "$P"; use_module "$P" static vault
cat > "$P/main.flx" << 'FLX'
import static vault
vault.bump_twice(80)
print(vault.Vault.get_best())
FLX
OUT="$("$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 | tail -1)"
[[ "$OUT" == "81" ]] && pass "c22b fachada intra-módulo" || fail "c22b" "81" "$OUT"

# ── v0.22: singleton de módulo de DENTRO de método de Block ───────────────
P="$WORK_DIR/c22c"; setup_proj "$P"; use_module "$P" static vault
cat > "$P/main.flx" << 'FLX'
import static vault
Block Game {
    int last = 0
    fn play(int pts) nil {
        vault.Vault.bump(pts)
        last = vault.Vault.get_best()
    }
    fn get_last() int { return last }
}
Block g typeof Game
g.play(200)
print(g.get_last())
FLX
OUT="$("$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 | tail -1)"
[[ "$OUT" == "200" ]] && pass "c22c singleton de módulo em método" || fail "c22c" "200" "$OUT"

# ── v0.22: campo de singleton namespaced — escrita e leitura ──────────────
P="$WORK_DIR/c22d"; setup_proj "$P"; use_module "$P" static vault
cat > "$P/main.flx" << 'FLX'
import static vault
vault.Vault.best = 999
print(vault.Vault.best)
FLX
OUT="$("$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 | tail -1)"
[[ "$OUT" == "999" ]] && pass "c22d campo de singleton (rw)" || fail "c22d" "999" "$OUT"

# ── v0.22: estado do singleton compartilhado entre módulos ────────────────
P="$WORK_DIR/c22e"; setup_proj "$P"; use_module "$P" static vault
cat > "$P/static/hud.flx" << 'FLX'
Block Hud {
    int shown = 0
    fn sync() nil { shown = vault.Vault.get_best() }
    fn get() int { return shown }
}
FLX
cat > "$P/main.flx" << 'FLX'
import static vault
import static hud
vault.Vault.bump(70)
hud.Hud.sync()
print(hud.Hud.get())
FLX
OUT="$("$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 | tail -1)"
[[ "$OUT" == "70" ]] && pass "c22e cross-módulo entre singletons" || fail "c22e" "70" "$OUT"

echo ""
if [ "$FAILS" -eq 0 ]; then
    echo "  → modules: PASS"
    exit 0
else
    echo "  → modules: FAIL ($FAILS failures)"
    exit 1
fi
