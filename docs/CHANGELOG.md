# Fluxa-lang Changelog

## v0.30.4 — a rasteriser for std.image, alpha, and the 3D door

This release touches only the two graphics libraries. Nothing in the runtime
changed, and no existing signature did: every argument added is optional, sits
at the end, and defaults to what the function already did.

### std.image can draw

Until now a buffer could only be filled by blitting another whole image or
element by element from Fluxa. There was no drawing primitive at all, which is
what decided how much a program could do in real time.

Per-pixel work cannot live in Fluxa. A textured, depth-tested fill runs around
thirty-five operations per written pixel; at the cost of one iteration of a
compiled loop that is a quarter of a second for a 640×480 frame, against
single-digit milliseconds in C. The floor is the instruction count per pixel,
so no amount of work on the language side closes it.

`image.fill_tris` rasterises a batch of textured, depth-tested triangles in one
call — one per texture, never one per triangle, since a call per triangle puts
the cost back where it was. Vertices arrive in screen space; projection stays
with the caller, which is where it belongs. `image.fill_rect` and
`image.fill_tri` cover the flat cases on the same core.

The rules it fixes are not new: they are the ones a real Fluxa rasteriser
already settled on, so the call is a drop-in replacement for that loop and its
output can be compared against it bit for bit. A negative signed area is the
front face, every division truncates, a texel coordinate wraps and a negative
result is corrected once, the texel's alpha multiplies the argument, and depth
is written only above a threshold so a translucent texel does not hide what is
behind it. They are written down in the standard library reference because
callers depend on them exactly.

### Partial image updates

`image.update_rgba` required exactly `width * height * 4` components, so a
program changing a corner paid for the whole buffer. `image.update_rgba_rect`
replaces one rectangle instead.

Both still validate every component before writing any of them. Folding the two
passes into one would have been faster and would have broken a guarantee that
already existed: a rejected update leaves the image untouched, which a caller
that catches the error in a `danger` block can see. The passes stay; what went
is the 24-byte copy each of them made per component.

### Every primitive can draw translucent

`draw_rect`, `draw_circle`, `draw_line`, `draw_triangle`, `draw_text`,
`draw_text_font`, `draw_rect_lines`, `draw_circle_lines` and `draw_ring` fixed
alpha at 255, so an overlay, a highlight or a shadow had to go through an
intermediate image. Each now takes an optional alpha at the end. `graph.clear`
does not: it paints the background.

### Per-vertex colour and texture coordinates

`draw_triangle` carries neither, and gaining them per triangle would have meant
nine more arguments on a per-triangle call — the shape the batch exists to
avoid. `graph.draw_tris` and `graph.draw_tris3d` take a batch with a bound
texture, per-vertex texture coordinates and per-vertex RGBA, in screen space
and in world space respectively.

### The 3D pipeline has a door

rmodels was compiled into the binary and no name reached it: no camera, no way
into 3D mode, no mesh. There is now a camera that can be moved without being
reallocated, a way in and out, a mesh uploaded once and drawn many times, and
box, line and grid primitives. Cursors follow the discipline fonts and render
targets already use, and a released one is refused rather than crashing.

This is not a substitute for `image.fill_tris` and does not compete with it.
The rasteriser keeps every pixel under the caller's control and produces the
same bytes on every machine, which is what an image test, an emulator or an
offline render needs; this hands the work to the GPU and takes the GPU's rules
with it.

## v0.30.3 — frame sizing, one ownership contract, and function-body indexing

Everything in this release except the last section is a defect that predated
the bytecode work and only became reachable once loops started compiling.

### A call inside a compiled loop destroyed the caller's locals

The frame a nested call saves and restores is bounded by `rt->stack_size`. The
evaluator raises that bound whenever it writes a local, but the VM writes
locals straight into `stack[resolved_offset]` and never did — so a loop body
with more locals than the temporary watermark had slots above the bound zeroed
by the callee and never restored. A caller's live local came back nil and the
callee's own parameter went unbound; in one real program a 247-declaration
loop body reported `undefined variable` from a function three frames away.

A chunk now records the highest param/local slot it names and the runtime
sizes the frame by it. The work is done while compiling; nothing is added to
the per-iteration path.

### A stdlib call that failed printed nothing at all

Libraries report by pushing onto the error stack and setting `had_error`, which
is what a `danger` block reads. Outside one nobody read it: a misspelled
library function ended the process with a non-zero status and no diagnostic,
which reads exactly like a silently returned nil. Both dispatch sites now
surface what the call pushed.

### Errors name the file they came from

Modules are parsed one at a time and each numbers its own lines from 1, so a
line number alone could not say which file it belonged to once several were
imported. Every AST node now carries a one-byte source id — it fits in existing
padding, `sizeof(ASTNode)` is unchanged — and errors read `nitro.flx:415`
rather than `line 415`.

### One ownership contract for inlined method bodies

`eval_simple_expr` returned an owned reference for a string literal and a
borrowed alias for a parameter or field, so each of its four consumers had to
know which branch it had reached. The inline-call bridge retained
unconditionally, correct for a borrowed alias and one reference too many for a
literal: every method inlining `return "..."` leaked a string per call. Every
branch now hands back an owned reference, as §13.6 already specified, and the
consumers release or adopt accordingly.

`scope_set` released the old entry before reading the incoming value, so
`obj.f = obj.f` was a use-after-free unless the caller happened to hold an
independent reference. It now copies before releasing and carries the same
same-pointer guards `scope_set_owned` has, behind one comparison so an int
store still walks past them.

### str Block fields reach the bytecode path

The field probe admitted `int`, `float` and `bool` only, so one assignment to a
`str` field sent its whole enclosing loop back to the evaluator — in the
reference project a single field accounted for 96,835 of those. `str` crosses
as an owned reference through the field opcodes that already existed. Every
`while` loop in that project now compiles.

### Indexing inside function-body chunks

A function body compiles to a chunk cached on its AST node and reused by every
instance, so it could not consult the live array and refused all indexing —
which excluded exactly the small helpers that are called millions of times.
Eligibility is now decided from declarations alone; see the execution contract
in the specification for the table. Measured on the shape of one such helper,
the cost of a call falls from 362 ns to 157 ns.

Element types are revalidated on every indexed read and write, so the
compile-time decision is a performance boundary and not the thing safety rests
on. `make build-asan` no longer hand-rolls a second flag set that omitted
`_POSIX_C_SOURCE` and every std lib define, and `make test-leaks` runs the leak
cases at two iteration counts and fails only when the allocation count grows.

## v0.30.2 — Block fields, logical operators and value semantics in the VM

This is the first release whose public documentation and build manifest use the
same standard-library inventory: 34 modules. README, specification, programming
guide and stdlib reference now identify the release consistently as v0.30.2.

Loops reach the bytecode VM far more often than before, which is where most of
this release lives. Two defects that had been latent for as long as the VM has
existed became reachable as a direct result, and both are fixed here.

### Bool and nil comparison read indeterminate bytes

`vm_compare` reached its numeric path for every operand pair that was not two
ints or two strings, and that path read `Value.as` as a `double`. `val_bool()`
writes only the `int` member of that union, so the remaining bytes were
indeterminate: `flag == false` inside a compiled loop answered from whatever
the destination register had held before it. The same read applied to `nil` and
to Block instances, and `vm_arith` reinterpreted its operands the same way.

Equality now mirrors the evaluator member for member, including `nil` identity
and content-based string comparison, and ordering is restricted to `int` and
`float` — the only types the evaluator accepts there. `vm_arith` treats a
non-numeric operand as zero rather than reinterpreting it.

The defect predates indexed array opcodes and was invisible while few loops
compiled; the presence of an array read in the same loop was enough to change
the answer. `tests/vm_value_semantics.sh` compares each operator inside a
compiled loop against the same expression at top level.

### Reading a str Block field inside a compiled loop was a use-after-free

`scope_get` hands back a borrowed alias of a field's own storage, while every
VM string register owns one reference and releases it at the end of the
statement. Reading `obj.name` in a compiled loop therefore freed the field
itself, and the loop went on to read reused heap — silently, with no
diagnostic, producing whatever bytes the allocator had put there.

The field callback now retains before the value crosses into the VM, the same
rule `method_try_inline` already applied to an inlined `return field`. The
value-typed field opcode releases rather than strands a reference in the case
its compile-time guarantee is ever violated.

### Block fields execute in the bytecode VM

A bare identifier inside a Block method is the instance's own field, and the
resolver leaves it without a stack slot. Any such read or write rejected the
whole enclosing loop, so a method that touched one of its own fields ran in the
tree walker no matter what else it contained. A loop doing nothing but
`total = total + i` on a field measured 340 ms where the same loop over a local
measured 58 ms.

These now compile to the existing field opcodes. A compile-time probe reads the
Block declaration's AST — the declared type, never the current value — and
admits a field only when that type is `int`, `float` or `bool`, so no string,
array or `dyn` ownership crosses the VM boundary through this path. `prst`
fields are refused: they carry pool synchronisation and, in thread clones,
locking that the field opcodes do not perform. Fields proven to be value-typed
use an opcode that keeps its destination register off the string band, removing
a release before every field read.

The probe runs only while a chunk is being built. Nothing it does is on the
per-iteration path.

### Logical operators compile

`!`, `&&` and `||` had no compiler support: the unary form reached
`compile_expr` with a null right operand and the operator table ended at `>=`.
A single `!` anywhere in a loop body demoted the entire loop. In one real
program a two-million-iteration loop went from 0.07 s to 0.45 s for exactly
that reason.

They compile to two opcodes rather than one, because the evaluator applies two
different truthiness rules and the VM has to reproduce each: under `!` a `0.0`
float is false, while under `&&` and `||` every non-nil value that is not a
bool or an int is true. Short-circuit evaluation is preserved. Regression
coverage runs eleven operand combinations inside a compiled loop and at top
level and requires both to agree.

### Calls cost two fewer dispatches

A literal argument is emitted straight into the call's argument window instead
of into a temporary that is then moved, and the result register joins the
string band only when the callee's declared return type could produce a string.
Both are decided while compiling.

### Fixed primitive arrays execute in the bytecode VM

`NODE_ARR_ACCESS` and `NODE_ARR_ASSIGN` now compile to dedicated indexed-read,
preflight and indexed-write opcodes. Previously either node rejected the whole
enclosing loop or function chunk, sending pixel-heavy code to the tree walker;
the same two-million-iteration loop measured approximately 8x slower after one
array operation was introduced.

Array payloads remain heap-resident. Each VM invocation caches only the stable
owning `Value` slot, never the payload pointer, then checks the slot type, index
type and bounds on every operation. Writes preserve homogeneous element types
and mirror primitive `prst` elements to their independent pool copy. A separate
preflight opcode validates the target before evaluating the assignment's right
side, preserving the language's observable error and side-effect order.

The optimized path is deliberately limited to fixed `int`, `float` and `bool`
arrays. `str arr` stays on the evaluator until VM temporary ownership can retain
and release element reads explicitly; `dyn` stays there because indexing also
owns auto-growth and GC rules. This prevents the performance change from
introducing aliases, stale payload pointers, leaks or use-after-free behavior.

Regression coverage exercises global and Block-owned arrays, array parameters,
all three primitive element types, read/write parity, bounds-before-RHS order,
type errors, and the unchanged `str arr`/`dyn` fallbacks. Sanitizer stress runs
cover 100 million indexed reads and writes.

### Returns from hot loops propagate to the owning function

Standalone `while` chunks now return both a `Value` and an explicit
`did_return` signal to the tree evaluator. Previously `OP_RETURN_VAL` stopped
`vm_run` but discarded its register, so execution resumed after the loop and a
later return could replace the intended result. The defect predated indexed
array opcodes, but array-heavy loops had normally failed bytecode compilation
and hidden it on the evaluator path.

`NODE_WHILE` now translates the VM signal into the existing `rt->ret` contract.
The loop stays in bytecode: no return-bearing chunk is demoted. The signal exits
nested loops correctly, distinguishes normal completion from `return nil`, and
keeps the existing mutual-tail-call protection. Regression coverage includes
unconditional, conditional, nested and array-bearing returns plus 2,000 mutual
tail calls.

### Block fields and methods have separate namespaces

A Block field and method may now use the same public identifier. Fields remain
instance-owned data while methods are stored under a private, reversible key
created before bytecode execution. Different Block definitions therefore do
not compete for member names, and `obj.name` and `obj.name()` resolve
independently without adding a second runtime table or another hot-path lookup.

The internal key preserves the identifier length and is decoded before an
error is exposed, so diagnostics contain only the source-level name. Bytecode
tracks ownership of synthesized method keys and releases them with the chunk;
this prevents the namespace conversion from adding a per-call-site leak.
Regression coverage includes a global and parameter with the same name, two
Blocks with identical field/method names, and both external and internal calls.

### Tail calls across bytecode loops preserve their frame

Calls made by a compiled `while` now use an iterative tail-call trampoline when
the callee falls back to the tree interpreter. Previously both VM callback
fallbacks executed one function body and inspected only `ret.active`; a callee
ending in `return sibling(args)` sets `ret.tco_active`, so its valid return was
silently replaced by `nil`. A following method then appeared to have an unbound
parameter (`undefined variable`) even though its arguments and lexical scope
had been resolved correctly.

The shared fallback preserves one saved VM frame for the entire tail-call
chain. Initial VM-register parameters remain borrowed, tail-call-produced
arguments are owned and released by the reused frame, and the 500-frame guard
still applies to genuinely nested calls. Dynamic argument copying replaces the
old fixed 64-value arrays, with an explicit 512-slot bound before allocation or
binding. This avoids silent truncation and prevents an oversized count from
becoming an out-of-bounds stack write.

Regression coverage includes plain functions, external and internal Block
method calls, non-tail controls, string ownership, and 2,000 mutual tail calls
from a bytecode loop. The original texture-decoder reduction now completes
without exposing another frame's state.

### Mutable RGBA images and tinted drawing

- `image.update_rgba(img, pixels)` atomically replaces a live image's complete
  RGBA buffer from an `int arr` and invalidates its cached GPU texture.
- `graph.draw_image_tint(win, img, x, y, r, g, b, a, scale)` draws an image
  with color and alpha modulation without changing the source pixels.

Both APIs validate their dimensions, ranges and handles on the real and stub
backends. The image and graph suites cover their successful and error paths.

### String equality in bytecode loops

`str == str` and `str != str` now compare string contents in the bytecode VM,
matching the tree-walker. Previously the VM's generic numeric fallback read the
string-pointer arm of `Value` as a `double`, so equal, separately owned strings
could compare unequal inside compiled `while` loops and functions. Identity-
sharing paths such as some field, array and parameter reads could hide the bug.

A regression test covers local strings, inline literals, inequality and a loop
compiled as part of a function.

## v0.31.1 — auto-grow released an uninitialised slot

Growing a `dyn` past its end could segfault, with no error and no message, in
some fraction of runs. Reported from a real program: the crash appeared only
when a `graph.init` window existed, moved around with the amount of growth, and
`prst`, anchoring and `gc_cap` changed nothing.

Both auto-grow paths filled the gap up to `idx - 1`:

```c
for (long fi = d->count; fi < idx; fi++) d->items[fi] = val_nil();
d->count = (int)idx + 1;
...
value_release_data(&d->items[idx]);   /* the slot never initialised */
d->items[idx] = v;
```

The slot at `idx` — the one being assigned — was left holding whatever `realloc`
found in the reused block, and was then released before being overwritten. When
those bytes happened to look like `VAL_STRING` or `VAL_ARR`, that release was a
`free()` of an arbitrary pointer.

That accounts for every observation in the report at once: the window mattered
because raylib's large allocations meant the reused blocks held pointer-shaped
data instead of zeros; more growth was worse because each one is another chance;
`prst` and anchoring did not help because the *value* was rooted and the *bytes*
were the problem; `gc_cap` did not help because nothing was being collected
early; raylib never appeared on the stack because it was not raylib's memory
that was wrong; and the death point moved because it depended on what the
allocator had done last. `value_release_data` appeared twice on the stack
because a container release walks its elements.

The fix is `<= idx` in both paths.

Two regression cases were added to `tests/suite2/s2_dyn.sh`. They churn the heap
with discarded strings first, so the reused blocks are full of pointer-shaped
bytes rather than zeros, and then grow — which reproduces the fault without
raylib and without depending on the allocator's mood. The first fails against
the previous binary.


## v0.31 — `std.compute`

General-purpose GPU computation. No window, no swapchain, no input, no notion
of a frame: buffers, kernels, dispatch, and a way to ask whether the work is
done.

```fluxa
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 32, "storage")
    compute.upload(g, b, values, 0)

    int k = compute.load_kernel(g, "double.spv")
    compute.begin(g)
    compute.bind_kernel(g, k)
    compute.bind_buffer(g, 0, b)
    compute.dispatch(g, 8, 1, 1)
    int t = compute.end(g)
    compute.wait(g, t)

    dyn out = compute.download(g, b, 0, 32)
    free(out)
    compute.close(g)
}
if err != nil { print(err[0]) }
```

### Handles carry the generation of the context that issued them

The context is a `dyn` cursor meant to be held in `prst dyn`, so it survives a
hot reload and the GPU work of the previous run is not thrown away on every
save. Buffers and kernels are plain `int`, which is what lets a Block method
receive them.

That combination has a trap, and it is the same one that produces *"Address
already in use"* for sockets. An in-process reload keeps the context alive, so
an int handle stays valid. But a **runtime swap** cannot carry a pointer across
the snapshot: the context is rebuilt from its declaration while a `prst int`
handle is restored intact, leaving a number that refers to a resource table
which no longer exists.

So a handle is not a bare index:

```
handle = (generation << 20) | (slot + 1)
```

Every `compute.init()` takes the next generation. A handle from a previous
context fails a check with a message that names the cause, instead of silently
addressing whatever now occupies that slot — which would be a *wrong answer*
rather than an error, and far harder to find. Slot 0 is never used, so `0` is
always invalid, matching the convention the language already uses for socket
and request handles.

### The stub runs the whole lifecycle

Buffers are real allocations and `upload`, `download`, `copy_buffer` and
`fill_buffer` move real bytes, so the library is testable on a machine with no
GPU — a CI runner, a container, a laptop with no driver installed. Every
argument check and every handle rule behaves identically on both backends.

The one thing the stub does not do is run a kernel: executing SPIR-V without a
device is not something a stub can honestly pretend at, so `dispatch` is a
no-op. `compute.version()` states which backend is present, so a program is
never fooled into thinking a kernel ran.

### Vulkan backend

Compute only — no surface, no swapchain, no presentation — which is what lets
the same code run headless on a server and on a desktop. Device selection
prefers a discrete GPU but accepts anything with a compute queue, a software
rasteriser included.

Buffers are `HOST_VISIBLE | HOST_COHERENT`, which makes upload and download a
`memcpy` with no staging buffer and no transfer command to synchronise. For
data that comes from the program, gets computed on, and goes back, that is the
right trade; a device-local path with staging can be added later without
changing a line of the Fluxa-facing API.

A batch is recorded lazily, at `compute.end()` rather than as each `dispatch`
is called. The descriptor set has to name the buffers bound at the moment of
the dispatch, and Fluxa binds them one call at a time, so recording at the end
is the point where the bindings are known and settled. One command buffer, one
submit, one fence per batch. Bindings the script never set point at a one-word
dummy buffer, since every binding in the layout must be written or validation
rejects the set.

Opt in with `make FLUXA_COMPUTE_VULKAN=1 build`. It is not automatic: linking
libvulkan on a machine with no driver produces a binary that fails at
`compute.init()` rather than falling back, so the default stays the stub.

### Validation of untrusted input

A shader module goes straight into the GPU driver, so SPIR-V is checked before
the driver ever sees it: magic number `0x07230203`, size a multiple of 4, and
within a 64 MiB cap. A truncated or unrelated file is rejected with a message
that suggests `glslangValidator -V` or `glslc`, instead of becoming the
driver's problem.

Buffer sizes, offsets, copy regions, workgroup counts, descriptor slots and
push-constant sizes are all bounded before anything is allocated or written.

### Knowing whether the work is really accelerated

`device_name` returns a vendor string, which does not answer that without
parsing it, so `compute.device_type(ctx)` reports `"discrete"`,
`"integrated"`, `"virtual"`, `"cpu"` or `"stub"`, and
`compute.accelerated(ctx)` is the one-line form.

Two of those answers matter. A software rasteriser such as Mesa's lavapipe —
common on servers and in containers — reports `"cpu"`: it computes the right
result, but on the CPU, which is not what a program written for a GPU expects.
The stub reports `"stub"` and is more serious still, since buffers and
transfers behave normally while kernels never run, so a program could believe
it computed something when it did not. A program that cares can now ask and
fall back to its own path.

### Deliberate choices worth knowing

- `upload` takes an `int arr` or a `float arr` and packs it to int32 or
  float32. A **mixed array is refused**: a shader reads one packed type, so
  silently picking one would corrupt the data.
- An unknown feature name in `has()` is an **error, not a silent false** —
  false would read as "the GPU lacks it" when the truth is "no such feature".
- `profile_ms` returns **-1.0 when not measured**, not 0.0, which would read as
  an instantaneous kernel.
- Destroying the bound kernel unbinds it, so the next `dispatch` fails with the
  obvious message rather than a confusing one.
- `close` is silent on an already-closed context, matching `pg.free_result`,
  `json2.discard` and `image.discard`.
- `download` allocates the `dyn` it returns. In a simulation loop, `free()` it
  each turn: the collector runs at a safe point and a loop never reaches one.

### Two bugs the tests found, both needing a real device

The first version of the "a valid SPIR-V module loads" test wrote a synthetic
8-byte file with the right magic number. It passed on the stub and failed on a
real driver, which rejected it at pipeline creation — correctly, since a valid
magic number does not make a valid module. The fixture is now a genuine compute
shader, compiled and embedded so the suite needs no shader toolchain.

With a device available, the second bug appeared: the fence was created
unsignalled, so the first `submit` waited forever for a submission that had not
happened yet. It did not fail — it **hung**. The fence is now created signalled,
and `successive_batches_reuse_fence` runs three batches in a row, which is the
shape that catches it.

### Validation

`std.compute` passes 39/39 on **both** backends. The end-to-end case runs a
shader that doubles every element of a buffer and accepts either `2 4 6 16` on a
device or `1 2 3 8` on the stub, reporting which path it took — a stub silently
"passing" a GPU test would be worse than no test at all.

`make test-all` passes. AddressSanitizer reports no errors. Builds are
warning-free under `-Wall -Wextra -pedantic` for `build`, `build-secure`, both
SRAM simulators, `build-rp2040`, `build-cortex-m` and `build-windows`, and with
the Vulkan backend enabled.


## v0.30 — `std.graph` completed, and `std.video`

Two additions driven by what Fluxa Turtle needed and could not express: rotating
a sprite, and producing a video file.

### `std.graph` — nothing existing changed

Everything here is a **new** dispatch name. No pre-existing function changed
shape, argument count, or the types it accepts, so a program written against the
earlier `std.graph` behaves identically. The 35 cases of the previous test suite
were frozen before any edit and their output is byte-identical afterwards; the
suite now runs 56.

That constraint decided two designs. Widening `draw_circle` to accept a float
radius was **not** done — a program relying on the `expected int` error would
have changed behaviour — so the documentation states the type instead, and a new
test pins it. And `draw_image` was left untouched rather than rewritten to route
through a rotation-capable path, where sub-pixel sampling could shift under
existing code.

**Rotation.** `graph.draw_image_rot(win, img, x, y, rot[, scale])` is the simple
form: a rotation in degrees, pivoting on the image's own centre — which is what
makes a sprite point where it moves instead of orbiting its corner — while
`(x, y)` keeps meaning top-left exactly as in `draw_image`. `rot = 0` therefore
draws the same pixels `draw_image` would. `graph.draw_sprite` is the full form:
spritesheet region, rotation, tint and alpha in one call, with the source
rectangle bounds-checked rather than sampling undefined texels.

**Also added:** `draw_rect_lines`, `draw_circle_lines`, `draw_ring`,
`draw_triangle`; off-screen render targets (`render_target`,
`begin`/`end`/`draw`/`release_render_target`); `set_blend_mode`, `scissor`,
`scissor_off`; `text_height`, `char_pressed`, `mouse_btn_pressed`,
`mouse_btn_down`, `mouse_wheel`; gamepad support (`pad_connected`,
`pad_pressed`, `pad_down`, `pad_axis`); a 2D camera (`begin_cam2d`, `end_cam2d`,
`screen_to_world`, `world_to_screen`); and `set_window_title`,
`set_window_size`, `hide_cursor`, `show_cursor`.

The 2D camera transform is computed in the library rather than delegated to the
backend. Both builds then answer identically, the round trip is testable without
a display, and the stub does not become a second implementation free to drift
from the real one.

Unknown button and axis names are errors rather than a silent fallback to
button 0, so a typo surfaces immediately instead of acting on the wrong control.

### `std.video` — MP4/H.264, no external dependency

`video.open` / `frame` / `close` writes an MP4; `play_open` / `play_eof` /
`play_frame` reads one back. Frames are the same image handle `graph.capture`
produces and `std.image` consumes, so a render loop reaches disk with no
conversion in the script.

The codec is vendored under `vendor/video`: **minimp4** (mux and demux, CC0),
**minih264e** (encode, CC0) and **h264bsd** (decode, Apache-2.0, from AOSP). All
plain C99 with no external dependency, and all three compile for MinGW and
bare-metal ARM as well as POSIX — which is what made them the right choice over
pulling in ffmpeg.

**Audio is remuxed, never re-encoded.** `video.audio(v, path)` copies an ADTS
`.aac` or an `.mp3` into the container verbatim: no quality loss, no extra
dependency, and no licence question — every lightweight AAC encoder available
carries either patent-encumbered terms or an LGPL obligation, and the real use
does not need one. The format is identified by signature, not by extension.

**Subtitles are a sidecar.** `video.subtitle(v, start, end, text)` queues cues
that `video.close` writes as a SubRip `.srt` beside the video. The muxer has no
timed-text track, and a sidecar is what every player already reads and what a
person can edit by hand afterwards. Burned-in text still works with
`graph.draw_text` before `graph.capture`; the difference is that a sidecar can
be switched off.

Three things only surfaced by running the code, and each is now commented where
it matters and covered by a test:

- **h264bsd has a two-phase start.** On the first IDR it returns `HDRS_RDY`
  having consumed nothing; the same NAL must be fed again to actually decode.
  Miss it and every frame silently decodes to nothing, with no error to explain
  why.
- **H.264 codes whole 16×16 macroblocks**, so a 160×120 video decodes onto a
  160×128 surface. Without cropping back to the declared size, every frame comes
  out eight rows taller than the video is.
- **`dyn x = f()` does not create the slot when `f` returns nil**, leaving `x`
  undeclared. So playback loops on `video.play_eof(v)` rather than testing the
  frame against nil — deciding from a return value, the same rule the language
  uses elsewhere.

Decoding untrusted video is a classic attack surface, so nothing sizes an
allocation from an unvalidated field: dimensions, frame counts and NAL sizes are
checked against caps before any allocation, and a NAL length that would read past
the buffer ends the frame rather than being trusted.

**Release is the caller's job, as everywhere else in the standard library.**
`play_frame` allocates a full RGBA frame per call and `image.discard` releases
it. That is not a nicety: the collector runs at a safe point, and a playback
loop never reaches one, so an undiscarded frame stays resident for the length of
the loop. Measured on a 96×64 clip, fifteen undiscarded frames held an extra
536 KB — exactly fifteen times the 24.5 KB each occupies; at 1920×1080 it is
8 MB per iteration. `video.close` and `video.play_close` are silent on an
already-released cursor, matching `pg.free_result`, `json2.discard` and
`image.discard`: a second release is a no-op, never an error and never a double
free.

The vendored sources are built as three translation units — minimp4 and
minih264e both define `bs_t` and `nal_put_esc` and cannot share one, and the 26
h264bsd files are pulled into a single unit so a local pragma can silence
third-party warnings there and nowhere else. The zero-warning gate therefore
keeps meaning what it says for code we own.

### Validation

`make test-all` passes: `std.graph` 56/56, `std.video` 17/17, and the previously
failing suites unchanged. Builds are warning-free under `-Wall -Wextra
-pedantic`, including `build-secure`, both SRAM simulators, `build-rp2040`,
`build-cortex-m` and `build-windows`. The video vendor also compiles clean under
MinGW with `-pedantic`, ready for the day the Windows profile enables it.
AddressSanitizer reports no errors across the write/read cycle; benchmarks are
unchanged.


## v0.29 — `prst` across reloads, runtime swaps, and every build target

Hot reload with an external resource in `prst` did not work. `prst dyn win = graph.init(800, 600, "app")` opened a **new window on every save**, and the same shape reopened a database with `sqlite.open` or reallocated a cursor with `csv.open`. Five separate defects sat on that one path; fixing them exposed a sixth in the Atomic Handover and made three cross-compilation targets build again.

**The reload path re-ran every initializer.** The `prst` restore branch evaluated the declared initializer unconditionally, in order to detect that the source had been edited. That comparison only has branches for `int`, `float`, `bool`, and `str` — so for a `dyn` the freshly built value was always discarded, and the only lasting effect was the constructor running a second time. Initializers are now replayed only when they are side-effect free and comparable: a literal, or a binary expression of literals. For a lib call, an FFI call, or a Block method, the pooled value wins, which is what `prst` promises. Editing `prst int n = 12` to `= 99` still takes precedence over the runtime value, unchanged.

**The teardown collect swept the surviving pool.** `gc_collect_all` frees every registered wrapper regardless of `pin_count`, including the `VAL_DYN` the outgoing `PrstPool` still points at. The opaque `VAL_PTR` inside it — a window, a cursor — is invisible to the GC and survived, so the resource stayed alive while the only handle reaching it was freed underneath the pool. Dyn wrappers owned by a surviving pool are now detached from the GC table before the collect and re-registered when the declaration is restored.

**`-dev` discarded the first run's state.** The first cycle went through `runtime_exec`, which frees its own pool; only later cycles used `runtime_apply`. A counter therefore read `1`, then `1` again after the first save, and only began counting from the second reload. `runtime_exec_persist(program, pool_out)` now hands the pool to the caller; `runtime_exec` is the `pool_out == NULL` case and is unchanged for every other caller.

**A finished script was re-executed in a loop.** `-dev` treated normal termination as a reload trigger, respawning the program as fast as a thread could be created — 38 runs in six seconds with no file change, each one opening another window. A script that returns is now reaped and waited on.

**The watcher never looked at modules.** Only the entry file was watched, so saving `live/turtle.flx` or `static/layout.flx` did nothing. `-dev` now watches the entry file plus every `import live` / `import static` target, rebuilding the set on each cycle so a new import takes effect on the next save. The pre-existing `dev_module_file_change_triggers_reload` test had been passing on the respawn loop rather than on a working watcher.

### Two axes, not one

A reload and a runtime swap are different questions, and `prst` answers them differently:

| | in-process reload (`-dev`, `apply`) | runtime swap (`handover`, `update`) |
|---|---|---|
| `int`, `float`, `bool`, `str`, `arr` | preserved | preserved — carried by the snapshot |
| `dyn` holding an external resource | **same handle** — same window | **rebuilt** — new window |

The snapshot wire format has no `VAL_DYN` case, because a pointer cannot survive `execve` — or, in `HANDOVER_MODE_FLASH`, a reboot. Deserialization therefore leaves `VAL_NIL` while `declared_type` still reads `VAL_DYN`. That combination is now recognized as a **headstone**: the resource is rebuilt from its declaration instead of restored as nil. Measurements, counters, and every serializable value are restored either way.

**The Dry Run is a rehearsal.** `dry_run` suppresses `print` but not lib calls, so step 3 really does open a window — and it collects Runtime B's GC while deliberately letting the pool survive into step 4. Any resource the rehearsal opened is therefore already freed when the pool crosses over. Those entries are now marked as headstones before the collect, so the runtime taking over builds the resource for real rather than inheriting a dangling handle.

The 5-step protocol is unchanged: same steps, same order, same wire format, same `FLUXA_HANDOVER_VERSION`. The pool checksum, serialized size, and byte content are bit-identical before and after the headstone pass, and `declared_type` is preserved.

### C ABI on Windows

`fluxa_cabi.h` had two visibility states for three situations. Building the DLL gets `dllexport` and an external host consuming it gets `dllimport`, but `wire.c` and `context.c` compiled **into** `fluxa.exe` to provide `std.cabi` were also marked `dllimport` — so MinGW emitted the definitions under their plain names while every caller asked for the `__imp_` thunk of a DLL that was not being linked. `FLUXA_CABI_STATIC` is the third state, set by `WINDOWS_CABI_CFLAGS`. POSIX never showed the fault because `visibility("default")` does not rename symbols.

This blocked every Windows target, since `build-windows-essential`, `build-windows-essential-static`, and `build-windows-packaged` all route through `build-windows-profile`. The runtime executable links no third-party DLL — its only imports are `KERNEL32.dll` and `msvcrt.dll` — and `fluxa_cabi.dll` still exports its 32 symbols unchanged.

### Bare metal: the platform clock fails closed

`handover.c` is listed in `SRCS_EMBEDDED` but could not cross-compile, because `ms_now()` uses `clock_gettime(CLOCK_MONOTONIC)` and newlib bare-metal provides neither that nor `nanosleep`. `make build-rp2040` and `make build-cortex-m` failed at the first object.

The protocol needs a clock in exactly two places — the safe-point deadline in step 4 and the grace period in step 5. Neither is on the data path: the safe point itself is `call_depth == 0 && danger_depth == 0`, pure counters. With `FLUXA_EMBEDDED` set and no `FLUXA_HAS_POSIX_CLOCK`, those two calls become weak hooks the SDK integration overrides:

```c
long fluxa_platform_ms_now(void);       /* monotonic ms since boot */
void fluxa_platform_sleep_us(long us);
```

pico-sdk supplies `to_ms_since_boot(get_absolute_time())` and `sleep_us()`. esp-idf already has POSIX time, so it builds with `-DFLUXA_HAS_POSIX_CLOCK=1` and uses the unchanged `clock_gettime` path.

The weak default returns `-1` deliberately. A runtime with no clock must not invent a deadline and must not skip the wait: step 4 refuses the switchover with `HANDOVER_ERR_SAFE_POINT`, Runtime A has not been touched, and the device keeps serving on the old runtime. **Forgetting to wire the hook costs an upgrade, never the running service** — the same fail-closed rule every other step already follows.

### Validation

`tests/prst_reload_resources.sh` is new: 13 cases covering window identity across reloads, `prst` survival through the first reload, the respawn loop, initializer-edit precedence, `live/` and `static/` module watching, snapshot restore versus resource rebuild, handover with a resource in the pool, and the fail-closed clock. Against the previous binary, 7 of them fail. `tests/tools/mk_restart_snapshot.c` builds a restart snapshot by hand so the runtime-swap half can be tested without a second binary to swap to; `tests/tools/handover_no_clock.c` builds `handover.c` the way bare metal does and checks both that a missing clock refuses and that a wired clock proceeds.

`make test-all` passes completely on a host with all system libraries present: 86 unit tests, Suite 2 at 8/8 sections, `prst_reload` at 13/13, the std lib suites, the hardware simulation at 10/10 across the RP2040 (264 KB) and ESP32 (520 KB) SRAM caps, and the integration scenarios at 3/3 — including the fault-injection case that `SIGKILL`s the process ~20 ms in, which depends on the protocol's 5–20 ms end-to-end timing, and the 15 real PostgreSQL tests. AddressSanitizer reports no errors across reload, restart, and handover; the benchmark is unchanged; builds are warning-free.

`make build-rp2040`, `make build-cortex-m`, and the Windows targets build again.


## v0.28.2 — `image.get_text`: PNG iTXt metadata reader

`std.image` now completes the PNG metadata round trip with `image.get_text(path, key) -> str`, the read-side counterpart to `image.set_text`.

**PNG iTXt reader.** `get_text` scans the PNG chunk stream directly and returns the UTF-8 text from the first `iTXt` chunk whose keyword matches `key`. The keyword follows the same contract as `set_text`: Latin-1, 1–79 characters. If the PNG is valid but the keyword is not present, the function returns `""` rather than raising an error.

**Compressed and uncompressed metadata.** Both legal forms written by `set_text` are supported. An uncompressed `iTXt` payload (`compressionFlag == 0`) is returned directly; a compressed payload (`compressionFlag == 1`) is transparently inflated with zlib before becoming a Fluxa `str`. Callers therefore use the same read API regardless of whether the optional compression argument was supplied when the metadata was written.

**Chunk-only path.** Reading metadata does not decode image pixels and does not call the image decoder. The implementation only validates the PNG container and walks its chunks until the first matching `iTXt` entry is found, so duplicate keywords deterministically return the first occurrence.

**Errors and portability.** `get_text` is file I/O and follows the same `danger {}` / `err` contract as `save`, `load`, and `set_text`. Missing files, non-PNG input, malformed metadata, and builds without the required codec/zlib path produce ordinary Fluxa errors. The implementation uses portable C file I/O plus the same zlib dependency already used by `set_text`, keeping the behavior available on both Linux and Windows builds of the Raylib-backed `std.image`.

**Validation.** `tests/libs/image.sh` now covers key validation, the no-codec path, missing-key behavior, uncompressed iTXt, compressed iTXt, duplicate-key first-match semantics, and malformed/non-PNG error handling. The full updated image test suite passes.

## v0.28.1 — C ABI benchmark target and fixed-array documentation

The deterministic C ABI is now validated not only for correctness but also with a repeatable bridge throughput benchmark. `make bench-cabi` builds the normal C ABI artifact and runs `tests/cabi/bench.sh`, which measures exactly 10 seconds: a 5-second inbound-heavy READ phase followed by a 5-second outbound-heavy RESPONSE phase. The benchmark is deliberately separate from `make test-cabi` so the normal correctness gate remains fast and deterministic.

**Benchmark shape.** One persistent embedded runtime and one host thread are used throughout. Host request frames are built before timing starts, keeping message-builder setup outside the measurement. READ sends all eight supported wire tags (`int`, `float`, `bool`, `str`, and the four homogeneous array forms) and receives a boolean acknowledgement. RESPONSE sends a small integer trigger and receives all eight tags. The benchmark therefore measures the actual `C → C ABI → Fluxa → C ABI → C` exchange path rather than a standalone serializer loop.

**First validated Linux x64 baseline.** READ completed 1,902,889 exchanges in 5.000002 s — **380,578 exchanges/s**, **2,627.6 ns/exchange**, **72.59 MiB/s** for a 179-byte request and 21-byte response. RESPONSE completed 2,808,695 exchanges in 5.000001 s — **561,739 exchanges/s**, **1,780.2 ns/exchange**, **102.32 MiB/s** for a 24-byte request and 167-byte response. Combined, the bridge completed **4,711,584 exchanges in 10.000003 s**, or **471,158 exchanges/s** averaged across the two intentionally different workloads. These are machine-specific regression numbers, not ABI guarantees.

**Documentation cleanup.** Dispatcher examples now use the real Fluxa fixed-array syntax (`int arr values[N] = ...` followed by `cabi.read_int_arr(index, values)`). This removes the last documentation residue from the earlier prototype API that attempted to return dynamically sized arrays.

## v0.28 — `std.cabi`: deterministic typed host bridge

Fluxa-lang now has a stable C ABI for direct communication with external hosts without exposing runtime internals. The design was deliberately reduced after integration testing: **the ABI is a typed communication bridge, not a persistence, handover, snapshot, or VM-state interface.**

The v1 semantic protocol contains exactly five Fluxa families: `int`, `float`, `bool`, `str`, and homogeneous `arr` (`int arr`, `float arr`, `bool arr`, `str arr`). `dyn`, Blocks, pointers, native handles, AST/VM/GC state, `prst`, snapshots, request sequence metadata, simulation ticks, application status fields, and atomic rollback are not part of the protocol.

**Deterministic wire.** Clear frames use the `FXCB` v1 format with explicit little-endian encoding and no raw C-struct copies. `int` is fixed to signed i32 on the wire (avoids the LP64 Linux / LLP64 Win64 `long` difference); `float` is IEEE-754 binary64; `bool` is one canonical byte (`0`/`1`); strings are length + UTF-8 bytes; arrays are length + homogeneous elements. The same ordered values therefore produce byte-identical clear frames across Linux and Windows.

**Fluxa endpoint.** `std.cabi` now exposes indexed typed readers (`count`, `type`, `read_int`, `read_float`, `read_bool`, `read_str`, and the four typed array readers) plus typed response writers. The old raw-offset API (`read_u8`, `read_u16`, `read_i32`, `read_f64`, manual string offsets) is removed: callers communicate in Fluxa types, not byte offsets.

**Host boundary.** `src/cabi/fluxa_cabi.h` exposes an opaque runtime, deterministic message builder/reader functions, and `fluxa_cabi_exchange(runtime, FXCB request, FXCB response)`. The earlier request metadata (`opcode`, `request_id`, `sequence`, `tick`, status/control flags) and snapshot/restore API are removed. Applications that need an opcode or identifier send it explicitly as an ordinary `int` or `str`, which keeps the protocol universal rather than embedding application semantics in the ABI.

**Optional authenticated encryption.** Security is a separate envelope around the deterministic frame. When `std.crypto`/libsodium is enabled alongside `std.cabi`, host helpers can wrap an `FXCB` frame in `FXCS` using XChaCha20-Poly1305 with a 32-byte shared key. A fresh nonce is generated per seal, so encrypted packets are intentionally non-deterministic; successful unseal recovers the exact deterministic `FXCB` bytes. Key material is host-side and never becomes a Fluxa value. Builds without libsodium retain the full clear C ABI and report secure-envelope support as unavailable.

**Build integration.** `std.cabi = true` in `fluxa.libs` participates in the ordinary `make build` / `make build-windows` flows; there are no separate public CABI build targets. The normal native artifact is `libfluxa_cabi.so`/`.dylib`; Windows emits `fluxa_cabi.dll` plus its import library. The host library follows the same enabled stdlib source/dependency graph as the normal runtime. Native builds keep the current `ipc_server.c` compatibility implementation solely because `runtime.c` still references three IPC bookkeeping helpers; no CLI IPC protocol is exposed by the C ABI.

**Configuration fix found during integration.** Embedded `fluxa_cabi_open()` now performs both `fluxa_config_load()` and `fluxa_config_load_libs()` for an explicit `config_path`, matching the CLI loader. The first implementation loaded the TOML file but silently skipped `[libs]`, causing a valid `std.cabi = "1.0"` declaration to be rejected.

**Tests.** `tests/libs/cabi.sh` covers permission gating, version/no-context errors, every typed reader, dispatcher parsing, and unknown calls. `tests/cabi/cabi_host.c` round-trips all eight wire tags through a real persistent runtime. `tests/cabi/wire_smoke.c` pins the little-endian deterministic encoding. When libsodium is present, the host test also seals/unseals an FXCB frame and verifies byte-identical recovery.


## v0.27.3 — `graph.init` fails cleanly instead of crashing with no GL driver

On a Windows machine with no usable OpenGL driver — the exact case
`docs/WINDOWS.md`'s Mesa3D section exists for — `graph.init` did not error.
It crashed the process (access violation, exit `-1073741819` /
`0xC0000005`), bypassing `danger` entirely, later in the script.

**Root cause.** `InitWindow()` fails by design without aborting: on a
failed platform init it logs a `WARNING` and returns, leaving
`IsWindowReady() == false`. `graph_new_win()` never checked that, so it
kept going — installing the GPU-free hook, calling `LoadRenderTexture()`
— and handed back what looked like a valid window. Everything downstream
runs on an `rlgl` state that was never allocated: draw calls quietly
warn-and-no-op (confirmed with a minimal repro under `gdb`), but
`CloseWindow()` does not — it crashes in
`rlglClose() -> rlUnloadRenderBatch()`, dereferencing a render batch that
`rlglInit()` never got to build. Because the working directory is where
the game runs it, and this is precisely the failure mode a VM without
Guest Additions or 3D acceleration hits, this was reachable by exactly
the audience `docs/WINDOWS.md` already tells to expect it — they just
crashed instead of seeing the documented Mesa3D fallback pointer.

**Fix.** `graph_new_win()` (raylib backend) now checks `IsWindowReady()`
immediately after `InitWindow()` and returns `NULL` on failure, before
installing the GPU hook or calling `LoadRenderTexture()` — and, critically,
without ever calling `CloseWindow()` on it, since that call is what
crashes. `graph.init`'s dispatch checks for that `NULL` and raises a
catchable error pointing at the Mesa3D fallback instead of wrapping and
returning an unusable window. The stub backend (no Raylib) is unaffected;
`graph_new_win()` there always succeeds by design.

**Validation.** New `platform/windows/tests/graph_init_safety.flx`
(`stdlib/graph-init-safety` in `run.ps1`) opens and immediately closes a
window inside `danger` and prints unconditionally afterward — on a host
with no GL driver that exercises the fixed error path, on a host with one
it exercises the ordinary success path, and either way a crash still
shows up as the harness's PASS/FAIL check already catches: a non-zero
exit code. `run.ps1`'s log filter grew to drop `WARNING:` lines alongside
the existing `INFO`/`DEBUG`/`TRACE` ones — raylib logs `WARNING` for the
exact failure this test deliberately triggers, and that is diagnostic
noise, not part of the test's output contract.

Manually confirmed on this GPU-less host both ways: without Mesa,
`graph.init` now returns `graph.init: no usable OpenGL driver — see the
Mesa3D fallback in docs/WINDOWS.md` inside `err[0]` and the script runs to
completion (exit 0, previously exit `-1073741819`); with the Mesa3D
fallback from v0.27's validation still in place, the full open → 5 frames
→ capture → PNG export → close sequence is byte-identical to before this
change (320×240, 3638-byte PNG). `make windows-test` is 6/6
(`standalone/system-dlls-only`, `language/core`, `stdlib/essential`,
`stdlib/fs-confinement`, `stdlib/graph-init-safety`, `stdlib/network`),
zero warnings from Fluxa sources on both the shared and static Windows
profiles.


## v0.27.2 — Windows HTTPS verifies against the machine's own trust store

Every HTTPS request from the standalone Windows runtime failed:

```text
https: SSL peer certificate or SSH remote key was not OK
```

Plain HTTP was fine, so this was a trust problem, not a transport one.

**Root cause.** The runtime links MSYS2's libcurl (8.21.0, OpenSSL 3.6.3),
whose CA bundle path is compiled in as `/mingw64/etc/ssl/certs/ca-bundle.crt`.
That is a build-host path. On any machine that merely runs the distributed
executable it does not resolve, so libcurl had no anchors at all and every
verification failed. `curl_version_info()` reports the baked-in value directly,
which is how it was pinned down.

`CURLSSLOPT_NATIVE_CA` — which `std.httpc` and `std.https` already set, and
which `docs/WINDOWS.md` described as the mechanism — does not cover it. The
option is defined, and `curl_easy_setopt` returns `CURLE_OK`, but the OpenSSL
backend acts on it only when curl was built with native-CA support; MSYS2's is
not. It was silently ignored, which is exactly why the failure read like a code
bug rather than a build one. The `CURL_CA_BUNDLE` escape hatch did work, but a
runtime that needs an environment variable set before it can make an HTTPS
request is not a standalone runtime.

**Implementation.** New `src/std/fluxa_win_ca.h`, included by
`std.httpc` and `std.https` and compiled to nothing off Windows.
`fluxa_win_ca_apply()` resolves trust in a fixed order:

1. `CURL_CA_BUNDLE`, when set — the documented operator override for private
   CAs and corporate proxies. It wins outright, and an override that cannot be
   read now fails the request instead of quietly falling back to a different
   trust source.
2. Schannel-backed libcurl — left to `CURLSSLOPT_NATIVE_CA`, which is genuinely
   native there. The backend is detected from `curl_version_info()->ssl_version`
   rather than assumed, because overriding Schannel with a static snapshot would
   lose Windows' automatic root updates.
3. Everything else — the Windows **ROOT** store, enumerated through
   `CertOpenSystemStoreA`/`CertEnumCertificatesInStore`, each certificate
   converted with `CryptBinaryToStringA(CRYPT_STRING_BASE64HEADER)`, and the
   concatenation handed to curl as `CURLOPT_CAINFO_BLOB`. The bundle is built
   once per process and lent to curl with `CURL_BLOB_NOCOPY`, so a ~40 KB copy
   is not made per request.

Nothing is shipped beside the executable and no vendored `cacert.pem` can go
stale — the anchors are the ones the machine itself already trusts. Verification
is never disabled; when no anchors can be found the handle is left untouched and
the request fails closed. `crypt32` was already on the standalone gate's allowed
system-DLL list, so linking it (`-lcrypt32`, added to both the shared and static
Windows curl link flags) keeps `standalone/system-dlls-only` green.

**Validation.** `FLUXA_WINDOWS_NETWORK_TESTS=1 make windows-test` is 5/5 with
`stdlib/network` passing and no `CURL_CA_BUNDLE` set — previously it was the one
failing case. Precedence was checked both ways: a valid `CURL_CA_BUNDLE` still
succeeds, and a bogus one fails with `Problem with the SSL CA cert` rather than
silently falling through to the store. `objdump -p` shows system DLLs only, now
including `CRYPT32.dll`. Both `build-windows-essential` (shared) and
`build-windows-essential-static` build with zero warnings from Fluxa sources.

`std.mcpc` and `std.mcps` use libcurl the same way and have the same latent
issue, but are not built in any Windows profile today; the header notes where to
call `fluxa_win_ca_apply()` when they are, rather than shipping a path that
cannot be tested here.


## v0.27.1 — `std.fs` builds on Windows again; `read_base64` confinement made native

`fs.read_base64` (added in v0.27) called `realpath()` and `getcwd()` directly.
MinGW provides `getcwd()` but not `realpath()`, so every Windows profile that
enables `std.fs` stopped compiling:

```text
src/std/fs/fluxa_std_fs.h:399:14: error: implicit declaration of
function 'realpath' [-Wimplicit-function-declaration]
```

That is every Windows target except the minimal one — `build-windows-fs`,
`build-windows-essential`, `build-windows-essential-static`,
`build-windows-packaged`, and therefore `windows-test` as well. The Windows
runtime had been green at v0.26; the regression arrived with the feature and
was never Windows-only in intent, so the fix restores the platform rather than
carving out an exception.

**Why not `_fullpath`.** The obvious bridge — `#define realpath(a,b)
_fullpath((b),(a),PATH_MAX)` — compiles but quietly weakens the guard it
implements. `read_base64` is an exfiltration-sensitive primitive: its first
check is that the path canonically resolves *inside* the working directory, and
that is only meaningful if `..` is collapsed **and** links are followed.
`_fullpath()` collapses `..` lexically, does not require the target to exist,
and resolves neither symlinks nor NTFS junctions — so a junction planted in the
working directory would still resolve "inside" and read whatever it points at.

**Implementation.** `fluxa_std_fs.h` gains three `static inline` helpers used
only by `read_base64`. `fs_real_path()` is `realpath()` on POSIX; on Win32 it
opens the target with `CreateFileA` (`FILE_FLAG_BACKUP_SEMANTICS`, so
directories work) and reads back `GetFinalPathNameByHandleA` with
`FILE_NAME_NORMALIZED | VOLUME_NAME_DOS` — the kernel resolves links and the
open fails when the target is missing, matching `realpath()`'s two guarantees.
The returned `\\?\` extended prefix is stripped (`\\?\UNC\srv\shr` → `\\srv\shr`)
so the existing `stat()`/`fopen()` calls take it unchanged. `fs_path_is_sep()`
accepts `\` as well as `/` on Windows, and `fs_path_ncmp()` folds case there
because the filesystem does — the fold is hand-rolled to match the existing
`fs_has_ext()` rather than reach for `_strnicmp`, which `-std=c99` can hide.
The working directory now goes through `fs_real_path(".")` instead of
`getcwd()`, so both sides of the prefix comparison are produced by one function
and always share separator style and casing; on POSIX the two are equivalent.
Buffers move from `PATH_MAX` to `FS_PATH_CAP`, which is `PATH_MAX` on POSIX and
4096 on Windows — MinGW's `PATH_MAX` is 260, too small once the `\\?\` prefix
is added. A resolution that fails or does not fit returns 0, which callers
treat as deny, never as allow. Trailing separators are trimmed off the working
directory before comparing, which also fixes a latent POSIX bug: with the
process at `/`, `cwd_len` was 1 and the old `real_path[cwd_len] != '/'` test
rejected every path beneath it.

Behavior on POSIX is otherwise unchanged: same `realpath()`, same `strncmp()`,
same separator, same buffer size.

**Validation.** New `platform/windows/tests/fs_secure.flx`, wired into
`platform/windows/tests/run.ps1` as `stdlib/fs-confinement`, runs natively on
Windows: a PNG in a subdirectory reads back as base64, a `..` that rejoins the
working directory is still allowed (proving the path is collapsed rather than
the characters rejected), while a `..` escape to a file that really exists
outside, an absolute path outside, and a type mismatch inside the directory are
all refused. `make windows-test` is 4/4 (`standalone/system-dlls-only`,
`language/core`, `stdlib/essential`, `stdlib/fs-confinement`) on the
`build-windows-essential-static` runtime, zero warnings from Fluxa sources.
`runtime info` reports `Target: windows-x64`, `Packaged: false`.

The POSIX branch was checked on the same Windows host through MSYS2's
Cygwin-style environment, where `_WIN32` is undefined and the POSIX branch is
what compiles: the full Unix target builds clean, and `tests/libs/fs.sh` was run
against binaries built with and without this change, giving identical results
(9 passed, 24 failed — the failures are pre-existing on that environment and
present in the unmodified baseline too, so they are a property of the host, not
of this change). A fully green POSIX run still belongs on a Linux host and was
not performed here. Benchmarks are unaffected by
construction: the entire diff sits inside `read_base64` and three helpers only
it calls, and `src/bytecode.c` — the bytecode VM the benches measure — does not
include the `std.fs` header at all.


## v0.27 — configurable AST pool caps (`ast_pool_cap`, `ast_str_pool_cap`) + overflow log fix

The AST arena (`ASTPool` in `pool.h` — nodes + interned strings) was a
hard-coded `nodes[4096]` / `str_buf[65536]` pair. Once exceeded, every
individual overflow allocation printed an unconditional `fprintf(stderr,
...)` line; a real game workload that overflowed by ~95x produced 384,345
node-overflow + 208,144 string-overflow lines — 592,489 total — making the
log unreadable. Overflow itself never crashed (silent per-item
malloc/strdup fallback, tracked and freed by the pool) — only the logging
was the bug.

Both capacities are now configurable via `[runtime]` in `fluxa.toml`, and
the overflow log is capped at one line per pool per overflow-kind per pool
lifetime (once for the first node overflow, once for the first string
overflow — each pool is re-init'd on every parse/hot-reload cycle):

```toml
[runtime]
ast_pool_cap     = 4096   # AST node arena, default 4096 (range 4096..1048576)
ast_str_pool_cap = 65536  # AST string arena bytes, default 65536 (range 65536..16777216)
```

**Implementation.** `pool.h`'s `ASTPool` now carries both a fixed
`default_nodes[4096]`/`default_str_buf[65536]` pair *and* `nodes`/`str_buf`
pointers. At `pool_init()`, if the configured cap equals the compiled-in
default, `nodes`/`str_buf` point at the embedded arrays — no allocation, no
behavior change from before this option existed. Only a *non-default*
`ast_pool_cap`/`ast_str_pool_cap` triggers a `malloc()` sized to the
configured cap (freed in `pool_free()`); a first pass that always
heap-allocated measured ~5-8% slower on `bench_ast.flx` (the pure
AST-walker path, pre-warm-path function calls) because it lost the
static-array locality the interpreter's tight pointer-chasing depends on —
the embedded-array fast path was added specifically to close that gap and
was re-verified against the bytecode-VM benches too. `malloc()` failure
(relevant on embedded targets with a large configured cap) degrades that
cap to 0 rather than aborting, routing every allocation through the
existing overflow fallback — never crashes; covered by the existing
`sim/{RP2040,ESP32}/oom_no_crash` hardware-simulation tests. The single
`overflowed` flag becomes two (`overflowed_nodes`/`overflowed_str`), each
gating its `fprintf` to fire once per pool life. `toml_config.h` adds
`ast_pool_cap` (floor 4096, ceiling 1,048,576) and `ast_str_pool_cap`
(floor 65536, ceiling 16,777,216) — both 256x the default, mirroring
`scope_cap`'s ceiling ratio — with a warn-on-clamp message (matches
`gc_cap`/`str_concat_cap`'s style). `main.c` calls both setters at the same
4 sites that already call `resolver_set_scope_cap`/`parser_set_module_cap`
(`run_once`, `dev_exec_thread`, the `FLUXA_SECURE` block in `run_prod`, and
`run_handover`). Behavior is identical to before when unset (4096
nodes/65536 bytes, same overflow fallback, same performance).

**Validation.** New `tests/sprint14_ast_pool.sh` (9 cases): a >4096-node
program still runs correctly against the default cap and logs exactly one
node-overflow line (not thousands); a >65536-byte-string program logs
exactly one string-overflow line; raising either cap above the program's
actual usage eliminates the overflow log entirely; values below the
default floor are clamped back up and small programs still run. Full
regression suite green: `make test-runner` 85/85 (was 84/84 before this
sprint's test), `make test-suite2` 8/8, all 30 `tests/libs/*.sh` scripts
individually, `make test-all` end-to-end including the real
Docker+PostgreSQL integration suite (15/15) and Atomic Handover scenarios
(3/3). Zero-warning build across the default, `HUGEPAGES_CFLAGS`,
`build-secure`, `build-sim-rp2040`, and `build-sim-esp32` variants.
Benchmarked against the pre-change binary: `bench.flx`/`bench_block.flx`/
`bench_field.flx` (bytecode VM path — what real programs run) show no
measurable difference; `bench_ast.flx` (AST-walker path), after the
embedded-array fast path fix, is within run-to-run noise of baseline
(~2.1-2.5s both, interleaved measurement).


## v0.26 — configurable str_concat_cap and module_cap

Two more fixed limits are now configurable via `[runtime]` in `fluxa.toml`,
and a truncation bug in `strings.concat` is fixed.

### strings.concat no longer truncates; `str_concat_cap` added
`strings.concat` used fixed 512-byte per-argument and 4096-byte total buffers,
silently truncating larger results (e.g. a base64-encoded file). It now allocates
to fit. A new `[runtime] str_concat_cap` (default 8 MiB, matching common HTTP
body limits) bounds a single concat driven by untrusted input; a concat over the
cap errors unless `str_autogrow = yes`. Range 4096..256 MiB. The strings lib is
now `cfg_aware` to read the cap.

```toml
[runtime]
str_concat_cap = 8388608   # 8 MiB (default)
str_autogrow   = no        # error over cap rather than grow (default)
```

### `module_cap` — configurable module import limit
The number of importable live/static modules was hard-coded at 32. It is now
`[runtime] module_cap` (default 32). Fluxa targets small systems, so the loader
and parser allocate exactly `module_cap` slots — no fixed waste. Over the cap
errors with a clear message. Range 1..4096.

```toml
[runtime]
module_cap = 48   # default 32
```

Implementation: `main.c` allocates the module list dynamically (was `mods[32]`);
`parser.c` allocates the imported-namespace list dynamically (was `imported[32]`)
with a settable global `g_module_cap` and `parser_set_module_cap(int)`. Both
free on every exit path (verified). `toml_config.h` adds `str_concat_cap`,
`str_autogrow`, and `module_cap` with range checks. Behavior is identical to
before when unset (concat cap 8 MiB, module cap 32).


## v0.25 — frame capture, `std.image` (PNG/JPG/BMP/TGA/QOI), and `open_url` (current)

Two capabilities the Elite Achievement Cards need — snapshot the running game
frame, and encode that snapshot to real image files — plus a build-ordering fix
that surfaced while integrating them.

**`graph.capture(win) → dyn`.** Snapshots the current frame into a neutral,
backend-independent RGBA buffer. On the Raylib backend it pulls pixels from the
offscreen render target (`win->target`, the same texture used for scaled
fullscreen) via `LoadImageFromTexture`, normalizes to 32-bit RGBA, and flips
vertically (GPU textures are bottom-up); with no target it falls back to
`LoadImageFromScreen`. On the stub backend it returns a blank buffer of the
logical size, so game logic and tests run headless. The result is released with
`image.discard`.

**`graph.draw_image(win, img, x, y [, scale]) → nil`** — the inverse of
`capture`, completing the round trip: `graph.capture` is `graph → image`, this is
`image → graph`. Draws any RGBA image buffer (a loaded/composed card, or a
captured frame) onto the current frame, with an optional scale argument
(1.0 = original; 0.5 = half). The uploaded GPU texture is **cached on the image
buffer** and reused across frames — re-uploaded only when the pixels change
(`resize`/`blit` bump a version counter on the buffer) — so drawing a card or HUD
image every frame in the game loop is cheap, not a per-frame GPU upload. The
cache is an opaque `void*` on the shared buffer that only `std.graph` interprets
(via a cleanup hook), so `std.image` still never sees a GPU type and the two libs
stay decoupled; the texture is freed when the image is discarded.

**New `std.image` lib.** `new`, `save`, `load`, `resize`, `blit`, `width`,
`height`, `set_text`, `discard`, `version`. `save` encodes by file **extension** —
PNG, JPG, BMP, TGA, QOI — via Raylib's bundled stb_image_write; `load` decodes
via stb_image. `blit` composes one buffer onto another (alpha-blended, clipped),
with an optional mask argument that gates the source by the mask's alpha — so a
captured frame drops into a card's clipped/rounded art window. `set_text` embeds
a text field into an existing PNG as an `iTXt` chunk (the card's cryptographic
proof), written by hand since stb emits no text chunks: it splices the chunk
before IEND with a CRC-32 validated against zlib's, and takes an optional 4th
argument to deflate long text. Both `blit` and `set_text` are designed so the
optional mechanism is off when the trailing argument is omitted and engages when
it's supplied — the signature is final now, so wiring the mask or compression
later won't change the API. Dual-backend, like the rest of the graphics stack:
the default stub keeps the full API (`new`/`width`/`height`/`resize`/`blit`/
`discard` run for real, with a nearest-neighbour resize), and `save`/`load`/
`set_text` report a clear "no codec" error, so the card logic and tests run
without the encoder. IO (`save`/`load`/`set_text`) runs inside `danger {}` like
`sqlite`/`csv`/`fs`. Enable the real codec with `make FLUXA_IMAGE_RAYLIB=1`; when
`std.graph` already links Raylib, the codec adds no new dependency (only `-lz`,
for the iTXt deflate path).

```fluxa
import std graph
import std image

danger {
    dyn frame = graph.capture(win)         // RGBA snapshot of the frame
    image.resize(frame, 360, 200)          // fit the card art window
    dyn card  = image.load("card_frame.png")
    dyn mask  = image.load("card_mask.png")
    image.blit(card, frame, 40, 92, mask)  // compose through the mask shape
    image.save(card, "elite_card.png")     // encode → PNG
    image.set_text("elite_card.png", "starfight-proof", proof_hex)  // seal proof
    image.discard(frame)  image.discard(card)  image.discard(mask)
}
if err != nil { print(err[0]) }
```

**`graph.open_url(url) → bool`** — hands a URL to the system's default browser
(a support or donation page, for instance). Implemented deliberately *without*
Raylib's `OpenURL`, which shells out through `system()` and so lets a crafted URL
carry a command alongside it. Here the URL goes to `exec` as a single argv
element with no shell in between — injection is impossible by construction, not
by filtering — behind a scheme check that accepts only `http://`, `https://` and
`mailto:` (so a URL arriving from config or a database can't reach a local file
or an odd scheme). A double `fork` reparents the browser to init, leaving no
zombie and never blocking the frame loop. Uses `xdg-open` / `open` /
`ShellExecuteA` per platform, and lives outside the backend `#ifdef`s since
opening a browser needs no display — it works on the stub build too.

**Decoupling by design.** A shared header (`src/std/fluxa_image_buffer.h`) defines
a neutral 32-bit RGBA buffer (`FluxaImageBuf`), so `std.graph` (producer) and
`std.image` (consumer) never depend on each other — a capture is just bytes, and
no Raylib type crosses the boundary. The buffer lives behind a `dyn` as a single
`VAL_PTR`: the pixels are a separate heap allocation, never copied into the `dyn`,
so even a 4K frame keeps the handle at one pointer. Size math is done in `long`
before the `w*h*4` multiply (no integer-overflow → under-allocation), with a
~268M-pixel cap and a graceful out-of-memory error rather than a crash.

**Two bugs fixed along the way.**
- **String returns must use `fxstr_new()`, not `strdup()`.** Since the
  refcounted-string change (bug K), a `strdup`'d `VAL_STRING` return is freed with
  the wrong allocator and aborts with `free(): invalid pointer` (hit on
  `image.version()`). Fixed in the lib and corrected in `docs/CREATING_LIBS.md`,
  which still said "always `strdup`" in three places — stale guidance that would
  break any new lib.
- **`image.free` didn't parse** — `free` is a reserved keyword (`TOK_FREE`).
  Renamed to `image.discard`, matching the `json2.discard` precedent.

**Build-ordering fix (Makefile).** A newly added lib was missing on the *first*
build after adding it (`undefined identifier 'image'`) and only appeared on a
second build. Cause: `-include src/lib_registry_flags.mk` is resolved at parse
time, before the `build` recipe runs the registry generator, so each lib's
`lib.mk` gate (`ifeq $(FLUXA_BUILDTIME_<LIB>),1`) saw an undefined flag. Fix: give
`lib_registry_flags.mk` a real rule (depends on `fluxa.libs` + all lib headers);
since it's a prerequisite of an included makefile, GNU Make regenerates it and
re-executes itself before reading the `lib.mk` files, so a single `make build` is
correct. Incremental builds don't over-regenerate. This benefits every future
lib, not just `std.image`.

**Tests.** `tests/libs/image.sh` (15 cases: version, new + size, zero-size and
overflow rejection, resize in place / upscale / bad-size, save no-extension /
bad-format / extension recognition, discard idempotence, use-after-discard,
1×1 no-off-by-one, unknown-fn, load error path). `tests/libs/graph.sh` gains 3
capture cases plus `draw_image` and `open_url` coverage, including the full
`graph.capture → image.resize → image.discard` flow. Zero-warning build; `std.image` 22/22, `std.graph` 35/35.

**Docs.** `docs/STDLIB.md` — `graph.capture` row + full `std.image` section with
the canonical capture→resize→save example. `README.md` — `std.image` and
`std.sound` added to the library list and backend table (lib count 29/28 → 31).
`docs/CREATING_LIBS.md` — the `strdup` → `fxstr_new` correction.

## v0.24 — configurable resolver scope pool (`[runtime] scope_cap`)

The resolver's lexical-scope pool was a hard-coded 256 (`#define SCOPE_POOL_CAP
256` in resolver.c). It allocates one scope per top-level function, per Block, and
per Block method (`if`/`while` bodies consume none); a program with more scopes
than 256 aborted with **"aborting due to resolver errors"** and no symbol name —
because the failure is capacity, not a bad reference. Large multi-module codebases
(many Blocks, each with several methods) hit this ceiling.

`scope_cap` is now configurable via `fluxa.toml`, following the existing
`[runtime]` cap pattern (`gc_cap`, `prst_cap`, …):

```toml
[runtime]
scope_cap = 1024
```

**Implementation.** `resolver.c` replaces the compile-time `#define` with a
settable global `g_scope_cap` (default 256) and a `resolver_set_scope_cap(int)`
entry point (declared in resolver.h). `toml_config.h` adds the `scope_cap` field,
its default (256), and `[runtime]` parsing with a floor of 256 (a misconfigured
toml can never make the resolver weaker than the built-in default) and a ceiling
of 65536. `main.c` and `handover.c` call `resolver_set_scope_cap()` from the
loaded `FluxaConfig` before `resolver_run()` on every entry path (`run_once`,
`-dev`, preflight, and the handover pass). The pool `calloc` uses the configured
value; behavior is identical to before when `scope_cap` is unset (still 256).

Note `fluxa dis` only parses and will not surface this error — validate large
programs with `fluxa run`.

**Measured behavior (empirical, not estimated).** A scope is allocated only for
each top-level function, each Block, and each Block method — `if`/`while` bodies
consume none (verified: 256 sequential `while`s resolve under the default). The
exact count is `(top-level functions) + (Blocks) + (Block methods)`; the reference
game measures 19 Blocks + 212 methods + 41 functions = 272 and breaks at
scope_cap 271, runs at 272 — an exact match. Overflow aborts cleanly (`had_error`,
`resolver_run` returns -1) with no crash or partial state. On memory: each scope
slot is ~130 KB (`sizeof(SymTable)`, an inline symbol array), but the pool is
allocated per resolver run and freed before the program executes, and the calloc
is lazy — only used scopes touch pages. A 1000-function program runs at the same
~10 MB resident whether scope_cap is 256 or 8192; the cost of a generous cap is a
transient *virtual* reservation (scope_cap 1024 ≈ 130 MB, 65536 ≈ 8 GB address
space), harmless under overcommit but a reason not to set the maximum on
overcommit-disabled or embedded targets, where an oversized pool calloc fails and
the resolver aborts gracefully.

**Validation.** make test-all green: unit suite 82/82, config tests
(sprint8/prst_cap and the other `[runtime]` cap cases) pass, floor-clamp verified
(`scope_cap = 10` runs as 256), default-unset verified (still 256). Zero-warning
build. The reference game (263 scopes, previously over the 256 ceiling) runs with
`scope_cap = 1024`, ASan clean.

---

## v0.23 — refcounted strings + module-local fix

The str memory model is now **refcounted immutable buffers** — designed with
the language author around one rule: *free checks whether anyone else still
points at the buffer; the heap dies only when the last name lets go.*

Every string buffer carries a hidden header `[rc | bytes...]`;
`Value.as.string` still points at the bytes, so all readers are untouched.
Because Fluxa strings are immutable (mutation always produces a new buffer),
sharing the pointer on read is always safe:

- **write / produce** → new buffer, rc = 1 (`fxstr_new` / `_new_len` / `_alloc`)
- **read** (identifier, `arr[i]`, `d[i]`, `inst.field`) → same buffer,
  rc + 1 — **O(1)**, size-independent, no copy
- **free / reassignment / frame teardown** → rc − 1; heap freed at rc 0

Counters are atomic (GCC `__atomic`), so strings crossing flxthread
boundaries are correct with no copy rule.

What this fixes and changes:
- **Bug K dead in all contexts**: str field reassignment in module Blocks,
  and per-call str-local leaks. `frame_release_slots_from()` releases the
  callee's heap-owning slots at 4 sites: call_function return, TCO
  trampoline, and both VM→interpreter bridges (locals only there — param
  slots hold VM register pointers owned by the VM). VAL_DYN slots are
  untouched by teardown (param binding never pins; unpinning would steal
  the caller's pin — verified use-after-free on a live `win` bind).
- **The §5 aliasing trap family is gone**: `str x = names[0]; free(x)`
  releases x's reference and leaves the element intact; reassigning the
  reader in a loop never double-frees. Same observable semantics as an
  owned copy, at pointer cost.
- **free(field) in a Block** releases the field's ref and nils it; the next
  assignment revives it. free on prst still rejects (pool owns it).
- **Uniform producer discipline**: every site that puts a string into a
  Value allocates via fxstr (val_string, scope copy-ins, all 30 stdlib
  string constructors, fs/zlib/strings direct-fill buffers, FFI writeback,
  mcp name slices). The cache arena borrow convention was unsound (any
  release path corrupted the arena) and now copies out. An exhaustive
  audit gates the invariant: zero raw strdup/malloc/free on Value strings
  outside the prst pool (which keeps plain-malloc internals behind a
  copy-in/copy-out boundary; slots never adopt pool pointers).
- Also fixed on the way: a per-call leak of every prst scalar string
  (the frame-populate pre-copy was doubled by scope_set's copy-in), and
  scope_set_owned's same-pointer guard (a self-assign ref leak under
  refcount — releasing old and adopting incoming is naturally safe now).
- Removed: the method-result strdup in call_function, FLUXA_DBG_RTSET.

Performance (measured):
- 1M element reads of a 512 B string: **2.047 s → 0.441 s** (was strdup-
  per-read during an intermediate design; vs the original leaky build the
  read was pointer-cheap but unsound).
- Reads are size-independent: 512 B / 4 KB / 16 KB → 0.44 / 0.52 / 0.32 s
  (flat; a copying design scales linearly and would take seconds at 16 KB).
- bench 1M and bench_field unchanged (int paths untouched).

Validation: suite 82/82 (incl. tests/bugk_ownership.sh, 9 cases with ASan
no-per-call-leak gates); leak matrix (module/main × Block/fn) ZERO;
reference game ASan totals byte-identical across 300/700-frame budgets;
zero-warning build; protected files bit-identical (md5-verified).

**Boundary hardening (post-review):** value_free_data is now explicitly the
PLAIN-free helper — it is the prst pool's contract (the pool, a protected
file, frees its own plain-malloc storage through it, including on the
handover deserialize path). A new twin, `value_release_data`, carries the
fxstr semantics and is used by every runtime-side owner (scopes, stack
slots, dyn items — dyn items are always runtime Values, so fluxa_dyn_free
lives entirely on the release side). Two pool-internal frees in runtime.c
(init_value refresh, prst-arr element sync) were reverted to plain free
for the same reason. Caught by suite2's mixed-prst handover cases —
suite2, libs, integration and hardware-sim now gate every change:
`make test-all` green end-to-end (82/82 + 8/8 sections + libs + sim).

**Test-suite robustness (post-review, no runtime change):** a pre-existing
intermittent failure in the shell test harness, unrelated to the string
work, was fixed in the tests. `set -o pipefail` combined with
`echo "$out" | grep -q PAT` produced spurious failures: when grep matches
early and closes the pipe, echo takes SIGPIPE (non-zero), and pipefail
propagates that as the pipeline status — turning a successful match into a
failure (measured ~13-38 per 300 under load; 0 without pipefail). None of
these scripts rely on pipefail for correctness, so it is disabled right
after the shell-options line in the affected files; no grep lines change.
The full `make test-all` gate (unit + suite2 + libs + integration +
hardware-sim) is now deterministic.

Known remaining (separate, tracked): bug A — const-pool strings leak once
per compiled while (bytecode.c is protected); constant residual, not
per-iteration. The anti-compile array-touch anchor now only mitigates that
residual.

**Parser: module-local variables no longer namespace-mangled (bug J).** A
variable declared inside a function or Block method within a module was being
rewritten to the module-qualified name (`mod__d`) at declaration, the same as
a module top-level symbol. For a scalar this happened to stay consistent, but
for a `dyn` local it desynchronized the dyn's type/identity binding from its
later index read — `dyn d = [...]; d[0]` inside a module fn (or module Block
method) errored "'d' is not an array or dyn". The parser now tracks
`fn_body_depth` and mangles only module TOP-LEVEL declarations; locals inside
any function body keep their raw name. Both forms (`fn` and Block method) are
fixed; module top-level state, `prst`, and cross-module calls are unchanged.
Verified with a 4-line dyn-literal repro that reproduces outside the game.
Files: parser.c, parser.h. Suite 82/82; zero-warning; protected files
untouched (parser was already an authorized surface).

## v0.22 — module Block singletons + graph patches

- **parser:** `mod.Block.method(args)`, `mod.Block.field` (read/write) now
  parse in both expression and statement positions, emitting
  MEMBER_CALL/ACCESS/ASSIGN with the mangled owner (`mod__Block`).
  Previously: "expected '(' or '=' after module member name".
- **parser:** inside a module, references to Blocks declared in the same
  module (`Vault.bump(x)` from a module fn or method) now mangle the owner
  via the existing module_decls table. Previously: undefined identifier.
- Tests: tests/modules/modules.sh cases c22a–c22e + fixtures/static/vault.flx.
- No runtime/resolver/VM changes — parser-only; full suite green.

### graph: BACKSPACE / TAB key names

- graph_key_code now maps "BACKSPACE" → KEY_BACKSPACE and "TAB" → KEY_TAB
  (raylib backend). Previously these strings returned 0, so
  key_pressed(win, "BACKSPACE") always reported false — text-entry backspace
  could never fire. ("F" already resolved via the single-letter A-Z rule;
  the F-key crash some callers saw was an OLD binary predating graph.fullscreen,
  not a key-mapping gap.)
- No behavior change for the stub backend (headless, no key events).

### graph: proportional fullscreen (render-to-texture scaling)

- The raylib backend now renders each frame into an offscreen RenderTexture at
  the logical (design) resolution passed to graph.init, then blits it to the
  real window scaled to fit and centered, with black letterbox/pillarbox bars.
  Previously fullscreen just enlarged the window and the game stayed at its
  original size in the top-left corner with the rest painted in the clear color.
- graph.begin_frame draws into the target (BeginTextureMode); graph.end_frame
  finishes it and does the scaled DrawTexturePro blit. graph.close unloads the
  texture.
- graph.mouse_x / mouse_y now un-project window coordinates back into logical
  space (accounting for the letterboxed scale), so mouse input still lines up
  with what the game drew.
- Stub backend unchanged (headless). NOTE: the raylib path can only be compiled
  with raylib present; verify on a FLUXA_GRAPH_RAYLIB=1 build.

## v0.22.3 — std.graph: fullscreen toggle

- `graph.fullscreen(win)` → bool: toggles fullscreen and returns the new
  state. Raylib backend uses `ToggleFullscreen()`; stub tracks the flag so
  program logic is testable headless.
- Key map: `"F11"` now recognized by `key_pressed`/`key_down` (raylib).
- Tests: 2 new cases in tests/libs/graph.sh (toggle state, invalid window).

## v0.22.2 — std.graph: custom TTF/OTF font support

### feat(graph): load_font / draw_text_font / text_width / unload_font

`std.graph` gains custom-font text rendering, on both backends:

- **`graph.load_font(win, path, size)` → `dyn`** — loads a TTF/OTF file and
  rasterizes a glyph atlas at the given base size (1–512). The atlas covers
  ASCII 32–126 **plus Latin-1 160–255**, so Portuguese/Western European accented
  characters render correctly from plain UTF-8 `str` values. Returns an opaque
  font cursor (same `VAL_PTR`-in-`dyn` pattern as the window cursor).
- **`graph.draw_text_font(win, font, text, x, y, size, r, g, b)` → `nil`** —
  draws with the loaded font (`DrawTextEx` on Raylib; spacing = size/10; bilinear
  filtering on the atlas texture for scaled sizes).
- **`graph.text_width(win, font, text, size)` → `int`** — rendered width in
  pixels (`MeasureTextEx` on Raylib), for centering and layout.
- **`graph.unload_font(win, font)` → `nil`** — releases the font (GPU texture on
  Raylib) and nulls the cursor; any later use is an "invalid font cursor" error.

Error contract (identical on both backends where possible): missing file →
"cannot open font file"; size outside 1–512 → error; Raylib additionally rejects
unsupported/corrupt files ("failed to load font") by detecting the
fall-back-to-default-font case — the default font is never unloaded. All errors
route through `LIB_ERR` and are captured by `danger` as usual.

Stub backend: validates the file exists, no-op rendering, and a deterministic
`text_width` approximation (`len * size * 6 / 10`) so layout logic is testable
headless.

Tests: `tests/libs/graph.sh` extended 13 → 20 cases (happy path, missing file,
bad size, UTF-8 accents in a frame, deterministic width, use-after-unload,
bad cursor). Raylib branch compile-verified against raylib 5.5 headers with
`-Wall -Wextra -pedantic` — zero warnings. Docs: `STDLIB.md` std.graph section
updated with the function table and a custom-font usage guide.

## v0.22.1 — docs: correctness pass across all reference material

### docs: correct the Block/`danger`/`dyn` rule and other verified behavior

A full review of `docs/` against the actual runtime behavior. The central fix
corrects a rule that several documents stated wrong.

**The Block + `danger`/`dyn` rule (corrected).** Earlier docs (spec §7, the
How-to-Program guide, STDLIB, and the v0.19 changelog entry below) stated
categorically that `danger` is "not permitted inside Block methods". This is
**wrong** and was verified against the runtime. The correct rule:

- `dyn` and `danger` **cannot be Block fields** — a Block body accepts only typed
  field declarations (`int`, `float`, `str`, `bool`, `char`, `arr`) and `fn`
  methods. A `dyn buffer = [0]` field or a loose `danger { ... }` statement in the
  Block body is a **parse error**.
- `dyn` and `danger` **work normally inside a Block method**. This is the idiomatic
  place for a Block that owns fallible IO: the method opens the resource inside
  `danger`, updates typed fields, and closes with `if err != nil`. This is exactly
  what the `ScoreBoard` persistence Block does.

Corrected in: `FLUXA_GUIDE.md` (invariants 4–5, §7 Rule 2, §11, §14 table),
`fluxa_spec_v16.md` §7, `STDLIB.md`. The obsolete v0.19 entry below is left as
historical record.

**`danger` framed as intentional containment.** All docs now present `danger` as a
deliberate declaration ("this may fail and I will handle it"), with the idiom that
**every `danger` block closes with `if err != nil`**. Outside `danger`, a failure
aborts with a line number — on purpose.

**Error ring behavior (corrected).** The spec previously said errors "do not
interrupt flow". Verified: inside a `danger`, execution **stops at the first error**
(code after the failing line does not run); `err` is cleared before each `danger`;
`err[0]` is most recent and higher indices hold earlier errors, with the oldest
pushed out when the 32-entry ring fills. Corrected in `fluxa_spec_v16.md` §8.3–8.4
and `FLUXA_GUIDE.md` §9.

**Memory model made explicit.** `str`, `arr`, `dyn` are pointers into the heap (a
deliberate performance decision — no implicit copy of large structures); scalars are
values. The **array-element aliasing trap** (`str x = arr[i]` creates a second owner
and corrupts the buffer on `free`/reassign) is now documented with verified examples
and the safe patterns (`for-in`, direct argument, `concat` copy) in
`fluxa_spec_v16.md` §13.6, `FLUXA_GUIDE.md` §12.5, and §14.

**Performance idiom documented.** `PERFORMANCE.md` now connects the Block-method
benchmark to the idiom: put the loop **inside** the method (VM fast path) rather than
an outer loop hammering a one-step method (AST slow path + per-call Block copy).

Every corrected rule in this pass was confirmed by executing code in the runtime.

## v0.22.0 — std.sound

### feat(stdlib): `std.sound` — audio playback (wav/mp3/flac) + sine tone generation

New lib following the dual-backend pattern. Default backend is an
API-complete stub with no external deps and no audio device requirement:
it tracks engine/sound state (loaded, playing, paused, volume), so the
play/stop/pause/resume/is_playing state machine is fully testable
headless and in CI. `make FLUXA_SOUND_MINIAUDIO=1 build` enables the
real backend via a vendored single-header miniaudio
(`vendor/miniaudio.h`, not committed — same policy as the optional
Raylib backend); miniaudio resolves the OS audio subsystem at runtime
(ALSA/PulseAudio/JACK, WASAPI, CoreAudio, sndio, AAudio), so the wrapper
is OS-portable with zero code changes. Not applicable to bare-metal
targets — on RP2040/ESP32 set `std.sound = false` for zero code size.

Design follows the validated `std.wserver` pattern: opaque `int` handles
(engine + per-engine sound slots, mutex-guarded tables), no `dyn`
cursors, so Block methods interoperate with plain `int` parameters.
Limits: 4 engines, 64 sounds/engine. API: `init`, `close`, `load`,
`unload`, `play` (always from the start), `stop` (rewinds), `pause`
(keeps position), `resume`, `is_playing`, `volume` (accepts `float` or
`int`, range-checked 0.0–1.0), `tone` (sine, 1–20000 Hz, ≤10 s —
blocking on miniaudio, immediate on stub), `version`. miniaudio's
implementation TU (`fluxa_std_sound_ma.c`, added via
`FLUXA_EXTRA_SRCS`) relaxes diagnostics for the vendored header only;
Fluxa sources remain zero-warning. Adds 15 tests (state-machine cycle,
handle validation in and out of `danger`, double-close, unload
invalidation, volume/tone range checks). Full suite 81/81 green.

Doc note added to CREATING_LIBS.md: the first `make build` after adding
a brand-new lib parses the previous `lib_registry_flags.mk` (Make
`-include` happens at parse time, the generator runs inside the build
rule) — run `make build` twice when a lib is created; subsequent builds
are unaffected.

## v0.21.0 — std.flxthread batch spawn

### fix(vm): `OP_MOVE` keeps the GC slot⇒pin invariant — `dyn` reassigned in a compiled `while` no longer freed while live

Second instance of the bug class fixed earlier in `rt_set` (see *"symmetric
pin/unpin of VAL_DYN slots in rt_set"* below): the invariant **"every slot
holds exactly one strong reference to its VAL_DYN"** was maintained by the
evaluator but not by the bytecode VM. `chunk = csv.next(...)` inside a
compiled `while` stores the returned dyn via a bare `R[a] = R[b]` `OP_MOVE`,
leaving it at `pin_count 0`; the back-edge `gc_sweep` (`vm_tick_callback`)
then frees it while the variable still references it. Any condition-position
read of the variable in the next iteration is a heap-use-after-free — the
canonical docs pattern `while len(chunk) > 0 { … chunk = csv.next(…) }` hung
non-deterministically (~30% at `-O2`; deterministic under ASan). Producer-
independent (reproduced with `std.json2`); the same code forced through the
evaluator ran clean.

Fix keeps the VM runtime-agnostic: new `vm_store_cb_t` callback on `vm_run`;
`L_MOVE` (and the non-GCC fallback) delegates to it only when the destination
is a variable register (`< 128`) and a `VAL_DYN` is involved — two type
compares otherwise, no cost. `vm_store_callback` in `runtime.c` mirrors
`rt_set`: unpin the overwritten dyn, pin the stored one, self-move no-op.
Orphan collection at the back-edge is unchanged, as is manual `free()`.
Validated: ASan 12/12 clean on the repro; peak RSS flat on a 200k-line chunked
scan (11.6 MB, matching the len-once pattern); `make bench` within noise of
baseline on all three benches; csv ×5, flxthread 29/29, full suite2 green.

### feat(stdlib): `ft.new` batch form — spawn N named worker threads in one call

`ft.new("w", 16, "worker", srv)` spawns 16 global-function threads named
`w1`..`w16` (1-indexed), each running `worker(srv)` — replacing sixteen
`ft.new("w1", "worker", srv)` … lines. The form is selected purely by the type
of the second argument (`int` = batch, `string` = single global fn, Block =
method), so it coexists with the existing forms with no new syntax and no
lexer/parser/runtime change — it is a single added branch in the `ft.new`
dispatch that returns before the single-thread allocation. A numeric *name* like
`ft.new("w10", "worker", srv)` is unaffected (`"w10"` is the name in the first
slot; the second argument is still the string `"worker"`). Count is bounded to
`1..FLUXA_THREAD_MAX`; the same `max_msg_args` and arity checks apply to the
trailing arguments. Adds 6 tests (spawn count + 1-indexed names, arg passing,
numeric-name coexistence, out-of-range/unknown-fn/arity errors).

## Archived development note — exact KNN index (VKN3) + wserver TCP_NODELAY

This development entry was formerly labeled `v0.20.0` while the public release
line was still at v0.19. It is retained for technical history, but is not a
second v0.20.0 release.

Two stdlib changes driven by a high-throughput vector-search service (3M-vector
fraud scoring at 900 req/s under a sub-core budget). The runtime is unchanged —
this is purely a stdlib change.

### perf(stdlib): `std.libv` VKN3 nearest-neighbor index — per-node AABB + best-first exact search

The KNN index now stores a 14-dimensional axis-aligned bounding box (AABB) on
every tree node and searches best-first with full box-distance pruning. The old
split-plane bound prunes on a single dimension and collapses in 14-d: atypical
("off-manifold") queries degenerated to scanning ~60% of the 3M points (~23 ms
each), and a handful of those per second saturated a fractional core and
generated the entire P99 tail. The box bound prunes across all 14 dimensions and
visits children nearest-box-first, keeping the search **exact** (identical
results) while cutting the worst case ~80×.

Adds `build_index.c` (the offline VKN3 builder) and `vk_count_stats` (exposes
leaves visited). `kd_count`/`kd_score` gain an optional `budget` argument plus an
`FLUXA_KD_BUDGET` env default that caps leaf visits; both are now optional —
exact search is fast enough that the budget is no longer required.

The on-disk format magic bumped to `VKN3` (was `VKN2`); a stale index is rejected
at load, so rebuild with `build_index` after upgrading.

| 3M refs, per query | VKN2 (old) | VKN3 (new) |
|---|---|---|
| typical (near-manifold), exact | 0.24 ms | 0.20 ms |
| worst (off-manifold), exact | ~23 ms | ~0.29 ms |

### perf(stdlib): `std.wserver` sets TCP_NODELAY on every connection

MHD left Nagle enabled, so the server's separate header/body writes could stall
on the peer's delayed-ACK timer — a low-median / ~40–100 ms-tail profile that
only shows up over a real (bridged) network behind a reverse proxy, never on
loopback. The server now disables Nagle on each accepted connection via
`MHD_OPTION_NOTIFY_CONNECTION`, and logs one self-verifying line at startup:
`[wserver] MHD started: thread_pool=N, TCP_NODELAY=on (build OK)` — handy for
confirming the running build inside a container.

### docs: document `std.libv` KNN/temporal API and the `[libs.wserver] workers` key

`kd_load` / `kd_ready` / `kd_count` / `kd_score` / `dow` / `daymin` and the
manual-mode `workers` thread-pool size shipped but were undocumented; both are
now in STDLIB.md.

### Deployment note — sub-core tail latency

On a fractional-CPU budget, server P99 is dominated by topology, not compute.
Keep the MHD thread pool small (`workers = 2–4`) so the CFS quota is not burned
on context switches, and give a fronting reverse proxy enough CPU that it does
not become the serialization point. With VKN3, compute (KNN + HTTP) is
sub-millisecond; in a measured 3M-vector / 900 req/s service the residual tail
was the proxy's CPU share, not the Fluxa server.

## v0.19.2 — std.cache eviction + capacity

The cache implementation in v0.19 silently dropped inserts once all 8 probe slots in a key's natural shard were taken. Under sustained high-cardinality workloads — HTTP services with UUID-keyed records, in particular — this meant the cache filled in the first few hundred POSTs and then **stopped accepting new entries entirely**. Every subsequent GET missed cache and went to the backing database, saturating the worker pool and producing pathological P99 latency.

This release fixes the eviction policy and bumps capacity. The runtime code is unchanged — this is purely a stdlib change.

### perf(stdlib): random eviction in `std.cache` when probe slots fill

`shard_locate` now returns a random slot from the 8-probe window when no match and no empty slot are found. The caller (`cache.put`) frees the old key/value and reuses the slot. This approximates Random Replacement (RR) at the per-bucket level — close in behavior to per-shard LRU without the overhead of maintaining an access-order list. Each `CacheShard` carries its own xorshift32 state, so eviction-slot selection has zero cross-thread coordination cost.

The previous "silent drop" behavior was the worst case: caches that look right at startup but become useless after the working set exceeds capacity, with no error signal.

### feat(stdlib): `cache.stats()` and `cache.stats_reset()`

Diagnostic counters tracking puts (inserts, updates, evictions, failures), gets (hits, misses), and current size. Returned as a `key=value` string for easy grepping. Atomic via `__sync_fetch_and_add`, so reading them is safe under concurrent traffic.

`cache.stats_reset()` zeroes the counters for clean before/after measurements (typically called after warm-up traffic, before the measured load).

### perf(stdlib): bump cache capacity to 8192 entries (32 shards × 256 slots)

Previous limits were 16 × 64 = 1024 entries — adequate for IoT scenarios but undersized for HTTP services. Doubled shard count (32) reduces per-shard contention with high concurrent worker counts, and quadrupled per-shard slots (256) increases the effective working set that can be cached before random eviction starts.

Memory cost: ~8 KB of static globals (`CacheShard` array) + ~1 MB peak heap when all 8192 slots are populated with typical UUID keys + small JSON values. The previous footprint was ~256 KB peak.

### Measurement notes

Local stress test with simulated 2 ms backing-store cost per cache miss, 96 concurrent clients, k6-like access pattern (60% writes, 40% reads from a per-VU pool of IDs grown over time):

| | OLD cache (v0.19) | NEW cache (v0.19.2) | Delta |
|---|---|---|---|
| RPS median | 880 | 1127 | **+28%** |
| Median latency | ~101 ms | ~77 ms | **-23%** |
| P99 latency | ~140 ms | ~120 ms | **-14%** |

Synthetic isolation test (50 000 distinct keys inserted, then query most-recent 1 000):

| | OLD cache | NEW cache |
|---|---|---|
| Inserts succeeded | 1 024 | 8 192 |
| Silent failures | 48 976 | 0 |
| Recent-key hit ratio | **0 / 1 000 (0%)** | **930 / 1 000 (93%)** |

The user-visible impact under real-world high-cardinality load is expected to be much larger than the synthetic +28% because production k6 generates ~100 000 distinct UUIDs over a 12-minute run, far exceeding even the new 8 192 capacity. With the previous silent-drop behavior, the hit ratio collapsed to essentially zero; with random eviction it stays proportional to recent traffic.

### Tests

- 3 new tests in `tests/libs/cache.sh` (now 13/13):
  - `overflow_random_eviction_keeps_recent` — 30 000 puts, recent 1 000 keys still mostly hit
  - `stats_returns_counters` — round-trip of put/get counters via `cache.stats()`
  - `stats_reset_zeroes` — verifies `stats_reset()` clears the snapshot
- All 19 sprint10c free tests still pass.
- Full suite: 30 known-environmental fails (unchanged from v0.19).
- `make bench` within container variance of v0.19 baseline.

---

## v0.19 — Memory ownership: from leaks to bounded

Eight latent memory leaks in the AST evaluator, stdlib dispatch, and built-ins. Each one was bounded per allocation but cumulative across the per-request workload of a long-running HTTP worker. Together they caused a 24-worker SUT under sustained k6 load to grow from ~12 MB idle to >300 MB after ten minutes of traffic. Fixed here.

**Headline result.** The reference HTTP+DB SUT (24 workers, 1k+ req/s, PostgreSQL via libpq) now runs at 30–40 MB resident under load and returns to ~28 MB when traffic stops. Stable across hours of operation. **About a 10x improvement over the equivalent workload in Python.**

### fix(runtime): release owned `VAL_STRING` lib-call arguments after dispatch

Every `lib.fn("literal", x)` evaluates the literal via `eval(NODE_STRING_LIT)` which calls `val_string(s)` — a `strdup`. That `strdup` was passed to the lib and then dropped when the dispatch returned, leaking once per literal per call. In an HTTP worker doing 5–15 literal-bearing lib calls per request, ~50 bytes/req leak compounded to ~300 MB residual per 600k-request run.

Fix in `NODE_MEMBER_CALL`, `NODE_FUNC_CALL`, and `NODE_FFI_CALL` evaluation: classify each arg's source node as **owned** (literal, call return, identifier read — see ownership classifier below) or **aliased** (member/array access). After the lib call returns, free owned `VAL_STRING` args. Aliased args are left untouched.

### fix(runtime): register lib-returned `VAL_DYN` with the GC

`json2.parse`, `csv.open`, `pg.connect`, and similar functions return a `FluxaDyn *` wrapper. The wrapper was constructed by the lib's helper (e.g. `j2_wrap`) but never registered with the runtime GC. Each new dyn returned was outside the sweeper's reach — when the holding slot was overwritten, the wrapper became unreachable and unfreeable.

Fix in `NODE_MEMBER_CALL` and `vm_call_callback`: at the dispatch return boundary, any `VAL_DYN` not already in the GC table is registered with `cap`-derived size. Subsequent slot reassignment can then drop the pin and let `gc_sweep` collect it.

### fix(runtime): symmetric pin/unpin of `VAL_DYN` slots in `rt_set`

`NODE_VAR_DECL` pinned a new `VAL_DYN` after writing it to the stack slot. `NODE_ASSIGN` did neither — the assigned `VAL_DYN` was unpinned (`pin_count == 0`), the next `gc_sweep` would collect it, and the next read would hit freed memory.

Fix in `rt_set`: when overwriting a slot, unpin the old `VAL_DYN` if present (pointer-difference guard against self-assignment), write the new value, then pin the new `VAL_DYN`. The pre-existing explicit `rt_gc_pin` calls in `NODE_VAR_DECL` were removed to prevent double-pinning. `rt_set` now maintains the invariant **"every slot holds exactly one strong reference to its `VAL_DYN`"** automatically.

### fix(runtime): release owned `VAL_STRING` slot contents on `rt_set` overwrite

`str x = ""` initializes the slot with `strdup("")`. The subsequent `x = call_result` overwrote the slot without releasing the original `""` strdup — every HTTP worker doing `str out_name = ""` followed by `out_name = json2.get(...)` leaked one strdup per request.

Fix in `rt_set`: detect overwrite of `VAL_STRING` and `VAL_ARR` slots and call `free` / `value_free_data` before storing the new value. Pointer-difference guard prevents double-free on self-assignment. To make this safe in the presence of aliased reads, `NODE_IDENTIFIER` was changed to `strdup` its `VAL_STRING` result — every reader now becomes the sole owner of its copy. Cost: O(strlen) per identifier-as-string read.

`NODE_FOR` was updated in tandem: loop variables binding to `VAL_STRING` array/dyn elements are now bound to a `strdup` of the element, so iteration cannot corrupt the source on slot reassignment.

### fix(runtime): release owned `VAL_STRING` operands in `eval_binary`

`if cached == ""` evaluates `""` to a strdup'd `VAL_STRING`, runs `strcmp`, returns `val_bool(...)`, and drops the operand without freeing. Every literal-bearing comparison in a worker loop leaked one strdup. The SUT had 5–15 such comparisons per request.

Fix in `eval_binary`: ownership-classify both operands at entry. Wrap every return path with `_BIN_RET` macro which frees owned `VAL_STRING` operands before returning.

### fix(runtime): release owned `VAL_STRING` / `VAL_DYN` returns discarded by `NODE_BLOCK_STMT`

`json2.get(doc, "name")` called as a statement (its return discarded for side effect) leaked the owned strdup. `NODE_BLOCK_STMT` evaluated each child statement and dropped its `Value` without releasing heap data.

Fix in `NODE_BLOCK_STMT`: classify each child node by AST type. For owned `VAL_STRING` returns, free the buffer; for owned `VAL_DYN` returns with `pin_count == 0`, unregister and free immediately.

### fix(builtins): `len()` and `print()` release transient owned strings

After the `NODE_IDENTIFIER` strdup change, `len(path)` in a worker loop strdup'd `path`, returned an `int`, and dropped the `char *`. Same for `print(var)`. Both leaked once per call.

Fix in `builtins.c`: added a `builtin_release_owned(v, node)` helper and applied it to `builtin_len` and `builtin_print` so the transient strdup is released before the builtin returns.

### fix(runtime): `NODE_ARR_DECL` element strdup is now conditional

Once `NODE_IDENTIFIER` returned an owned strdup, `NODE_ARR_DECL`'s defensive `data[i].as.string = strdup(data[i].as.string)` became a second strdup over an already-owned buffer — the first strdup leaked once per element per array literal. The `str arr params[3] = [out_name, out_email, hash]` pattern that every SUT path uses for `pg.query_params` leaked 3 strdups per request: ~100 bytes/req × 600k req = ~60 MB residual per 10-minute run.

Fix in `NODE_ARR_DECL`: classify each element's source node. Skip the redundant strdup when the source already produced an owned copy (literal, call, identifier read). Keep the defensive strdup for aliased reads (member/array access) so the array continues to own its elements safely.

### feat(stdlib): `std.cache` — sharded k/v cache + bump-pointer arena

Two independent subsystems in one lib for HTTP workers building responses.

**Sharded cache.** 16 shards × 64 slots × 8-probe linear addressing = 1024 entries max. Per-shard `pthread_mutex`. FNV-1a 32-bit hash. Functions: `put`, `get` (returns owned copy), `del`, `clear`, `size`. Designed for response memoization across worker threads with minimal lock contention.

**Bump-pointer arena.** Up to 64 concurrent arenas, each a linked list of 64 KB slabs with 8-byte alignment. Functions: `arena_new`, `arena_str`, `arena_concat`, `arena_concat3`, `arena_concat5`, `arena_reset` (free all slabs except first, reset bump pointer), `arena_drop`. Arena strings live until the next `arena_reset` or `arena_drop` — `free()` rejects them.

The lib is zero-init with `pthread_once` lazy construction — no startup or steady-state cost when enabled but unused.

Replaces the GCC range-init pattern (`[0 ... CACHE_SHARDS - 1] = {...}`) with ISO C `pthread_once` to keep the build at zero warnings.

### Ownership classifier (for lib authors and tooling)

The runtime classifies AST node types as producing owned or aliased values:

| Owned (heap-allocated by this eval, single owner) | Aliased (shares storage with a slot or stable backing) |
|---|---|
| `NODE_STRING_LIT` | `NODE_MEMBER_ACCESS` |
| `NODE_MEMBER_CALL` | `NODE_ARR_ACCESS` |
| `NODE_FUNC_CALL` | `NODE_INDEXED_MEMBER_ACCESS` |
| `NODE_FFI_CALL` | identifier reads of non-string types |
| `NODE_IDENTIFIER` reading `VAL_STRING` (new) | |

This classifier appears in `NODE_MEMBER_CALL` arg release, `eval_binary` operand release, `NODE_BLOCK_STMT` discard release, `NODE_ARR_DECL` element handling, and `builtins.c` arg release. See [spec §13.6](fluxa_spec_v16.md#136-slot-ownership-and-the-free-operator) for the full contract.

### Tests

- `tests/sprint10c_free.sh` — 19 tests covering every fix. Stable across re-runs.
- SUT smoke (24 workers, 100k mixed POST/GET/PATCH requests across 5 rounds): baseline 1.7 MB → final 1.7 MB. Zero growth.
- `tests/libs/cache.sh` — 10 tests for the new lib.
- Full suite: 30 known environmental fails (crypto/httpc/sqlite — backend deps unavailable in CI), zero regressions vs upstream.
- `make bench` — within container variance of pre-fix baseline.

### Docs

- [STDLIB.md](STDLIB.md) — new "Memory Ownership Model" section in Design Principles. New `std.cache` section. `std.wserver` and `std.pg` examples updated to the production `free()`-per-request pattern.
- [FLUXA_GUIDE.md](FLUXA_GUIDE.md) — §12.5 fully rewritten. Common Mistakes table updated.
- [CREATING_LIBS.md](CREATING_LIBS.md) — new "Memory ownership contract" section for lib authors.
- [fluxa_spec_v16.md](fluxa_spec_v16.md) — new §13.6 "Slot Ownership and the `free` Operator" with the full runtime contract.

### Known limitations carried forward

Discovered while investigating the leak chain; not fixed in v0.19, tracked for follow-up:

- **Bytecode VM `vm_compare` does not handle `VAL_STRING`.** `s == "literal"` inside a function body that compiles to bytecode falls through to a double-cast. The SUT works because `free()` and `danger { }` inside worker loops force AST fallback. The bytecode path is reached only by pure-arithmetic loops where the bug cannot manifest.
- **`ft.new(instance, "method")` for HTTP workers using `wserver.accept`** produces an unresponsive server. The function-based `ft.new("name", "fn_name", arg)` pattern works correctly and is used in all SUT examples.
- **Same-name Block instances across threads race on `g_block_instances`.** Worker isolation by `typeof` requires distinct instance names per thread.
- **Concurrent `prst arr` writes (`NODE_ARR_ASSIGN`) are not atomic** across threads. Reading is safe; concurrent writes to the same `prst arr` from multiple workers can race.

## v0.18 — stdlib hardening for HTTP/DB workloads

Fixes uncovered while running Fluxa as an HTTP server backend against a PostgreSQL database under sustained k6 load (1640 RPS sustained, 24 worker threads, single CPU core, 1 GB memory cap).

### fix(json2): rename `json2.free` → `json2.discard`

`free` is a reserved keyword in the Fluxa parser, which prevented `json2.free(doc)` from parsing — making the function unreachable from Fluxa code despite being implemented. The function is renamed to `json2.discard(doc)` with identical semantics. **This is a breaking change** for any code that called `json2.free`, but no such code existed because the call site never parsed.

The function releases the heap-allocated parse tree (`Json2Doc`) behind the `dyn` wrapper. The `dyn` itself is GC-managed but the tree is opaque to the GC; without `discard`, parse trees accumulate until process exit. Calling `discard` on a `nil` or already-discarded document is a safe no-op.

### fix(pg): serialize first `PQconnectdb` per process

`libpq`'s GSSAPI / Kerberos initialization (`libkrb5`) is not thread-safe before the first successful `PQconnectdb`. When many worker threads called `pg.connect` concurrently at startup, this race manifested as `k5_mutex_lock: Invalid argument` and crashed the process. Fix: wrap the first connection per process in `PQinitOpenSSL(0, 0)` (called once via `pthread_once`) plus a process-wide `pg_connect_mu` mutex around `PQconnectdb` itself.

### fix(wserver): high-concurrency robustness

Multiple fixes for sustained-load reliability:
- `MHD_USE_EPOLL_INTERNAL_THREAD` + `MHD_OPTION_THREAD_POOL_SIZE` for proper kernel-level connection multiplexing
- `MHD_OPTION_LISTEN_BACKLOG_SIZE` and `MHD_OPTION_LISTENING_ADDRESS_REUSE` for burst handling and quick restart
- Idempotent `reply_json` (first reply wins; subsequent calls are no-ops, eliminating a race where multiple reply paths could double-queue)
- 30s outer-bound timeout in `ws_finish_reply` to detect stuck handlers
- `lib.mk` now falls back to a direct header probe when `pkg-config` lacks an entry for `libmicrohttpd`
- Move the non-static `fluxa_std_wserver_call` into its own TU (`fluxa_std_wserver.c`) so the linker sees the symbol exactly once

### feat(strings): `strings.hash(str s) → int`

FNV-1a 32-bit hash, suitable for hash-table indexing. Used by the benchmark SUT's response cache. Pure function, no `danger` required.

### tweak(flxthread): bump `FLUXA_THREAD_MAX` to 64

Was 16. The new compile-time cap allows one worker per CPU core on machines up to 64 cores, plus headroom for auxiliary threads. Embedded targets that need a tighter cap can override at build time.

### docs

- `STDLIB.md`: document `json2.discard` and its memory contract; document `strings.hash`; document the `FLUXA_THREAD_MAX` cap and its implications for HTTP-style workloads
- `FLUXA_GUIDE.md`: new section **12.5 Memory in Long-Running Loops** describing how `VAL_STRING` scope cleanup interacts with worker loops, the residual leak this produces, and the mitigations available within the current runtime

### Known limitation

`VAL_STRING` values assigned inside the body of a `while` loop in a worker function are not released until the function returns. Since worker functions run forever, this produces a slow accumulation that glibc consolidates under memory pressure but does not fully reclaim. A `free(x)` built-in or automatic-free-on-overwrite scheme would close this gap; both require runtime changes and are tracked separately.

---

## v0.17 — std.flxthread multi-arg

**23 passed, 0 failed.**

### feat(flxthread): multi-arg ft.new + ft.message/ft.await

`ft.new` now accepts arguments for global function threads:

```fluxa
ft.new("name", "fn")              // zero args — existing
ft.new("name", "fn", a)           // 1 arg
ft.new("name", "fn", a, b, c)     // N args (up to max_msg_args)
```

`ft.message` and `ft.await` now accept multiple arguments:

```fluxa
ft.message("name", "method", a, b)
ft.await("name", "method", a, b)
```

This enables parallel IO workers — each thread receives its own
connection handle or config via `ft.new` args, eliminating the need
for shared state or prst globals.

**Memory safety:** `FLUXA_FT_MAX_ARGS=8` hard cap at compile time.
`FlxMessage.args[FLUXA_FT_MAX_ARGS]` is a static array — no heap
allocation per message. Exceeding `max_msg_args` pushes a clear error
to `err`. Arity mismatch (args > fn params) caught at `ft.new` time.

**Configuration:**
```toml
[libs.flxthread]
max_msg_args = 2    # default 2, range [1..8]
```

**Tests:** 23 passed (15 existing + 8 new):
`new_fn_1arg`, `new_fn_3args`, `message_2args`, `await_2args`,
`new_fn_overflow_caught`, `message_overflow_caught`,
`new_fn_arity_mismatch`, `toml_max_msg_args_4`.

Also fixed: `ft_active_lifecycle` test was flaky — thread could finish
before `ft.active` check. Added `time.sleep` to keep thread alive
during the check.

---

## v0.16 — std.wserver + std.pg + Design Corrections

**Zero warnings. All tests pass.**

### API redesign — opaque int handles (pg + wserver)

`std.pg` and `std.wserver` were reworked from `dyn` cursors to opaque `int` handles. This change enforces Fluxa's ownership model: connection state lives entirely inside the lib's C layer; Fluxa code holds only an integer identifier.

**std.pg — before/after:**
- `dyn db = pg.connect(...)` → `int db = pg.connect(...)`
- `dyn res = pg.query(db, sql)` → `int res = pg.query(db, sql)`
- `dyn params = [...]` / `pg.query_params(db, sql, params)` → `str arr params[N] = [...]` / `pg.query_params(db, sql, params, N)`
- `pg.close`, `pg.free_result` are now no-ops on invalid handles (safe to call unconditionally)

**std.wserver — before/after:**
- `dyn srv = wserver.serve(port)` → `int srv = wserver.serve(port)`
- `dyn req = wserver.accept(srv, ms)` → `int req = wserver.accept(srv, ms)`
- `req != nil` → `req != 0`
- `wserver.reply_headers` now takes `str arr headers, int n` instead of a JSON string
- `wserver.stop` is now a no-op on invalid handles

**Config:** `fluxa.toml` gains `[libs.pg]` and `[libs.wserver]` sections with per-field limits enforced at the C boundary (not just validated).

### New feature — std.wserver auto-scaling pool

`wserver.serve(port, true)` activates an internal MHD thread pool managed by the lib:
- Starts with `min_threads` MHD daemons accepting connections
- Monitor thread scales up (adds daemon) when queue depth ≥ `scale_up_queue`
- Scales down (removes idle daemon) after `scale_down_idle` seconds of inactivity
- Never exceeds `max_threads`

With `auto=false` (or no second arg) behavior is unchanged — single daemon, user manages workers via `ft`.

New toml keys: `min_threads`, `max_threads`, `scale_up_queue`, `scale_down_idle` under `[libs.wserver]`.

### New feature — FFI str_buf_size

`[ffi] str_buf_size` controls the writable `char*` buffer allocated per pointer arg in FFI calls. Previously hardcoded, now configurable (default 1024, range 64–65536).

### Design documentation

`docs/fluxa_spec_v15.md` Section 7 now explicitly documents Block field restrictions:
- `dyn` is not a valid Block field type — use typed fields and pass cursors as arguments
- `danger` is not permitted inside Block methods — handle fallible operations in functions outside the Block
  > **[Corrected in v0.22.1]** This statement was wrong. `danger` and `dyn` work
  > inside Block methods; only *field* declarations are disallowed. See the v0.22.1
  > entry at the top.

`docs/STDLIB.md` rewritten with correct API for all 28 libs. Key corrections:
- `std.pg` and `std.wserver` fully updated to int handle API
- `std.http` (mongoose-backed) documented separately — retains `dyn` cursors by design
- Design principles updated: `dyn` cursor semantics, int handle semantics, Block field rules

New guide: `docs/FLUXA_GUIDE.md` — step-by-step reference for writing correct Fluxa programs.

### Tests and tooling

- `tests/libs/wserver.sh` — updated to int handle API (21 tests, 0 failed)
- `tests/libs/pg.sh` — updated to int handle API (28 tests, 0 failed)
- `tests/integration/pg/` — Docker-based integration test with real PostgreSQL (`make test-integration-pg`)
- `fuzz/fuzz_pg.c`, `fuzz/fuzz_wserver.c` — new libFuzzer harnesses; `make fuzz-build` includes both
- `fuzz/corpus/pg/`, `fuzz/corpus/wserver/` — seed corpora for handle bounds, overflow, type mismatch

### New Libraries

- **`std.wserver`** — Resilient HTTP server via libmicrohttpd. Thread-per-connection pool (`MHD_USE_THREAD_PER_CONNECTION`). Thread-safe request queue with mutex/condvar — designed for `std.flxthread` worker pattern. API: `serve`, `accept`, `req_method`, `req_path`, `req_body`, `req_header`, `reply`, `reply_json`, `reply_headers`, `connections`, `stop`, `version`. Dual-backend: libmicrohttpd when available via `pkg-config`, stub with clear error otherwise. Dep: `apt install libmicrohttpd-dev`.
- **`std.pg`** — PostgreSQL client via libpq. Full CRUD with parameterized queries (`query_params` with `$1`, `$2`, ...` — SQL injection safe). Typed accessors: `get`, `get_int`, `get_float`, `get_bool`, `is_null`. Connection health: `ping` (no full connect). Dual-backend: libpq when available, stub otherwise. Dep: `apt install libpq-dev`.

### Bug Fixes

- **`prst arr` double free** (`src/runtime.c`): `prst_pool_set` already deep-clones the array into `.value` and `.init_value`. The code immediately following overwrote `.init_value` with the original `arr` value — creating aliasing between the scope buffer and the pool entry. When both were freed (scope cleanup + pool cleanup), the same data buffer was freed twice. Fixed by removing the spurious overwrite — the clone already in place is correct.
- **`prst arr` handover** (consequence of above): With the double free resolved, Runtime B correctly initializes from the deserialized pool. Array values (`prst int arr readings[5]`) now survive `fluxa handover` identically to `prst int` and `prst float`.
- **`csv.load` OOM** (`src/std/csv/fluxa_std_csv.h`): `csv_read_chunk(fp, 1<<30, ...)` pre-allocated `cap = 1073741824` `Value` slots (~32 GB) before reading a single line, causing OOM on any call. Fixed by passing `chunk_size=0` as a sentinel for "read all" and updating the loop condition to `chunk_size <= 0 || lines_read < chunk_size`.

### Tests

- `tests/libs/wserver.sh` — 14 tests covering: import guard, cursor validation for all functions, stub/real detection, serve+stop round-trip, version string. 4 tests skipped (round-trip with curl) when libmicrohttpd not compiled in.
- `tests/libs/pg.sh` — 15 stub tests + 13 real-DB tests (activated via `FLUXA_PG_TEST_DSN`). Stub tests use `danger` for reliable error capture (stub has no `Runtime*` access for `rt_error`). Real-DB tests: connect/close, DDL/DML, query rows/cols, typed getters, `is_null`, `col_name`, `query_params`, bad query capture, `version`, `ping`.

### Docs

- `docs/STDLIB.md` — `std.wserver` and `std.pg` sections added with full function reference, examples, and design notes.
- `docs/fluxa_spec_v15.md` — stdlib catalogue updated; sprint 16 entry added to roadmap.
- `docs/CHANGELOG.md` — this entry.
- `README.md` — lib count updated (26 → 28); `std.wserver` and `std.pg` added to list and optional backends table.

---

## v0.15 — Module System


**Zero warnings. All tests pass. Bench regression: zero.**

### Module System

- `import live <name>` and `import static <name>` — multi-file project support.
- Modules are a **parse-time lens**, not a runtime concept. Zero changes to runtime.c, resolver.c, bytecode.c, or ast.h.
- Namespace mangling: `sensor.fn()` → `NODE_FUNC_CALL "sensor__fn"`. The runtime sees a single flat program.
- `Block` declarations in modules automatically namespaced: `Block Motor` in `live/devices.flx` → `devices__Motor`. `typeof devices.Motor` resolves correctly.
- `parser_parse_module()` — new public API. Called by `main.c` before parsing main source. Inserts module declarations first, in import order.
- Single-level imports only — modules cannot import other modules. Design decision: eliminates namespace hell and transitive dependencies. Complete dependency manifest visible at the top of `main.flx`.
- `[project] module_root` in `fluxa.toml` — configures base directory for module path resolution. Default: CWD.
- `-dev` watcher extended: watches main + all imported module files. File change in any module triggers reload.
- 15 module tests in `tests/modules/` covering: static pure functions, live Block state, prst fields, multiple simultaneous modules, namespace isolation, module_root toml, watcher reload.
- `ffi_list` test timeout increased 10s → 20s (flaky under load).

### Spec

- `docs/fluxa_spec_v13.md` renamed to `docs/fluxa_spec_v15.md`.
- §4.0 added: Scope and Ownership — no global variables, unique ownership principle.
- §9.1 updated: four import forms including live/static.
- §9.5 added: Module System — full specification, design rationale, single-level defense.
- §15 roadmap: Sprint 15 entry.
- §16: `[project] module_root` added to fluxa.toml reference.
- §17.6 / §17.7: `std.libv` and `std.libdsp` marked ✅ implemented (were incorrectly marked planned).

---

## v0.14 — Performance Sprint


**Zero warnings. 73/75 tests (2 system-dep: httpc, sqlite). All examples pass.**

### Bytecode VM — Phase 1: WarmProfile dynamic heap

- `WarmProfile` converted from a static `WarmFunc[256]` array embedded in
  the `Runtime` struct to a single contiguous heap-allocated block with
  power-of-2 growth via `realloc` at 75% fill. Starts at `warm_func_cap`
  slots (default 32 = 8.7 KB), grows automatically with no ceiling.
- `WARM_OBS_LIMIT` and cold-lock removed. Fluxa is strongly typed — types
  never change at runtime. Every function promotes after 2 stable WHT runs.
- `current_instance == NULL` gate removed — Block methods now enter the warm
  path and promote correctly.
- `warm_func_cap` in `fluxa.toml` is now an **initial** capacity (like
  `prst_cap`), not a ceiling.
- `FluxaConfig` struct reduced from ~1.4 MB to ~87 KB:
  `TOML_FFI_MAX` 32→8, `TOML_SIG_MAX` 64→32, string buffers tightened.
  This eliminates the stack overflow risk in deeply nested call chains.

### Bytecode VM — Phase 2: New opcodes

- **`OP_CALL_METHOD`** — Block methods compile to bytecode. Args passed
  directly from VM registers — zero malloc in the VM.
- **`OP_CALL_FUNC`** — Plain functions compile to bytecode.
- **`OP_GET_FIELD`** — Block field read directly into VM register.
  No scope traversal on the hot path.
- **`OP_SET_FIELD`** — Block field write from VM register to Block scope.
- `NODE_MEMBER_ACCESS` and `NODE_MEMBER_ASSIGN` now compile to bytecode.
- `vm_call_cb_t`, `vm_get_field_cb_t`, `vm_set_field_cb_t` — callbacks
  passed to `vm_run` bridge the VM back to the runtime C layer without
  circular dependencies.
- `vm_tick_cb_t` — called at every `OP_JUMP` back-edge for GC sweep +
  `flxthread` mailbox processing.

### Bytecode VM — Phase 3: Inline cache + compiled function bodies

- **Instance inline cache** (`resolve_inst_cached`): `OP_CALL_METHOD`,
  `OP_GET_FIELD`, `OP_SET_FIELD` patch their owner constant from
  `VAL_STRING("c1")` to `VAL_PTR(BlockInstance*)` on first call.
  All subsequent iterations deref the pointer directly — zero hash lookup.
- **`method_try_inline`**: Block methods whose body consists exclusively of
  `NODE_MEMBER_ASSIGN` with pure expressions execute inline — no
  `scope_new`/`scope_free`/frame save-restore per call.
- **`chunk_compile_fn`** + **`vm_run_fn`**: plain Fluxa functions with
  `return expr` now compile to a cached bytecode chunk (`fn_chunk` on the
  `ASTNode`). `vm_run_fn` executes the chunk with an isolated register file —
  no frame save-restore, no `eval()` per instruction.
- New opcodes: `OP_RETURN_VAL`, `OP_RETURN_NIL`.
- `fn_chunk` field added to `ASTNode` — compiled once, cached permanently.

### Sprint 10 — Hardware simulation + torture testing

- **`src/fluxa_alloc.h`** — hardware simulation memory allocator.
  `FLUXA_SIM_RP2040` caps heap at 264 KB; `FLUXA_SIM_ESP32` caps at 520 KB.
  Uses GCC `__atomic` builtins (C99-compatible). Allocations beyond the cap
  return NULL; the runtime reports OOM cleanly without crashing.
  No-sim build: zero-overhead aliases to `malloc`/`free`.
- `make build-sim-rp2040` / `make build-sim-esp32` — hardware-sim binaries.
- `make test-sim` — 10 tests across both platforms; part of `make test-all`.
- **Docker torture test** (`tests/torture/`) — binary compiled on the host
  (full CPU), injected into a container running at `cpus: 0.1`, `mem: 128 MB`,
  no swap. Simulates IoT runtime execution under resource starvation.
  `FLUXA_TORTURE=1` scales IPC test timeouts 5× automatically.
- `make test-torture` — separate from `test-all`; requires Docker.

### Performance benchmarks (on author's machine)

| Benchmark | v0.13.3 | v0.14 | Δ |
|---|---|---|---|
| `bench` — 10M loop (bytecode) | 0.161s | 0.160s | neutral |
| `bench_block` — 1M Block method calls | 0.497s | 0.460s | +7% |
| `bench_field` — 1M direct field rw | ~0.650s | **0.041s** | **+94%** |
| fn with return — 1M calls | ~0.486s | **~0.161s** | **+67%** |

### Bug fixes

- `vm_call_callback`: `print` and builtins dispatched via
  `builtin_dispatch_values` (pre-evaluated args, no ASTNode needed).
- `fluxa explain` segfault eliminated by `FluxaConfig` size reduction.
- `warm_profile_init` missing from `runtime_exec_explain` and
  `runtime_apply` — caused double-free on explain/apply paths.
- `ft_message_non_blocking`: `vm_tick_cb_t` drains the flxthread mailbox
  at every VM back-edge — threads no longer miss messages in compiled loops.
- Args aliasing: `args = &R[first_arg]` points into `rt->stack` — copied
  to local buffer before zeroing in `vm_call_callback`.
- `build-secure` missing `$(FLUXA_EXTRA_SRCS)` (mongoose.c) — fixed.
- Security tests: invalid semicolons in Fluxa programs replaced with
  valid newline separators; `RT_PID=0` initialization added to prevent
  `set -u` errors; `fluxa status <pid>` now accepts explicit pid argument.
- `iterate()` undefined in pagerank example: `vm_call_callback` now searches
  `current_instance->scope` for intra-Block function calls.
- `expr_is_inlinable` rejects `NODE_IDENTIFIER` with `warm_local=0` or
  `resolved_offset >= param_count` — prevents Block fields from being
  incorrectly treated as function parameters during inline.
- `chunk_compile_fn` tracks peak `next_reg` correctly across statement resets.

---

## v0.13.3 — Beta

**Zero warnings. 74/74 tests. 26 stdlib libs.**

### Fixes
- Zero compiler warnings policy restored.
- `tests/libs/httpc.sh`: Python 3 version fallback, server wait increased.

### New libs
- `std.graph` — 2D/3D graphics (stub + Raylib opt-in)
- `std.infer` — local LLM inference (stub + llama.cpp opt-in)
- `std.http` — HTTP server + client (mongoose 7.21, vendored)
- `std.mcp` — Fluxa as MCP server (JSON-RPC 2.0, mongoose)
- `std.websocket` — WebSocket client (pure C99 + libwebsockets opt-in)
- `std.zlib` — deflate, gzip, crc32, adler32
- `std.fs` — read, write, listdir, mkdir, copy, stat (POSIX)
- `std.https`, `std.mcps` — TLS-enforced variants
- `std.json2` — full DOM JSON

### Docs
- `docs/fluxa_spec_v13.md`, `docs/STDLIB.md`, `docs/CHANGELOG.md`,
  `docs/CREATING_LIBS.md`, `docs/FLUXA_DIS.md` — all updated for v0.13.3.

---

## v0.13.2 — std.http + std.mcp

- `std.http`: HTTP server + client via mongoose 7.21.
- `std.mcp`: Fluxa as MCP server. JSON-RPC 2.0.
- `FLUXA_EXTRA_SRCS` Makefile support for extra `.c` files.

---

## v0.12.x — Stdlib expansion

- Lib linker system: `FLUXA_LIB_EXPORT` macro + `gen_lib_registry.py` + `lib.mk`.
- `fluxa.libs` — build-time binary control.
- Libs: math, csv, json, strings, time, flxthread, crypto, pid, sqlite,
  serial, i2c, httpc, https, mqtt, mcpc, mcps, libv, libdsp.
- `FLUXA_SECURE`: Ed25519 signing, IPC HMAC, RESCUE_MODE.
- `fluxa init` scaffolds new projects.
- Docker Compose integration tests.

---

## v0.11.0 — Warm Path (WHT + QJL)

- WarmHotTable (WHT): function promotion after first execution.
- QuasiJIT Loop (QJL): bytecode VM for tight loops in warm functions.
- `fluxa dis` extended with warm forecast and bytecode output.

---

## v0.10.0 — GC, dyn, Block isolation

- Generational GC with configurable cap.
- `dyn` type: runtime-typed dynamic list.
- Block isolation: each Block instance owns its own scope.

---

## v0.9.0 — IPC server

- Unix socket IPC at `/tmp/fluxa-<pid>.sock`.
- Commands: observe, set, logs, status, explain.

---

## v0.8.0 — Atomic Handover

- 5-step protocol: Standby → Migrate → Dry Run → Switchover → Confirm.
- `HANDOVER_MODE_MEMORY` (x86) and `HANDOVER_MODE_FLASH` (RP2040).

---

## Earlier (v0.1–v0.7)

v0.7 — prst, hot reload, `fluxa apply`
v0.6 — FFI, arr heap, Block
v0.5 — `danger`, err_stack
v0.4 — prst_graph, type check
v0.3 — Blocks, methods
v0.2 — Functions, scope
v0.1 — Lexer, parser, runtime basics
