# FLUXA

**Technical Specification**

**v0.16 — Beta**

*Reflects runtime behavior as of v0.23 (reference-counted immutable strings; see §13.6).*

*Runtime · Hot Reload · Atomic Handover · Runtime Update Protocol · 28 stdlib libs · Module System*

*Hobby language — Rio de Janeiro, Brazil*

---

## 1. Philosophy

Fluxa is a runtime-oriented language. Its identity is not compilation but explicit control over what persists, what dies, and when each thing happens.

Every design decision is anchored to one or more of these five principles:

- **Explicit > Implicit**
- **Simple > Complete**
- **Dynamic Runtime > Heavy Compilation**
- **Local Control > Global Magic**
- **Visible Error > Silent Error**

The language is not gentle with inconsistent state. If a contract changed, there is no safe transition — everything dies and restarts clean.

*State mantra: What is not `prst` does not exist between reloads. What was `prst` and disappeared takes everything with it.*

---

## 2. Keywords (Core — ≤ 25)

The language never grows beyond 25 keywords. Any new feature must reuse existing syntax or be implemented via the standard library.

| Category | Keywords |
|---|---|
| Structural | `fn` `return` `import` `Block` `typeof` |
| Primitives | `int` `float` `str` `bool` `char` |
| Compound | `arr` `dyn` |
| Memory / State | `prst` `free` |
| Control Flow | `if` `else` `for` `while` |
| Special | `err` `danger` `nil` |

*`in` (used in `for x in arr`) is not a keyword — it is validated by value in the parser. Preserves the 25-keyword limit.*

---

## 3. Type System

Fluxa uses explicit static typing. The type always precedes the identifier — no inference ever.

### 3.1 Primitives

- `int` — native-precision integer
- `float` — floating point (double internally)
- `str` — dynamic string (internally via sds)
- `bool` — true / false
- `char` — single character

### 3.2 arr — Fixed Array

Size declared at write time. All elements share the declared type. Out-of-bounds access produces a runtime error with line number (fail-fast).

**Declaration forms:**

```fluxa
// Explicit list initializer
int arr values[3] = [1, 2, 3]

// Scalar fill — all elements initialized to the same value
int arr zeroes[2000] = 0       // all 2000 elements set to 0
float arr temps[100] = 0.0     // all 100 elements set to 0.0
bool arr flags[64] = false     // all 64 elements set to false

// Element access and assignment
values[0] = 9       // ok
values[5] = 1       // [fluxa] Runtime error (line N): array index out of bounds
```

The scalar form (`int arr a[N] = 0`) is the standard pattern for large arrays on embedded targets — no need to list N elements explicitly, and the type is enforced on the fill value at runtime.

### 3.3 dyn — Heterogeneous Dynamic Array

Variable-size array storing any value. Each element carries a runtime type tag. Type switching between assignments is permitted. Auto-grows via realloc.

```fluxa
dyn events = [1, "hello", true, 3.14]
events[4] = 99          // auto-grows; gaps filled with nil
events[1] = false       // type switch: permitted
len(events)             // 5
print(events)           // [1, false, true, 3.14, 99]
```

**Internal structure:**
```c
typedef struct { Value *items; int count; int cap; } FluxaDyn;
```

**Block in dyn:** Block instances can be stored as dyn elements. When a Block instance is placed into a dyn, the runtime creates a fully independent copy — the same isolation guarantee as `typeof`. The element in the dyn and the original Block variable are completely separate: mutations to one never affect the other.

```fluxa
Block Sensor {
    prst float reading = 0.0
    fn set(float v) nil { reading = v }
    fn get() float { return reading }
}
Block s1 typeof Sensor
Block s2 typeof Sensor
s1.set(5.0)
dyn sensors = [s1, s2]
sensors[0].set(99.0)
// s1.get() → 5.0   — s1 is unaffected; sensors[0] is an independent copy
// sensors[0].get() → 99.0
// sensors[1].get() → 0.0  — copied from s2 at creation time
```

*Isolation invariant: placing a Block into a dyn is equivalent to a typeof clone. The original variable and the dyn element are fully independent from the moment of assignment.*

**VAL_PTR — Opaque C Pointer:** dyn can store opaque pointers returned by FFI. The GC never touches VAL_PTR. Responsibility for freeing belongs to the user via `danger`.

```fluxa
danger {
    dyn handle = libpng.open("photo.png")    // VAL_PTR
    libpng.close(handle[0])                  // only valid use: pass back to C
}
```

**prst dyn:** A dyn marked as prst survives hot reloads. Primitive elements (int, float, str, bool) are serialized in PrstPool. VAL_BLOCK_INST elements are not serializable to Flash (RP2040) — they survive only in HANDOVER_MODE_MEMORY (x86/ARM64) where the heap pointer is preserved. Block elements in a prst dyn are independent copies (same as typeof) — they are not references to the original Block variables.

```fluxa
prst dyn history = [10, 20, 30]             // primitives serialized on reload
prst dyn data    = csv.load("sales.csv")    // opaque pointer — survives in memory
```

### 3.4 Logical Operators

Fluxa supports `&&`, `||`, and `!` with short-circuit semantics. The right operand of `&&` is only evaluated if the left is true. The right operand of `||` is only evaluated if the left is false.

Precedence (highest → lowest): `!` > comparisons > `&&` > `||` > assignment.

```fluxa
bool a = true
bool b = false
if a && !b { print("ok") }
if a || b  { print("either") }
bool inv = !a    // false
```

### 3.5 Type Enforcement

Declared types are checked at runtime via `rt_type_check()`. Covers `NODE_VAR_DECL`, `NODE_ASSIGN`, and `NODE_ARR_ASSIGN`. Violation emits an error with line number and aborts (fail-fast outside danger; captured in `err` inside danger).

```fluxa
int x = 10
x = "text"    // [fluxa] Runtime error (line 2): type error: expected int, got str
```

### 3.6 nil

Represents absence of value. Functions without a return value declare nil as their return type. nil is not assignable to typed variables.

---

## 4. Variable Declaration

Static typing always precedes the identifier. No untyped declaration, no inference.

### 4.0 Scope and Ownership

Fluxa has **no global variables**. Every value has exactly one owner. Ownership is the organizing principle of the entire language.

A variable declared at the script body level belongs to the script execution context. It lives for the duration of that execution and dies when the context ends. Functions receive values as arguments and return values — there is no shared mutable state between them except through `prst` (which persists across reloads) or `Block` members (which are explicitly encapsulated).

```fluxa
int x = 10         // x belongs to this execution context

fn double(int n) int {
    return n + n   // n is a parameter — x is not visible here
}

// To use x in a function, pass it explicitly:
int result = double(x)
```

This is not a limitation — it is a feature. It eliminates an entire class of bugs caused by hidden state coupling between functions. Every data flow in a Fluxa program is explicit.

The only cross-call state mechanisms are:
- `prst` — survives hot reloads, stored in PrstPool. Belongs to the scope where it was declared (main program, module, or Block). Not shared globally — each scope owns its own `prst` variables.
- `Block` members — state encapsulated within a Block, accessible through that Block's methods

The runtime maintains the invariant that **every slot is the sole owner of its heap-bearing contents** through automatic release on reassignment and at scope teardown. For the full implementation contract and the `free(x)` operator, see [§13.6 Slot Ownership and the `free` Operator](#136-slot-ownership-and-the-free-operator).

```fluxa
int   a    = 10
float pi   = 3.14
str   name = "fluxa"
bool  active = true
prst int counter = 0    // survives reloads
```

### 4.1 prst — Persistent State

`prst` marks a variable to survive hot reloads. Without `prst`, every variable dies and is reborn on each reload.

- `prst` belongs to the scope where it was declared: main, module, or Block
- `prst` cannot change type between reloads — state error (ERR_RELOAD)
- Removing a `prst` declaration atomically invalidates all state and execution that depended on it
- Invalidation is recursive — if A depends on B (prst) and B disappears, A dies too
- No grace period, no tombstone, no transition cycle

*prst contract: A removed prst variable atomically invalidates all state and execution that depended on it. Interruption is immediate and total.*

#### Initializer replay on reload

On a reload the restore path compares the declared initializer against the
baseline recorded in the pool, so that editing a declaration wins over the value
the previous run left behind:

```fluxa
prst int base = 12      // runtime mutated it to 41
prst int base = 99      // after the edit and a save: 99 wins, not 41
```

Evaluating that initializer is only safe when doing so has no side effects and
produces a value the comparison can read. The initializer is therefore replayed
**only when it is a literal or a binary expression of literals** — `int`,
`float`, `bool`, `str`. Anything else — a std lib call, an FFI call, a Block
method, a `dyn` literal — is **not** re-evaluated, and the pooled value wins
unconditionally.

This is what makes a resource handle usable as persistent state. Replaying
`graph.init(800, 600, "app")` would open a second window and then discard it,
because the comparison has no branch for `dyn`; the same shape would reopen a
database with `sqlite.open` or reallocate a cursor with `csv.open`.

*Consequence: editing a non-literal initializer has no effect until the next
process. `prst str label = strings.concat("v", n)` keeps its pooled value across
reloads no matter how the source is edited.*

#### The two boundaries: reload vs. runtime swap

`prst` answers two different questions, and the answers differ:

| | in-process reload (`-dev`, `apply`) | runtime swap (`handover`, `update`) |
|---|---|---|
| `int`, `float`, `bool`, `str`, `arr` | preserved | preserved — carried by the snapshot |
| `dyn` holding an external resource | **same handle** — same window, same cursor | **rebuilt** — a new window is born |

The dividing line is the snapshot, not the process. An in-process reload hands
the pool over in memory, so a pointer stays valid and the window never blinks. A
runtime swap serializes — and the wire format has no `VAL_DYN` case, because a
pointer survives neither `execve` nor, in `HANDOVER_MODE_FLASH`, a reboot. Note
that Stage 2 (§10.2) serializes even though it never leaves the process, so a
resource is rebuilt there too.

Deserialization leaves `VAL_NIL` while `declared_type` still reads `VAL_DYN`.
That combination is a **headstone**, and it is the signal to rebuild the
resource from its declaration rather than restore a handle that no longer
exists. Measurements, counters, and every serializable value are restored either
way — a sensor loop keeps its accumulated readings across a firmware upgrade
while its display reopens.

**External OS handles stay plain `int`.** A socket or PostgreSQL connection is
not a `dyn` cursor and must not be marked `prst`: the persistence layer would
restore a handle number the OS has already reclaimed, producing *"Address
already in use"* on restart. The durable thing is the startup code that recreates
them.

### 4.2 PrstPool, PrstGraph, and Resolver Caps

The initial size of the prst variable pool and the dependency graph is configurable via `fluxa.toml`. Both structures are dynamic — grow via realloc without a fixed ceiling. The resolver scope pool is also configurable, but is a true capacity (see below).

```toml
[runtime]
gc_cap         = 1024   # GC hard cap (static array)
prst_cap       = 64     # PrstPool initial capacity (dynamic, grows via realloc)
prst_graph_cap = 256    # PrstGraph initial capacity (dynamic, max 65536)
scope_cap      = 256    # resolver scope pool size (default 256, floor 256, max 65536)
module_cap     = 32     # max imported live/static modules (default 32, 1..4096)
ast_pool_cap     = 4096   # AST node arena size (default 4096, range 4096..1048576)
ast_str_pool_cap = 65536  # AST string arena bytes (default 65536, range 65536..16777216)
str_concat_cap = 8388608 # max bytes one strings.concat may build (default 8 MiB, 4096..256 MiB)
str_autogrow   = false  # if true, a concat over str_concat_cap grows instead of erroring
```

*`prst_cap` and `prst_graph_cap` are initial caps, not ceilings. Structures grow automatically. Setting the correct initial cap improves allocation performance — it does not limit usage.*

**`scope_cap`** sizes the resolver's lexical-scope pool: the number of scopes the resolver can allocate for a single program. A scope is allocated for **each top-level function, each Block, and each Block method** — and only those; `if`/`while` bodies do not consume a scope. Unlike the prst caps, this pool does **not** grow — when a program needs more scopes than `scope_cap`, name resolution aborts cleanly (no crash, no partial state) with *"aborting due to resolver errors"* and no symbol name, since the failure is capacity rather than a bad reference. The default is 256, which suits small and medium programs; a large codebase can exceed it and must raise `scope_cap`. Values below 256 are clamped up to 256 so the setting can never be weaker than the built-in default; the nominal maximum is 65536.

The scope count of a program is:

```
scopes = (top-level functions) + (Blocks) + (Block methods)
```

**Memory characteristics.** The pool is allocated once per resolver run and freed as soon as resolution completes — before the program executes — so its cost is transient, not part of steady-state footprint. The allocation is lazy: only the scopes actually used touch physical pages, so an unfilled `scope_cap` costs nothing in resident memory. The reservation is *virtual*, however: each scope slot is roughly 130 KB (`sizeof(SymTable)`, dominated by an inline symbol array), so `scope_cap = 1024` reserves ~130 MB of address space and `scope_cap = 65536` reserves ~8 GB. On an overcommit host the unused reservation is harmless; on a target without overcommit or a memory-constrained embedded build, an oversized `scope_cap` can make the pool allocation fail (handled by a graceful abort). Set `scope_cap` a margin above the real scope count, not to the maximum.

**`module_cap`** bounds how many `import live` / `import static` modules `main.flx` may load. The default is 32. Because Fluxa targets small systems, the loader and the parser each allocate *exactly* `module_cap` slots — the module source list in the loader and the imported-namespace list in the parser — rather than reserving a large fixed array, so the memory cost tracks the actual limit set. Importing more modules than the cap aborts cleanly before execution with *"too many modules: the import limit is N (raise module_cap in [runtime])"*; nothing is partially loaded. Values are clamped to the range 1..4096. Only `live`/`static` module namespaces count against this; `std.*` libraries declared under `[libs]` do not.

**`ast_pool_cap`** and **`ast_str_pool_cap`** size the parser's AST arena (`ASTPool` in `pool.h`): the number of AST nodes (default 4096, 72 bytes each) and bytes of interned string storage (default 65536) allocated per parse/hot-reload cycle. Unlike `scope_cap`, exceeding either cap never aborts — the pool transparently falls back to per-item `malloc()`/`strdup()` for the remainder of that pool's life, exactly as it did before this option existed; raising the caps only avoids that per-item allocation overhead on programs whose AST regularly exceeds the default. At most one line is logged to stderr per pool life per overflow kind — the first time node allocation overflows, and separately the first time string allocation overflows — not once per allocation, so a large program's log stays readable even under heavy overflow. When left at the compiled-in default (4096 / 65536), the arena uses a fixed-size buffer embedded in the pool struct — no allocation and no performance cost beyond what existed before this option, verified by benchmark. A pool only allocates its arena on the heap when a non-default cap is configured. Values are clamped to 4096..1,048,576 nodes and 65536..16,777,216 bytes respectively.

**`str_concat_cap`** bounds the size, in bytes, of the result a single `strings.concat` call may build. The default is 8 MiB, chosen to match common HTTP request-body limits (and this project's own 8 MiB server body limit) so that a base64-encoded file — the largest thing an ordinary program concatenates — fits comfortably. `strings.concat` allocates its result to fit (two passes: measure, then build), so there is no truncation; the cap is a **safety limit**, not a buffer size. A concat whose result would exceed the cap raises an error — *"str.concat: result N bytes exceeds str_concat_cap M"* — rather than performing a very large allocation driven by (possibly untrusted) input. Setting **`str_autogrow = true`** changes this: an over-cap concat then allocates anyway, for programs that legitimately build large strings and accept the cost. `str_concat_cap` is clamped to 4096..256 MiB; below 4096 would break ordinary string building. This makes the concat path bounded by default while remaining flexible when a program genuinely needs it.

---

## 5. Control Flow

All control blocks require braces. No single-line shorthand without braces.

### 5.1 if / else

```fluxa
if x > 5 { print("high") } else { print("low") }
```

### 5.2 while

```fluxa
int i = 0
while i < 3 {
    print(i)
    i = i + 1
}
```

### 5.3 for x in arr / dyn

Iterates over all elements of an arr or dyn. The loop variable exists only inside the body.

```fluxa
int arr nums[3] = [10, 20, 30]
for n in nums  { print(n) }

dyn events = [1, "two", true]
for e in events { print(e) }
```

---

## 6. Functions

The return type is mandatory and declared at the end of the signature. TCO (Tail Call Optimization) supports mutual recursion without stack overflow.

```fluxa
fn add(int a, int b) int { return a + b }

fn ping(int n) int {
    if n <= 0 { return 0 }
    return pong(n - 1)    // tail call → TCO
}
```

---

## 7. Block and typeof

Block is Fluxa's unit of encapsulation. Groups state and behavior without inheritance, hierarchy, or implicit coupling.

### 7.1 Definition and Own State

```fluxa
Block Counter {
    prst int total = 0      // persists across hot reloads
    int   step    = 1       // valid non-prst field — dies on reload
    fn increment() nil { total = total + step }
    fn value() int     { return total }
}

Counter.increment()
Counter.total = 10    // direct field access
```

Fields inside a Block can be declared with or without `prst`:
- `prst int x = 0` — survives hot reloads, stored in PrstPool
- `int x = 0` — valid, lives for the duration of the execution, dies on reload

### 7.2 Block field restrictions

The following are **not permitted** inside a Block:

**No `dyn` fields.** `dyn` has no declared type and carries ownership ambiguity. Block fields must have a declared type: `int`, `float`, `str`, `bool`, `char`, `arr`. If you need a `dyn` cursor (file handle, DB connection, etc.) as state, keep it in the main program scope and pass it as an argument to Block methods.

```fluxa
// WRONG — dyn as Block field
Block Worker {
    prst dyn conn = [0]     // ERROR: dyn is not a valid Block field type
}

// CORRECT — int handle as Block field, dyn lives outside
int conn = 0
danger { conn = pg.connect("...") }

Block Worker {
    prst int count = 0
    fn record(float v) nil { count = count + 1 }
}
```

**`danger` and `dyn` work inside Block methods, but not as Block fields.** A Block
body accepts only typed field declarations and `fn` methods; a loose `danger`
statement or a `dyn` field declaration there is a parse error. Inside a **method**,
both work normally — and that is the idiomatic place for a Block that owns fallible
IO: the method opens the resource inside `danger`, updates typed fields, and closes
with `if err != nil`. When a Block method runs `danger`, the captured error belongs
to that method's `err` check, immediately after the block.

```fluxa
// WRONG — dyn/danger as a Block field (parse error)
Block Bad {
    dyn buffer = [0]                 // ERROR: dyn is not a valid field type
    danger { int y = 5 }             // ERROR: only fields and fn methods allowed here
}

// CORRECT — danger and dyn inside a method (idiomatic)
Block Store {
    prst int count = 0
    fn load() nil {
        danger {
            dyn db = sqlite.open("data.db")
            dyn rows = sqlite.query(db, "SELECT v FROM readings")
            count = len(rows)
            sqlite.close(db)
        }
        if err != nil { print(err[0]) }
    }
    fn get() int { return count }
}
```

**No nested Block declarations.** A Block cannot contain another Block declaration. Compose behavior via methods and arguments, not nesting.

### 7.3 typeof — Cloning with Isolated State

`typeof` creates a new independent instance. Copies structure and values from the code. Does not copy current runtime state. `typeof` can only be applied to a defined Block — never to another instance.

```fluxa
Block c1 typeof Counter
Block c2 typeof Counter
c1.increment()    // c1.total == 1, c2.total == 0
Block c3 typeof c1    // ERROR: instance cannot be the origin of typeof
```

*Total isolation: A.x and instance.x are independent symbols. Changing one does not affect the other.*

---

## 8. Errors and Risk Control

### 8.1 Line Numbers in Errors

Errors include the source line number where the error occurred. This applies to both errors outside danger (stderr) and errors inside danger (ErrEntry.line).

```fluxa
int x = 1 / 0
// → [fluxa] Runtime error (line 1): division by zero

danger {
    int y = arr[99]
    // err[0] contains: message + context + line
}
```

### 8.2 Default Mode — Fail-Fast

Any operation that fails outside a `danger` block aborts execution immediately with a line number.

### 8.3 danger Block

Isolates risky operations. A failure inside the block does not abort the program —
it is captured into the `err` ring. But execution of the block **stops at the first
error**: statements after the failing line do not run. Control resumes right after
the `danger` block, where the idiom is to check `err` and decide what to do. `danger`
is mandatory for `import c` FFI calls and for all file/network/database IO.

```fluxa
danger {
    print("before")
    float r = libm.sqrt(-1.0)    // error captured in err; the block stops here
    print("after")               // does NOT run
}
if err != nil { print(err[0]) }  // the idiom: every danger closes with a decision
```

`danger` is intentional: it marks a region where failure is acceptable *and
handled*. A `danger` block without an `if err != nil` right after it is code that
was contained but never checked — the program then proceeds on inconsistent state.

### 8.4 err — Error Ring

- `err[0]` → most recent error; `err[1]`, `err[2]`, … → earlier errors when several
  exist in the same context. Ring buffer of 32 entries; when full, the oldest entry
  is pushed out first.
- Each entry: message + context (fn/Block) + source line.
- `err` is cleared to nil before each `danger` block — errors do not carry across
  blocks.
- Because a block stops at its first error, you normally see one error per block in
  `err[0]`; higher indices matter when errors from different contexts accumulate
  before you check.

| ErrKind | When generated |
|---|---|
| ERR_FLUXA | Fluxa runtime error: div/0, OOB, undefined variable |
| ERR_C_FFI | FFI call failure via import c |
| ERR_RELOAD | prst type collision or invalidation during reload |
| ERR_HANDOVER | Dry Run validation failure (Atomic Handover) |

---

## 9. Hot Reload

Hot reload is a first-class citizen. The runtime maintains a dependency graph (PrstGraph) between prst variables and active executions.

### 9.1 Import Types

Fluxa supports four import forms:

- `import std <lib>` → opt-in standard library (declared in `[libs]` of fluxa.toml)
- `import c <lib>`   → C FFI via dlopen/libffi — only valid inside `danger` blocks
- `import live <name>` → load a Fluxa module from `live/<name>.flx` (or `<module_root>/live/<name>.flx`)
- `import static <name>` → load a Fluxa module from `static/<name>.flx`

See §9.5 for the module system.

### 9.5 Module System

Fluxa v0.15 introduces `import live` and `import static` for organizing code across multiple `.flx` files. Modules are a **lens of organization**, not a runtime concept. They do not introduce new scoping rules, new runtime objects, or new execution phases.

#### How it works

The module loader runs entirely before the lexer. When `main.flx` contains `import live sensor`, the loader:

1. Reads `live/sensor.flx`
2. Prefixes all top-level declarations with the namespace: `fn set(...)` → `fn sensor__set(...)`
3. Inserts these declarations into the program **before** the main body
4. Registers `sensor` as a known namespace in the parser
5. When the parser sees `sensor.set(42.0)` in main, it emits `NODE_FUNC_CALL "sensor__set"` — a plain function call

The result is a single flat program. The runtime sees no modules — only functions and Block declarations with mangled names.

#### Syntax

```fluxa
// main.flx
import live sensor      // loads live/sensor.flx
import static utils     // loads static/utils.flx

Block s typeof sensor.Sensor
s.set(3.14)
print(s.get())
print(utils.double(7))
```

```fluxa
// live/sensor.flx
Block Sensor {
    prst float reading = 0.0
    fn set(float v) nil { reading = v }
    fn get() float       { return reading }
}
```

```fluxa
// static/utils.flx
fn double(int n) int { return n + n }
fn clamp(int v, int lo, int hi) int {
    if v < lo { return lo }
    if v > hi { return hi }
    return v
}
```

#### Convention: live vs static

The `live/` and `static/` directories are convention, not enforcement. The runtime treats them identically. The distinction communicates intent:

| Directory | Convention |
|---|---|
| `live/` | Modules with `prst` state — designed for hot reload |
| `static/` | Pure function modules — no state, just logic |

#### Namespace isolation

The double-underscore separator (`__`) prevents accidental collision between module names and user identifiers:

```fluxa
import static utils
int utils__double = 99       // user variable — different from utils.double
print(utils.double(3))       // → 6  (calls utils__double fn)
print(utils__double)         // → 99 (accesses user variable)
```

#### Why single-level imports only

Fluxa deliberately limits imports to `main.flx`. Modules cannot import other modules.

This is not a missing feature — it is a design decision:

- **No namespace hell.** In systems where modules can import modules, understanding what is in scope requires tracing a graph of imports. In Fluxa, everything in scope is declared in one of two places: the current file, or a module listed at the top of `main.flx`. No transitive dependencies, no diamond problems, no hidden coupling.

- **No circular dependencies.** Circular imports between modules are structurally impossible when imports are one-level only.

- **Complete visibility.** The top of `main.flx` is a complete manifest of everything the program depends on. Reading 10 lines tells you everything the program uses.

- **Consistent with Fluxa's philosophy.** *Simple > Complete. Local Control > Global Magic.* A module system that requires a build tool to understand is not simple. Fluxa's module system fits in a few lines of `main.c`.

The solution to "I need to share code between modules" is the same as in any Fluxa code: extract it to a module that both import from `main.flx`, or pass the value explicitly.

#### Block declarations in modules

A `Block` declared in a module is namespaced automatically:

```fluxa
// live/devices.flx
Block Motor {
    prst int rpm = 0
    fn set_rpm(int v) nil { rpm = v }
    fn get_rpm()      int { return rpm }
}
```

```fluxa
// main.flx
import live devices
Block m typeof devices.Motor   // typeof accepts namespace.BlockName
m.set_rpm(1200)
print(m.get_rpm())
```

#### module_root — custom path resolution

By default, `import live sensor` resolves to `live/sensor.flx` relative to the CWD. A project with a different layout can configure the root:

```toml
[project]
module_root = "src"
# import live sensor → src/live/sensor.flx
```

#### Scope rules in modules

Modules follow the same scope rules as the rest of Fluxa. There are no module-level globals. Functions in modules receive all values as arguments and return values. State lives in `Block` members (via `prst`).

```fluxa
// live/counter.flx
Block Counter {
    prst int count = 0
    fn increment() nil { count = count + 1 }  // count is a Block member
    fn get()       int { return count }
    fn reset()     nil { count = 0 }
}
```

```fluxa
// main.flx
import live counter
Block c typeof counter.Counter
c.increment()
c.increment()
print(c.get())    // 2
```

### 9.2 Execution Mode — Script vs Project

Fluxa has two internal modes determined automatically at parse time:

| Internal Mode | Condition | Characteristics |
|---|---|---|
| FLUXA_MODE_SCRIPT | No `prst` declarations anywhere in the file | PrstPool and PrstGraph not instantiated. Zero overhead. No toml required. |
| FLUXA_MODE_PROJECT | At least one `prst` declaration | PrstPool + PrstGraph active. Reads `fluxa.toml`. Reload-capable. |

### 9.3 Runtime Flags — How You Invoke Fluxa

The runtime flag controls lifecycle behavior, independent of SCRIPT/PROJECT mode:

| Flag | Command | Behavior |
|---|---|---|
| *(none)* | `fluxa run file.flx` | **Default** — run once, exit. Auto-detects SCRIPT vs PROJECT mode. No file watcher, no IPC server. Ideal for one-shot programs, tools, and development iteration. |
| `-dev` | `fluxa run file.flx -dev` | **Dev mode** — starts IPC server + file watcher. On every save, the runtime reloads automatically, preserving `prst` state. Hot reload is instant: parse → resolver → dry run → apply. Errors are logged but do not crash the watcher loop. |
| `-prod` | `fluxa run file.flx -prod` | **Prod mode** — starts IPC server, no file watcher. Reload is manual via `fluxa apply` or `fluxa handover`. Designed for long-running production deployments. With `make build-secure` + `FLUXA_SECURE=1`, adds IPC hardening: rate limiting, RESCUE_SOFT/HARD, configurable timeouts and connection caps. |
| `-p` | `fluxa run file.flx -p` | **Preflight** — parse + resolve only, no execution. Validates syntax, name resolution, and type declarations. Exits with code 0 (ok) or 1 (errors). Use in CI before deploying. |

```bash
# Development workflow
fluxa run main.flx -dev           # watch + hot reload on save

# Production deployment
fluxa run main.flx -prod          # IPC server, manual reload
fluxa apply new.flx               # reload preserving prst state
fluxa handover old.flx new.flx    # atomic 5-step handover

# CI validation
fluxa run main.flx -p             # parse + resolve only, exit 0/1
```

*`-dev` and `-prod` both require at least one `prst` declaration to activate the PrstPool. A pure script (no `prst`) runs identically under any flag — prst infrastructure is never instantiated for FLUXA_MODE_SCRIPT programs.*

### 9.4 PrstGraph — Dependency Graph

Records which function/method read each prst variable. Dynamic — grows via realloc.

- `prst_graph_record(g, name, ctx)` — records with deduplication
- `prst_graph_invalidate(g, name)` — removes deps, readers re-register
- `prst_graph_checksum(g)` — FNV-32; used in Handover for integrity
- `prst_graph_init_cap(g, cap)` — initializes with configurable cap

### 9.5 Reload Behavior Table

| Action on Reload | Result | Guarantee |
|---|---|---|
| Keeps `prst int a` | Retains value in memory | prst contract |
| Removes `prst int a` | Atomic death of a and dependents | No ghost state |
| Changes type of `prst int a` | ERR_RELOAD, previous value preserved | Type immutable in prst |
| Changes fn signature | fn restarted as new execution | No previous iteration state |
| Changes Block definition | Block root + instances invalidated | Cascade invalidation |

---

## 10. State as a First-Class Citizen

In most systems, state is the thing you work around when deploying: drain connections, flush queues, stop the process, restart, rebuild context. Fluxa inverts this — **`prst` state is the one thing that is never disrupted, at any level of the system**.

A `prst` variable declared in a Fluxa program is not bound to a script file, a binary version, or a process. It is bound to the **runtime identity** of the system itself. You can replace the script, the configuration, the binary, or the entire runtime without losing a single `prst` value. This is not a feature bolted on — it is the organizing principle of everything in sections 10 through 13.

This commitment is expressed through three stages of live update. Each stage replaces more of the system than the last. None of them lose state:

| Stage | Command | What changes | State preserved |
|---|---|---|---|
| **Stage 1 — Script Swap** | `fluxa apply` | The `.flx` program file | ✅ all `prst` vars |
| **Stage 2 — Atomic Handover** | `fluxa handover` | Script + `fluxa.toml` + lib config | ✅ all `prst` vars |
| **Stage 3 — Runtime Swap** | `fluxa update` | The `./fluxa` binary itself | ✅ all `prst` vars |

Every stage uses the same underlying mechanism — `prst_pool_serialize` / `prst_pool_deserialize` — writing a flat binary snapshot that is safe for Flash storage (RP2040) and for passing across `execve` (Stage 3). The scope of what changes grows. The guarantee that state survives does not.

**Why three stages matter for systems that cannot stop:**

A sensor loop accumulating readings, a PID controller tracking position, a state machine managing a conveyor — none of these can tolerate restarts for software updates. Stage 1 handles most day-to-day deployments: fix a bug, tune a constant, restructure logic. Stage 2 handles version upgrades where the data model changes. Stage 3 handles the cases Stage 2 cannot: a vulnerability in the interpreter, a new stdlib lib, a compile-time flag change like `FLUXA_HUGEPAGES=1`. Together, the three stages cover the full lifecycle of a running system with zero forced restarts.

*Embedded equivalent: Stage 1 ≈ config/parameter reload; Stage 2 ≈ firmware patch with state migration; Stage 3 ≈ full firmware update via HANDOVER_MODE_FLASH on RP2040.*

---

### 10.1 Stage 1 — Script Swap (`fluxa apply`)

The simplest form of live update. The running script is replaced with a new one; the `prst` pool migrates directly in memory.

**When to use:** Bug fixes, logic changes, parameter tuning — anything that does not require changing the binary or its config.

**Mechanism:** The runtime executes until a safe point (`call_depth==0 && danger_depth==0`), stops, parses the new script, migrates the `prst` pool, and resumes. Total gap: ~2–10ms (dominated by parse time, not migration).

```bash
fluxa run main.flx -prod      # start
fluxa apply new_main.flx      # swap script — state survives
fluxa apply new_main.flx -p   # preflight before applying
```

**State gap:**

| Component | Typical Time | Notes |
|---|---|---|
| Parse + Resolve of new script | ~1–5ms | Proportional to file size |
| PrstPool migration | ~1–50µs | Proportional to prst var count |
| Wait for safe point | 0 to 1 cycle | Worst case: 1 full loop iteration |
| **Total typical** | **~2–10ms** | Dominated by parse, not migration |

> **Implementation note.** The in-memory migration described above is the design
> intent, and it is what the `-dev` reload path does today. The `fluxa apply`
> CLI, however, runs in a **new process**: it calls `runtime_apply(program, NULL)`
> with an empty pool, and there is no `IPC_OP_APPLY` opcode for a running
> runtime to receive a script swap. Programs that need state to survive a swap
> should use `-dev` (in-process reload) or Stage 2 (`fluxa handover`), both of
> which do migrate the pool. This gap is documented rather than silently
> assumed.

---

### 10.2 Stage 2 — Atomic Handover (`fluxa handover`)

Replaces the script **and** the runtime configuration (`fluxa.toml`) in one atomic operation. Because `fluxa.toml` controls `[runtime]` parameters, `[libs]` selection, and GC tuning, Stage 2 is the right choice whenever the new version of the system is meaningfully different from the current one — not just a logic fix, but a version with a different shape.

**When to use:**
- New or removed `prst` variables (schema change)
- Different `[libs]` section (enabling a new std lib)
- Changed `[runtime]` parameters (GC cap, prst cap, warm profile budget)
- Any deployment where you want the Dry Run safety net before committing

**What Stage 2 provides that Stage 1 does not:** The 5-step protocol includes a **Dry Run** (Step 3) — the new program executes completely, with `print()` suppressed, before the old program is touched (see §10.7 for what the rehearsal does and does not suppress). If the new version has a runtime error, the handover is aborted and the system stays on the old version. Stage 1 (`fluxa apply`) has no such rollback.

**Mechanism:** Five-step protocol. Steps 1–3 run with Runtime A fully active — the gap occurs only at Step 4 (Switchover). The pool swap itself is a pointer operation — submicrosecond.

```bash
fluxa handover old.flx new.flx          # atomic handover
fluxa handover old.flx new.flx --grace 0  # mission-critical: zero grace
```

**5-Step Protocol:**

| Step | Name | What happens |
|---|---|---|
| 1 | Standby | Runtime B allocated. New program parsed and resolved. Failure here → B discarded, A intact. |
| 2 | Migration | PrstPool and PrstGraph from A serialized into flat binary snapshot. FNV-32 checksum calculated. Snapshot deserialized into B with validation. |
| 3 | Dry Run | B executes complete program with `dry_run=1`. `print()` suppressed; std lib calls really run (§10.7). Any error → handover aborted, A untouched. Resources opened by the rehearsal are marked as headstones so they never reach production. |
| 4 | Switchover | Waits for safe point in A (`call_depth==0 && danger_depth==0`). Pool from B transferred atomically. Requires a monotonic clock for the deadline — a target without one refuses the switchover and keeps A serving (§10.9). |
| 5 | Cleanup | Grace period (default 100ms). Temporary B destroyed. Execution resumes from B with transferred pool. |

*Central invariant: Runtime A is never modified during a handover attempt. Any failure in B destroys B and keeps A active without corruption.*

**State gap:**

| Component | Time | Notes |
|---|---|---|
| Steps 1–3 (Standby, Migration, Dry Run) | Zero | Runtime A continues active throughout |
| gc_collect_all() before swap | ~10–100µs | Proportional to GC objects |
| Atomic pool swap (pointer) | ~nanoseconds | Pointer operation — submicrosecond |
| grace_period_ms (default 100ms) | 100ms | Configurable. Use 0 for mission-critical. |
| **Total without grace period** | **~10–200µs** | Worst case on modern hardware |
| **Total with default grace period** | **~100ms** | Dominated entirely by grace period |

*Mission-critical: `fluxa handover --grace 0` or `grace_period_ms = 0` in fluxa.toml. The swap itself is submicrosecond.*

---

### 10.3 Stage 3 — Runtime Swap (`fluxa update`)

The deepest form of live update: replaces the `./fluxa` binary itself. When Stage 2 is not enough — because the change is in the interpreter, not the program — Stage 3 is the answer. The running Fluxa program and all its `prst` state survive the binary replacement unchanged.

Stage 3 reaches places Stage 1 and Stage 2 cannot:

| Need | Stage 1 | Stage 2 | Stage 3 |
|---|---|---|---|
| Fix a bug in `.flx` logic | ✅ | ✅ | ✅ |
| Add/remove `prst` variable | ✗ | ✅ | ✅ |
| Change `[libs]` in fluxa.toml | ✗ | ✅ | ✅ |
| Patch interpreter bug (runtime.c) | ✗ | ✗ | ✅ |
| Add new stdlib lib (requires rebuild) | ✗ | ✗ | ✅ |
| Enable `FLUXA_HUGEPAGES=1` or `FLUXA_SECURE=1` | ✗ | ✗ | ✅ |
| Apply a CVE fix to the binary | ✗ | ✗ | ✅ |

**When to use:** Runtime upgrades, security patches to the interpreter, adding new stdlib libs, `FLUXA_HUGEPAGES` or other compile-time flag changes that require a rebuild.

**Mechanism:** The old binary serializes its `prst` pool to a temp file, then calls `execve(new_binary)` passing `FLUXA_RESTART_SNAPSHOT` as an environment variable. The new binary detects this on startup, loads the snapshot, and continues execution. The old process is replaced — not killed.

```bash
fluxa update ./fluxa_v2          # replace running binary
fluxa update ./fluxa_v2 -p       # preflight: verify binary first
```

**Protocol:**

```
CLI                    IPC Socket           Runtime (old)         New binary
 │── IPC_OP_UPDATE ────►│── dispatch ─────►│                          │
 │   payload: /path     │                  │  1. UID check (always)   │
 │   to new binary      │                  │  2. path traversal guard │
 │                      │                  │  3. wait safe point      │
 │                      │                  │  4. serialize prst pool  │
 │                      │                  │  5. write to /tmp/*.snap │
 │◄── IPC_RESP OK ──────│◄── reply ────────│                          │
 │                      │                  │  6. execve(new_binary)   │
 │                      │                  │──────────────────────────►│
 │                      │                  X (old process replaced)   │
 │                      │                                             │
 │                      │                           detect FLUXA_RESTART_SNAPSHOT
 │                      │                           load prst from snapshot
 │                      │                           delete snapshot file
 │                      │                           continue execution
```

**No Dry Run in Stage 3.** Unlike Stage 2, there is no dry run of the new binary before execve — a binary cannot be safely pre-executed in isolation. The equivalent safety check is the preflight flag (`-p`), which verifies the binary's ELF/Mach-O magic header before sending the update request.

**IPC opcode:**

```c
IPC_OP_UPDATE = 0x07   /* payload: new binary path in req.name field */
```

**Security model:**

`IPC_OP_UPDATE` is the highest-privilege opcode — it executes `execve`. Defense in depth:

| Layer | Enforcement |
|---|---|
| Socket permissions | `/tmp/fluxa-<pid>.sock` is `0600` — only owner can connect |
| UID check | `check_peer_uid()` called **always**, regardless of `FLUXA_SECURE` |
| Path validation | Must be absolute, no `..` components |
| Binary check | Must exist and be executable (`S_IXUSR`) |
| Error messages | Generic to client — details only to `stderr` (prevents filesystem oracle attacks) |
| FLUXA_SECURE | Requires `<binary>.sig` alongside new binary if `security.mode != OFF` |
| Safe point | `call_depth == 0 && danger_depth == 0` before snapshot |
| Preflight `-p` | Verifies ELF/Mach-O magic before sending update request |

---

### 10.4 Snapshot Format (shared by all three stages)

All three stages use the same flat binary snapshot format — safe for writing to Flash (RP2040) and for passing across `execve` (Stage 3).

```
HandoverSnapshotHeader {
    magic          uint32   // 0xF10A8888
    version        uint32   // FLUXA_HANDOVER_VERSION
    pool_checksum  uint32   // FNV-32 of PrstPool before serialization
    graph_checksum uint32   // FNV-32 of PrstGraph before serialization
    pool_size      uint32   // bytes of serialized pool
    graph_size     uint32   // bytes of serialized graph
    pool_count     int32    // number of entries
    graph_count    int32    // number of deps
    cycle_count_a  int32    // cycle_count of A at snapshot time
}
[pool_size bytes — serialized PrstPool]
[graph_size bytes — serialized PrstGraph]
```

Stage 3 uses only the `PrstPool` portion (no graph needed for binary swap). Stages 1–2 use both pool and graph.

---

### 10.5 Protocol Versioning

- v1.000 — first stable beta version
- v1.xxx — compatible with v1.000 (same major)
- v2.000 — breaking change; rejects v1.xxx snapshots

---

### 10.6 RP2040 — Flash Mode

On hardware with limited SRAM (264 KB), two runtimes in parallel don't fit. `HANDOVER_MODE_FLASH` serializes the snapshot to a reserved Flash sector before rebooting. After boot, the new firmware reads the snapshot, deserializes state, runs the Dry Run, and only assumes control after approval. This is equivalent to Stage 3 (`fluxa update`) — a complete runtime replacement preserving all state.

| Mode | Platform | Behavior |
|---|---|---|
| HANDOVER_MODE_MEMORY | x86 / ARM64 | Two runtimes in parallel in RAM (Stages 1–2) |
| HANDOVER_MODE_FLASH | RP2040 (264 KB SRAM) | Snapshot → Flash → reboot → deserialize → dry_run (Stage 3 equivalent) |

---

### 10.7 Dry Run (`dry_run=1`)

Used in Stage 2 (Atomic Handover) step 3. When `dry_run = 1`, `print()` output is suppressed. Internal logic executes normally: loops, calculations, `prst` reads/writes happen and are validated. If `rt_error()` is called during a dry run, `ERR_HANDOVER` is generated in A and the handover is aborted.

**The Dry Run is a rehearsal that really runs.** `dry_run` gates `print()` and prevents `prst` mutations from being committed to the pool — it does **not** gate std lib calls. `graph.init()` during a dry run genuinely opens a window, `sqlite.open()` genuinely opens a database, and an HTTP request is genuinely sent. This is deliberate: a validation pass that stubbed out every library call would not validate much. The consequence is that a program whose top level performs an externally visible action performs it twice during a handover — once in the rehearsal, once for real.

**Nothing the rehearsal opens reaches production.** Step 3 collects Runtime B's GC while allowing the pool to survive into step 4, so any resource the rehearsal created is freed at the end of the rehearsal. Those pool entries are reset to headstones (`VAL_NIL` under the original `declared_type`, see §4.1) before the collect, which tells the runtime taking over to build the resource for real rather than inherit a freed handle. Serializable values pass through untouched, and the pool's checksum and serialized bytes are unchanged by the reset.


### 10.9 Platform clock (embedded targets)

The protocol reads a clock in exactly two places: the safe-point deadline in step 4 and the grace period in step 5. Neither is on the data path — the safe point itself is `call_depth == 0 && danger_depth == 0`, pure runtime counters. The clock only decides when to give up waiting.

POSIX hosts use `clock_gettime(CLOCK_MONOTONIC)`. A bare-metal target linked against newlib has neither that nor `nanosleep`, so when `FLUXA_EMBEDDED` is set without `FLUXA_HAS_POSIX_CLOCK`, both calls become weak hooks the SDK integration overrides:

```c
long fluxa_platform_ms_now(void);       /* monotonic ms since boot */
void fluxa_platform_sleep_us(long us);
```

| Target | Wiring |
|---|---|
| pico-sdk (RP2040) | `to_ms_since_boot(get_absolute_time())` and `sleep_us()` |
| esp-idf (ESP32) | already provides POSIX time — build with `-DFLUXA_HAS_POSIX_CLOCK=1` |
| POSIX host | unchanged, no hooks involved |

**Fail closed.** The weak default returns `-1`. A runtime with no clock must not invent a deadline and must not silently skip the wait, so step 4 refuses the switchover with `HANDOVER_ERR_SAFE_POINT`. Runtime A has not been touched at that point, so the device keeps serving on the old runtime and the upgrade is simply declined. Forgetting to wire the hook costs an upgrade, never the running service — the same rule every other step of the protocol follows.


## 11. CLI — Commands

```
fluxa run <file.flx>                Run — auto-detects script vs project
fluxa run <file.flx> -proj <dir>    Run as project (enables prst, reads fluxa.toml)
fluxa run <file.flx> -dev           Dev: watch + auto-reload on save (inotify/kqueue)
fluxa run <file.flx> -prod          Prod: manual reload via fluxa apply
fluxa run <file.flx> -p             Preflight: parse + resolve only, no execution
fluxa explain <file.flx>            Print prst state + dependency graph
fluxa dis <file.flx>                Static analysis: AST, warm forecast, hot
                                      bytecode, call order, prst fork — writes .dis
fluxa dis <file.flx> -o out.txt     Write dis report to explicit path
fluxa apply <file.flx>              One-shot reload preserving prst state
fluxa apply <file.flx> -p           Preflight before applying
fluxa handover <old> <new>          Atomic Handover: replace old.flx with new.flx
fluxa update <new_binary>           Runtime Update Protocol: replace the fluxa binary
fluxa update <new_binary> -p        Preflight: verify binary before sending update
fluxa test-handover                 Internal suite: validates all 5 protocol steps
fluxa observe <var>                 Stream live value of a prst variable (IPC)
fluxa set <var>=<value>             Mutate a prst variable in a live runtime (IPC)
fluxa logs                          Stream runtime log output (IPC)
fluxa status                        Snapshot: cycle count, prst count, errors, mode
fluxa init                          Create a new fluxa.toml in the current directory
fluxa ffi list                      List available shared libraries via ldconfig
fluxa ffi inspect <lib>             Generate suggested toml signatures for a library
fluxa runtime info                  Show current runtime configuration
fluxa keygen [--dir <path>]         Generate Ed25519 + HMAC keys for FLUXA_SECURE
                                      (only in fluxa_secure binary)
```

### 11.1 fluxa -dev: File Watcher

| OS | Backend | Mechanism |
|---|---|---|
| Linux | inotify | IN_CLOSE_WRITE \| IN_MOVED_TO |
| macOS / BSD | kqueue | EVFILT_VNODE NOTE_WRITE \| NOTE_ATTRIB |
| Others | select() | stat() mtime, 500ms interval |

**What is watched.** The entry file plus every module it imports — each
`import live <name>` and `import static <name>` resolved against `module_root`.
The set is rebuilt on every cycle, so adding or removing an import takes effect
on the next save. Saving `live/turtle.flx` reloads exactly as saving
`main.flx` does. A module that does not exist yet is skipped rather than
treated as an error.

**A script that returns is not restarted.** Normal termination is not a reload
trigger: the runtime is reaped and the watcher keeps waiting for a file change.
Only an actual save starts the next cycle. This matters for programs that hold
an external resource — a re-execution loop would open a new window every few
milliseconds.

**First-run state.** The pool produced by the very first cycle is carried into
the first reload, so a `prst` counter reads 1, 2, 3 across successive saves
rather than restarting once before it begins to count.

**One thread for the whole session.** Every execution runs on the same worker
thread, created once when `-dev` starts and reused for each reload. This is
required for correctness, not tidiness: anything holding thread-local state
would otherwise lose it when the previous run's thread exited. An OpenGL
context is the clearest case — it is current *per thread*, so a context made
current inside `graph.init` on the first run stopped being current anywhere
once that thread finished, and every subsequent GL call became a silent no-op:
the window froze on the last frame drawn, `graph.capture` returned a zeroed
buffer, and texture uploads failed. A reload therefore preserves both the
`prst` values and the thread-local state the libraries built on top of them.

Cancellation is cooperative. On a save the watcher raises the cancel flag and
waits for the current execution to unwind before starting the next one — the
two share the same `prst` pool, so they must not overlap. The flag stops loops
at the next check; execution continues from the statement after the loop, which
leaves room for a script to clean up.

### 11.2 IPC — Unix Socket

In `-prod` and `-dev` modes the runtime opens a Unix socket at `/tmp/fluxa-<pid>.sock` (mode `0600` — owner only). The protocol is a fixed-size binary struct — see `src/fluxa_ipc.h`.

| Opcode | Value | Command | Description |
|---|---|---|---|
| `IPC_OP_PING` | `0x01` | — | Health check |
| `IPC_OP_OBSERVE` | `0x02` | `fluxa observe` | Read prst var value |
| `IPC_OP_SET` | `0x03` | `fluxa set` | Mutate prst var |
| `IPC_OP_LOGS` | `0x04` | `fluxa logs` | Last N log entries |
| `IPC_OP_STATUS` | `0x05` | `fluxa status` | Runtime snapshot |
| `IPC_OP_EXPLAIN` | `0x06` | `fluxa explain` | All prst vars + dep graph |
| `IPC_OP_UPDATE` | `0x07` | `fluxa update` | Runtime Update Protocol |

### 11.3 fluxa dis — Static Disassembler

`fluxa dis` is a static analysis tool. It parses and resolves a `.flx` file without executing it, then writes a `.dis` report covering seven sections. It is available both as a subcommand of the main binary and as a standalone binary (`fluxa_dis`).

```bash
fluxa dis examples/problems/07_dijkstra.flx         # writes dijkstra.dis
fluxa dis examples/problems/07_dijkstra.flx -o out.txt  # explicit path

# Standalone binary (same output)
fluxa_dis examples/problems/07_dijkstra.flx
```

**Seven sections in the .dis report:**

| Section | Content |
|---|---|
| **1. AST** | Every node: type, source line, `warm_local` flag, `resolved_offset`. Shows the full parse tree. |
| **2. Warm Forecast** | Per-function: PROMOTABLE or COLD-LOCKED, cold bytes/read vs warm bytes/read, savings estimate based on actual prst var count. |
| **3. Hot Path — Bytecode VM** | Real VM instructions for `while`/`if` bodies: opcodes, registers, constants, jumps. Only shows functions with compilable loops. |
| **4. Call Order** | Call graph per function: direct calls, recursive calls, mutual recursion (DFS cycle detection), topological order for DAG programs. |
| **5. prst Fork** | All `prst` vars with owner and line. Shows what state dies atomically if each var is removed. |
| **6. Execution Paths** | Per-function tier summary: Tier 0/1/2 eligibility, bytes/read, TCO flag. |
| **7. Statistics** | Total AST nodes, functions (promotable), WarmProfile heap usage, VM eligibility, TCO presence. |

**Build:**

```bash
make build-dis    # produces ./fluxa_dis
```

`fluxa dis` (subcommand) is compiled into the main `./fluxa` binary — no separate build needed. `fluxa_dis` is a standalone binary that doesn't link the runtime, IPC, or stdlib — useful for CI pipelines where you only want static analysis.

### 11.4 Builds

Fluxa has three build targets with different capabilities:

| Target | Command | Binary | Use case |
|---|---|---|---|
| Standard | `make build` | `./fluxa` | Development, testing, general use. All stdlib + FFI. |
| Secure | `make build-secure` | `./fluxa_secure` | Production deployments requiring IPC hardening. |
| Disassembler | `make build-dis` | `./fluxa_dis` | Static analysis only. No runtime, no IPC, no stdlib. |

#### Standard build — `./fluxa`

The default binary. Includes all stdlib, FFI, IPC server, and hot reload. Zero security overhead — IPC accepts connections from the owner UID with a 100ms timeout and no rate limiting.

```bash
make build
./fluxa run main.flx -dev
```

#### Secure build — `./fluxa_secure` (`FLUXA_SECURE=1`)

Compiled with `-DFLUXA_SECURE=1`. Identical to the standard build in all functionality, but the IPC server adds:

- **50ms handshake timeout** (configurable via `handshake_timeout_ms` in `[security]`)
- **Connection cap** of 16 simultaneous connections (configurable via `ipc_max_conns`)
- **Two-level RESCUE mode:**
  - `RESCUE_SOFT` — activates when invalid burst threshold is exceeded. Silent drop on bad packets. Self-clears via leaky bucket decay when the attack stops.
  - `RESCUE_HARD` — activates when burst hits threshold while already in RESCUE_SOFT. Starts an immune drain timer (`IPC_RESCUE_DRAIN_SEC = 30s`). During HARD, new attack packets **do not** increment the counter — the attacker cannot extend the drain. Auto-clears after 30s.
- **Silent drop** — in any RESCUE level, invalid connections receive no response. The attacker sees a timeout, not a rejection. No information about detection is leaked.
- Valid operator commands (correct magic + same UID) pass through at both RESCUE levels.

```bash
make build-secure
./fluxa_secure keygen --dir /etc/fluxa/keys    # generate keys first
./fluxa_secure run main.flx -prod              # hardened prod runtime
```

**Generating keys (`fluxa keygen`):**

```bash
./fluxa_secure keygen --dir /etc/fluxa/keys
# Produces:
#   /etc/fluxa/keys/signing.key         (Ed25519 private, 0400)
#   /etc/fluxa/keys/signing.pub         (Ed25519 public,  0444)
#   /etc/fluxa/keys/signing.fingerprint (hex fingerprint, 0444)
#   /etc/fluxa/keys/ipc_hmac.key        (HMAC-SHA512 secret, 0400)
```

Keys are raw bytes — NEVER stored inline in `fluxa.toml`. The toml stores only file paths:

```toml
[security]
signing_key          = "/etc/fluxa/keys/signing.key"
ipc_hmac_key         = "/etc/fluxa/keys/ipc_hmac.key"
mode                 = "strict"        # off | warn | strict
handshake_timeout_ms = 50             # increase for slow/embedded links
ipc_max_conns        = 16             # increase for high-traffic deployments
```

Security modes:

| Mode | Behavior |
|---|---|
| `off` | No validation. Default for dev builds. |
| `warn` | IPC starts, logs `security mode=warn`. Key files validated at startup. |
| `strict` | Startup fails if `signing_key` is missing or unreadable. Enforces all checks. |

`./fluxa` (standard build) ignores `[security]` entirely — the section is parsed but has no effect without `FLUXA_SECURE=1`.

#### Disassembler build — `./fluxa_dis`

A minimal binary for static analysis. Does not link the runtime, IPC server, or any stdlib. Safe to run in CI on arbitrary `.flx` files without executing them.

```bash
make build-dis
./fluxa_dis src/main.flx              # writes src/main.dis
./fluxa_dis src/main.flx -o /tmp/out  # explicit output
```

---

## 12. FFI (C)

Integration with C is permitted exclusively inside `danger` blocks. Implemented via dlopen/libffi — zero static linking overhead.

Libraries declared in `[ffi]` of `fluxa.toml` are loaded automatically at runtime boot. `import c` is only needed for libraries NOT declared in the toml (ad-hoc loading):

```fluxa
// Option A: declared in fluxa.toml [ffi] — no import needed
danger {
    float r = libm.sqrt(16.0)    // libm loaded automatically from toml
}

// Option B: ad-hoc — not in toml, imported at runtime
import c libpng
danger {
    dyn handle = libpng.open("photo.png")
}
```

*If an FFI call writes to a prst variable during the Handover dry_run, the write occurs in the isolated state of B and does not contaminate A.*

### 12.1 Pointer Type Mapping

The runtime reads C signatures from `fluxa.toml` before making the libffi call. Pointer parameters are handled automatically — the user writes plain Fluxa variables and the runtime passes addresses and writes results back. No `ref`, `*`, or `&` syntax exists in Fluxa.

| C signature type | Fluxa type | Runtime behavior |
|---|---|---|
| `int` / `double` / `bool` | scalar | passed by value directly |
| `int*` | `int` | `&var` passed; int32 written back after call |
| `double*` / `float*` | `float` | `&var` passed; double written back after call |
| `bool*` | `bool` | `&var` passed; int32 written back after call |
| `char*` (input/output) | `str` | writable buffer (str_buf_size bytes) → result copied back to str |
| `uint8_t*` / `void*` buffer | `int arr` | arr elements flattened to bytes → scattered back after call |
| `struct*` / `void*` opaque | `dyn` | VAL_PTR extracted from dyn[0] |
| pointer return value | `dyn` | stored as VAL_PTR in dyn[0] |

The `char*` buffer size is configurable via `str_buf_size` in `[ffi]` (default 1024, range 64–65536).

**Example — scanf reads two ints from stdin, both written back automatically:**

```toml
[ffi.libc.signatures]
scanf = "(char*, int*, int*) -> int"
```

```fluxa
int a = 0
int b = 0
danger {
    int matched = libc.scanf("%d %d", a, b)
}
print(a)    // first value typed by user
print(b)    // second value typed by user
```

**What does NOT exist in Fluxa:**
```fluxa
ptr[0] = 42         // no pointer arithmetic
int x = ptr + 1     // pointer is not int
free(dyn_opaque)    // only the C lib that created it can free it
```

### 12.2 Library Declaration

Libraries declared in `[ffi]` of `fluxa.toml` are loaded automatically at runtime boot via dlopen. No `import c` required.

```toml
[ffi]
libm = "auto"        # auto-resolves via platform candidates
libc = "auto"
str_buf_size = 1024  # writable char* buffer per pointer arg (default 1024, range 64–65536)

[ffi.libm.signatures]
sqrt  = "(double) -> double"
modf  = "(double, double*) -> double"
frexp = "(double, int*) -> double"
```

---

## 13. Implementation Architecture (C)

### 13.1 Engine Pipeline

```
Lexer     → tokenizes .flx (via sds)
Parser    → validates EBNF, generates AST (arena pool, 4096 nodes)
           → propagates node->line for precise errors
Resolver  → converts names to stack offsets
           → sets warm_local=1 on all function-local identifiers
           → resolver_has_prst() → bifurcates SCRIPT/PROJECT
Bytecode  → compiles while/if to 3-address register VM
Runtime   → three execution tiers per function:
             Cold:  AST walker, danger_depth, ErrStack (32 entries)
                    warm_local skips prst_pool_has even in cold mode
             Warm:  WarmSlot (1 byte) + stack[off] (8 bytes) = 9 bytes total
                    WHT path signature + QJL 1-bit guard per slot
             Hot:   bytecode VM (while/if loops — deterministic)
           → arr contiguous on heap, non-aggressive GC
           → PrstPool (reload) + PrstGraph (deps) — both dynamic
           → cycle_count, dry_run (Atomic Handover)
           → current_line tracked by eval()
           → runtime_exec_with_rt() for Dry Run
Handover  → 5-step protocol, flat binary snapshot
           → serialize/deserialize with FNV-32 checksum
```

### 13.2 Core Data Structures

**PrstGraph — Dynamic Array**

```c
typedef struct {
    PrstDep *deps;    // heap-allocated — realloc as it grows
    int      count;
    int      cap;     // current cap — configurable via fluxa.toml
} PrstGraph;
```

**Runtime — key fields**

```c
typedef struct Runtime {
    // ... core fields ...
    long           cycle_count;      // top-level statements executed
    int            dry_run;          // 1 = Dry Run (suppresses output)
    volatile int  *cancel_flag;      // set to 1 to abort VM in -dev mode
    int            current_line;     // line currently executing

    // Sprint 11 — warm path
    WarmProfile    warm;             // compact execution profile (dynamic heap, power-of-2 growth)
    ASTNode       *current_fn;       // ASTNode* of current function (warm key)
    WarmFunc      *current_wf;       // cached WarmFunc — set once per call_function entry
} Runtime;
```

**ASTNode — warm path flag**

```c
struct ASTNode {
    NodeType type;
    int      resolved_offset;   // set by resolver; -1 = unresolved
    int      line;              // source line
    uint8_t  warm_local;        // 1 = confirmed function-local, never prst
                                // set by resolver; skips prst_pool_has in rt_get
    union { ... };
};
```

**HandoverCtx**

```c
typedef struct {
    HandoverMode  mode;                 // MEMORY or FLASH
    HandoverState state;                // IDLE/STANDBY/.../COMMITTED
    HandoverResult last_result;
    Runtime      *rt_a;                 // active — NEVER modified
    Runtime      *rt_b;                 // candidate — discarded on failure
    void         *snapshot;             // flat binary buffer
    size_t        snapshot_size;
    int           safe_point_timeout_ms; // default 5000
    int           grace_period_ms;      // default 100
    PrstPool      pool_after;           // pool transferred after commit
} HandoverCtx;
```

### 13.3 Runtime Static Limits

| Structure | Limit | Notes |
|---|---|---|
| Variable stack | 512 Value slots | Fixed in Runtime — zero fragmentation |
| Call stack | 500 frames | Maximum recursion depth |
| ErrStack (err) | 32 entries | Static ring buffer |
| ERR_MSG_MAX | 512 bytes | Per error message |
| GCTable | 1024 objects | Hard cap, configurable via gc_cap |
| PrstPool | dynamic | Initial cap via prst_cap, automatic realloc |
| PrstGraph | dynamic | Initial cap via prst_graph_cap, max 65536 |
| ASTPool nodes | 4096 nodes (default) | Parse arena — batch free at end. Configurable via `ast_pool_cap`, 4096..1048576. Over cap: falls back to per-node malloc, logged once. |
| ASTPool strings | 64 KB (default) | Interned string buffer. Configurable via `ast_str_pool_cap`, 65536..16777216. Over cap: falls back to per-string strdup, logged once. |
| WarmProfile | dynamic heap | starts at warm_func_cap × 276B, doubles at 75% fill |
| WarmSlot | 1 byte/node | 3-bit type + 1-bit QJL guard + 4-bit obs counter |
| warm_local flag | 1 byte/ASTNode | Set by resolver; skips prst_pool_has for fn-local vars |
| Parser expr depth | 200 levels | `FLUXA_MAX_EXPR_DEPTH` — guards `parse_expr` + `parse_primary`. Input exceeding this limit produces a parse error instead of stack overflow. |
| Parser stmt depth | 200 levels | `FLUXA_MAX_STMT_DEPTH` — guards `parse_body` + `parse_block_decl`. Protects against deeply nested `if`/`while`/`for`/`danger`/`Block` inputs. |
| Lexer comment recursion | none (iterative) | Comment skip (`//`) uses `goto restart` — iterative, never recursive. Unlimited consecutive comment lines without stack growth. |

### 13.4 Warm Path — Sprint 11

#### Inspiration

TurboQuant (Google Research, ICLR 2026) applies two-stage quantization to KV cache vectors in large language models: a random orthogonal rotation (WHT/PolarQuant) concentrates vector energy and enables near-optimal scalar quantization per coordinate; a 1-bit QJL residual removes the inner-product bias introduced by the first stage. Result: 3-4 bits per value with near-zero distortion, applied once per forward pass.

The insight transferred to Fluxa: a function's execution state is a vector of types observed at each stack slot. Two executions of the same function with the same type sequence produce the same WHT signature. A 1-bit residual (the QJL guard) detects divergence. This is the first known application of this quantization technique to a language runtime execution profiler.

#### Three execution tiers

**Tier 0 — Cold (first 4 calls):** full AST walker. The resolver has already set `warm_local=1` on every `NODE_IDENTIFIER` inside a fn body (`in_func_depth > 0`, not `prst`), which skips `prst_pool_has` (O(n) strcmp scan) even in cold mode. The runtime records the observed ValType (3 bits) of each stack slot into the function's `WarmFunc`.

Observation runs until promotion — no cap, no cold-lock. Fluxa is strongly typed; types never change at runtime. After 2 consecutive stable WHT runs, the function promotes to Tier 1. Block methods also promote (the `current_instance == NULL` gate was removed in v0.14).

**Tier 1 — Warm (stable_runs ≥ 2):** promoted functions skip ASTNode traversal entirely. Per `rt_get`:

1. Load `rt->current_wf` — pointer set once at `call_function` entry, zero hash cost inside the loop
2. Read `WarmSlot` (1 byte): `qjl_guard` bit + `observed_type` (3 bits)
3. Read `stack[resolved_offset]` (8 bytes)
4. QJL residual: if `warm_type_from_val_type(v.type) == ws->observed_type` → return. **9 bytes total.**
5. On type mismatch: QJL guard fires, `stable_runs` reset, function demoted to Tier 0

**Tier 2 — Hot (bytecode VM):** `while` and `if` loops compiled to 3-address register bytecode. In v0.14, function bodies with `return expr` also compile to bytecode chunks via `vm_run_fn` with an isolated register file — no frame save/restore.

#### Key implementation details

**`warm_local` flag (resolver):** Set at resolve time for every `NODE_IDENTIFIER` and `NODE_VAR_DECL` where `in_func_depth > 0` and `persistent == 0`. Script-body declarations (`in_func_depth == 0`) are never warm_local — the script body is not a function scope. `prst` declarations inside functions are also excluded — they must be read via the pool, not the stack.

**`current_wf` cache (runtime):** The O(1) hash (`warm_profile_get_func`) is called **once** at `call_function` entry and the result stored in `rt->current_wf`. All `rt_get` calls for that function frame use the cached pointer — zero hash overhead inside the hot loop.

**TCO trampoline fix:** When a tail call targets a different function (`pong` calling `ping`), the trampoline updates both `rt->current_fn` and `rt->current_wf` before continuing. Without this fix, the wrong WarmFunc slot would be used for the new target function.

**VM callback fallback:** A function or Block method called by hot-loop
bytecode may fall back to the tree interpreter when its body is not bytecode-
compilable. That bridge uses the same iterative TCO contract: `ret.tco_active`
reuses one saved frame for the complete chain, including mutual recursion.
Parameters copied from VM registers are borrowed and are not released by the
callee; arguments produced by a subsequent tail call are owned by the reused
frame and are released at its teardown. Argument and parameter counts are
validated against the 512-slot variable stack before binding.

**Block methods excluded:** `current_instance != NULL` in Block method frames → warm path disabled. Block methods use `inst->scope`, not the stack-slot path, so `warm_local` would be incorrect.

**Hash table (WarmProfile):** Open-addressing hash keyed by `(uintptr_t)fn_node` — the ASTNode pointer is stable across all calls to the same function. The table is a single contiguous heap-allocated `WarmFunc[]` block, starting at `warm_func_cap` entries (default 32) and growing via `realloc × 2` when > 75% full. `warm_func_cap` in `fluxa.toml` sets the **initial** capacity — not a ceiling. One pointer indirection total; all WarmFuncs are contiguous for cache locality.

**Slot wrap:** Functions with more than `WARM_SLOTS_MAX = 256` local variables wrap their slot index (`slot_idx % 256`). Colliding slots with different observed types cause the QJL guard to fire, keeping the function cold-locked. The direct stack read via warm_local still works correctly in all cases.

**WHT signature:** After each function body execution, `warm_update_sig` computes:
```
type_vec = pack(slots[0..15], 4 bits each) → uint64_t
path_sig = WHT(type_vec)                  → uint64_t via XOR/shifts, zero alloc
```
If `path_sig == wf->path_sig`: `stable_runs++`. If it matches for ≥ 2 consecutive runs: promoted. Type change → `stable_runs = 0`, restart.

**Memory profile** (cold AST vs warm path per `rt_get`):

| prst vars in program | Cold bytes touched | Warm bytes touched | Cache lines cold | Cache lines warm |
|---|---|---|---|---|
| 0 | ~18B | 9B | 2 | 1 |
| 5 | ~118B | 9B | 2–3 | 1 |
| 20 | ~418B | 9B | 7–8 | 1 |
| 100 | ~2018B | 9B | 32 | 1 |

On RP2040 (264 KB SRAM, no L1 cache): each cache miss is a SRAM access. The warm path reduces from 7–32 SRAM accesses to 2 per variable read in large PROJECT-mode programs.

**WHT path signature** (Walsh-Hadamard Transform):

The observed types of up to 16 nodes per function are packed into a `uint64_t` (4 bits per node) and transformed via WHT (pure XOR/shifts, zero parameters, zero malloc). Two function calls with the same observed type sequence produce the same `uint64_t` signature. After 2 consecutive runs with matching signatures, `stable_runs` reaches `WARM_STABLE_RUNS` and the function is promoted.

Inspired by TurboQuant (Google Research, ICLR 2026): *"randomly rotating input vectors induces a concentrated Beta distribution on coordinates, leveraging near-independence in high dimensions."* Applied here to execution state vectors instead of KV cache embeddings — the first known application of this quantization technique to a language runtime.

**Benchmark results** (7-run average, Linux x86-64):

| Benchmark | Before Sprint 11 | After Sprint 11 | Gain |
|---|---|---|---|
| fib(32) SCRIPT | ~3220ms | 2554ms | **+21%** |
| block 1M method calls | ~765ms | 677ms | **+12%** |
| compute 1M PROJECT (20 prst vars) | ~2600ms | 2005ms | **+23%** |
| while 10M (VM hot path) | ~250ms | 252ms | ≈ even |

---

## 13.5 Garbage Collector

### Philosophy

Fluxa's GC is **non-aggressive by design**. It targets only one type: `dyn`
(heterogeneous dynamic arrays). All other values — `int`, `float`, `str`,
`bool`, `arr` — are managed by scope lifetime and freed deterministically when
the scope that owns them is released (`scope_free`).

The GC never runs speculatively. It sweeps only at explicit safe points:
the back-edge of every `while` loop (with a fast-path guard), and on
`free(x)` when the target is a `dyn`. This keeps execution behavior
predictable — essential for real-time embedded control loops.

`free(x)` always works and is preferred for large `dyn` allocations. The GC
is a safety net, not the primary reclamation mechanism.

### What enters the GC

Only `VAL_DYN` objects are registered. When the runtime creates a new `dyn`
literal or a stdlib function returns a `dyn` (e.g. `crypto.keygen()`,
`csv.load()`), the `FluxaDyn*` pointer is registered via `gc_register()`.
Every other value type is freed by the scope destructor — no GC involvement.

### Data structure

The GC is an open-addressing hash table keyed by `FluxaDyn*` pointer, with
FNV-32 hashing and tombstone deletion (three slot states: `EMPTY`, `USED`,
`DEAD`). Capacity starts at `gc_cap` (default 1024, configurable via
`fluxa.toml`), rounded up to the next power of two. When the table reaches
75% load, it doubles and rehashes in-place.

```c
typedef struct {
    void  *ptr;        // FluxaDyn* — the tracked object
    int    pin_count;  // > 0 means at least one scope holds a reference
    size_t size_bytes; // telemetry — bytes tracked
    int    state;      // EMPTY | USED | DEAD (tombstone)
} GCEntry;
```

Average cost: O(1) for register, pin, unpin, unregister. O(n) for sweep,
where n is the number of currently tracked `dyn` objects.

### Pin / unpin model

Every `dyn` value in a live scope is **pinned** — its `pin_count` is
incremented when the value enters scope (`gc_pin`) and decremented when it
leaves (`gc_unpin`). A `dyn` with `pin_count == 0` is eligible for collection.

```
dyn events = [1, "hello", true]   ← gc_register + gc_pin
                                    pin_count = 1
// events goes out of scope
                                    gc_unpin → pin_count = 0
// next while back-edge or free()
                                    gc_sweep → fluxa_dyn_free(ptr)
```

When a `dyn` is passed into a function or assigned to another variable,
the receiving scope also pins it, so the `pin_count` reflects the number
of live references. This is a reference-counting model at the scope level,
not a tracing GC.

### Sweep points

The GC sweeps at two explicit points:

1. **`while` back-edge** — At the end of every `while` loop iteration, if
   `rt->gc.count > 0` (fast-path guard: zero cost when nothing is tracked),
   `gc_sweep` runs and frees all `dyn` objects with `pin_count == 0`.

2. **`free(x)` on a `dyn`** — The runtime calls `gc_unregister` immediately,
   removes the entry from the table, and frees the `FluxaDyn` in-place.
   Does not wait for the next sweep.

```fluxa
dyn buf = [0, 0, 0, 0]    // registered, pin_count = 1
// ... use buf ...
free(buf)                   // gc_unregister → immediate free, no GC lag
```

### Lifecycle — full picture

```
dyn x = [1, 2, 3]
    │
    ├─ gc_register(ptr, size)     pin_count = 0, state = USED
    ├─ gc_pin(ptr)                pin_count = 1  (x is in scope)
    │
    │  ... x used ...
    │
    ├─ gc_unpin(ptr)              pin_count = 0  (x leaves scope)
    │
    └─ gc_sweep() at while edge
       or free(x)
           └─ pin_count == 0 → fluxa_dyn_free(ptr) → state = DEAD
```

### Capacity and configuration

```toml
[runtime]
gc_cap = 1024   # initial GC table capacity (default 1024)
                # increase for programs that create many dyn objects
                # decrease for RP2040 / memory-constrained targets
```

`gc_cap` is the **initial** capacity, not a ceiling. The table grows
automatically via realloc when it exceeds 75% load. Setting a higher initial
cap only reduces the number of realloc calls — it does not limit total tracked
objects.

On RP2040 (264 KB SRAM), set `gc_cap` low (e.g. 64 or 128) to reduce the
initial table footprint. Each `GCEntry` is 24 bytes; `gc_cap = 64` costs
1.5 KB of SRAM.

### Thread safety

The GC is **not thread-safe**. Each thread spawned via `std.flxthread`
gets its own `Runtime` clone with its own independent `GCTable`. There is
no shared GC state between threads. `prst dyn` values in the `PrstPool` are
synchronized via the pool's own lock, not via the GC.

### What the GC does NOT do

- **No mark-and-sweep tracing.** It does not traverse the object graph. Only
  the scope pin mechanism tracks liveness.
- **No compaction.** `dyn` objects stay at their original heap address. The
  GC only frees them.
- **No generational collection.** All tracked objects are in a single
  generation.
- **No finalization.** There are no destructors. `fluxa_dyn_free` frees the
  `items` array and the `FluxaDyn` struct — nothing else.
- **Does not touch `int arr`.** Fixed arrays are scope-managed. When the
  scope that owns an `int arr` is freed, `value_free_data` frees the data
  array directly — no GC involvement.

---

## 13.6 Slot Ownership and the `free` Operator

This section defines the runtime contract for heap-bearing slot lifetimes. As of
runtime v0.23, `str` buffers are **reference-counted and immutable**: a read shares
the buffer and adds a reference (O(1), no copy), and every release path — slot
reassignment, discarded produced values, explicit `free(x)`, and frame teardown —
drops a reference, freeing the buffer only at zero. The contract is observable from
Fluxa code through three mechanisms — automatic release on slot reassignment,
automatic release of references taken by reads/calls/comparisons, and the explicit
`free(x)` operator. Together they guarantee that a long-running worker can run
indefinitely at bounded memory.

### Heap-bearing types

Two value types carry heap data:

- `VAL_STRING` — a pointer to a **refcounted, immutable** buffer. The buffer
  carries a hidden header `[rc | bytes...]`; `Value.as.string` points at the bytes,
  so all existing readers of `as.string` are unaffected. Because strings are
  immutable (any modification produces a new buffer), the buffer is shared freely on
  read and its reference count records how many live names point at it. The heap is
  released when — and only when — the count reaches zero. Reference counts are atomic
  (`__atomic`), so a buffer may be shared across `std.flxthread` threads without a
  copy rule.
- `VAL_DYN` — a `FluxaDyn *` registered with the GC and tracked by pin count.

Fixed arrays (`VAL_ARR` with `owned = 1`) also carry heap data (the `data` pointer
and any refcounted string elements). The same rules apply, but `VAL_ARR` is a value
type, not a separate identity tracked by the GC.

### Automatic release on slot reassignment (`rt_set`)

When the runtime writes to `rt->stack[offset]` via `rt_set`, the previous slot contents are released before the new value is stored. This applies to **every** assignment that targets a resolved local slot:

| Old slot type | Action before overwrite |
|---|---|
| `VAL_STRING` | Release the old buffer's reference (drop rc by one; free at zero) |
| `VAL_DYN` | `gc_unpin(old.as.dyn)` so a later sweep can collect orphans |
| `VAL_ARR` (owned) | Release the data array and each owned element's reference |
| `VAL_INT`, `VAL_FLOAT`, `VAL_BOOL`, `VAL_NIL` | No-op |

A same-pointer self-assignment needs no special case under reference counting:
the incoming value already holds a reference (a read retains), so releasing the old
reference and adopting the new one is correct even when both name the same buffer.

After the new value is stored, `rt_set` re-establishes ownership invariants:

- New `VAL_DYN`: `gc_pin(new.as.dyn)` so the slot holds exactly one strong reference.

This produces the invariant **"every slot is the sole owner of its heap-bearing contents"** at all times outside the body of `rt_set` itself.

### Reference discipline for produced values

Any expression evaluation produces a `Value`. Under reference counting the model is
uniform: **every producer of a `VAL_STRING` hands the consumer its own reference,
and every store adopts that reference.** There is no "aliased" class to reason about
— a read never yields a bare pointer that a second name could unsafely share.

Producers and how they obtain a reference:
- `NODE_STRING_LIT`, `NODE_MEMBER_CALL`, `NODE_FUNC_CALL`, `NODE_FFI_CALL` — allocate
  a fresh refcounted buffer (rc = 1); a literal via the string constructor, a call
  via its return path.
- `NODE_IDENTIFIER`, `NODE_MEMBER_ACCESS` (Block field read), `NODE_ARR_ACCESS`,
  `NODE_INDEXED_MEMBER_ACCESS` (array/dyn element read) — **retain** the existing
  buffer (rc + 1) and return the same pointer. This is O(1) regardless of string
  size; the caller receives an independent reference to the shared immutable buffer.
- Reads of non-string types — by value, no reference involved.

Consumers release the references they were handed:

| Consumer | When |
|---|---|
| `NODE_MEMBER_CALL` / `NODE_FUNC_CALL` / `NODE_FFI_CALL` arg evaluation | After the called function returns, each `VAL_STRING` argument reference is released. |
| `eval_binary` (comparison, arithmetic) | At every return path, `VAL_STRING` operand references of `==`, `!=`, `+`, `-`, `*`, `/` are released. |
| `NODE_BLOCK_STMT` statement loop | After each child statement returns a value, a `VAL_STRING` return reference is released; a `VAL_DYN` return is unregistered and freed if `pin_count == 0`. |
| `builtin_len`, `builtin_print` | After the body uses the value, its reference is released (O(1); no copy was made to read it). |
| `NODE_FOR` element binding | The loop variable retains a reference to each element buffer; iteration cannot corrupt the source. |
| `NODE_ARR_DECL` element evaluation | Producers are stored by adopting their reference (the store does not re-copy). |
| Frame teardown (function return, TCO trampoline, both VM→interpreter bridges) | Releases the references held by the frame's `VAL_STRING` slots and owned `VAL_ARR` slots. In the VM bridges, parameter slots hold VM-register pointers owned by the VM and are skipped; only locals are released. `VAL_DYN` slots are left untouched — parameter binding adopts the caller's `dyn` without pinning, so releasing here would drop the caller's pin. |
| End of `danger { }` | `gc_sweep` collects unpinned `dyn` objects. |

### Explicit `free(x)`

`free(x)` releases slot `x`'s reference to its heap data and sets the slot to `nil`.
For a `VAL_STRING` this drops the buffer's reference count by one, freeing the buffer
only if no other name still holds it; other live references remain valid (the buffer
is immutable, so no reader is disturbed). `free()` is needed only for intermediate
local bindings the compiler cannot infer are dead — typically a chain of
`strings.concat` calls building a response body in a worker loop that never returns.

```fluxa
str p1 = strings.concat("{\"id\":\"", id)
str p2 = strings.concat(p1, "\",\"name\":\"")
str p3 = strings.concat(p2, name)
str p4 = strings.concat(p3, "\"}")
wserver.reply_json(req, 200, p4)
free(p1); free(p2); free(p3); free(p4)
```

Each `p1..p4` is a fresh declaration: no reassignment releases the previous one, and
the function never returns to release the scope. `free()` is the only way to release
these in bounded memory.

`free(x)` behavior by target:

| Target | Behavior |
|---|---|
| Local stack slot holding `VAL_STRING` | Release the buffer reference (free at rc 0), slot → nil |
| Local stack slot holding `VAL_DYN` | `gc_unregister + fluxa_dyn_free`, slot → nil |
| Local stack slot holding `VAL_ARR` (owned) | Release the array and each element reference, slot → nil |
| Local stack slot holding a by-value type | No-op, slot → nil |
| Slot already `nil` | No-op |
| Block instance field holding `VAL_STRING` (v0.23+) | Release the field's reference, field → nil; a later assignment revives it |
| `prst` variable | Runtime error (`free` rejects prst — persistence layer owns the heap) |
| Anything not resolved to a stack slot or Block field | Runtime error (cannot infer target) |

`free(x)` is idempotent. Calling it twice in a row on the same slot is harmless.

### Interaction with `prst`

`prst` variables are owned by the per-program `PrstPool`, not by their stack slots. Writes to a `prst` go through the pool's deep-copy or overwrite path (see §10.4). `free()` rejects `prst` because releasing the slot would break the pool's snapshot/restore guarantees during hot reload.

To clear a `prst` value, assign a replacement: `my_prst = ""` for strings, `my_prst = nil` for dyn — the pool's set path handles the previous value's release.

### Reading array and `dyn` elements

Reading an array or `dyn` element (`NODE_ARR_ACCESS`, `NODE_INDEXED_MEMBER_ACCESS`)
**retains** the element's buffer and returns an independent reference to it — O(1),
no copy. The reader holds its own reference to the shared immutable buffer, so
freeing or reassigning the reader affects only the reader's reference; the element is
untouched. All faces below are verified by execution:

```fluxa
str arr names[3] = ""
names[0] = "Alpha"

// Reading gives you your own reference:
str x = names[0]      // retains names[0]'s buffer (O(1))
free(x)               // drops x's reference; names[0] still holds one
print(names[0])       // → "Alpha"   (intact)

// Reassigning a reader releases only its reference:
str y = names[0]
y = names[1]          // releases y's reference to names[0]'s buffer; that buffer
                      //   still has names[0]'s reference and survives
print(names[0])       // → "Alpha"   (intact)
```

Writing to an element (`arr[i] = var`) adopts the produced reference into the slot.
All reading patterns are equally safe and equally cheap: `for-in` (the loop variable
retains each element), passing an element directly as a call argument (the parameter
retains), or binding to a variable. The earlier `strings.concat(arr[i], "")` copy
idiom is no longer necessary. This applies identically to `dyn` elements from
`csv`/`sqlite` rows.

| Operation | Reference behavior | Safe? |
|---|---|---|
| `str x = arr[i]` / `d[i]` / `inst.field` | retain (own reference, O(1)) | ✅ safe |
| `arr[i] = var` | store adopts the reference | ✅ safe |
| `for e in arr` | retain per element | ✅ safe |
| `f(arr[i])` | parameter retains | ✅ safe |
| building a new string (`strings.*`) | new buffer, rc = 1 (O(n)) | ✅ safe |

### Interaction with `dyn` cursors

Library-returned `VAL_DYN` cursors (e.g. `json2.parse`, `csv.open`, `sqlite.open`, `pg.connect` results) are registered with the GC by the dispatch layer at return time. Use the library's release function for the external resource (`json2.discard`, `csv.close`, `pg.free_result`, `sqlite.close`). The `FluxaDyn` wrapper itself is collected automatically when the holding slot is reassigned or when the surrounding scope ends.

Calling `free()` on a `dyn` cursor is permitted and triggers both the wrapper release and slot nilling, but it does **not** release the underlying external resource — the library's release function must still be called first.

### Memory bound for long-running workers

Combining the above rules, a worker function that:

1. Reassigns every per-request local (`str method = wserver.req_method(req)`, etc.)
2. Frees each intermediate in any `strings.concat` chain (`free(p1); free(p2); ...`)
3. Calls the matching release function for every `dyn` cursor (`json2.discard`, `pg.free_result`, ...)

runs at bounded memory indefinitely. Measured profile for the reference HTTP+DB server: 24 worker threads, 1k+ req/s sustained, resident ~30–40 MB peak, returning to ~28 MB when traffic stops.

---

## 14. EBNF Grammar (Reference)

```
<program>      ::= <statement>*
<statement>    ::= <import_decl> | <block_decl> | <block_inst>
                 | <var_decl> | <arr_decl> | <assignment>
                 | <arr_assign> | <if_stmt> | <while_stmt>
                 | <for_stmt> | <danger_stmt> | <free_stmt>
                 | <func_call> | <return_stmt>
<import_decl>  ::= "import" ("std"|"c"|"live"|"static") <id> ["as" <id>]
<block_decl>   ::= "Block" <id> "{" (<var_decl>|<func_decl>)* "}"
<block_inst>   ::= "Block" <id> "typeof" <id>
<type>         ::= "int"|"float"|"str"|"bool"|"char"|"dyn"|"nil"
<var_decl>     ::= ["prst"] <type> <id> "=" <expression>
<arr_decl>     ::= ["prst"] <type> "arr" <id> "[" <int> "]"
                   "=" ("[" <expr_list>? "]" | <expression>)
<danger_stmt>  ::= "danger" "{" <statement>* "}"
<func_decl>    ::= "fn" <id> "(" <param_list>? ")" <type>
                   "{" <statement>* "}"
```

---

## 15. Implementation Roadmap

| Sprint | Status | Scope |
|---|---|---|
| 1 | ✅ | Lexer, Parser, AST, Arena Pool, print(), len() |
| 2 | ✅ | Scopes (uthash), variables, arithmetic, assignment |
| 3 | ✅ | if/else, while, for, arr declaration and access |
| 4 | ✅ | Functions: fn, return, call stack, recursion |
| 4.b | ✅ | Performance: Name Resolution, Inline Cache, Computed Gotos. Baseline: ~0.25s |
| 4.c | ✅ | Performance: 3-address register VM, unified loops. Baseline: ~0.16s |
| 5 | ✅ | Blocks: Block, typeof, total isolation, member access/call/assign |
| 6 | ✅ | danger, static err stack, contiguous arr on heap, free(), GC stub |
| 6.b | ✅ | import c + FFI via dlopen/libffi, arr default init |
| 6.c | ✅ | fn calling fn, TCO (tail call optimization), mutual recursion, examples/ |
| 7.a | ✅ | FluxaMode SCRIPT/PROJECT, prst semantics, PrstGraph, GC cap via toml, fluxa explain |
| 7.b | ✅ | Watcher -dev (inotify/kqueue), fluxa apply, Pool+Graph serialization, cycle_count, dry_run, ERR_HANDOVER |
| 8 | ✅ | Atomic Handover (5 steps), Dry Run, flat binary snapshot, checksum, versioning, dynamic PrstGraph, prst_cap/prst_graph_cap via toml, line numbers in errors |
| 9 | ✅ | Full CLI: fluxa run/apply/handover/observe/set/logs/status/init, IPC unix socket, preflight (-p), --force, fluxa.toml [libs] |
| 9.c | ✅ | FFI pointer type mapping: int\*, double\*, bool\* → &var writeback; char\* → writable buffer; uint8_t\* → arr byte scatter; dyn → opaque void\* round-trip. str_buf_size configurable via [ffi] (default 1024). Improved error messages for json/csv. All source strings in English. |
| 10 | ✅ | std.math, std.csv, std.json, std.strings, std.time, std.flxthread (native concurrency). All opt-in via fluxa.toml [libs]. |
| 11 | ✅ | Warm Path: warm_local flag; WarmProfile (WHT + QJL); promoted reads 9B vs 418B+ cold. fib +21%, block calls +12%. TurboQuant-inspired. |
| 14 | ✅ | Performance Sprint: WarmProfile dynamic heap (no cap, no cold-lock). OP_CALL_METHOD, OP_CALL_FUNC, OP_GET_FIELD, OP_SET_FIELD. Instance inline cache (VAL_STRING→VAL_PTR). method_try_inline. vm_run_fn / chunk_compile_fn. Hardware sim (RP2040/ESP32). Docker torture. bench_field 94% faster. |
| 12.a | ✅ | std.crypto (libsodium 1.0.18+): BLAKE2b-256, XSalsa20-Poly1305, Ed25519, Curve25519. `fluxa keygen` CLI. `[security]` toml. FLUXA\_SECURE=1: two-level RESCUE (SOFT/HARD). Silent drop. Configurable `handshake_timeout_ms`, `ipc_max_conns`. Bug fixes: arr return UAF, resolver stack overflow. |
| 12.b | ✅ | **Libs 1** — std.pid, std.sqlite, std.serial, std.i2c. **Lib Linker** — FLUXA_LIB_EXPORT macro + gen_lib_registry.py + lib.mk: new libs need zero runtime.c/parser.c/toml edits. **fluxa.libs** — build-time binary control. **fluxa init** — generates full project structure (main.flx, fluxa.toml, fluxa.libs, live/, static/, tests/). |
| 12.c | ✅ | **Huge Pages** — `FLUXA_HUGEPAGES=1` via `madvise(MADV_HUGEPAGE)` on AST arena arrays. Reduces dTLB pressure on RAM-intensive workloads. See §20 for when to enable. |
| 12.d | ✅ | **Libs 2** — std.httpc (libcurl HTTP client), std.mqtt (libmosquitto), std.mcpc (MCP client via libcurl). std.http (mongoose server) and std.mcp (Fluxa as MCP server) remain planned. |
| 12.e | ✅ | **std.libv** — N-dimensional vectors, matrices, tensors. GLM-inspired API. Col-major. In-place ops. Pure C99 ~800L, zero deps. |
| 12.f | ✅ | **std.libdsp** — FFT (Cooley-Tukey, in-place), STFT, windowing, FIR/IIR, range-Doppler, CFAR, matched filter. Radar/DSP math. Requires std.libv. |
| 12.g | ✅ | **Libs 3** — std.graph (Raylib), std.infer (llama.cpp). Dual backend: stub (zero deps, default) + real backend when vendored. `make FLUXA_GRAPH_RAYLIB=1` / `make FLUXA_INFER_LLAMA=1`. |
| 15   | ✅ | **Module System** — `import live <name>` / `import static <name>`. Single-level imports in `main.flx` only. Namespace mangling at parse time (`sensor.fn()` → `sensor__fn()`). Zero runtime changes. `[project] module_root` in fluxa.toml. `Block` declarations in modules with `typeof ns.Block`. 15 module tests. |
| 16   | ✅ | **Libs 4** — std.wserver (resilient HTTP server via libmicrohttpd: thread pool, std.flxthread-compatible accept loop, auto-scaling MHD pool via `serve(port, true)`), std.pg (PostgreSQL client via libpq: opaque int handles, connect, exec, query, query_params with `str arr + int n`, typed getters, ping). Both dual-backend: stub with clear error when dep absent, real backend when pkg-config finds the library. **API redesign:** pg and wserver migrated from `dyn` cursors to opaque `int` handles — `pg.connect()` → `int`, `wserver.serve()` → `int`, `wserver.accept()` → `int`. **Parser hardening:** `FLUXA_MAX_EXPR_DEPTH=200` guard on `parse_expr`/`parse_primary`; `FLUXA_MAX_STMT_DEPTH=200` guard on `parse_body`/`parse_block_decl`; `parser_new` now initializes depth counters. **Lexer hardening:** comment handler converted from recursive `lexer_next` self-call to iterative `goto restart` — eliminates stack overflow on inputs with many consecutive comment lines. **Toml hardening:** `atoi` replaced with `strtol` + range clamp in all `[runtime]` fields — eliminates UB on oversized values (e.g. `warm_func_cap = 16000000000000000`). **Fuzz harnesses:** `fuzz_pg.c`, `fuzz_wserver.c`, `fuzz_snapshot.c` depth guard for synthetic deep-array inputs. **Integration tests:** `tests/integration/pg/` — Docker-based PostgreSQL integration test (`make test-integration-pg`). Bug fixes: `prst arr` double free on first run; `prst arr` values not surviving handover; `csv.load` pre-allocating 32 GB (`1<<30` chunk_size). |
| 13   | ✅ | **Runtime Update Protocol** — — `fluxa update <new_binary> [-p]` replaces the running binary with zero downtime. IPC_OP_UPDATE (0x07) triggers prst serialization at safe point, then `execve` with `FLUXA_RESTART_SNAPSHOT` env var. New binary loads snapshot and continues execution. Security: UID check always enforced, generic errors to client, path traversal guard, FLUXA_SECURE requires `.sig` file. |

---

## 16. fluxa.toml — Complete Configuration

```toml
# fluxa.toml — optional config at project root

[project]
name        = "my_project"
entry       = "main.flx"
module_root = "src"   # optional — base dir for import live/static resolution
                      # default: CWD  →  import live sensor = live/sensor.flx
                      # with src:     →  import live sensor = src/live/sensor.flx

[runtime]
gc_cap         = 1024   # GC table hard cap (static array, default 1024)
prst_cap       = 64     # PrstPool initial capacity (dynamic, default 64)
prst_graph_cap = 256    # PrstGraph initial capacity (dynamic, default 256)
warm_func_cap  = 32     # WarmProfile hash table size: number of functions
                        # the runtime can profile simultaneously.
                        # Must be power of 2, range 4..256.
                        # 32 = 8.8 KB, 64 = 17.6 KB, 256 = 70 KB.
                        # Increase for programs with >32 functions that
                        # benefit from warm path promotion.
                        # The static array is always 256 slots (WARM_FUNC_CAP_MAX);
                        # warm_func_cap controls how many are used at runtime.
ast_pool_cap     = 4096   # AST node arena size (default 4096, range 4096..1048576)
ast_str_pool_cap = 65536  # AST string arena bytes (default 65536, range 65536..16777216)
# When to increase:
# prst_cap > 64        → programs with many prst variables (e.g. 500+)
# prst_graph_cap > 256 → many functions reading many prst vars
# gc_cap < 1024        → memory-constrained environments (e.g. simulated RP2040)
# warm_func_cap > 32   → programs with >32 hot functions (embedded controllers,
#                        large IoT state machines)
# ast_pool_cap/ast_str_pool_cap → programs whose AST regularly exceeds
#                        4096 nodes / 65536 bytes (avoids per-item malloc
#                        fallback cost — never required for correctness,
#                        and never adds cost at the compiled-in default)

# ── [security] (FLUXA_SECURE builds only) ─────────────────────────────────
# Key file paths only — NEVER put key material inline in the toml.
# Generate keys with: fluxa_secure keygen --dir /etc/fluxa/keys
[security]
signing_key  = "/etc/fluxa/keys/signing.key"  # Ed25519 private key, 0400
ipc_hmac_key = "/etc/fluxa/keys/ipc_hmac.key" # HMAC-SHA512 secret, 0400
mode         = "strict"   # off | warn | strict
                          # off    — no validation (default, dev builds)
                          # warn   — accept but log unsigned commands
                          # strict — reject apply/update without valid sig
handshake_timeout_ms = 50   # IPC recv timeout in ms (default 50, range 10..5000)
                             # Increase for slow/embedded links
ipc_max_conns = 16           # Max simultaneous IPC connections (default 16, range 1..256)
                             # Increase for high-traffic deployments


[ffi]
libm = "auto"           # auto-resolves via platform candidates (libm.so.6, libm.dylib, ...)
libc = "auto"
str_buf_size = 1024     # writable char* buffer allocated per pointer arg
                        # range: 64–65536. Default: 1024.
                        # Increase for functions writing large strings (e.g. sprintf)
                        # Decrease for memory-constrained embedded targets

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

[libs]
std.math      = "1.0"   # opt-in stdlib — not compiled in unless declared
std.csv       = "1.0"
std.json      = "1.0"
std.strings   = "1.0"
std.time      = "1.0"
std.flxthread = "1.0"
std.crypto    = "1.0"

[libs.csv]
max_line_bytes = 1024   # max bytes per CSV line (default 1024)
max_fields     = 64     # max fields for csv.field (default 64)

[libs.json]
max_str_bytes  = 4096   # max JSON string size (default 4096)

[libs.pg]
max_connections = 16     # max simultaneous PGconn*   (hard cap 256)
max_results     = 64     # max simultaneous PGresult* (hard cap 1024)
max_cell_bytes  = 4096   # max bytes returned by pg.get (hard cap 65536)
max_param_bytes = 1024   # max bytes per query_params element (hard cap 65536)
max_params      = 16     # max elements in query_params arr (hard cap 64)

[libs.wserver]
max_servers       = 4      # max simultaneous MHD daemons (hard cap 32)
max_requests      = 128    # max simultaneous live request handles (hard cap 4096)
max_body_bytes    = 65536  # max request body size (hard cap 16 MB)
max_header_pairs  = 16     # max pairs in reply_headers (hard cap 128)
max_header_bytes  = 4096   # max bytes per header key+value (hard cap 65536)
queue_depth       = 256    # per-server accept queue depth (hard cap 4096)
# Auto-scaling pool — only active when serve(port, true):
min_threads       = 2      # min MHD threads in pool (hard cap 64)
max_threads       = 16     # max MHD threads in pool (hard cap 256)
scale_up_queue    = 4      # queue depth to trigger scale-up
scale_down_idle   = 10     # idle seconds to trigger scale-down
```

*`prst_cap` and `prst_graph_cap` are INITIAL caps, not ceilings. Structures grow via realloc automatically. Setting the correct initial cap only improves allocation performance — it does not limit usage.*

---

## 16b. fluxa.libs — Build-Time Library Configuration

`fluxa.libs` lives at the project root alongside `fluxa.toml`. It controls which libraries are compiled into the Fluxa runtime binary. Libraries set to `false` are completely excluded — zero code size, zero link time, zero overhead. Essential for embedded targets (RP2040, ESP32) where binary size matters.

Generated automatically by `fluxa init <n>`. Read by `make build` via `scripts/gen_lib_registry.py`.

```toml
# fluxa.libs — build-time library configuration

[libs.build]
std.math      = true    # no external deps
std.csv       = true    # no external deps
std.json      = true    # no external deps
std.strings   = true    # no external deps
std.time      = true    # POSIX
std.flxthread = true    # pthreads
std.pid       = true    # no external deps
std.crypto    = false   # requires: libsodium-dev
std.sqlite    = false   # requires: libsqlite3-dev
std.serial    = false   # requires: libserialport-dev
std.i2c       = true    # Linux kernel header (no external lib)
```

**Two levels of control:**

| Level | File | Controls |
|---|---|---|
| Build-time | `fluxa.libs` | What enters the binary (code size) |
| Run-time | `fluxa.toml [libs]` | What the program imports |

A lib must be `true` in `fluxa.libs` AND declared in `fluxa.toml [libs]` to be usable. If declared in `fluxa.toml` but `false` in `fluxa.libs`, the runtime emits a clear error at import time.

After changing `fluxa.libs`: run `make build` to rebuild with the new set of libs.

---

## 17. Standard Library

The Fluxa stdlib is opt-in by design. No library enters the binary without explicit declaration in `fluxa.toml`. The base binary remains minimal — essential for RP2040 and other memory-constrained environments.

### 17.1 Selection Criteria

A library enters the stdlib only if it simultaneously satisfies all three criteria:

1. Needs real-time reload — it makes sense to swap behavior without stopping execution.
2. Has state that needs to survive — there is real prst state, not just stateless processing.
3. The underlying C library is stable — API doesn't change every release. Zero or near-zero maintenance.

*Stdlib principle: If the underlying C lib has breaking changes more than once per year, it doesn't belong here. Fluxa cannot be held hostage by unstable dependencies.*

### 17.2 Declaration in fluxa.toml

Only libs declared in `[libs]` are compiled and linked into the runtime. The parser rejects `import std <lib>` with a clear error if the lib was not declared in the toml — failure at parse time, not at runtime.

```toml
[libs]
std.mqtt = "1.0"    # IoT — enters the binary
std.csv  = "1.0"    # Data
# std.i2c = "1.0"  # Commented — not compiled, no binary weight
```

*If `[libs]` does not exist in the toml, zero libs are included. The base binary runs with only core dependencies: sds, uthash, libffi.*

### 17.3 Catalogue

| Lib | Category | C Dep | Status | Use Case |
|---|---|---|---|---|
| std.math | Math | `<math.h>` | **✅ implemented** | 39 functions: sqrt, pow, sin/cos/tan, log, clamp, approx, pi, e, ... |
| std.csv | Data | own ~500L | **✅ implemented** | open/next/close cursor, chunk, load, save, field, field_count, skip, is_eof |
| std.json | Data | own | **✅ implemented** | object, set, get_*, has, parse_array, stringify, load, cursor, valid |
| std.strings | Text | own | **✅ implemented** | split, join, concat, trim, find, replace, upper, lower, from_int, to_int, ... |
| std.time | Time | POSIX | **✅ implemented** | sleep, sleep_us, now_ms, now_us, ticks, elapsed_ms, timeout, format |
| std.flxthread | Concurrency | pthread | **✅ implemented** | ft.new, ft.message, ft.await, ft.stop, ft.kill, ft.lock, ft.resolve_all |
| std.crypto | Security | libsodium | **✅ implemented** | hash (BLAKE2b-256), keygen, nonce, encrypt/decrypt (XSalsa20-Poly1305), sign\_keygen, sign, sign\_open (Ed25519), kx\_keygen, kx\_client, kx\_server (Curve25519), compare, wipe, to\_hex, from\_hex, version |
| std.mqtt | IoT | libmosquitto | **✅ implemented** | MQTT protocol. connect, connect_auth, publish, publish_qos, subscribe, loop, connected, disconnect. |
| std.serial | IoT | libserialport | **✅ implemented** | UART/serial. list, open/close, write, read, readline, flush, bytes_available. |
| std.i2c | Robotics | Linux i2c-dev | **✅ implemented** | I2C protocol. open/close, write, read, write_reg, read_reg, read_reg16, scan. Linux only (no-op stub on macOS). |
| std.pid | Robotics | own ~300L C99 | **✅ implemented** | PID controller. new, compute, reset, set_limits (anti-windup), set_deadband, state. |
| std.sqlite | Database | SQLite 3 | **✅ implemented** | Embedded SQL. open/close, exec, query, last_insert_id, changes, version. |
| std.httpc | Network | libcurl | **✅ implemented** | HTTP client. get, post, post_json, put, delete, status, body, ok. Response dyn: [status, body, ok]. |
| std.http  | Network | mongoose (vendored) | **✅ implemented** | HTTP server (`serve`, `poll`, `reply`, `reply_json`) + client (`get`, `post`, `post_json`, `put`, `delete`, `status`, `body`, `ok`). mongoose 7.21. |
| std.graph | Visual | stub default + Raylib optional | **✅ implemented** | 2D/3D graphics, input. Stub: zero deps, no-op. Raylib: `make FLUXA_GRAPH_RAYLIB=1` + vendor/raylib.h. |
| std.infer | AI | stub default + llama.cpp optional | **✅ implemented** | Local LLM inference (GGUF models). Stub: zero deps, placeholder output. llama.cpp: `make FLUXA_INFER_LLAMA=1` + vendor/llama.h. |
| std.mcpc | Protocol | libcurl | **✅ implemented** | MCP client. connect, connect_auth, list_tools, call, call_text, disconnect. Calls external MCP servers. |
| std.mcp  | Protocol | mongoose (vendored) | **✅ implemented** | Fluxa as MCP server (JSON-RPC 2.0). Exposes fluxa/observe, fluxa/set, fluxa/status, fluxa/logs, tools/list. Connects to running IPC socket. |
| std.https | Network | libcurl | **✅ implemented** | HTTPS client (TLS enforced). Same API as std.httpc, rejects plain http://. Verifies cert and hostname. |
| std.mcps | Protocol | libcurl | **✅ implemented** | MCP client over HTTPS (TLS enforced). Same API as std.mcpc. |
| std.json2 | Data | own ~600L C99 | **✅ implemented** | Full DOM JSON. parse, load, get/get_int/get_float/get_bool, has, type, length, key, set, delete, stringify. Path nav: "a.b[0].c". |
| std.zlib | Data | zlib | **✅ implemented** | Compression: compress/decompress (deflate+base64), gzip/gunzip, crc32, adler32, ratio. |
| std.fs | System | POSIX | **✅ implemented** | Filesystem: read, write, append, exists, delete, rename, copy, size, mkdir, listdir, isdir, isfile, join, basename, dirname, ext, tempfile. |
| std.websocket | Network | native C99 + libwebsockets optional | **✅ implemented** | WebSocket client. ws:// native (RFC 6455 pure C99, zero deps). wss:// with FLUXA_WS_LWS=1 + libssl-dev. |
| std.libv | Math / Graphics | own ~450L C99 | **✅ implemented** | N-dimensional vectors, matrices, tensors. GLM-inspired API. col-major. In-place ops. vec2/3/4, mat2/3/4, matmul, FFT-ready. |
| std.libdsp | DSP / Radar | own C99 + FFTW opt-in | **✅ implemented** | FFT (Cooley-Tukey), STFT, windowing, FIR/IIR, matched filter, range-Doppler, CFAR. FFTW backend planned. |
| std.wserver | Network | stub default + libmicrohttpd | **✅ implemented** | Resilient HTTP server. Opaque `int` handles. `serve(port)` or `serve(port, true)` for auto-scaling MHD thread pool. serve, accept, req_method, req_path, req_body, req_header, reply, reply_json, reply_headers (str arr + int n), connections, stop. Designed for std.flxthread workers. |
| std.pg | Database | stub default + libpq | **✅ implemented** | PostgreSQL client. Opaque `int` handles — `connect` returns `int`, `query`/`query_params` return `int`. `query_params` takes `str arr params` + `int n`. connect, close, exec, query, query_params, rows, cols, col_name, get, get_int, get_float, get_bool, is_null, free_result, last_error, ping, version. |

### 17.4 Library Memory Model

Libraries manage their own memory. Fluxa does not serialize library internals — only opaque references as `prst dyn`. The dataset survives a reload because the lib keeps the pointer alive.

```fluxa
prst dyn data = csv.load("sales.csv")    // loaded once
// fluxa apply → data survives, new formula recalculates
float total = csv.sum(data, "revenue") * 1.1
```

*RP2040 limitation: prst dyn of libs is not serialized to Flash (HANDOVER_MODE_FLASH). On hardware with limited SRAM, the dataset exists only while the process is alive. In HANDOVER_MODE_MEMORY (x86/ARM64) the pointer is preserved normally.*

### 17.5 std.mcp — Fluxa as MCP Server

**Status: ✅ implemented** — depends on `std.http` (mongoose).

MCP (Model Context Protocol) is an open standard that allows AI agents and tools to interact with services via a structured protocol. `std.mcp` exposes the Fluxa runtime as an MCP server, making it directly controllable by AI agents (Claude, GPT, Gemini, local models via llama.cpp) without any custom integration code.

**What it is NOT:** `std.mcp` is not the IPC socket (`/tmp/fluxa-<pid>.sock`) that `fluxa observe`, `fluxa set`, and `fluxa status` use. The IPC socket is a Unix-local binary protocol for CLI tools. `std.mcp` is an HTTP-based MCP server that external agents can reach over a network.

**Primary use case:** An AI agent running on a remote machine modifies a Fluxa program running on an embedded device — adjusting control parameters, triggering reloads, reading sensor state — without any human operator in the loop.

```toml
[libs]
std.http = "1.0"   # required — std.mcp depends on it
std.mcp  = "1.0"
```

```fluxa
import std mcp

// Start MCP server on port 7777
// Fluxa runtime becomes controllable by any MCP-compatible agent
mcp.serve(7777)
```

**MCP tools exposed:**

| MCP Tool | Maps to | Description |
|---|---|---|
| `fluxa/observe` | `fluxa observe <var>` | Read current value of a prst var |
| `fluxa/set` | `fluxa set <var> <val>` | Mutate a prst var at next safe point |
| `fluxa/apply` | `fluxa apply <file>` | Hot reload preserving prst state |
| `fluxa/handover` | `fluxa handover <old> <new>` | Full Atomic Handover (5 steps) |
| `fluxa/status` | `fluxa status` | cycle count, prst count, errors, mode |
| `fluxa/logs` | `fluxa logs` | Last entries in err_stack |

**Why this matters for IoT:** A Fluxa program running on an RP2040 (via `std.http` over Wi-Fi) can be tuned, reloaded, and inspected by an AI agent that observes sensor readings and decides to adjust PID parameters — all without stopping the control loop. The Atomic Handover guarantees the adjustment is zero-downtime.

---

### 17.6 std.libv — N-Dimensional Vectors, Matrices, Tensors

**Status: ✅ implemented**

`std.libv` brings GLM-style vector and matrix math to Fluxa. The mental model is deliberately identical to GLSL/GLM — anyone who has written a shader or used OpenGL already knows the API. No new types are introduced: all storage is backed by `float arr` or `int arr` (always flat, col-major). The lib adds shape semantics and algebraic operations on top of existing Fluxa arrays.

**Design principles:**
- **No new types** — `float arr` is the storage. libv adds operations, not a parallel type system.
- **Col-major storage** — same as GLSL/OpenGL. No silent transposes when sending data to the GPU.
- **In-place by default** — all operations that can be in-place are. Essential for RP2040 where heap allocation is scarce.
- **Caller allocates for output** — functions that produce a new vector/matrix take the output arr as first argument.
- **Runtime shape validation** — mismatched shapes produce a runtime error with line number, consistent with Fluxa fail-fast semantics.

#### Initializers

```fluxa
import std libv as v

// Named initializers — GLSL-compatible
float arr pos[2]  = v.vec2      // 2D vector  — [0, 0]
float arr pos[3]  = v.vec3      // 3D vector  — [0, 0, 0]
float arr pos[4]  = v.vec4      // 4D / RGBA  — [0, 0, 0, 0]
float arr m[4]    = v.mat2      // 2×2 matrix — identity
float arr m[9]    = v.mat3      // 3×3 matrix — identity
float arr m[16]   = v.mat4      // 4×4 matrix — identity (shader standard)
int arr  px[2]    = v.ivec2     // integer vec2
int arr  px[3]    = v.ivec3     // integer vec3

// Generic initializers — arbitrary shapes
float arr a[8]    = v.vec(8)    // N-vector, all zeros
float arr b[12]   = v.mat(3,4)  // 3×4 matrix, all zeros
float arr t[27]   = v.tens(3,3,3) // 3×3×3 tensor, all zeros
```

Identities: `v.mat2`, `v.mat3`, `v.mat4` initialize to the identity matrix. All other initializers produce zeros.

#### Vector operations

All modify the first argument in-place unless the operation is inherently scalar-returning:

```fluxa
v.add(a, b)           // a = a + b
v.sub(a, b)           // a = a - b
v.scale(a, 2.0)       // a = a * scalar
v.normalize(a)        // a = a / norm(a)
v.negate(a)           // a = -a
v.lerp(a, b, 0.5)     // a = mix(a, b, t)

float d = v.dot(a, b)   // scalar — dot product
float n = v.norm(a)     // scalar — Euclidean length
float angle = v.angle(a, b) // scalar — angle between vectors

// vec3 only
float arr c[3] = v.vec3
v.cross(c, a, b)      // c = a × b  (caller allocates output)

// Shape mismatch → runtime error
v.add(vec3_arr, vec4_arr)
// [fluxa] Runtime error (line N): libv: shape mismatch (3 != 4)
```

#### Matrix operations

```fluxa
// Multiply — caller allocates result
float arr r[16] = v.mat4
v.matmul(r, m1, m2)       // r = m1 × m2

v.transpose(m)            // in-place for square matrices
v.identity(m)             // reset to identity

float arr inv[16] = v.mat4
v.inverse(inv, m)         // inv = m⁻¹ (caller allocates)

float det = v.det(m)      // scalar — determinant
```

#### 3D transform helpers (shader / raylib workflow)

```fluxa
// Build transform matrices — all modify m in-place
v.translate(m, tx, ty, tz)
v.rotate(m, angle, ax, ay, az)
v.scale_mat(m, sx, sy, sz)

// Projection and view — result written into m
v.perspective(m, fov, aspect, near, far)
v.ortho(m, left, right, bottom, top, near, far)
v.lookat(m, eye_arr, center_arr, up_arr)
```

#### Tensor operations

```fluxa
float arr t[27] = v.tens(3,3,3)
v.tens_add(t, t2)          // element-wise add
v.tens_scale(t, 0.5)       // scalar multiply
float arr s[9] = v.mat3
v.tens_slice(s, t, 0)      // extract slice 0 along first axis
```

#### prst compatibility

```fluxa
prst float arr weights[16] = v.mat4   // survives hot reloads
```

`prst float arr` works exactly like any other `prst arr` — the flat storage serializes normally. Shape is implicit in the declared size.

#### toml declaration

```toml
[libs]
std.libv = "1.0"
```

No C dependencies — pure C99 ~800 lines. Works on RP2040 and ESP32.

---

### 17.7 std.libdsp — DSP and Radar Math

**Status: ✅ implemented** — requires std.libv.

`std.libdsp` provides frequency-domain operations for signal processing and radar applications. It uses `std.libv` arrays as its native storage format. FFT operates in-place on `float arr` — no separate complex type, interleaved real/imag layout.

```toml
[libs]
std.libv   = "1.0"   # required
std.libdsp = "1.0"
```

**Core operations:**

```fluxa
import std libv   as v
import std libdsp as dsp

// FFT — in-place, power-of-2 sizes
// Interleaved layout: [re0, im0, re1, im1, ...]
float arr signal[2048] = v.vec(2048)   // 1024-point complex FFT
dsp.fft(signal)           // forward FFT in-place
dsp.ifft(signal)          // inverse FFT in-place

// Windowing — applied before FFT
dsp.window(signal, "hann")     // Hann window
dsp.window(signal, "hamming")  // Hamming
dsp.window(signal, "blackman") // Blackman
dsp.window(signal, "rect")     // Rectangular (no window)

// Power spectrum
float arr psd[1024] = v.vec(1024)
dsp.power(psd, signal)    // psd[i] = re[i]² + im[i]²

// Filters
float arr h[64] = v.vec(64)
dsp.fir(signal, h)        // FIR filter, h = coefficients
dsp.iir(signal, b, a)     // IIR filter, b/a = coefficients

// Radar-specific
float arr rd[512] = v.vec(512)
dsp.range_doppler(rd, signal, prf, fs)  // range-Doppler map
dsp.cfar(rd, guard, ref, threshold)     // CFAR detector
dsp.matched_filter(signal, template)    // matched filter correlation
dsp.stft(output, signal, win_size, hop) // Short-time Fourier transform
```

**Implementation note:** FFT is a pure C99 Cooley-Tukey implementation (~300L), zero external deps — embedded-friendly. FFTW can be optionally linked for larger workloads via `[libs.libdsp] backend = "fftw"` in fluxa.toml.

---

## 18. Handover Latency — State Gap

*See §10 for the three-stage live update model (Script Swap, Atomic Handover, Runtime Swap).*

In mission-critical systems the relevant question is not "how long does the gap last" — it is "what is the guaranteed worst case." This section documents the real state gap for each stage.

### 18.1 Gap Definition

The state gap is the interval between the last statement executed by the previous runtime and the first statement executed by the new runtime. During this interval no user code is executing. In all stages, the runtime never starts with partially applied state — the gap is always from a valid previous state to a completely validated new state.

### 18.2 Stage 1 Gap — fluxa apply (Script Swap)

See §10.1 for mechanism. State gap summary:

| Component | Typical Time | Notes |
|---|---|---|
| Parse + Resolve of new script | ~1–5ms | Proportional to file size |
| PrstPool migration | ~1–50µs | Proportional to prst var count |
| Wait for safe point | 0 to 1 cycle | Worst case: 1 full loop iteration |
| **Total typical** | **~2–10ms** | Dominated by parse, not migration |

### 18.3 Stage 2 Gap — fluxa handover (Atomic Handover)

See §10.2 for mechanism. Steps 1–3 run with Runtime A fully active — zero gap. The real gap occurs only at Step 4 (Switchover).

| Component | Time | Notes |
|---|---|---|
| Steps 1–3 (Standby, Migration, Dry Run) | Zero | Runtime A continues active throughout |
| gc_collect_all() before swap | ~10–100µs | Proportional to GC objects |
| Atomic pool swap (pointer) | ~nanoseconds | Pointer operation — submicrosecond |
| grace_period_ms (default 100ms) | 100ms | Configurable. Use 0 for mission-critical. |
| **Total without grace period** | **~10–200µs** | Worst case on modern hardware |
| **Total with default grace period** | **~100ms** | Dominated entirely by grace period |

*Mission-critical: `fluxa handover --grace 0` or `grace_period_ms = 0` in fluxa.toml. The swap itself is submicrosecond.*

### 18.4 Stage 3 Gap — fluxa update (Runtime Swap)

See §10.3 for mechanism. The gap is dominated by `execve` startup time of the new binary.

| Component | Typical Time | Notes |
|---|---|---|
| Serialize prst pool | ~100–500µs | Proportional to prst var count |
| Write snapshot to /tmp | ~10–50µs | File I/O |
| execve + new binary startup | ~10–50ms | OS process replacement |
| Snapshot load in new binary | ~100–500µs | Deserialize prst pool |
| **Total typical** | **~10–50ms** | Dominated by process startup |

*No Dry Run in Stage 3 — use preflight `-p` to verify the new binary before sending.*

### 18.5 RP2040 — Flash Mode (Stage 3 equivalent)

| Component | Typical Time | Notes |
|---|---|---|
| PrstPool serialization | ~100–500µs | Proportional to prst var count |
| Flash write (reserved sector) | ~1–5ms | Depends on Flash hardware |
| Firmware reboot | ~10–50ms | Bootloader + peripheral init |
| Deserialization + Dry Run | ~1–5ms | Full validation before taking control |
| **Total RP2040** | **~15–60ms** | Deterministic — same behavior every time |

### 18.6 Comparison

| | Stage 1 (apply) | Stage 2 (handover) | Stage 3 (update) |
|---|---|---|---|
| Gap (typical) | ~2–10ms | ~10–200µs | ~10–50ms |
| Gap (mission-critical) | ~2–10ms | ~10–200µs | ~10–50ms |
| Dry Run | No | Yes | No (preflight instead) |
| Config changes | No | Yes | Yes (new binary) |
| Binary changes | No | No | Yes |
| RP2040 equivalent | Config reload | Firmware patch | Full firmware update |

---

## A. Practical Examples

### A.1 Hot Reload with prst

```fluxa
Block PongGame {
    prst int ball_x = 2
    prst int speed  = 1
    prst bool running = true
    fn run() nil {
        while running {
            ball_x = ball_x + speed
        }
    }
}
// fluxa run game.flx -dev
// → edit speed = 2 → auto reload
// → ball_x preserved, speed updated
```

### A.2 Atomic Handover via CLI

```bash
# Replace v1.flx with v2.flx preserving all prst state
fluxa handover v1.flx v2.flx

# Expected output:
# [handover] step 1: standby OK (slots=N)
# [handover] step 2: migration OK (M bytes)
# [handover] step 3: Dry Run OK (B cycle=K)
# [handover] step 4: switchover OK (A cycle=J)
# [handover] step 5: cleanup OK — handover COMMITTED
```

### A.3 fluxa.toml for Large Program

```toml
[runtime]
gc_cap         = 1024
prst_cap       = 512    # 500+ prst variables
prst_graph_cap = 1024   # many registered dependencies
```

### A.4 fluxa explain — Introspection

```bash
fluxa explain game.flx
# ── prst (survive reload) ──────────────────────────────────
# score int = 100
# running bool = true
# ── Blocks ─────────────────────────────────────────────────
# Game (root) — 1 prst, 2 fn
# ── Registered dependencies ────────────────────────────────
# score <- show_score
```

### A.5 FFI — Reading Two Integers from stdin

```toml
[ffi]
libc = "auto"

[ffi.libc.signatures]
scanf = "(char*, int*, int*) -> int"
```

```fluxa
int width = 0
int height = 0
danger {
    int matched = libc.scanf("%d %d", width, height)
}
print(width)     // first integer typed
print(height)    // second integer typed
```

### A.6 Warm Path — Observing Promotion

```fluxa
// After 2+ stable calls, compute() is promoted to warm tier.
// Reads touch 9 bytes instead of 418+ bytes (with 20 prst vars).
prst int p0 = 0
prst int p1 = 1
// ... 20 prst vars total

fn compute(int n) int {
    int a = n
    int b = n + 1
    return a + b    // warm: a, b read from WarmSlot (1B) + stack (8B) = 9B each
}

int total = 0
int i = 0
while i < 1000000 {
    total = total + compute(i)
    i = i + 1
}
print(total)
```

---

## B. FFI Pointer Mapping — Full Reference

### B.1 Signature Declaration

```toml
[ffi]
libm = "auto"
libc = "auto"
str_buf_size = 1024    # writable char* buffer size per arg (default 1024, range 64–65536)

[ffi.libm.signatures]
sqrt      = "(double) -> double"
modf      = "(double, double*) -> double"
frexp     = "(double, int*) -> double"
lgamma_r  = "(double, int*) -> double"

[ffi.libc.signatures]
scanf  = "(char*, int*) -> int"
fgets  = "(char*, int, dyn) -> dyn"
fopen  = "(char*, char*) -> dyn"
fclose = "(dyn) -> int"
fread  = "(uint8_t*, int, int, dyn) -> int"
puts   = "(char*) -> int"
strlen = "(char*) -> int"
```

### B.2 Type Mapping Table

| C signature type | Fluxa type | Runtime behaviour |
|---|---|---|
| `int` / `double` / `bool` | scalar | passed by value, no writeback |
| `int*` | `int` | `&var` passed; `int32_t` written back |
| `double*` / `float*` | `float` | `&var` passed; `double` written back |
| `bool*` | `bool` | `&var` passed; `int32_t` written back |
| `char*` | `str` | buffer of `str_buf_size` bytes allocated; result strdup'd back to `str` |
| `uint8_t*` / `void*` buf | `int arr` | elements gathered to `uint8_t[]`; bytes scattered back after call |
| `dyn` / `struct*` | `dyn` | `VAL_PTR` extracted from `dyn[0]` |
| pointer return | `dyn` | wrapped as `VAL_PTR` in `dyn[0]` |

### B.3 str_buf_size Configuration

`str_buf_size` under `[ffi]` controls the writable buffer the runtime allocates for every `char*` output argument. Default is 1024 bytes. Clamped to `[64, 65536]`. A warning is printed to stderr if the value falls outside this range.

---

## C. Programming Roadmap — Learning Order

**Stage 1 — Script basics**

Write a `.flx` file with no `prst`. Run with `fluxa run file.flx`. Cover: primitives, arithmetic, `if/else`, `while`, `for..in`, functions, `arr`, `dyn`. No project directory needed.

**Stage 2 — Persistent state**

Add `prst` declarations. Run with `fluxa run file.flx -proj ./myproject`. Observe that `prst` values survive `fluxa apply`. Use `fluxa explain file.flx` to inspect live state.

**Stage 3 — Hot reload development**

Run with `fluxa run file.flx -dev`. Edit and save the file. Watch values reload while `prst` state is preserved. Use this loop for iterating on formulas, thresholds, and logic without restarting.

**Stage 4 — Blocks and typeof**

Encapsulate state and behaviour in `Block`. Use `typeof` to create independent instances. Store instances in `dyn` arrays. Verify that modifying one instance does not affect others — including instances stored in dyn.

**Stage 5 — Error handling**

Wrap risky operations in `danger`. Read `err[0]` after the block. Understand that outside `danger`, errors abort with a line number — this is the intended behaviour.

**Stage 6 — Standard library**

Declare libs in `fluxa.toml` under `[libs]`. Import in code. Start with `std.math` (no danger, no files), then `std.strings`, then `std.json` and `std.csv` (which require `danger` for file operations).

**Stage 7 — C interop via FFI**

Use `fluxa ffi inspect <libname>` to get a starter signature template. Declare the library in `[ffi]`. Add function signatures in `[ffi.<lib>.signatures]`. Call inside `danger`. Let the runtime handle pointer marshalling.

**Stage 8 — Atomic Handover**

Write v1 of a program. Run it. Write v2 with new logic. Execute `fluxa handover v1.flx v2.flx`. Observe the Dry Run, migration, and switchover log. Verify that `prst` state survived and the new logic is active.

**Stage 9 — Warm path and performance**

Write a PROJECT-mode program with several `prst` variables and functions that are called in loops. Run it. After 2+ stable calls, functions are automatically promoted to the warm tier — reads drop from touching 418+ bytes (cold, with 20 prst vars) to 9 bytes (warm). No configuration needed. Type-polymorphic functions stay on the cold path — the QJL guard fires and demotes them when types diverge.

**Stage 10 — Embedded targets**

Reduce `gc_cap`, `prst_cap`, and `str_buf_size` in `fluxa.toml` to fit target memory budgets. Cross-compile with `make build-rp2040` or `make build-esp32`. The warm path is especially valuable on RP2040 — 9 bytes touched per warm read vs 418+ bytes cold reduces SRAM bus pressure proportionally to the number of `prst` variables.

---

## D. Future Proposals

### D.1 Huge Pages — `FLUXA_HUGEPAGES=1`

**Status: 📋 proposal — not scheduled**

On Linux x86-64 and ARM64, the runtime's working set (PrstPool, GCTable, Value stack) can span enough virtual pages to pressure the TLB (Translation Lookaside Buffer). With 4KB pages and 500+ prst variables, the PrstPool alone crosses multiple pages. Each TLB miss costs ~100 cycles for a page walk.

Huge pages (2MB) reduce the TLB footprint of the same working set from hundreds of entries to single digits. The mechanism is `madvise(MADV_HUGEPAGE)` — Transparent Huge Pages (THP). Zero system configuration required. The kernel promotes pages automatically when it decides it is beneficial.

**Candidate allocations:**

| Allocation | Size (typical) | Benefit |
|---|---|---|
| PrstPool buffer | grows with prst var count | High — accessed every hot loop iteration |
| GCTable (GCEntry array) | 24B × gc_cap | Medium — accessed at while back-edge |
| Value stack | 512 × 32B = 16KB | Low — already fits in L1 cache |
| WarmProfile | 8.7KB max | None — already in L1 |

**Implementation sketch:**

```c
#ifdef FLUXA_HUGEPAGES
#include <sys/mman.h>
#define fluxa_hint_huge(ptr, size) \
    madvise((void*)(ptr), (size), MADV_HUGEPAGE)
#else
#define fluxa_hint_huge(ptr, size) ((void)0)
#endif
// Applied after prst_pool_init() and gc_init()
```

**Build flag:** `make build HUGEPAGES=1` → `-DFLUXA_HUGEPAGES=1`

**Not the default** — THP has overhead on small short-lived programs. Only beneficial for long-running PROJECT-mode programs with large PrstPools (500+ prst vars).

**Platform:** Linux only (x86-64, ARM64). No-op on RP2040/ESP32 (no virtual memory). macOS uses a different mechanism (`VM_FLAGS_SUPERPAGE_SIZE_2MB`) — not planned.

**Before implementing:** Profile with `perf stat -e dTLB-load-misses` on a realistic workload. If TLB misses are not in the top 5 bottlenecks, the change is not worth the complexity.


---

## 19. Runtime Update Protocol — Stage 3 Detail

**Status: ✅ implemented**

`fluxa update` replaces the running `./fluxa` binary with zero downtime. The running program's `prst` state is preserved across the binary swap — equivalent to `HANDOVER_MODE_FLASH` for RP2040 but for the host binary itself.

### Usage

```bash
fluxa update ./fluxa_v2          # replace running binary
fluxa update ./fluxa_v2 -p       # preflight: verify binary before sending
FLUXA_TEST_UPDATE=1 bash tests/sprint13/update_protocol.sh  # full round-trip test
```

*Full protocol, security model, and IPC opcode documented in §10.3.*

### Summary

### Restart detection

The new binary checks `FLUXA_RESTART_SNAPSHOT` environment variable before any command dispatch. If set:

1. Loads serialized `PrstPool` from the snapshot file
2. Logs `[fluxa] restart: loaded N prst vars from snapshot`
3. Deletes the snapshot file (single-use)
4. Clears the env var (not inherited by grandchildren)
5. Continues normal execution with restored state

Works with all run modes: `-dev`, `-prod`, and `FLUXA_SECURE` builds.

---

## 20. Huge Pages — FLUXA_HUGEPAGES=1

**Status: ✅ implemented (opt-in)**

```bash
make FLUXA_HUGEPAGES=1 build
```

Calls `madvise(MADV_HUGEPAGE)` on the two large arrays inside `ASTPool`:
- `nodes[4096]` — ~400KB of ASTNode structs walked by the runtime every cycle
- `str_buf[65536]` — 64KB string arena walked by the resolver

The kernel backs these arenas with 2MB transparent huge pages instead of 4KB pages, reducing dTLB misses when the runtime walks the AST in tight loops.

### When to enable

**Good candidates (RAM-intensive, long-running):**

| Workload | Why it helps |
|---|---|
| Programs with large ASTs (>1000 nodes) | Runtime walks AST every cycle — many dTLB misses |
| Long-running `-prod` processes (hours/days) | TLB pressure accumulates over millions of cycles |
| `std.infer` workloads (LLM inference in a loop) | Large model + large AST = TLB thrashing |
| `std.libv` / `std.libdsp` with large arrays | Matrix ops touch many pages per cycle |
| Digital twin simulations | Many prst vars + deep prst_graph traversal |

**No benefit (skip it):**

| Workload | Why it doesn't help |
|---|---|
| Short scripts (run once and exit) | Kernel won't promote pages before process ends |
| Small programs (<200 AST nodes) | Fits in L2 cache — TLB is not the bottleneck |
| RP2040 / ESP32 embedded targets | No `madvise` syscall available (no-op on non-Linux) |
| IoT sensor loops with simple logic | CPU-bound on the sensor I/O, not memory |

### How to measure

Before enabling, verify TLB pressure is real:

```bash
# Check dTLB miss rate on your workload
perf stat -e dTLB-load-misses,dTLB-loads ./fluxa run main.flx -prod

# If miss rate > 5%, Huge Pages will help
# If miss rate < 1%, skip it — overhead is negligible either way

# Compare with and without:
make FLUXA_HUGEPAGES=1 build && perf stat ./fluxa run bench.flx
make build               && perf stat ./fluxa run bench.flx
```

### Implementation

`pool_init()` in `src/pool.h` calls `FLUXA_POOL_MADVISE()` on both arrays after the pool is initialized. Linux only — the macro is a no-op on macOS and embedded targets. No runtime cost if huge pages are not available (kernel silently ignores the hint).


## v0.22 — Module Block singletons

`mod.Block.method(args)` / `mod.Block.field` parse in expression and
statement positions (MEMBER_CALL/ACCESS/ASSIGN with mangled owner
`mod__Block`). Inside a module, references to same-module Blocks mangle
automatically. Tests: tests/modules/modules.sh c22a–c22e.
