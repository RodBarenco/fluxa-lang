# Fluxa-lang

This is the main source code repository for the Fluxa-lang programming language. It contains the interpreter, core toolchain, standard library, and base definitions.

---

## Why Fluxa-lang?

Fluxa-lang is in active development. The architecture is built around four concrete capabilities: embedding in constrained hardware, predictable error behavior, persistent state across hot reloads, and zero-downtime runtime replacement.

### Embeddability

Written in pure C99 with minimal dependencies. The interpreter is designed to embed into larger host programs or run bare-metal on microcontrollers. Memory is managed via ASTPool arenas — zero fragmentation during parsing, predictable teardown per cycle. Runtime behavior is configurable through `fluxa.toml`. Cross-compilation targets RP2040, ESP32, and generic ARM Cortex-M are built into the Makefile.

### Predictability

Fluxa enforces a strict fail-fast model. Silent errors are a design non-goal. The `danger` block explicitly contains FFI calls and runtime risks — errors accumulate in a fixed-size `err` ring buffer without crashing the host VM. Outside `danger`, any error aborts execution immediately with a precise line number. Type errors, array bounds violations, and domain errors (e.g. `sqrt(-1)`) are all reported at the exact call site.

### Persistent State and Hot Reload

The `prst` keyword marks variables that survive a file reload. The runtime tracks inter-variable dependencies via `PrstGraph` and serializes live values via `PrstPool`. In `fluxa run -dev` mode, the interpreter watches the source file for changes and reloads automatically while preserving all `prst` state. Arrays declared with `prst` survive reloads element-by-element — mutations made at runtime are preserved across handovers.

### Atomic Handover

The Atomic Handover protocol allows zero-downtime replacement of a running Fluxa program. The new version runs a Dry Run (Ciclo Imaginário) in parallel with the live runtime — if it passes, state is migrated and control is handed over atomically. If the Dry Run fails, Runtime A keeps control and the error is reported. This makes Fluxa suitable for long-running IoT processes and robotic controllers that cannot afford downtime.

### Prototypal Modularity

State and behavior are grouped in `Block` structures. Each `typeof` instance is fully isolated — no shared state, no hidden coupling, no locks. Block instances can be stored in `dyn` arrays, cloned independently, and passed to functions.

### C Interoperability

Built-in FFI via `libffi` allows calling compiled C libraries directly from Fluxa scripts inside `danger` blocks. The standard library (`std.math`, `std.csv`, `std.json`, `std.strings`) provides safe wrappers callable outside `danger`.

---

## Core Design Principles

Every language decision in Fluxa is anchored to at least one of these five principles:

| Principle | What it means in practice |
|---|---|
| **Explicit > Implicit** | `prst` is declared, `import std math` is declared, `danger` is declared. Nothing happens behind the scenes. |
| **Simple > Complete** | JSON is a `str`. CSV rows are `str`. No parse trees, no intermediate objects. |
| **Dynamic Runtime > Heavy Compilation** | Hot reload, Atomic Handover, and live state migration happen at runtime without recompilation. |
| **Local Control > Global Magic** | Buffer sizes, GC caps, prst pool capacity, and library opt-in are all explicit in `fluxa.toml`. |
| **Visible Error > Silent Error** | No null coercion, no silent type widening, no swallowed exceptions outside `danger`. |

---

## Where Does It Come From?

Fluxa-lang is designed and implemented in Rio de Janeiro, Brazil. It was born from a practical challenge in document processing and data analysis: the rules for calculations change constantly. In those environments, halting the system and reinstantiating a complex web of states just to update a single formula is a frustrating and non-trivial bottleneck.

Driven by curiosity and a "why not?" approach to memory management, Fluxa was built to let developers hot-swap core logic on the fly while keeping the data and the engine running continuously.

---

## What's in a Name?

"Fluxa-lang" refers to continuous flow. As a proper noun, it should be written with an initial capital: **Fluxa-lang**. Please avoid writing it as `FLUXA-LANG` — it is not an acronym.

---

## Quick Start

Fluxa-lang is built directly from source. You need a C99 compiler (GCC or Clang), `make`, and optionally `libffi` for C interop.

```bash
git clone https://github.com/RodBarenco/fluxa-lang.git
cd fluxa-lang
make
./fluxa run tests/hello.flx
```

To install `libffi` (recommended):

```bash
# Ubuntu / Debian
sudo apt install libffi-dev

# macOS (Homebrew)
brew install libffi
```

---

## Language Overview

```fluxa
// Variables — statically typed at declaration
int    count = 0
float  temp  = 23.5
str    label = "sensor_01"
bool   active = true

// Persistent state — survives hot reload and Atomic Handover
prst int tick = 0
tick = tick + 1

// Persistent array — element mutations survive handover
prst float arr readings[8] = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
readings[0] = 23.5

// Dynamic heterogeneous array
dyn events = ["startup", 42, true]
events[3] = "new_event"

// for..in works on both arr and dyn
for item in events {
    print(item)
}

// Block — prototypal struct with methods
Block PIDController {
    prst float kp = 2.0
    prst float ki = 0.1
    prst float integral = 0.0
    fn update(float error) float {
        integral = integral + error
        return kp * error + ki * integral
    }
}

// typeof — independent instance, no shared state
Block pid typeof PIDController
pid.kp = 3.0
float output = pid.update(5.0)

// danger — contains FFI and risky operations
danger {
    float bad = 1.0 / 0.0
}
// execution continues; err[0] has the message

// Standard library (opt-in via fluxa.toml)
import std math
import std csv
import std json
import std strings

float hyp = math.hypot(3.0, 4.0)          // 5.0
bool  ok  = math.approx(0.1 + 0.2, 0.3)  // true

dyn parts = strings.split("a,b,c", ",")
str upper = strings.upper("hello")

str obj = json.object()
obj = json.set(obj, "temp", json.from_float(23.5))
float t = json.get_float(obj, "temp")
```

---

## Standard Library

All standard libraries are opt-in via `fluxa.toml`. They compile into the binary but require explicit declaration to use at runtime.

```toml
[libs]
std.math    = "1.0"
std.csv     = "1.0"
std.json    = "1.0"
std.strings = "1.0"
```

| Library | Functions | Notes |
|---|---|---|
| `std.math` | 39 functions — sqrt, pow, sin/cos/tan, log, clamp, approx, ... | No `danger` required |
| `std.csv` | open/next/close cursor, chunk, load, save, field, field_count, skip | File I/O requires `danger` |
| `std.json` | object, set, get_*, has, parse_array, stringify, load, cursor | No `danger` for pure string ops |
| `std.strings` | split, join, slice, trim, find, replace, upper, lower, count, ... | No `danger` required |

Full documentation: [`STDLIB.md`](STDLIB.md)

---

## Embedded Targets

Fluxa-lang runs on Linux and macOS natively, and cross-compiles to embedded hardware:

| Target | Toolchain | Memory | Notes |
|---|---|---|---|
| RP2040 (Raspberry Pi Pico) | `arm-none-eabi-gcc` | 264 KB SRAM | No IPC, no libffi, HANDOVER_MODE_FLASH |
| ESP32 (Xtensa lx6) | `xtensa-esp32-elf-gcc` | 520 KB SRAM | Hardware FPU, no IPC |
| Generic Cortex-M | `arm-none-eabi-gcc` | configurable | M0+, M4, M7 presets in Makefile |

```bash
make build-rp2040    # produces .o files for pico-sdk integration
make build-esp32     # produces .o files for esp-idf integration
make build-embedded  # attempts all targets, skips missing toolchains
```

---

## Testing

```bash
make test-runner          # 52-case automated PASS/FAIL suite (CI-friendly)
make test-suite2          # 70-case edge cases: prst, handover, GC, dyn, Block, embedded
make test-libs            # stdlib tests: math (15), csv (10), json (10), strings (15)
make test-all             # everything: unit + suite2 + libs + integration
make examples             # run all example programs (sort, BST, Dijkstra, PageRank, ...)
make bench                # performance benchmarks
```

Test structure:

```
tests/
  run_tests.sh               Main runner — auto-discovers tests/libs/
  sprint10b_core_fixes.sh    for..in dyn + prst arr handover
  libs/
    math.sh                  std.math tests (15 cases)
    csv.sh                   std.csv tests (10 cases)
    json.sh                  std.json tests (10 cases)
    strings.sh               std.strings tests (15 cases)
  suite2/
    run_suite2.sh            Edge case master runner
    s2_prst.sh               Persistent state edge cases
    s2_handover.sh           Atomic Handover protocol
    s2_gc.sh                 Garbage collector
    s2_dyn.sh                Dynamic array extremes
    s2_block.sh              Block isolation
    s2_types_danger.sh       Type enforcement + danger
    s2_embedded.sh           Memory-constrained scenarios
  integration/
    scenario1/               Normal IoT handover simulation
    scenario2/               Fault injection (kill at each step)
```

---

## Project Layout

```
fluxa-lang/
  src/
    main.c              CLI entry point
    lexer.c/h           Tokenizer
    parser.c/h          Recursive descent parser
    ast.h               AST node definitions
    resolver.c/h        Symbol resolution and offset assignment
    runtime.c/h         Tree-walk interpreter and VM
    bytecode.c/h        Register-based bytecode for hot paths
    builtins.c/h        Built-in functions (print, len, typeof, ...)
    block.c/h           Block instantiation and cloning
    scope.c/h           Variable scope (uthash-based)
    prst_pool.h         Persistent state serialization/deserialization
    prst_graph.h        PrstGraph dependency tracker
    handover.c/h        Atomic Handover protocol
    gc.h                GC pin/unpin (open-addressing table)
    ffi.c/h             libffi C interop
    ipc_server.c/h      Unix socket IPC for live inspection
    toml_config.h       fluxa.toml parser
    err.h               Error ring buffer
    std/
      math/             std.math implementation
      csv/              std.csv implementation
      json/             std.json implementation
      str/              std.strings implementation
  tests/                Test suite
  examples/             Example programs
  vendor/               uthash (single-header hash table)
  fluxa.toml            Project configuration
  STDLIB.md             Standard library documentation
  Makefile
```

---

## `fluxa.toml` Configuration

```toml
[project]
name  = "my_project"
entry = "main.flx"

[runtime]
gc_cap         = 256   # GC table capacity (default 256)
prst_cap       = 32    # PrstPool initial capacity
prst_graph_cap = 64    # PrstGraph capacity

[libs]
std.math    = "1.0"
std.csv     = "1.0"
std.json    = "1.0"
std.strings = "1.0"

[libs.csv]
max_line_bytes = 1024
max_fields     = 64

[libs.json]
max_str_bytes = 4096

[ffi]
libm = "auto"          # resolved via dlopen at runtime

[ffi.libm.signatures]
cos = { ret = "double", params = ["double"] }
sin = { ret = "double", params = ["double"] }
```

---

## CLI Reference

```bash
fluxa run <file.flx>                  Run a script
fluxa run <file.flx> -proj <dir>      Run as project (enables prst)
fluxa run <file.flx> -dev             Watch mode — reload on file change
fluxa run <file.flx> -prod            Production mode — IPC server active
fluxa handover <old.flx> <new.flx>    Execute Atomic Handover
fluxa explain <file.flx>              Show prst vars, Block deps, GC state
fluxa apply <key>=<value>             Send IPC set command to live runtime
fluxa observe                         Stream live prst state from running process
fluxa logs                            Stream runtime log output
fluxa status                          Show live runtime status
```

---

## Building

```bash
make              # native binary → ./fluxa
make build-asan   # AddressSanitizer build (development only)
make clean        # remove all build artifacts
```

The Makefile auto-detects `libffi` via `pkg-config`. If unavailable, the build proceeds without FFI support (`FLUXA_HAS_FFI=0`).

---

## License

Fluxa-lang is open source. See `LICENSE` for details.
