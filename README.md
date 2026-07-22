# Fluxa-lang

**v0.19.2 — Beta** · Hobby language · Rio de Janeiro, Brazil

Fluxa is a statically-typed, C99-embedded scripting language designed for IoT and embedded systems (RP2040, ESP32). Feature-complete and stable. 29 standard library modules. Module system for multi-file projects. Three-tier execution: AST tree-walker → warm bytecode VM → compiled function bodies.

---

## Why Fluxa

Four concrete capabilities drive the design:

**1. Hot reload without downtime**
Swap a running program's logic while all `prst` (persistent) variables survive.

```bash
fluxa run main.flx -prod      # start in production mode
fluxa apply new_main.flx      # swap logic, state survives
```

**2. Atomic Handover (5-step protocol)**
Standby → Migrate → Dry Run → Switchover → Confirm. Runtime A stays active through steps 1–3. State gap is measured in microseconds.

**3. Runtime Update Protocol**
Replace the `./fluxa` binary itself with zero downtime. `prst` state survives via snapshot + `execve`.

```bash
fluxa update ./fluxa_v2       # replace binary, state preserved
fluxa update ./fluxa_v2 -p    # preflight verify before sending
```

**4. Embeddable, minimal**
Pure C99. Configurable via `fluxa.toml`. Cross-compiles to Linux, macOS, RP2040, ESP32.

---

## Quick start

```bash
make build
fluxa init myproject
cd myproject
fluxa run main.flx -dev      # watch + auto-reload
```

---

## Language

```fluxa
// Types: int, float, bool, str, arr, dyn, nil
int x = 10
float pi = 3.14159
str name = "fluxa"

// prst — survives hot reloads and binary updates
prst int counter = 0
counter = counter + 1

// danger — explicit error containment
int conn = 0
danger {
    conn = pg.connect("host=localhost dbname=mydb user=fluxa password=secret")
}
if err != nil { print(err[0]) }

// Block — encapsulated state + behavior
// Fields can be prst (survives reload) or plain (lives for execution)
Block Sensor {
    prst float reading = 0.0   // survives reload
    float   last_raw   = 0.0   // does not survive reload — also valid
    fn set(float v) nil { reading = v; last_raw = v }
    fn get() float { return reading }
}
Block s typeof Sensor
s.set(22.5)
print(s.get())

// Modules — organize code across files
import live sensor       // loads live/sensor.flx
import static utils      // loads static/utils.flx
Block s2 typeof sensor.Sensor
s2.set(3.14)
print(utils.double(7))
```

**Key rules — read this before writing Fluxa code:**

- No global variables. Everything has exactly one owner.
- No `dyn` as a Block field. Pass `dyn` cursors as arguments to methods.
- No `danger` inside Block methods. Handle fallible operations in functions.
- No Block declaration inside a Block.
- `prst` only marks variables for persistence across reloads — it does not change ownership.
- Long-running workers should `free()` intermediate strings built by `strings.concat` chains. Everything else (variable reassignment, lib literal args, comparison literals, `dyn` cursors, end-of-scope) is auto-released by the runtime. See [FLUXA_GUIDE §12.5](docs/FLUXA_GUIDE.md#125-memory-in-long-running-loops).

---

## Execution model (v0.14)

Three-tier system. Every function starts cold and promotes automatically:

```
Tier 0 — AST tree-walker      all functions, first 2 calls
Tier 1 — Warm bytecode VM     promoted after 2 stable WHT runs
           ├─ OP_GET_FIELD / OP_SET_FIELD  — Block field access (inline cache)
           ├─ OP_CALL_METHOD               — Block method calls
           └─ OP_CALL_FUNC                 — plain function calls
Tier 2 — Compiled fn bodies   vm_run_fn with isolated register file
           └─ fn with return expr compiled to OP_RETURN_VAL chunk
```

---

## Standard library — 31 libs

```toml
# fluxa.toml — runtime selection
[libs]
std.math       = "1.0"   # trig, sqrt, pow, log, clamp
std.csv        = "1.0"   # CSV streaming parser
std.json       = "1.0"   # JSON streaming (no DOM)
std.json2      = "1.0"   # JSON full DOM — path nav, typed getters
std.strings    = "1.0"   # split, join, trim, find
std.cache      = "1.0"   # sharded k/v cache + bump-pointer arena
std.time       = "1.0"   # sleep, now_ms, elapsed
std.fs         = "1.0"   # read, write, listdir, mkdir (POSIX)
std.zlib       = "1.0"   # deflate, gzip, crc32, adler32
std.flxthread  = "1.0"   # native concurrency (pthreads)
std.pid        = "1.0"   # PID controller (IoT/robotics)
std.libv       = "1.0"   # vectors, matrices, tensors
std.libdsp     = "1.0"   # FFT, STFT, FIR/IIR filters
std.crypto     = "1.0"   # BLAKE2b, XSalsa20, Ed25519 (libsodium)
std.sqlite     = "1.0"   # embedded SQL
std.serial     = "1.0"   # UART/serial (libserialport)
std.i2c        = "1.0"   # I2C protocol (Linux)
std.httpc      = "1.0"   # HTTP client (libcurl)
std.https      = "1.0"   # HTTPS client, TLS enforced (libcurl)
std.mqtt       = "1.0"   # MQTT client (libmosquitto)
std.mcpc       = "1.0"   # MCP client (libcurl)
std.mcps       = "1.0"   # MCP client, HTTPS enforced (libcurl)
std.websocket  = "1.0"   # WebSocket client (pure C99 or libwebsockets)
std.http       = "1.0"   # HTTP server + client (mongoose 7.21, dyn cursors)
std.mcp        = "1.0"   # Fluxa as MCP server (JSON-RPC 2.0)
std.graph      = "1.0"   # 2D/3D graphics (stub or Raylib)
std.infer      = "1.0"   # local LLM inference (stub or llama.cpp)
std.wserver    = "1.0"   # resilient HTTP server — int handles, auto-scaling pool
std.pg         = "1.0"   # PostgreSQL client — int handles (libpq)
std.sound      = "1.0"   # audio playback + tone — wav/mp3/flac (stub or miniaudio)
std.image      = "1.0"   # PNG/JPG/BMP/TGA/QOI encode/decode (stub or Raylib codec)
```

**Optional backends:**

| Lib | Default | Opt-in |
|---|---|---|
| `std.websocket` | pure C99 RFC6455 | `make FLUXA_WS_LWS=1` → libwebsockets |
| `std.libdsp` | pure C99 FFT | `[libs.libdsp] backend = "fftw"` → FFTW3 |
| `std.libv` | pure C99 linear algebra | `[libs.libv] backend = "blas"` → OpenBLAS |
| `std.graph` | stub (zero deps) | `make FLUXA_GRAPH_RAYLIB=1` → Raylib |
| `std.infer` | stub (placeholder) | `make FLUXA_INFER_LLAMA=1` → llama.cpp |
| `std.wserver` | stub (clear error) | `apt install libmicrohttpd-dev` → real backend |
| `std.pg` | stub (clear error) | `apt install libpq-dev` → real backend |
| `std.sound` | stub (silent no-op) | `make FLUXA_SOUND_MINIAUDIO=1` → miniaudio |
| `std.image` | stub (transforms only) | `make FLUXA_IMAGE_RAYLIB=1` → Raylib codec |

---

## Build targets

```bash
make build                    # standard binary
make build-secure             # FLUXA_SECURE=1 — Ed25519 + IPC HMAC
make build-sim-rp2040         # SRAM cap 264 KB (RP2040 simulation)
make build-sim-esp32          # SRAM cap 520 KB (ESP32 simulation)
```

---

## CLI

```
fluxa run <file.flx>              run script
fluxa run <file.flx> -dev         dev mode: watch + auto-reload
fluxa run <file.flx> -prod        prod mode: manual reload via IPC
fluxa run <file.flx> -p           preflight: parse + resolve only
fluxa apply <file.flx>            hot reload preserving prst state
fluxa handover <old> <new>        Atomic Handover (5-step protocol)
fluxa update <new_binary>         Runtime Update Protocol
fluxa observe <var>               stream prst variable live
fluxa set <var> <value>           mutate prst variable in live runtime
fluxa status [<pid>]              runtime health snapshot
fluxa logs                        last error entries
fluxa explain                     prst vars + dependency graph
fluxa init <project>              scaffold new project
fluxa dis <file.flx>              static analysis → .dis report
fluxa keygen                      generate Ed25519 + HMAC keys
```

---

## Testing

```bash
make test-all                    # full suite: unit + suite2 + integration + sim
make test-torture                # Docker: runtime under 0.1 CPU + 128 MB RAM
make test-integration            # Atomic Handover scenarios
make test-integration-pg         # PostgreSQL integration (requires Docker)
make test-sim                    # hardware simulation (RP2040 + ESP32)
make fuzz-build                  # build all libFuzzer harnesses
make bench                       # benchmark: loop, Block methods, field access
```

---

## Architecture

```
fluxa/
├── src/
│   ├── main.c             — CLI, command dispatch
│   ├── lexer.c            — tokenizer
│   ├── parser.c           — recursive descent parser
│   ├── resolver.c         — symbol resolution, scope offsets
│   ├── runtime.c          — tree-walker + bytecode VM orchestration
│   ├── bytecode.c         — VM compiler + vm_run + vm_run_fn
│   ├── handover.c         — Atomic Handover (5-step protocol)
│   ├── ipc_server.c       — Unix socket IPC server
│   ├── fluxa_alloc.h      — hardware simulation allocator
│   ├── warm_profile.h     — WarmProfile: dynamic heap, WHT + QJL
│   ├── prst_pool.h        — persistent variable pool + serialization
│   ├── scope.h            — value types, FluxaArr, FluxaDyn
│   └── std/               — 28 standard library modules
├── tests/
│   ├── run_tests.sh       — master runner
│   ├── libs/              — one script per stdlib lib
│   ├── suite2/            — edge cases + integration
│   ├── security/          — FLUXA_SECURE hardening tests
│   ├── integration/       — Atomic Handover + serial + pg Docker tests
│   ├── torture/           — IoT runtime simulation (Docker)
│   └── fuzz/              — libFuzzer harnesses (pg, wserver, json, csv, ...)
├── docs/
│   ├── fluxa_spec_v15.md  — language specification
│   ├── STDLIB.md          — standard library reference
│   ├── FLUXA_GUIDE.md     — step-by-step programming guide
│   ├── CHANGELOG.md       — version history
│   ├── CREATING_LIBS.md   — guide for adding libs
│   └── FLUXA_DIS.md       — disassembler reference
├── vendor/
│   ├── mongoose.h/.c      — mongoose 7.21 (vendored)
│   └── uthash.h           — hash table (vendored)
└── Makefile
```

---

## Status — v0.16

| Component | Status |
|---|---|
| Language (lexer, parser, resolver) | ✅ stable |
| Runtime + GC | ✅ stable |
| Bytecode VM (Phase 1–3) | ✅ stable |
| Hot reload (`fluxa apply`) | ✅ stable |
| Atomic Handover (5-step) | ✅ stable |
| IPC server | ✅ stable |
| Prod mode + FLUXA_SECURE | ✅ stable |
| Runtime Update Protocol | ✅ stable |
| Standard library (31 libs) | ✅ stable |
| Module system (import live/static) | ✅ stable |
| Hardware simulation (RP2040/ESP32) | ✅ stable |
| Docker torture testing | ✅ stable |
| Huge Pages (`FLUXA_HUGEPAGES=1`) | ✅ opt-in |
