# Creating Standard Libraries for Fluxa-lang
**A practical guide based on the implementation of std.math, std.csv, std.json, std.strings, and std.time**

---

## Overview

Every Fluxa standard library follows the same pattern across six files and four integration points. Once you understand the pattern, adding a new lib takes about two hours including tests.

The pattern is:
1. **Header** — `src/std/<lib>/fluxa_std_<lib>.h` — the entire implementation
2. **Config** — `src/toml_config.h` — opt-in flag in `FluxaStdLibs`
3. **Parser** — `src/parser.c` — accept `import std <lib>`
4. **Runtime** — `src/runtime.c` — include header + dispatch `lib.fn()` calls
5. **Makefile** — compile flag + test target
6. **Tests** — `tests/libs/<lib>.sh` — 10-15 cases

---

## Step 1: The Header

Create `src/std/<lib>/fluxa_std_<lib>.h`. This is a pure C header — no `.c` file. Everything lives here.

### Minimum structure

```c
#ifndef FLUXA_STD_<LIB>_H
#define FLUXA_STD_<LIB>_H

#include <string.h>
#include "../../scope.h"   /* Value, ValType, VAL_INT, VAL_STRING, ... */
#include "../../err.h"     /* ErrStack, errstack_push, ERR_FLUXA        */

/* ── Value constructors ─────────────────────────────────────────── */
static inline Value mylib_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value mylib_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}
static inline Value mylib_bool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b; return v;
}
static inline Value mylib_float(double d) {
    Value v; v.type = VAL_FLOAT; v.as.real = d; return v;
}
static inline Value mylib_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Main dispatch function ─────────────────────────────────────── */
static inline Value fluxa_std_<lib>_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "<lib>.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "<lib>", line); \
    *had_error = 1; return mylib_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "<lib>.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "<lib>", line); \
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

    snprintf(errbuf, sizeof(errbuf), "<lib>.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "<lib>", line);
    *had_error = 1;
    return mylib_nil();
}

#endif /* FLUXA_STD_<LIB>_H */
```

### The Value type system

Every function receives `const Value *args` and returns `Value`. The types:

| C type | `Value.type` | Access | Constructor |
|---|---|---|---|
| `long` | `VAL_INT` | `v.as.integer` | `mylib_int(n)` |
| `double` | `VAL_FLOAT` | `v.as.real` | `mylib_float(d)` |
| `int` | `VAL_BOOL` | `v.as.boolean` | `mylib_bool(b)` |
| `char*` | `VAL_STRING` | `v.as.string` | `mylib_str(s)` |
| `FluxaDyn*` | `VAL_DYN` | `v.as.dyn` | allocate manually |
| `void*` | `VAL_PTR` | `v.as.ptr` | set directly |
| nothing | `VAL_NIL` | — | `mylib_nil()` |

**Critical rule:** `VAL_STRING` values are heap-allocated. Always `strdup()` when creating one. The runtime will `free()` it when the value goes out of scope. Never return a pointer to a stack buffer.

### Returning a dyn

```c
FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
d->cap   = 8;
d->count = 0;
d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);

d->items[d->count++] = mylib_str("first");
d->items[d->count++] = mylib_int(42);

Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
return ret;
```

### Returning an opaque cursor (VAL_PTR wrapped in dyn)

For stateful objects like file cursors, wrap a heap-allocated C struct in a `VAL_PTR` inside a single-element `dyn`. The `dyn` is what the user holds (as `prst dyn cursor`). The `VAL_PTR` is opaque to the runtime.

```c
typedef struct {
    FILE *fp;
    int   eof;
} MyCursor;

MyCursor *cur = malloc(sizeof(MyCursor));
cur->fp  = fopen(path, "r");
cur->eof = 0;

FluxaDyn *d  = malloc(sizeof(FluxaDyn));
d->cap = 1; d->count = 1;
d->items = malloc(sizeof(Value));
d->items[0].type   = VAL_PTR;
d->items[0].as.ptr = cur;

Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
return ret;
```

To extract the cursor in another function:

```c
if (args[0].type != VAL_DYN || !args[0].as.dyn ||
    args[0].as.dyn->count < 1 ||
    args[0].as.dyn->items[0].type != VAL_PTR) {
    LIB_ERR("invalid cursor — use mylib.open() to create one");
}
MyCursor *cur = (MyCursor *)args[0].as.dyn->items[0].as.ptr;
```

### Error handling

Errors follow the standard Fluxa model automatically — you don't need to know whether you're inside `danger` or not. Just call `LIB_ERR(msg)` or `errstack_push` and set `*had_error = 1`. The runtime handles the rest:

- Outside `danger`: execution aborts with the error message and line number
- Inside `danger`: error is captured in `err[]`, execution continues

```c
// Domain error — same pattern as math.sqrt(-1)
if (value < 0) LIB_ERR("cannot process negative value");

// File not found
if (!fp) {
    snprintf(errbuf, sizeof(errbuf), "mylib.open: cannot open '%s'", path);
    LIB_ERR(errbuf);
}
```

### When to require danger

File I/O, network, IPC, and any operation that can fail for external reasons should be usable inside `danger`. They don't need special handling — `LIB_ERR` already integrates with the `danger` capture mechanism.

Pure computation functions (math, string manipulation, JSON field extraction) work outside `danger` by the same mechanism — the user just doesn't wrap them.

The lib doesn't decide this — the **user** decides by whether they wrap the call in `danger {}`. Your lib just provides correct error reporting via `LIB_ERR`.

---

## Step 2: FluxaStdLibs flag

In `src/toml_config.h`, add one field to `FluxaStdLibs`:

```c
typedef struct {
    int has_math;
    int has_csv;
    int has_json;
    int has_strings;
    int has_time;
    int has_mylib;   /* ← add this */
} FluxaStdLibs;
```

Then add the toml parser line in `fluxa_config_load_libs()`:

```c
if (strncmp(p, "std.math",    8)  == 0) cfg->std_libs.has_math    = 1;
if (strncmp(p, "std.csv",     7)  == 0) cfg->std_libs.has_csv     = 1;
// ...
if (strncmp(p, "std.mylib",   9)  == 0) cfg->std_libs.has_mylib   = 1; /* ← add */
```

The `strncmp` length must match exactly the length of the key string (`std.mylib` = 9 chars).

---

## Step 3: Parser

In `src/parser.c`, find the `import std <lib>` validation block and add your lib name:

```c
if (strcmp(lib_name, "math")    != 0 &&
    strcmp(lib_name, "csv")     != 0 &&
    strcmp(lib_name, "json")    != 0 &&
    strcmp(lib_name, "strings") != 0 &&
    strcmp(lib_name, "time")    != 0 &&
    strcmp(lib_name, "mylib")   != 0 &&  /* ← add */
    strcmp(lib_name, "vec")     != 0) {
```

Also update the error message:

```c
"unknown std library '%s' — available: math, csv, json, strings, time, mylib, vec"
```

### Special case: reserved type keywords as lib names

`str` is a reserved type keyword in Fluxa (`TOK_TYPE_STR`). If your lib name matches a type keyword, the parser's `check(p, TOK_IDENT)` check will fail. The fix is already in place — the import parser accepts `TOK_TYPE_STR`, `TOK_TYPE_INT`, `TOK_TYPE_FLOAT`, and `TOK_TYPE_BOOL` alongside `TOK_IDENT`.

Similarly, `lib.fn()` calls in expressions go through `NODE_MEMBER_CALL`. The expression parser already accepts type keyword tokens as namespace prefixes for this reason. No extra work needed for libs named after non-type words.

**Avoid naming your lib after a Fluxa keyword.** The full keyword list is: `int`, `float`, `str`, `bool`, `nil`, `fn`, `return`, `if`, `else`, `while`, `for`, `in`, `prst`, `Block`, `typeof`, `danger`, `free`, `import`, `err`, `dyn`, `arr`. If your lib name collides, use a compound name (e.g. `strings` not `str`).

---

## Step 4: Runtime

Three changes in `src/runtime.c`:

### 4a. Include the header

Find the block of stdlib includes (near the top of the file, after `#include "gc.h"`):

```c
#ifdef FLUXA_STD_TIME
#include "std/time/fluxa_std_time.h"
#endif
#ifdef FLUXA_STD_MYLIB                    /* ← add */
#include "std/mylib/fluxa_std_mylib.h"   /* ← add */
#endif                                    /* ← add */
```

### 4b. Dispatch in NODE_MEMBER_CALL

Find the std dispatch block in `NODE_MEMBER_CALL` (search for `FLUXA_STD_TIME`). Add your lib after the last existing one, before `BlockInstance *inst = ...`:

```c
#ifdef FLUXA_STD_MYLIB
            if (rt->config.std_libs.has_mylib && strcmp(owner, "mylib") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_mylib_call(method, args, argc,
                                            &rt->err_stack, &rt->had_error,
                                            rt->current_line);
            }
#endif /* FLUXA_STD_MYLIB */

            BlockInstance *inst = resolve_instance(rt, owner);
```

**Max args:** The dispatch evaluates up to 16 args. If your lib needs more, increase the local array size. No Fluxa stdlib has needed more than 4.

### 4c. NODE_IMPORT_STD validation

Find the block that validates the lib name at runtime (search for `declared = 1`):

```c
if (strcmp(lib, "time")   == 0 && rt->config.std_libs.has_time)   declared = 1;
if (strcmp(lib, "mylib")  == 0 && rt->config.std_libs.has_mylib)  declared = 1; /* ← add */
```

---

## Step 5: Makefile

Two changes:

### 5a. Add compile flag to CFLAGS

```makefile
CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2 \
          -Isrc -Ivendor $(FFI_CFLAGS)          \
          -DFLUXA_STD_MATH=1                    \
          # ... existing flags ...
          -DFLUXA_STD_TIME=1                    \
          -DFLUXA_STD_MYLIB=1                   # ← add
```

The flag being in CFLAGS means the code is always compiled into the binary. The toml `[libs]` declaration controls whether the user can access it at runtime. This is the design choice: zero binary overhead is achieved by not defining the flag — but for the default development build, all libs are included and gated by toml.

For embedded targets where binary size is critical, you can create a separate `CFLAGS_EMBEDDED` that omits specific lib flags.

### 5b. Add test target

```makefile
test-libs-mylib: build
	@bash tests/libs/mylib.sh --fluxa ./$(TARGET)
```

---

## Step 6: Tests

Create `tests/libs/<lib>.sh`. The test runner (`tests/run_tests.sh`) auto-discovers all scripts in `tests/libs/` — no changes needed there.

### Required structure

```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FLUXA="${PROJECT_ROOT}/fluxa"
WORK_DIR="$(mktemp -d)"; trap 'rm -rf "$WORK_DIR"' EXIT
FAILS=0

while [[ $# -gt 0 ]]; do
    case "$1" in --fluxa) FLUXA="$2"; shift 2 ;; *) shift ;; esac
done

pass() { printf "  PASS  std.<lib>/%s\n" "$1"; }
fail() { printf "  FAIL  std.<lib>/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

# Helper: create a project dir with fluxa.toml declaring the lib
setup() {
    local dir="$1" name="$2"
    mkdir -p "$dir"
    cat > "$dir/fluxa.toml" << TOML
[project]
name = "$name"
entry = "main.flx"
[libs]
std.<lib> = "1.0"
TOML
}

echo "── std.<lib>: <description> ─────────────────────────────────────────"

# ... test cases ...

echo "────────────────────────────────────────────────────────────────────"
total=N
if [ "$FAILS" -eq 0 ]; then
    echo "  Results: ${total} passed, 0 failed"
    echo "  → std.<lib>: PASS"    # ← this line is what run_tests.sh greps for
    exit 0
else
    echo "  Results: $((total-FAILS)) passed, $FAILS failed"
    exit 1
fi
```

The `→ std.<lib>: PASS` line is what `run_tests.sh` greps to determine if the lib passed. The lib name in the grep pattern is derived from the filename: `math.sh` → greps for `std.math: PASS`.

### Required test cases

Every lib must have at minimum:

1. **Import without `[libs]`** — confirms the toml gate works
2. **Happy path** — each function with valid input
3. **Error in `danger`** — confirm errors are captured, not aborted
4. **Error outside `danger`** — confirm errors abort (if applicable)
5. **Integration with `prst`** — lib works with persistent vars

### Each test case pattern

```bash
# CASE N: description
P="$WORK_DIR/pN"; setup "$P" "test_name"
cat > "$P/main.flx" << 'FLX'
import std mylib
# ... test code ...
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
if echo "$out" | grep -q "expected_output"; then
    pass "test_name"
else
    fail "test_name" "expected" "$out"
fi
```

Notes:
- Always use `timeout 5s` to prevent hangs
- Always `|| true` so a non-zero exit doesn't abort the script
- Use `-proj "$P"` so `prst` works and the toml is found
- Pure string operations that don't need `prst` can run without `-proj` (just `./fluxa run file.flx`)

---

## Common Mistakes

### 1. Forgetting to add to NODE_IMPORT_STD validation

If you add the dispatch but forget the validation, `import std mylib` produces "library not declared" even when it's in the toml. Both the dispatch and the validation need the flag.

### 2. strncmp length wrong in toml parser

`strncmp(p, "std.mylib", 9)` — count the characters. `std.mylib` = 9. If you write 8, `std.mylib` won't match because it compares only `std.mylib` to `std.mylib` but stops one char early. Use `strlen("std.mylib")` if unsure.

### 3. Returning stack-allocated string

```c
// WRONG — dangling pointer
char buf[64];
snprintf(buf, sizeof(buf), "result: %d", n);
Value v; v.type = VAL_STRING; v.as.string = buf;  // buf is on stack!
return v;

// CORRECT — heap-allocated
return mylib_str(buf);  // mylib_str calls strdup(buf)
```

### 4. Not freeing on error paths

```c
FILE *fp = fopen(path, "r");
MyCursor *cur = malloc(sizeof(MyCursor));
if (!cur) { fclose(fp); LIB_ERR("out of memory"); }  // ← close fp before error
```

### 5. Double-free on cursor close

When the user calls `mylib.close(cursor)`, null the pointer in the dyn slot:

```c
cur->fp = NULL;
free(cur);
// Zero the VAL_PTR so double-close is a no-op
if (args[0].type == VAL_DYN && args[0].as.dyn &&
    args[0].as.dyn->count >= 1)
    args[0].as.dyn->items[0].as.ptr = NULL;
```

### 6. Using `rt->danger_depth` to change behavior

Don't check whether you're inside danger. The error mechanism already handles this. Your lib should behave identically in both contexts — `LIB_ERR` does the right thing automatically.

### 7. Library name collision with another dispatch

The dispatch blocks use `strcmp(owner, "mylib")`. If two libs have the same name, only the first one in the dispatch chain is reached. Names must be globally unique. Check all existing lib names before choosing one.

---

## The Dispatch Chain (full picture)

This is the order in which `lib.fn()` calls are resolved in `NODE_MEMBER_CALL`:

```
lib.fn(args)
    │
    ├─ FLUXA_STD_MATH    → "math"    → fluxa_std_math_call()
    ├─ FLUXA_STD_CSV     → "csv"     → fluxa_std_csv_call()
    ├─ FLUXA_STD_JSON    → "json"    → fluxa_std_json_call()
    ├─ FLUXA_STD_STRINGS → "strings" → fluxa_std_strings_call()
    ├─ FLUXA_STD_TIME    → "time"    → fluxa_std_time_call()
    ├─ FLUXA_STD_MYLIB   → "mylib"   → fluxa_std_mylib_call()   ← your lib
    │
    ├─ resolve_instance()  → Block instance method call
    ├─ ffi_find_lib()      → FFI call (import c libname, inside danger)
    └─ error: undefined
```

Each check is `O(1)` — a `strcmp` and a struct field read. The chain is negligible even if it grows to 20+ libs.

---

## Checklist

When you think you're done, verify:

- [ ] `src/std/<lib>/fluxa_std_<lib>.h` — header exists, include guard, all functions return `Value`
- [ ] `FluxaStdLibs.has_<lib>` field added in `toml_config.h`
- [ ] `fluxa_config_load_libs()` has the `strncmp` line with correct length
- [ ] `src/parser.c` — lib name added to known-libs list and error message
- [ ] `src/runtime.c` — `#ifdef FLUXA_STD_<LIB>` include added
- [ ] `src/runtime.c` — dispatch block added in `NODE_MEMBER_CALL`
- [ ] `src/runtime.c` — `declared = 1` line added in `NODE_IMPORT_STD`
- [ ] `Makefile` — `-DFLUXA_STD_<LIB>=1` in `CFLAGS`
- [ ] `Makefile` — `test-libs-<lib>` target added
- [ ] `tests/libs/<lib>.sh` — test script with `→ std.<lib>: PASS` line
- [ ] `make build` — zero errors, zero warnings
- [ ] `bash tests/libs/<lib>.sh` — all cases pass
- [ ] `bash tests/run_tests.sh` — full suite still green
- [ ] `STDLIB.md` — lib documented with function reference and example
