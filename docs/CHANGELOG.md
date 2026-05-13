# Fluxa-lang Changelog

## v0.15 — Module System (current)

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
