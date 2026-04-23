# Fluxa-lang Changelog

## v0.13.3 — Beta (current)

**Zero warnings. 74/74 tests. 26 stdlib libs.**

### Fixes
- Zero compiler warnings policy restored:
  - `std.graph` stub backend: unused variables in draw/input calls suppressed with `(void)` casts
  - `src/ipc_server.c`: format-truncation on `snap_path[256]` into `message[128]` — generic messages now, detail to stderr
  - `src/std/websocket`: format-truncation on `host+port_s` — simplified error message
- `tests/libs/httpc.sh`: Python 3 version fallback (`python3.11`, `python3.10`, etc.), server wait increased to 5s — fixes false FAIL on systems with non-standard Python name

### New libs
- `std.graph` — 2D/3D graphics. Stub (zero deps) + Raylib backend (`make FLUXA_GRAPH_RAYLIB=1`)
- `std.infer` — local LLM inference. Stub (zero deps) + llama.cpp backend (`make FLUXA_INFER_LLAMA=1`)
- `std.http` — HTTP server + client via mongoose 7.21 (vendored)
- `std.mcp` — Fluxa as MCP server (JSON-RPC 2.0, mongoose)
- `std.websocket` — WebSocket client. Pure C99 RFC6455 + libwebsockets opt-in
- `std.zlib` — deflate, gzip, crc32, adler32
- `std.fs` — read, write, listdir, mkdir, copy, stat (POSIX)
- `std.https`, `std.mcps` — TLS-enforced variants of httpc/mcpc
- `std.json2` — full DOM JSON, path navigation, typed getters

### Docs
- `docs/fluxa_spec_v13.md` — all sprints marked, section 20 (Huge Pages), section 19 (RUP)
- `docs/STDLIB.md`, `docs/CHANGELOG.md`, `docs/CREATING_LIBS.md`, `docs/FLUXA_DIS.md` — updated
- `README.md` — rewritten for v0.13.3
- All docs up to date for beta delivery

---

## v0.13.2 — std.http + std.mcp (mongoose 7.21)

- `std.http`: HTTP server (`serve`, `poll`, `reply`, `reply_json`) + client (`get`, `post`, `post_json`, `put`, `delete`, `status`, `body`, `ok`). mongoose 7.21 vendored.
- `std.mcp`: Fluxa as MCP server. JSON-RPC 2.0. Tools: `fluxa/observe`, `fluxa/set`, `fluxa/status`, `fluxa/logs`, `tools/list`. Connects to IPC socket.
- `ipc_req_set_str()` added to `fluxa_ipc.h`
- `FLUXA_EXTRA_SRCS` Makefile support for extra `.c` files (mongoose.c)
- Key fixes: `mg_url_host()` returns `mg_str` not `char*` (segfault on POST), request must be sent in `MG_EV_CONNECT` callback, flush with `mg_mgr_poll` after `mg_http_reply`

---

## v0.13.1 — std.websocket + Huge Pages (12.c)

- `std.websocket`: WebSocket client. Pure C99 RFC6455 backend (zero deps, `ws://`). libwebsockets backend (`make FLUXA_WS_LWS=1`, adds `wss://`). Same design pattern as FFTW/OpenBLAS.
- Bug fixed: `ws_b64enc` — `rem` variable was the decrementing loop counter instead of remaining bytes in current group → 16-byte key encoded to 12 chars instead of 24, breaking WebSocket handshake.
- `FLUXA_HUGEPAGES=1`: `madvise(MADV_HUGEPAGE)` on `ASTPool` arenas. Linux only, benchmark-gated.
- `std.zlib`, `std.fs`, `std.json2`, `std.https`, `std.mcps` added.
- `docs/fluxa_spec_v10.md` renamed to `fluxa_spec_v13.md`.

---

## v0.13.0 — Beta Milestone: Runtime Update Protocol (Sprint 13)

**All planned runtime changes complete. Runtime is stable.**

- `fluxa update <new_binary> [-p]`: replaces running binary with zero downtime.
- `IPC_OP_UPDATE` (0x07): prst serialized at safe point → `/tmp/fluxa-update-<pid>.snap` → `execve(new_binary)` with `FLUXA_RESTART_SNAPSHOT` env var.
- Security: UID check always enforced (not just FLUXA_SECURE), path traversal guard, generic errors to client.
- `_POSIX_C_SOURCE=200809L` added to Makefile CFLAGS.
- Spec section 19: Runtime Update Protocol.

---

## v0.12.x — Stdlib expansion (Sprints 12.b–12.f)

- Lib linker system: `FLUXA_LIB_EXPORT` macro + `gen_lib_registry.py` + `lib.mk` per lib. Zero core edits to add new libs.
- `fluxa.libs` — build-time binary control.
- Libs added: std.math, std.csv, std.json, std.strings, std.time, std.flxthread, std.crypto (libsodium), std.pid, std.sqlite, std.serial, std.i2c, std.httpc, std.https, std.mqtt, std.mcpc, std.mcps, std.libv (OpenBLAS opt-in), std.libdsp (FFTW3 opt-in).
- FLUXA_SECURE: script signing (Ed25519), IPC HMAC, flood detection (RESCUE_MODE).
- `fluxa init` scaffolds new projects.
- Docker Compose serial integration tests.

---

## v0.11.0 — Warm Path (WHT + QJL)

- WarmHotTable (WHT): function promotion to warm tier after first execution.
- QuasiJIT Loop (QJL): bytecode VM for tight loops in warm functions.
- `fluxa dis` extended with warm forecast, bytecode, call order, prst fork.
- Cold/warm/hot tier system with configurable budget.

---

## v0.10.0 — GC, dyn, Block isolation

- Generational GC with configurable cap.
- `dyn` type: runtime-typed dynamic list.
- Block isolation: each Block instance owns its own scope.
- `int arr` type enforcement at runtime.

---

## v0.9.0 — IPC server

- Unix socket IPC at `/tmp/fluxa-<pid>.sock` (mode 0600).
- Commands: observe, set, logs, status, explain.
- `fluxa_ipc.h` wire format.

---

## v0.8.0 — Atomic Handover

- 5-step protocol: Standby → Migrate → Dry Run → Switchover → Confirm.
- `HandoverSnapshotHeader` — flat binary format, Flash-safe for RP2040.
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
