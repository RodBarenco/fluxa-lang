# Fluxa-lang

**v0.13.3 — Beta** · Hobby language · Rio de Janeiro, Brazil

Fluxa is a statically-typed, C99-embedded scripting language designed for IoT and embedded systems (RP2040, ESP32). The runtime is now feature-complete and stable. 26 standard library modules available.

---

## Why Fluxa

Four concrete capabilities drive the design:

**1. Hot reload without downtime**
Swap a running program's logic while all `prst` (persistent) variables survive. No restart, no lost state.

```bash
fluxa run main.flx -prod      # start in production mode
fluxa apply new_main.flx      # swap logic, state survives
```

**2. Atomic Handover (5-step protocol)**
Standby → Migrate → Dry Run → Switchover → Confirm. Runtime A stays active through steps 1–3. The state gap is measured in microseconds.

**3. Runtime Update Protocol**
Replace the `./fluxa` binary itself with zero downtime. `prst` state survives the binary swap via snapshot + `execve`.

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
fluxa run main.flx -dev
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

// danger — explicit error containment block
danger {
    dyn r = httpc.get("http://api.example.com/temp")
    int s = httpc.status(r)
    str b = httpc.body(r)
    print(s)
}
if err != nil { print(err[0]) }

// Blocks — lightweight objects
Block Sensor {
    float temp = 0.0
    fn read() { temp = ffi.read_adc() }
}
Sensor s
s.read()
print(s.temp)
```

---

## Standard library — 26 libs

```toml
# fluxa.toml — runtime selection
[libs]
std.math       = "1.0"   # trig, sqrt, pow, log, clamp
std.csv        = "1.0"   # CSV streaming parser
std.json       = "1.0"   # JSON streaming (no DOM)
std.json2      = "1.0"   # JSON full DOM — path nav, typed getters
std.strings    = "1.0"   # split, join, trim, find
std.time       = "1.0"   # sleep, now_ms, elapsed
std.fs         = "1.0"   # read, write, listdir, mkdir (POSIX)
std.zlib       = "1.0"   # deflate, gzip, crc32, adler32
std.flxthread  = "1.0"   # native concurrency (pthreads)
std.pid        = "1.0"   # PID controller (IoT/robotics)
std.libv       = "1.0"   # vectors, matrices, tensors (GLM-style)
std.libdsp     = "1.0"   # FFT, STFT, FIR/IIR, CFAR, range-Doppler
std.crypto     = "1.0"   # BLAKE2b, XSalsa20, Ed25519 (libsodium)
std.sqlite     = "1.0"   # embedded SQL
std.serial     = "1.0"   # UART/serial (libserialport)
std.i2c        = "1.0"   # I2C protocol (Linux)
std.httpc      = "1.0"   # HTTP client, plain (libcurl)
std.https      = "1.0"   # HTTPS client, TLS enforced (libcurl)
std.mqtt       = "1.0"   # MQTT client (libmosquitto)
std.mcpc       = "1.0"   # MCP client, plain (libcurl)
std.mcps       = "1.0"   # MCP client, HTTPS enforced (libcurl)
std.websocket  = "1.0"   # WebSocket client (native RFC6455 or libwebsockets)
std.http       = "1.0"   # HTTP server + client (mongoose 7.21)
std.mcp        = "1.0"   # Fluxa as MCP server, JSON-RPC 2.0 (mongoose)
std.graph      = "1.0"   # 2D/3D graphics + input (stub or Raylib)
std.infer      = "1.0"   # local LLM inference (stub or llama.cpp)
```

**Optional backends** (compiled in when available, same API):

| Lib | Default | Opt-in backend |
|---|---|---|
| `std.websocket` | pure C99 RFC6455, `ws://` | `make FLUXA_WS_LWS=1` → libwebsockets + TLS |
| `std.libdsp` | pure C99 FFT | `[libs.libdsp] backend = "fftw"` → FFTW3 |
| `std.libv` | pure C99 linear algebra | `[libs.libv] backend = "blas"` → OpenBLAS |
| `std.graph` | stub (no-op, zero deps) | `make FLUXA_GRAPH_RAYLIB=1` → Raylib |
| `std.infer` | stub (placeholder output) | `make FLUXA_INFER_LLAMA=1` → llama.cpp |

---

## Build-time control

`fluxa.libs` controls which libs enter the binary — critical for RP2040/ESP32 where binary size matters:

```toml
# fluxa.libs — build-time enable/disable (run: make build after changes)
[libs.build]
std.math      = true    # trig, sqrt, pow, log, clamp         (none)
std.graph     = true    # 2D/3D graphics                       (none) [Raylib optional]
std.sqlite    = false   # disabled — saves ~500KB on embedded
```

---

## CLI

```
fluxa run <file.flx>              run script (auto-detects mode)
fluxa run <file.flx> -dev         dev mode: watch + auto-reload on save
fluxa run <file.flx> -prod        prod mode: manual reload via IPC
fluxa run <file.flx> -p           preflight: parse + resolve only
fluxa apply <file.flx>            hot reload preserving prst state
fluxa apply <file.flx> -p         preflight before applying
fluxa handover <old> <new>        Atomic Handover (5-step protocol)
fluxa update <new_binary>         Runtime Update Protocol
fluxa update <new_binary> -p      preflight binary before sending
fluxa observe <var>               stream prst variable live (IPC)
fluxa set <var> <value>           mutate prst variable in live runtime
fluxa status                      runtime health snapshot
fluxa logs                        last error entries
fluxa explain                     all prst vars + dependency graph
fluxa init <project>              scaffold new project
fluxa dis <file.flx>              static analysis → .dis report
fluxa keygen                      generate Ed25519 + HMAC keys (FLUXA_SECURE)
```

---

## Security (FLUXA_SECURE)

```bash
make FLUXA_SECURE=1 build     # hardened build
```

Enables: script signing (Ed25519), IPC HMAC authentication, flood detection (RESCUE_MODE), connection rate limiting. All `fluxa update` calls require a `.sig` file alongside the new binary. See `docs/fluxa_spec_v13.md` §18 and `tests/security/README.md`.

---

## Testing

```bash
make test-all                              # full suite (74 tests)
FLUXA_TEST_HTTP=1  make test-all           # include live HTTP tests
FLUXA_TEST_MQTT=1  make test-all           # include live MQTT tests
FLUXA_TEST_I2C=1   make test-all           # include real I2C hardware
FLUXA_TEST_SERIAL=1 make test-all          # include real serial hardware
FLUXA_TEST_UPDATE=1 bash tests/sprint13/update_protocol.sh  # full update roundtrip
bash tests/integration/run_all.sh          # integration scenario tests
bash tests/integration/serial/run.sh       # Docker serial IO (tty0tty)
```

---

## Architecture

```
fluxa/
├── src/
│   ├── main.c           — CLI, command dispatch
│   ├── lexer.c          — tokenizer
│   ├── parser.c         — recursive descent parser
│   ├── resolver.c       — symbol resolution, scope offsets
│   ├── runtime.c        — tree-walking interpreter
│   ├── handover.c       — Atomic Handover (5-step protocol)
│   ├── ipc_server.c     — Unix socket IPC server (opcodes 0x01–0x07)
│   ├── prst_pool.h      — persistent variable pool + serialization
│   ├── scope.h          — value types, FluxaArr, FluxaDyn
│   ├── fluxa_ipc.h      — IPC wire format + request builders
│   ├── pool.h           — ASTPool arena (FLUXA_HUGEPAGES opt-in)
│   └── std/             — 26 standard library modules
│       ├── math/  csv/  json/  json2/  strings/  time/
│       ├── fs/    zlib/  flxthread/  pid/  libv/  libdsp/
│       ├── crypto/  sqlite/  serial/  i2c/
│       ├── httpc/  https/  mqtt/  mcpc/  mcps/
│       ├── websocket/  http/  mcp/
│       ├── graph/       — 2D/3D (stub + Raylib)
│       └── infer/       — LLM inference (stub + llama.cpp)
├── tests/
│   ├── run_tests.sh     — master runner (74 tests)
│   ├── libs/            — one test script per stdlib lib
│   ├── sprint13/        — Runtime Update Protocol tests
│   ├── suite2/          — edge cases + integration (IoT scenarios)
│   ├── security/        — FLUXA_SECURE hardening tests
│   └── integration/     — Atomic Handover + serial Docker tests
├── vendor/
│   ├── mongoose.h       — mongoose 7.21 (vendored)
│   └── mongoose.c
├── docs/
│   ├── fluxa_spec_v13.md  — language specification (v0.13.3)
│   ├── STDLIB.md          — standard library reference
│   ├── CHANGELOG.md       — version history
│   ├── CREATING_LIBS.md   — guide for adding new libs
│   └── FLUXA_DIS.md       — disassembler reference
├── fluxa.libs           — build-time library enable/disable
└── Makefile
```

---

## Status

**Beta — v0.13.3**

| Component | Status |
|---|---|
| Language (lexer, parser, resolver) | ✅ stable |
| Runtime + GC | ✅ stable |
| Hot reload (`fluxa apply`) | ✅ stable |
| Atomic Handover (5-step) | ✅ stable |
| IPC server (7 opcodes) | ✅ stable |
| Prod mode + FLUXA_SECURE | ✅ stable |
| Runtime Update Protocol | ✅ stable |
| Standard library (26 libs) | ✅ stable |
| Huge Pages (`FLUXA_HUGEPAGES=1`) | ✅ opt-in |
