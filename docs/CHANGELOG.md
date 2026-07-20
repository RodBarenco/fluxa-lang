# Fluxa-lang Changelog

## v0.22.3 — std.graph: fullscreen toggle (current)

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

## v0.20.0 — exact KNN index (VKN3) + wserver TCP_NODELAY

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
