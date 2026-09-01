#!/usr/bin/env bash
# Leak growth per iteration, under LeakSanitizer.
#
# A leak that scales with the iteration count is the one that ends a long run;
# a bounded residue is a fixed cost paid once. Neither is visible to the rest
# of the suite, because a leaking program still prints the right answer — the
# defect this guards against was found only by varying the iteration count by
# hand and watching the allocation count follow.
#
# Each case runs at N and at 10N. The check is on the *allocation count*, not
# the byte total: a bounded residue keeps the same count at both sizes, while
# anything per-iteration multiplies. ASan and UBSan errors fail immediately.
#
#   bash tests/leak_scaling.sh --fluxa ./fluxa_asan
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLUXA="${SCRIPT_DIR}/../fluxa_asan"
while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done
[[ -x "$FLUXA" ]] || { echo "  leak_scaling: no sanitizer build at $FLUXA"; exit 1; }
FLUXA="$(cd "$(dirname "$FLUXA")" && pwd)/$(basename "$FLUXA")"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
cat > "$WORK_DIR/fluxa.toml" << 'TOML'
[project]
name = "leak_scaling"
version = "0.1.0"

[libs]
std.strings = "1.0"
std.image = "1.0"
std.graph = "1.0"
TOML

PASS=0; FAIL=0
N_SMALL=100
N_BIG=1000

# run <name> — reads the .flx body on stdin with NNN as the iteration count
run_case() {
    local name="$1" body; body="$(cat)"
    local small big out sa ba sb bb
    for n in "$N_SMALL" "$N_BIG"; do
        printf '%s\n' "${body//NNN/$n}" > "$WORK_DIR/case.flx"
        out=$(cd "$WORK_DIR" && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
              timeout 900s "$FLUXA" run case.flx 2>&1)
        if grep -q "ERROR: AddressSanitizer" <<<"$out" ||
           grep -q "runtime error:" <<<"$out"; then
            echo "  FAIL  $name — sanitizer error at $n iterations"
            grep -m3 -E "ERROR: AddressSanitizer|runtime error:" <<<"$out" | sed 's/^/        /'
            FAIL=$((FAIL+1)); return
        fi
        local allocs bytes
        allocs=$(grep -oE 'leaked in [0-9]+ allocation' <<<"$out" | grep -oE '[0-9]+' | tail -1)
        bytes=$(grep -oE 'AddressSanitizer: [0-9]+ byte' <<<"$out" | grep -oE '[0-9]+' | tail -1)
        [[ -n "$allocs" ]] || allocs=0
        [[ -n "$bytes"  ]] || bytes=0
        if [[ "$n" == "$N_SMALL" ]]; then sa=$allocs; ba=$bytes; else sb=$allocs; bb=$bytes; fi
    done
    if (( sb > sa )); then
        echo "  FAIL  $name — leak grows with iterations:"
        echo "        ${N_SMALL}x: $ba B / $sa alloc      ${N_BIG}x: $bb B / $sb alloc"
        FAIL=$((FAIL+1))
    else
        printf '  PASS  %-38s bounded (%s B / %s alloc at both sizes)\n' "$name" "$bb" "$sb"
        PASS=$((PASS+1))
    fi
}

echo "── leak growth per iteration ───────────────────────────────────────"

run_case "method inlining a str literal return" << 'FLX'
Block B { str tag = "campo"  fn get() str { return "literal" } }
Block b typeof B
int arr a[4] = 1
int i = 0
str s = ""
while i < NNN { s = b.get()  i = i + a[0] }
print("R", s)
FLX

run_case "str field read and write in a loop" << 'FLX'
import std strings
Block B { str name = "x"  str tag = "t" }
Block b typeof B
int arr a[4] = 1
int i = 0
str s = ""
while i < NNN { b.name = strings.concat("n", b.tag)  s = b.name  i = i + a[0] }
print("R", s)
FLX

run_case "str field self and cross assignment" << 'FLX'
Block B { str x = "aa"  str y = "bb" }
Block b typeof B
int arr a[4] = 1
int i = 0
while i < NNN { b.x = b.x  b.y = b.x  b.x = b.y  i = i + a[0] }
print("R", b.x, b.y)
FLX

run_case "array read and write in a loop" << 'FLX'
int arr fb[256] = 0
float arr fv[64] = 0.5
int i = 0
int acc = 0
while i < NNN { int p = i % 256  fb[p] = fb[p] + 1  acc = acc + fb[p]  i = i + 1 }
print("R", acc, fv[0])
FLX

run_case "Block field scalars in a loop" << 'FLX'
Block B {
    int total = 0
    float scale = 1.5
    bool on = true
    fn work(int n) int {
        int arr a[4] = 1
        int i = 0
        while i < n { total = total + a[0]  if on { total = total + 1 }  i = i + a[0] }
        return total
    }
}
Block b typeof B
print("R", b.work(NNN), b.scale)
FLX

run_case "logical operators over fields and arrays" << 'FLX'
Block B { bool on = false  int hits = 0 }
Block b typeof B
int arr a[8] = 1
bool t = true
int i = 0
while i < NNN {
    if !b.on { b.hits = b.hits + a[0] }
    if t && !b.on { b.hits = b.hits + a[1] }
    if b.on || t { b.hits = b.hits + a[2] }
    i = i + a[0]
}
print("R", b.hits)
FLX

run_case "method call with literal argument in a loop" << 'FLX'
Block B { int total = 0  fn add(int v) nil { total = total + v } }
Block b typeof B
int arr a[4] = 1
int i = 0
while i < NNN { b.add(1)  i = i + a[0] }
print("R", b.total)
FLX

run_case "str built in a loop and stored to a field" << 'FLX'
import std strings
Block B { str name = ""  fn build(int n) nil {
    int arr a[4] = 1
    int i = 0
    str nm = ""
    while i < n { nm = strings.concat("ma", "p")  i = i + a[0] }
    name = nm
} }
Block b typeof B
b.build(NNN)
print("R", b.name)
FLX

# The graphics primitives added in this release: each one is called at two
# iteration counts, so anything they hold on to per call shows up as growth.
# graph.init is deliberately outside the loop — a window is a resource, not a
# per-iteration allocation, and the case here is the drawing, not the window.
run_case "image rasteriser called in a loop" << 'FLX'
import std image
dyn im = image.new(16, 16)
int arr dep[256] = 0
int arr tris[15] = [0,0,10,0,0,  0,15,10,0,0,  15,0,10,0,0]
int arr tex[16] = [255,0,0,255, 0,255,0,255, 0,0,255,255, 9,9,9,128]
int arr g[4] = 1
int i = 0
int total = 0
while i < NNN {
    total = total + image.fill_tris(im, dep, tris, 1, tex, 2,2,2, 200, 1)
    total = total + image.fill_rect(im, 1, 1, 4, 4, 16711680, 128)
    total = total + image.fill_tri(im, 0, 0, 8, 0, 0, 8, 255, 64)
    i = i + g[0]
}
print("R", total)
image.discard(im)
FLX

run_case "image.update_rgba_rect in a loop" << 'FLX'
import std image
dyn im = image.new(8, 8)
int arr px[16] = [1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16]
int arr g[4] = 1
int i = 0
while i < NNN { image.update_rgba_rect(im, px, 0, 0, 2, 2)  i = i + g[0] }
print("R done")
image.discard(im)
FLX

run_case "graph primitives with alpha in a loop" << 'FLX'
import std graph
dyn w = graph.init(32, 32, "leak")
int arr g[4] = 1
int i = 0
while i < NNN {
    graph.begin_frame(w)
    graph.clear(w, 0, 0, 0)
    graph.draw_rect(w, 0, 0, 4, 4, 255, 0, 0, 128)
    graph.draw_circle(w, 2, 2, 1, 0, 255, 0, 64)
    graph.draw_line(w, 0, 0, 4, 4, 0, 0, 255, 200)
    graph.draw_text(w, "x", 0, 0, 8, 255, 255, 255, 90)
    graph.draw_triangle(w, 0, 0, 4, 0, 0, 4, 255, 255, 0, 30)
    graph.end_frame(w)
    i = i + g[0]
}
print("R done")
graph.close(w)
FLX

run_case "3D camera, mesh and batch in a loop" << 'FLX'
import std graph
dyn w = graph.init(32, 32, "leak3d")
dyn cam = graph.camera3d(0.0, 2.0, 5.0, 0.0, 0.0, 0.0)
float arr verts[9] = [0.0,0.0,0.0, 1.0,0.0,0.0, 0.0,1.0,0.0]
float arr uv[6] = [0.0,0.0, 1.0,0.0, 0.0,1.0]
int arr co[12] = [255,0,0,255, 0,255,0,255, 0,0,255,255]
dyn mesh = graph.mesh_upload(verts, 1, uv, co)
int arr g[4] = 1
int i = 0
while i < NNN {
    graph.camera3d_set(cam, 0.0, 2.0, 5.0, 0.0, 0.0, 0.0)
    graph.begin_frame(w)
    graph.begin_3d(w, cam)
    graph.draw_grid(w, 4, 1.0)
    graph.draw_cube(w, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 255, 0, 0, 200)
    graph.draw_line3d(w, 0.0,0.0,0.0, 1.0,1.0,1.0, 255,255,0)
    graph.draw_mesh(w, mesh, 0.0, 0.0, 0.0)
    graph.draw_tris3d(w, verts, 1, nil, uv, co)
    graph.end_3d(w)
    graph.end_frame(w)
    i = i + g[0]
}
print("R done")
graph.mesh_free(mesh)
graph.camera3d_free(cam)
graph.close(w)
FLX

# A handle created and released every iteration is the case most likely to
# leak: it is the only one where the lib allocates per call.
run_case "3D handles created and freed per iteration" << 'FLX'
import std graph
dyn w = graph.init(32, 32, "leakh")
float arr verts[9] = [0.0,0.0,0.0, 1.0,0.0,0.0, 0.0,1.0,0.0]
int arr g[4] = 1
int i = 0
while i < NNN {
    dyn cam = graph.camera3d(0.0, 2.0, 5.0, 0.0, 0.0, 0.0)
    dyn m = graph.mesh_upload(verts, 1)
    graph.mesh_free(m)
    graph.camera3d_free(cam)
    i = i + g[0]
}
print("R done")
graph.close(w)
FLX

echo "────────────────────────────────────────────────────────────────────"
echo "  leak_scaling: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]] || exit 1
