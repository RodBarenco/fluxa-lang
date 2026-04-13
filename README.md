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

The `prst` keyword marks variables that survive a file reload. The runtime tracks inter-variable dependencies via `PrstGraph` and serializes live values via `PrstPool`. In `fluxa run -dev` mode, the interpreter watches the source file for changes and reloads automatically while preserving all `prst` state.

### Atomic Handover

The Atomic Handover protocol allows zero-downtime replacement of a running Fluxa program. The new version runs a Dry Run in parallel with the live runtime — if it passes, state is migrated and control is handed over atomically. If the Dry Run fails, the original runtime keeps control and the error is reported.

### C Interoperability

Built-in FFI via `libffi` calls compiled C libraries directly from Fluxa scripts inside `danger` blocks. The runtime reads C signatures from `fluxa.toml` and handles all pointer marshalling automatically: `int*` / `double*` / `bool*` parameters are passed by address and written back, `char*` output buffers are allocated internally and copied back into `str` variables, and `uint8_t*` buffers are flattened from `int arr` and scattered back after the call. The user writes plain Fluxa types and the runtime does the rest.

### Three-Tier Execution

The Fluxa runtime selects the fastest execution path per function automatically:

**Cold** — full AST walker on the first few calls. The resolver already marks function-local variables with `warm_local`, skipping the `prst_pool_has` scan even here.

**Warm** — after 2 stable executions, the runtime promotes the function. Variable reads touch a 1-byte `WarmSlot` (type tag + QJL guard) and the stack slot directly — 9 bytes total, versus 18–2000+ bytes in cold mode depending on how many `prst` variables the program has. A Walsh-Hadamard Transform (WHT) signature detects type instability; a 1-bit QJL residual demotes the function back to cold if types diverge.

**Hot** — the bytecode VM handles `while` and `if` loops deterministically, unchanged.

Memory touched per variable read in a function with 20 `prst` variables: **418 bytes cold → 9 bytes warm**. On RP2040 (264 KB SRAM, no cache) this directly reduces SRAM accesses from 7–8 to 2 per variable read. Benchmark results on x86-64: recursive functions +21%, method calls +12%, PROJECT-mode loops +23%. Zero regression on VM hot paths.

This is the first known application of TurboQuant-inspired orthogonal quantization to language runtime execution profiling.

---

## Core Design Principles

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

## Writing Fluxa — Programming Guide

This section walks through the core concepts in the order you would encounter them writing a real Fluxa program.

### 1. Variables — always typed, always explicit

Every variable must declare its type before its name. There is no inference.

```fluxa
int    count  = 0
float  temp   = 23.5
str    label  = "sensor_01"
bool   active = true
char   sep    = ','
```

Assignment enforces the declared type at runtime. Assigning a `str` to an `int` variable is a runtime error with a line number.

### 2. Persistent state — `prst`

Add `prst` before a declaration to make it survive a hot reload. Without `prst`, every variable is reset when the file is reloaded.

```fluxa
prst int tick   = 0     // survives reload — keeps its value
int      temp   = 0     // reset to 0 on every reload
```

`prst` variables are serialized in `PrstPool`. Removing a `prst` declaration invalidates it and all variables that depended on it atomically — no ghost state.

### 3. Arrays — `arr` for fixed, `dyn` for flexible

`arr` is a fixed-size, single-type array. Size is declared at write time and cannot change.

```fluxa
int arr readings[4] = [10, 20, 30, 40]
readings[0] = 99          // ok
readings[10] = 1          // runtime error: index out of bounds (line N)
```

`dyn` is a heterogeneous dynamic array. It grows automatically and each element carries its own type tag.

```fluxa
dyn events = ["startup", 42, true, 3.14]
events[4] = "new_event"   // auto-grows
len(events)               // 5
```

Both work with `for..in`:

```fluxa
for r in readings { print(r) }
for e in events   { print(e) }
```

### 4. Control flow

Standard `if/else`, `while`, and `for..in`. Braces are always required — no single-line shortcuts.

```fluxa
if count > 10 {
    print("high")
} else {
    print("low")
}

while active {
    count = count + 1
    if count >= 100 { active = false }
}
```

Logical operators with short-circuit evaluation: `&&`, `||`, `!`.

### 5. Functions

Return type is declared after the parameter list. `nil` means no return value. TCO handles tail calls without stack overflow.

```fluxa
fn add(int a, int b) int {
    return a + b
}

fn greet(str name) nil {
    print("hello " + name)
}

fn factorial(int n) int {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)    // TCO applies at tail position
}
```

### 6. Blocks — state + behavior grouped together

`Block` is Fluxa's unit of encapsulation. No inheritance, no hierarchy.

```fluxa
Block Counter {
    prst int total = 0

    fn increment() nil { total = total + 1 }
    fn value() int     { return total }
}

Counter.increment()
print(Counter.value())   // 1
```

`typeof` creates an independent clone. State is fully isolated — changing one instance never affects another.

```fluxa
Block c1 typeof Counter
Block c2 typeof Counter

c1.increment()
c1.increment()

print(c1.value())   // 2
print(c2.value())   // 0 — completely independent
```

### 7. Error handling — `danger` and `err`

`danger` contains operations that might fail. Errors inside are captured in `err[]` instead of aborting.

```fluxa
danger {
    float r = libm.sqrt(-1.0)   // domain error — captured
    int   x = arr[999]          // OOB — captured
}

if err != nil {
    print(err[0])   // most recent error message + line number
}
```

Outside `danger`, any error aborts immediately with a line number. This is intentional — fail-fast outside, capture inside.

### 8. Standard library

All stdlib is opt-in. Declare in `fluxa.toml`, then `import` in code.

```toml
[libs]
std.math    = "1.0"
std.csv     = "1.0"
std.json    = "1.0"
std.strings = "1.0"
std.time    = "1.0"
```

```fluxa
import std math
import std strings
import std json

float h = math.hypot(3.0, 4.0)         // 5.0
bool  ok = math.approx(0.1 + 0.2, 0.3) // true

dyn parts = strings.split("a,b,c", ",")
str upper = strings.upper("hello")

str obj = json.object()
obj = json.set(obj, "temp", json.from_float(23.5))
float t = json.get_float(obj, "temp")
```

### 9. Calling C libraries — FFI

Declare the library and its function signatures in `fluxa.toml`. Then call inside `danger`. The runtime handles all pointer marshalling.

```toml
[ffi]
libm = "auto"
libc = "auto"
str_buf_size = 1024    # writable char* buffer size per argument (default)

[ffi.libm.signatures]
sqrt  = "(double) -> double"
modf  = "(double, double*) -> double"
frexp = "(double, int*) -> double"

[ffi.libc.signatures]
scanf  = "(char*, int*) -> int"
fgets  = "(char*, int, dyn) -> dyn"
fopen  = "(char*, char*) -> dyn"
fclose = "(dyn) -> int"
fread  = "(uint8_t*, int, int, dyn) -> int"
```

```fluxa
// int* — runtime passes &exp, writes result back automatically
int exp = 0
float mantissa = 0.0
danger {
    mantissa = libm.frexp(8.0, exp)
}
print(exp)        // 4  (8 == 0.5 * 2^4)
print(mantissa)   // 0.5

// double* — same pattern
float whole = 0.0
float frac  = 0.0
danger {
    frac = libm.modf(3.7, whole)
}
print(whole)   // 3
print(frac)    // 0.7

// char* output — runtime allocates buffer, copies result back into str
str buf = ""
danger {
    dyn fp = libc.fopen("/dev/stdin", "r")
    dyn ret = libc.fgets(buf, 64, fp)
}
print(buf)   // whatever the user typed

// int* from stdin
int val = 0
danger {
    int matched = libc.scanf("%d", val)
}
print(val)   // the integer the user typed

// uint8_t* — runtime flattens int arr to bytes, scatters back after call
int arr buf[8] = [0,0,0,0,0,0,0,0]
danger {
    dyn fp = libc.fopen("/dev/stdin", "r")
    int n = libc.fread(buf, 1, 8, fp)
}
print(buf[0])   // ASCII value of first byte read
print(buf[1])   // ASCII value of second byte read

// dyn opaque — void* stored as VAL_PTR, passed back to C unchanged
danger {
    dyn fp = libc.fopen("/etc/hostname", "r")
    int r  = libc.fclose(fp)
    print(r)    // 0 = success
}
```

**Pointer mapping reference:**

| C signature type | Fluxa type | Behaviour |
|---|---|---|
| `int` / `double` / `bool` | `int` / `float` / `bool` | Passed by value directly |
| `int*` | `int` | `&var` passed; value written back after call |
| `double*` / `float*` | `float` | `&var` passed; value written back after call |
| `bool*` | `bool` | `&var` passed; value written back after call |
| `char*` | `str` | Internal buffer allocated; result copied back to `str` |
| `uint8_t*` / `void*` buf | `int arr` | Flattened to byte buffer; bytes scattered back to arr |
| `dyn` / `struct*` / `void*` | `dyn` | `VAL_PTR` extracted from `dyn[0]` |
| Pointer return type | `dyn` | Stored as `VAL_PTR` in `dyn[0]` |

### 10. Hot reload and Atomic Handover

Run in development mode — the runtime watches the file and reloads on every save:

```bash
fluxa run main.flx -dev
```

`prst` variables survive the reload. Everything else resets. Edit the file, save — the running program picks up the new logic instantly.

For zero-downtime production handover between two different program versions:

```bash
fluxa handover v1.flx v2.flx
```

The new program runs a full Dry Run in parallel. If it passes, state is migrated atomically. If it fails, v1 keeps running untouched.

---

## Language Overview — Quick Reference

```fluxa
// Types
int count = 0 | float temp = 23.5 | str name = "x" | bool on = true | char c = 'a'

// Arrays
int arr fixed[3] = [1, 2, 3]          // fixed size, one type
dyn  mixed       = [1, "two", true]   // dynamic, any type, auto-grows

// Persistence
prst int tick = 0                      // survives hot reload

// Control
if x > 0 { ... } else { ... }
while active { ... }
for item in arr { ... }

// Functions
fn add(int a, int b) int { return a + b }

// Blocks
Block Sensor { prst float reading = 0.0; fn read() float { return reading } }
Block s typeof Sensor                  // independent clone

// Errors
danger { risky_operation() }
if err != nil { print(err[0]) }       // err[0] = most recent error

// Stdlib (declare in fluxa.toml first)
import std math | import std csv | import std json | import std strings

// FFI (declare lib + signatures in fluxa.toml)
danger { float r = libm.sqrt(9.0) }   // r = 3.0
```

---

## Standard Library

All standard libraries are opt-in via `fluxa.toml`:

```toml
[libs]
std.math    = "1.0"
std.csv     = "1.0"
std.json    = "1.0"
std.strings = "1.0"
std.time    = "1.0"
```

| Library | Key functions | `danger` required? |
|---|---|---|
| `std.math` | `sqrt`, `pow`, `sin/cos/tan`, `log`, `clamp`, `approx`, `pi`, `e` | No |
| `std.csv` | `open/next/close`, `chunk`, `load`, `save`, `field`, `field_count` | File ops only |
| `std.json` | `object`, `set`, `get_str/float/int/bool`, `has`, `stringify`, `load` | File ops only |
| `std.strings` | `split`, `join`, `trim`, `replace`, `upper`, `lower`, `find`, `count` | No |
| `std.time` | `now_ms`, `sleep`, `elapsed` | No |

Full documentation: [`STDLIB.md`](STDLIB.md)

---

## `fluxa.toml` Configuration

```toml
[project]
name  = "my_project"
entry = "main.flx"

[runtime]
gc_cap         = 1024  # GC table hard cap (default 1024)
prst_cap       = 64    # PrstPool initial capacity (default 64, grows automatically)
prst_graph_cap = 256   # PrstGraph initial capacity (default 256, grows automatically)

[libs]
std.math    = "1.0"
std.csv     = "1.0"
std.json    = "1.0"
std.strings = "1.0"

[ffi]
libm = "auto"           # resolved via dlopen — tries platform candidates automatically
libc = "auto"
str_buf_size = 1024     # writable char* buffer per pointer arg (default 1024, range 64–65536)
                        # increase for functions that write large strings (e.g. sprintf into big buffers)
                        # decrease for memory-constrained embedded targets

[ffi.libm.signatures]
sqrt  = "(double) -> double"
modf  = "(double, double*) -> double"
frexp = "(double, int*) -> double"

[ffi.libc.signatures]
scanf  = "(char*, int*) -> int"
fgets  = "(char*, int, dyn) -> dyn"
fopen  = "(char*, char*) -> dyn"
fclose = "(dyn) -> int"
fread  = "(uint8_t*, int, int, dyn) -> int"
puts   = "(char*) -> int"
strlen = "(char*) -> int"
```

---

## CLI Reference

```bash
fluxa run <file.flx>                   Run a script (auto-detects script vs project)
fluxa run <file.flx> -proj <dir>       Run as project (enables prst, reads fluxa.toml)
fluxa run <file.flx> -dev              Watch mode — reload on file save (inotify/kqueue)
fluxa run <file.flx> -prod             Production mode — IPC server active
fluxa run <file.flx> -p                Preflight check — parse + resolve, no execution
fluxa handover <old.flx> <new.flx>     Execute Atomic Handover (5-step protocol)
fluxa explain <file.flx>               Show prst vars, Block deps, GC state
fluxa apply <key>=<value>              Send IPC set command to live runtime
fluxa observe                          Stream live prst state from running process
fluxa logs                             Stream runtime log output
fluxa status                           Show live runtime status (cycle count, errors)
fluxa ffi list                         List available shared libraries via ldconfig
fluxa ffi inspect <lib>                Generate suggested toml signatures for a library
fluxa runtime info                     Show current runtime configuration
```

---

## Embedded Targets

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
make test-runner   # full automated suite — PASS/FAIL per section
make test-all      # unit + suite2 + libs + integration
make bench         # performance benchmarks
```

```bash
bash tests/run_tests.sh              # 54 cases across all sprints and stdlib
bash tests/ffi_ptr.sh                # 12 cases: int*, double*, char*, uint8_t*, dyn
bash tests/sprint9c_ffi_toml.sh      # 10 cases: [ffi] toml auto-resolve
bash tests/libs/math.sh              # std.math
bash tests/libs/csv.sh               # std.csv
bash tests/libs/json.sh              # std.json
bash tests/libs/strings.sh           # std.strings
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
    ffi.c               libffi C interop — pointer marshalling, writeback
    fluxa_ffi.h         FFI public API
    ipc_server.c/h      Unix socket IPC for live inspection
    toml_config.h       fluxa.toml parser
    err.h               Error ring buffer
    warm_profile.h      Warm path: WarmProfile (WHT signature + QJL guard),
                        WarmSlot (1 byte/node), warm_func_is_promoted()
    std/
      math/             std.math implementation
      csv/              std.csv implementation
      json/             std.json implementation
      str/              std.strings implementation
      time/             std.time implementation
      flxthread/        std.flxthread implementation
  tests/
    run_tests.sh        Main runner — auto-discovers tests/libs/
    ffi_ptr.sh          FFI pointer mapping tests (12 cases)
    sprint9c_ffi_toml.sh  [ffi] toml auto-resolve tests (10 cases)
    libs/               stdlib test scripts
    suite2/             Edge case master suite
    integration/        Real-world scenario simulations
  examples/             Example programs
  vendor/               uthash (single-header hash table)
  fluxa_spec_v10.md     Full language specification
  STDLIB.md             Standard library reference
  CREATING_LIBS.md      Guide for adding new stdlib libraries
  Makefile
```

---

## License

Fluxa-lang is open source. See `LICENSE` for details.
