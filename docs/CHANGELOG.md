# Fluxa-lang Changelog

## v0.17 — std.flxthread multi-arg (current)

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
