# Creating Standard Libraries for Fluxa-lang

**Current workflow — v0.15+**

Adding a new lib requires **4 files** and **zero edits to core runtime files**.
The lib linker (`scripts/gen_lib_registry.py`) handles all integration automatically.

---

## Overview

```
src/std/mylib/
├── fluxa_std_mylib.h   ← the entire lib implementation
└── lib.mk              ← external deps + build gate

fluxa.libs              ← set std.mylib = true to include in binary
tests/libs/mylib.sh     ← 10-15 test cases
```

**Files you do NOT touch:**
- `src/toml_config.h` — no new fields
- `src/parser.c` — no new strcmp entries
- `src/runtime.c` — no new includes or dispatch blocks
- `Makefile` — no new detection blocks

After creating the 4 files: `make build` detects and integrates everything.

---

## Step 1 — The Header

Create `src/std/mylib/fluxa_std_mylib.h`. The entire lib lives in this one header.

### Minimum structure

```c
#ifndef FLUXA_STD_MYLIB_H
#define FLUXA_STD_MYLIB_H

#include <string.h>
#include "../../scope.h"   /* Value, ValType, VAL_INT, VAL_STRING, ... */
#include "../../err.h"     /* ErrStack, errstack_push, ERR_FLUXA        */

/* ── Value constructors ─────────────────────────────────────────── */
static inline Value mylib_int(long n)       { Value v; v.type = VAL_INT;    v.as.integer  = n; return v; }
static inline Value mylib_float(double d)   { Value v; v.type = VAL_FLOAT;  v.as.real     = d; return v; }
static inline Value mylib_bool(int b)       { Value v; v.type = VAL_BOOL;   v.as.boolean  = b; return v; }
static inline Value mylib_nil(void)         { Value v; v.type = VAL_NIL;                       return v; }
static inline Value mylib_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = fxstr_new(s ? s : "");   /* refcounted allocator — NOT strdup */
    return v;
}

/* ── Main dispatch function ─────────────────────────────────────── */
static inline Value fluxa_std_mylib_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "mylib.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "mylib", line); \
    *had_error = 1; return mylib_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "mylib.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "mylib", line); \
        *had_error = 1; return mylib_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        LIB_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) LIB_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

    /* ── mylib.hello(str name) → str ──────────────────────────── */
    if (strcmp(fn_name, "hello") == 0) {
        NEED(1); GET_STR(0, name);
        char buf[256];
        snprintf(buf, sizeof(buf), "Hello, %s!", name);
        return mylib_str(buf);
    }

#undef LIB_ERR
#undef NEED
#undef GET_STR
#undef GET_INT

    snprintf(errbuf, sizeof(errbuf), "mylib.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "mylib", line);
    *had_error = 1;
    return mylib_nil();
}

/* ── Lib descriptor — read by scripts/gen_lib_registry.py ──────── *
 * FLUXA_LIB_EXPORT is a no-op macro at compile time.              *
 * The scanner reads it to register the lib in the dispatch table.  */
FLUXA_LIB_EXPORT(
    name     = "mylib",
    toml_key = "std.mylib",
    owner    = "mylib",
    call     = fluxa_std_mylib_call,
    rt_aware = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_MYLIB_H */
```

### Dispatch function variants

Three signatures are supported. Declare the matching one in `FLUXA_LIB_EXPORT`:

**Standard** (`rt_aware=0, cfg_aware=0`) — most libs:
```c
static inline Value fluxa_std_mylib_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line);
```

**rt-aware** (`rt_aware=1`) — libs that spawn threads (flxthread pattern):
```c
static inline Value fluxa_std_mylib_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line, void *rt_ptr);
```

**cfg-aware** (`cfg_aware=1`) — libs that read `fluxa.toml` config at dispatch time (json pattern):
```c
static inline Value fluxa_std_mylib_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line, const FluxaConfig *cfg);
```

The runtime dispatch loop selects the right call path based on the flags in `FLUXA_LIB_EXPORT`. No changes to `runtime.c` needed.

### Value type system

| C type | `Value.type` | Access | Constructor |
|---|---|---|---|
| `long` | `VAL_INT` | `v.as.integer` | `mylib_int(n)` |
| `double` | `VAL_FLOAT` | `v.as.real` | `mylib_float(d)` |
| `int` | `VAL_BOOL` | `v.as.boolean` | `mylib_bool(b)` |
| `char*` | `VAL_STRING` | `v.as.string` | `mylib_str(s)` — uses `fxstr_new()` |
| `FluxaDyn*` | `VAL_DYN` | `v.as.dyn` | allocate manually |
| `void*` | `VAL_PTR` | `v.as.ptr` | set directly |
| nothing | `VAL_NIL` | — | `mylib_nil()` |

**Critical:** `VAL_STRING` values returned to the runtime must be allocated with **`fxstr_new()`** (the refcounted string allocator from `scope.h`), **not** `strdup()`. Since the string refcount change, a `strdup`'d return is freed with the wrong allocator and aborts with `free(): invalid pointer`. `mylib_str()` already calls `fxstr_new` — always route returns through it. Never return a pointer to a stack buffer.

### Returning a dyn

```c
FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
d->cap   = 8; d->count = 0;
d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
d->items[d->count++] = mylib_str("first");
d->items[d->count++] = mylib_int(42);
Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
return ret;
```

### Returning an opaque cursor (VAL_PTR in dyn)

For stateful objects (file handles, DB connections, serial ports). The user holds `prst dyn cursor` — state survives hot reloads.

```c
typedef struct { FILE *fp; int eof; } MyCursor;

MyCursor *cur = malloc(sizeof(MyCursor));
cur->fp = fopen(path, "r"); cur->eof = 0;

FluxaDyn *d  = malloc(sizeof(FluxaDyn));
d->cap = 1; d->count = 1;
d->items = malloc(sizeof(Value));
d->items[0].type   = VAL_PTR;
d->items[0].as.ptr = cur;

Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
return ret;
```

Extract in another function:
```c
if (args[0].type != VAL_DYN || !args[0].as.dyn ||
    args[0].as.dyn->count < 1 ||
    args[0].as.dyn->items[0].type != VAL_PTR ||
    !args[0].as.dyn->items[0].as.ptr)
    LIB_ERR("invalid cursor — use mylib.open() to create one");
MyCursor *cur = (MyCursor *)args[0].as.dyn->items[0].as.ptr;
```

Close pattern (prevents double-free):
```c
cur->fp = NULL; free(cur);
if (args[0].type == VAL_DYN && args[0].as.dyn && args[0].as.dyn->count >= 1)
    args[0].as.dyn->items[0].as.ptr = NULL;
```

### Error handling

`LIB_ERR` works identically inside and outside `danger`. The runtime handles routing:
- Outside `danger` → aborts with line number
- Inside `danger` → captured in `err[]`, execution continues

The lib never checks `danger_depth`. Just call `LIB_ERR`.

---

## Step 2 — lib.mk

Create `src/std/mylib/lib.mk`. This file declares external dependencies and gates compilation on the build-time flag set by `fluxa.libs`.

**No external deps:**
```makefile
# std.mylib — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_MYLIB),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_MYLIB=1
endif
```

**With external deps (pkg-config):**
```makefile
# std.mylib — requires libmylib
ifeq ($(FLUXA_BUILDTIME_MYLIB),1)
ifeq ($(shell pkg-config --exists libmylib 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_MYLIB=1 $(shell pkg-config --cflags libmylib)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libmylib)
endif
endif
```

`FLUXA_BUILDTIME_MYLIB` is set by `scripts/gen_lib_registry.py` reading `fluxa.libs`. If a lib is `false` in `fluxa.libs`, `FLUXA_BUILDTIME_MYLIB` is never set to 1, so the lib's code never enters the binary.

---

## Step 3 — fluxa.libs

Add your lib to `fluxa.libs` at the project root:

```toml
[libs.build]
# ... existing libs ...
std.mylib = true    # true = compiled into binary; false = excluded entirely
```

`false` means zero code size, zero link time, zero overhead. Use `false` for libs with heavy deps (libsodium, sqlite) on embedded targets.

---

## Step 4 — Tests

Create `tests/libs/mylib.sh`. The test runner auto-discovers all scripts in `tests/libs/` — no changes needed elsewhere.

### Required structure

```bash
#!/usr/bin/env bash
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/mylib/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/mylib/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.mylib="1.0"\n' \
        > "$P/fluxa.toml"
}
run() { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.mylib ────────────────────────────────────────────────────"

# 1. import without [libs] → error
cat > "$P/main.flx" << 'FLX'
import std mylib
mylib.hello("world")
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" \
    || fail "import_without_toml_error" "not declared error" "$out"

# 2. happy path
out=$(run << 'FLX'
import std mylib
str r = mylib.hello("world")
print(r)
FLX
)
echo "$out" | grep -q "Hello, world!" \
    && pass "hello_returns_string" \
    || fail "hello_returns_string" "Hello, world!" "$out"

# 3. error captured in danger
toml
cat > "$P/main.flx" << 'FLX'
import std mylib
danger { mylib.unknown_fn() }
if err != nil { print("caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "caught" \
    && pass "error_captured_in_danger" \
    || fail "error_captured_in_danger" "caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.mylib: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.mylib: PASS" && exit 0 || exit 1
```

**The `→ std.mylib: PASS` line** is what `tests/run_tests.sh` greps for. Required.

### Error tests outside danger

When testing errors that should fire outside `danger`, write to a file first — `if err != nil` outside danger isn't reachable (execution aborted). Check stderr instead:

```bash
toml
cat > "$P/main.flx" << 'FLX'
import std mylib
mylib.bad_call()
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "Runtime error" \
    && pass "error_aborts_outside_danger" \
    || fail "error_aborts_outside_danger" "Runtime error" "$out"
```

---

## Step 5 — Run make build

```bash
make build
```

This automatically:
1. Runs `scripts/gen_lib_registry.py` — scans `src/std/*/fluxa_std_*.h`, reads `fluxa.libs`
2. Generates `src/lib_registry_gen.h` — includes, registry array, lookup helpers
3. Generates `src/lib_registry_flags.mk` — `FLUXA_BUILDTIME_MYLIB` Make variable
4. Includes `src/std/mylib/lib.mk` which sets `-DFLUXA_STD_MYLIB=1` if enabled
5. Compiles everything

The lib is automatically known to the parser (`import std mylib`) and dispatch (`mylib.fn()`).

**Note — run `make build` twice for a brand-new lib.** Make evaluates
`-include src/lib_registry_flags.mk` at parse time, but the registry
generator runs inside the build rule. The first `make build` after
creating a new lib therefore compiles with the *previous* flags file —
`FLUXA_BUILDTIME_MYLIB` isn't set yet and the lib is silently absent
from that binary (calls fail with "undefined identifier"). The second
`make build` picks it up. Only lib *creation* is affected; every later
build is normal.

---

## Dispatch chain — how it works now

The runtime dispatch loop in `runtime.c` walks `fluxa_lib_registry[]` (from `lib_registry_gen.h`). For each entry it checks:

1. `enabled` — compiled in (`#ifdef FLUXA_STD_MYLIB`)
2. `fluxa_std_lib_enabled(...)` — declared in `fluxa.toml [libs]`
3. `strcmp(owner, _e->owner)` — matches the call namespace (`mylib.fn()`)

Then calls the appropriate function pointer:

```
cfg_aware=1  → call_cfg(fn, args, argc, err, had_error, line, &rt->config)
rt_aware=1   → call_rt(fn, args, argc, err, had_error, line, rt)
else         → call(fn, args, argc, err, had_error, line)
```

---

## Checklist

- [ ] `src/std/mylib/fluxa_std_mylib.h` — header with `FLUXA_LIB_EXPORT` at end
- [ ] `src/std/mylib/lib.mk` — gates `FLUXA_STD_MYLIB` on `FLUXA_BUILDTIME_MYLIB`
- [ ] `fluxa.libs` — `std.mylib = true` under `[libs.build]`
- [ ] `tests/libs/mylib.sh` — test script with `→ std.mylib: PASS` footer line
- [ ] `make build` — zero errors, zero warnings
- [ ] `bash tests/libs/mylib.sh` — all cases pass
- [ ] `bash tests/run_tests.sh` — full suite still green
- [ ] `docs/STDLIB.md` — lib documented with function reference and example

---

## Common mistakes

**Returning stack-allocated string:**
```c
// WRONG — dangling pointer
char buf[64]; snprintf(buf, sizeof(buf), "x");
Value v; v.type = VAL_STRING; v.as.string = buf; // buf is on stack!

// CORRECT
return mylib_str(buf); // mylib_str calls strdup(buf)
```

**Not freeing on error paths:**
```c
FILE *fp = fopen(path, "r");
MyCursor *cur = malloc(sizeof(MyCursor));
if (!cur) { fclose(fp); LIB_ERR("out of memory"); } // close fp first
```

**Lib name collision:** Two libs with the same `owner` in `FLUXA_LIB_EXPORT` — only the first is reached. Names must be globally unique. Avoid naming libs after Fluxa keywords (`int`, `float`, `str`, `bool`, `nil`, `fn`, `return`, `if`, `else`, `while`, `for`, `in`, `prst`, `Block`, `typeof`, `danger`, `free`, `import`, `err`, `dyn`, `arr`).

**Checking `danger_depth`:** Don't. `LIB_ERR` behaves correctly in both contexts automatically.

**cfg_aware lib not including toml_config.h:** Not needed — `lib_registry_gen.h` includes `toml_config.h` before all lib headers. `FluxaConfig` is already defined when your header is compiled.

---

## Memory ownership contract

The runtime and your lib share a single ownership rule that makes memory predictable end-to-end. Following it correctly is most of what separates a clean lib from a leaky one.

### Inputs — what `args[]` carries

`args[]` is a read-only view of caller-evaluated `Value`s. The runtime decides which entries are heap-owned (strdup of a literal, return of another call) and which alias caller storage (a variable read, a Block field). **You do not need to know which is which.** The runtime releases owned `VAL_STRING` args automatically after your dispatch function returns.

What this means in practice:

```c
// CORRECT — treat args[] as read-only borrowed views
if (strcmp(fn_name, "echo") == 0) {
    NEED(1); GET_STR(0, msg);
    return mylib_str(msg);                  // mylib_str strdups — your output owns its char*
}

// WRONG — never write through args[]
args[0].as.string = strdup(something);      // corrupts caller's slot, may double-free

// WRONG — never call free() on args[]
free((char*)args[0].as.string);             // runtime owns args[]; double-free
```

`GET_STR` extracts `const char *` from `args[idx].as.string`. Treat that pointer as borrowed for the duration of your call. If you need to keep a copy past return (e.g. storing in a global table), `strdup` it.

### Outputs — what your dispatch returns

The dispatch function returns a single `Value`. Ownership rules per type:

| Return type | What you must do |
|---|---|
| `VAL_NIL` / `VAL_BOOL` / `VAL_INT` / `VAL_FLOAT` | Nothing — by-value types are free. |
| `VAL_STRING` | The `char*` must be heap-allocated and owned solely by this return (use `mylib_str(...)` which `strdup`s). |
| `VAL_DYN` | Heap-allocated `FluxaDyn`. The runtime's dispatch layer automatically calls `gc_register` for any `VAL_DYN` returned to it — you do not call `gc_register` yourself. |
| `VAL_ARR` | Heap-allocated `data` array with `owned = 1` (default via `val_arr(...)`). Element strings must each be `strdup`'d copies. |

After your dispatch returns, the runtime decides whether the caller will store the value, drop it (statement expression), use it as a comparison operand, etc., and releases the heap data accordingly. **You do not need to think about the caller's usage.**

### Common pitfalls

**Stack-allocated string passed through `mylib_str` — fine. Pointer-only — leaks or UAF:**

```c
// CORRECT
char buf[256];
snprintf(buf, sizeof(buf), "result: %d", n);
return mylib_str(buf);                      // strdup, return owned

// WRONG — UAF
char buf[256];
snprintf(buf, sizeof(buf), "result: %d", n);
Value v; v.type = VAL_STRING; v.as.string = buf;
return v;                                   // buf goes out of scope
```

**Returning literal `const char *` — also fine when wrapped:**

```c
// CORRECT
return mylib_str("ok");                     // strdup of the literal

// WRONG — runtime will free a string constant
Value v; v.type = VAL_STRING; v.as.string = (char*)"ok";
return v;                                   // SEGV when runtime tries to free
```

**Returning a pre-allocated buffer you'll re-use:**

```c
// WRONG — caller and your next call both point at the same buf
static char shared_buf[256];
snprintf(shared_buf, sizeof(shared_buf), ...);
Value v; v.type = VAL_STRING; v.as.string = shared_buf;
return v;                                   // next call clobbers caller's data
```

Always `strdup` (via `mylib_str`). If allocation is a hot-path concern, expose an arena like `std.cache.arena_*` and let the caller manage release timing.

**Manually calling `gc_register` on a returned `VAL_DYN` — harmless but redundant:**

The runtime registers any unregistered `VAL_DYN` it sees at the dispatch return boundary. Calling `gc_register` yourself is idempotent (the second register is a no-op) but adds no value.

**`VAL_DYN` whose `cap` lies about size:**

`gc_register` reads `_ret.as.dyn->cap` to size the GC bookkeeping. If you return a `FluxaDyn` with `cap = 0` but `items` allocated for N elements, sweep accounting is wrong. Always set `cap` to the actual allocation size, `count` to the populated prefix.

### One-line summary

> Borrow `args[]`, return owned. The runtime handles everything else.

---

## Thread-spawning libs (rt_aware)

If your lib spawns background threads (like `std.flxthread`):

1. Set `rt_aware = 1` in `FLUXA_LIB_EXPORT`
2. Add `void *rt_ptr` as last dispatch parameter, cast to `Runtime *` inside
3. Wrap the entire header in `#ifdef FLUXA_STD_FLXTHREAD` (dependency)
4. Declare `std.flxthread = "1.0"` in your lib's toml example

Every spawned thread gets its own `Runtime` clone via `runtime_clone_for_thread()` — own stack, scope, GC; shared prst pool (synchronized automatically by `NODE_ASSIGN`).

Document which loop pattern the thread uses (sleep / hot / polling) — it determines mailbox drain frequency.

---

## Dual-Backend Pattern

When a lib benefits from an optional high-performance dependency (libwebsockets, FFTW3, OpenBLAS, Raylib, llama.cpp), use the dual-backend pattern:

```c
#ifdef FLUXA_MYLIB_FAST_BACKEND
#include <fastlib.h>
// Full implementation using fast library
#else
// Pure C99 fallback — same API, simpler/slower implementation
// Use fprintf(stderr, "[fluxa] std.mylib: stub backend\n") to inform user
#endif
```

In `lib.mk`:

```make
ifeq ($(FLUXA_BUILDTIME_MYLIB),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_MYLIB=1

ifdef FLUXA_MYLIB_FAST_BACKEND
  ifneq ($(wildcard vendor/fastlib.h),)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_MYLIB_FAST_BACKEND=1 -Ivendor
    FLUXA_EXTRA_LDFLAGS += vendor/libfast.a -lm
  else ifeq ($(shell pkg-config --exists fastlib 2>/dev/null && echo 1 || echo 0),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_MYLIB_FAST_BACKEND=1 $(shell pkg-config --cflags fastlib)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs fastlib)
  else
    $(warning std.mylib: fast backend requested but not found — using stub)
  endif
endif

endif
```

**Examples using this pattern:**
- `std.websocket`: native RFC6455 ↔ libwebsockets (`make FLUXA_WS_LWS=1`)
- `std.libdsp`: pure C99 FFT ↔ FFTW3 (`[libs.libdsp] backend = "fftw"`)
- `std.libv`: pure C99 ↔ OpenBLAS (`[libs.libv] backend = "blas"`)
- `std.graph`: stub no-op ↔ Raylib (`make FLUXA_GRAPH_RAYLIB=1`)
- `std.infer`: stub placeholder ↔ llama.cpp (`make FLUXA_INFER_LLAMA=1`)
