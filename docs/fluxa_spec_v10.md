# FLUXA

**Technical Specification**

**v0.10 — Sprint 11 Edition**

*Base v0.10 + Sprint 11: Warm Path · FFI Pointer Mapping · Full Stdlib · Atomic Handover*

*FLUXA v0.10 — Hobby language — Rio de Janeiro, Brazil*

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

```fluxa
int arr values[3] = [1, 2, 3]
values[0] = 9       // ok
values[5] = 1       // [fluxa] Runtime error (line N): array index out of bounds
```

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
Block Sensor { prst float reading = 0.0; fn set(float v) nil { reading = v } fn get() float { return reading } }
Block s1 typeof Sensor; Block s2 typeof Sensor
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

### 4.2 PrstPool and PrstGraph Caps

The initial size of the prst variable pool and the dependency graph is configurable via `fluxa.toml`. Both structures are dynamic — grow via realloc without a fixed ceiling.

```toml
[runtime]
gc_cap         = 1024   # GC hard cap (static array)
prst_cap       = 64     # PrstPool initial capacity (dynamic, grows via realloc)
prst_graph_cap = 256    # PrstGraph initial capacity (dynamic, max 65536)
```

*`prst_cap` and `prst_graph_cap` are initial caps, not ceilings. Structures grow automatically. Setting the correct initial cap improves allocation performance — it does not limit usage.*

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
while i < 3 { print(i); i = i + 1 }
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
    prst int total = 0
    fn increment() nil { total = total + 1 }
    fn value() int     { return total }
}

Counter.increment()
Counter.total = 10    // direct field access
```

### 7.2 typeof — Cloning with Isolated State

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

Isolates risky operations. Errors inside the block do not interrupt flow — they are accumulated in the `err` stack. `danger` is mandatory for `import c` FFI calls.

```fluxa
danger {
    float r = libm.sqrt(-1.0)    // error captured in err
}
if err != nil { print(err[0]) }
```

### 8.4 err — Error Stack

- `err[0]` → most recent error; `err[1]` → previous (ring buffer, 32 entries)
- Each entry: message + context (fn/Block) + source line
- `err` is automatically nil before any `danger` block

| ErrKind | When generated |
|---|---|
| ERR_FLUXA | Fluxa runtime error: div/0, OOB, undefined variable |
| ERR_C_FFI | FFI call failure via import c |
| ERR_RELOAD | prst type collision or invalidation during reload |
| ERR_HANDOVER | Dry Run validation failure (Atomic Handover) |

---

## 9. Hot Reload

Hot reload is a first-class citizen. The runtime maintains a dependency graph (PrstGraph) between prst variables and active executions.

### 9.1 Live vs Static Modules

- `live` → reloadable automatically on save
- `static` → loaded once at startup
- `c` → FFI integration (only inside danger)

### 9.2 Execution Mode — Script vs Project

| Mode | Condition / Characteristics |
|---|---|
| FLUXA_MODE_SCRIPT | No prst declarations. PrstPool and PrstGraph not instantiated. Zero overhead. |
| FLUXA_MODE_PROJECT | At least one prst declaration. PrstPool + PrstGraph active. Reload-capable. |

### 9.3 PrstGraph — Dependency Graph

Records which function/method read each prst variable. Dynamic — grows via realloc.

- `prst_graph_record(g, name, ctx)` — records with deduplication
- `prst_graph_invalidate(g, name)` — removes deps, readers re-register
- `prst_graph_checksum(g)` — FNV-32; used in Handover for integrity
- `prst_graph_init_cap(g, cap)` — initializes with configurable cap

### 9.4 Reload Behavior Table

| Action on Reload | Result | Guarantee |
|---|---|---|
| Keeps `prst int a` | Retains value in memory | prst contract |
| Removes `prst int a` | Atomic death of a and dependents | No ghost state |
| Changes type of `prst int a` | ERR_RELOAD, previous value preserved | Type immutable in prst |
| Changes fn signature | fn restarted as new execution | No previous iteration state |
| Changes Block definition | Block root + instances invalidated | Cascade invalidation |

---

## 10. Atomic Handover

The Atomic Handover replaces a running runtime with a new one without stopping the system. No prst data is lost. No cycle is interrupted unless the new program is invalid.

*Central invariant: Runtime A is never modified during a handover attempt. Any failure in B destroys B and keeps A active without corruption.*

### 10.1 5-Step Protocol

| Step | Name | What happens |
|---|---|---|
| 1 | Standby | Runtime B allocated (calloc). New program parsed and resolved. Failure here → B discarded, A intact. |
| 2 | Migration | PrstPool and PrstGraph from A serialized into flat binary snapshot. FNV-32 checksum calculated. Snapshot deserialized into B with validation of magic + version + checksum. |
| 3 | Dry Run | B executes the complete program with dry_run=1. All logic runs normally; stdout and FFI suppressed. Any error in err_stack → ERR_HANDOVER in A, handover aborted. |
| 4 | Switchover | Waits for safe point in A (call_depth==0 && danger_depth==0), with configurable timeout (default 5s). Pool from B transferred atomically. |
| 5 | Cleanup | Grace period (default 100ms). Temporary B destroyed. runtime_apply() starts real execution of B with the transferred pool. |

### 10.2 Snapshot — Flat Binary Format

The snapshot is a contiguous buffer with no pointers — safe for writing to Flash (RP2040).

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

### 10.3 Protocol Versioning

- v1.000 — first stable beta version
- v1.xxx — compatible with v1.000 (same major)
- v2.000 — breaking change; rejects v1.xxx snapshots

### 10.4 Dry Run — dry_run

When `dry_run = 1`, all external output is suppressed — print(), FFI calls, scope writes. Internal logic executes normally: loops, calculations, prst reads/writes happen and are validated. If `rt_error()` is called during a dry_run, ERR_HANDOVER is generated in A and the handover is aborted.

### 10.5 RP2040 — Flash Mode

On hardware with limited SRAM (264 KB), two runtimes in parallel don't fit. HANDOVER_MODE_FLASH serializes the snapshot to a reserved Flash sector before rebooting. After boot, the new firmware reads the snapshot, deserializes state, runs the Dry Run for validation, and only assumes control after approval.

| Mode | Platform | Behavior |
|---|---|---|
| HANDOVER_MODE_MEMORY | x86 / ARM64 | Two runtimes in parallel in RAM |
| HANDOVER_MODE_FLASH | RP2040 (264 KB SRAM) | Snapshot → Flash → reboot → deserialize → dry_run |

---

## 11. CLI — Commands

```
fluxa run <file.flx>                Run — auto-detects script vs project
fluxa run <file.flx> -proj <dir>    Run as project (enables prst, reads fluxa.toml)
fluxa run <file.flx> -dev           Dev: watch + auto-reload on save (inotify/kqueue)
fluxa run <file.flx> -prod          Prod: manual reload via fluxa apply
fluxa run <file.flx> -p             Preflight: parse + resolve only, no execution
fluxa explain <file.flx>            Print prst state + dependency graph
fluxa apply <file.flx>              One-shot reload preserving prst state
fluxa handover <old> <new>          Atomic Handover: replace old.flx with new.flx
fluxa test-handover                 Internal suite: validates all 5 protocol steps
fluxa observe <var>                 Stream live value of a prst variable (IPC)
fluxa set <var>=<value>             Mutate a prst variable in a live runtime (IPC)
fluxa logs                          Stream runtime log output (IPC)
fluxa status                        Snapshot: cycle count, prst count, errors, mode
fluxa init                          Create a new fluxa.toml in the current directory
fluxa ffi list                      List available shared libraries via ldconfig
fluxa ffi inspect <lib>             Generate suggested toml signatures for a library
fluxa runtime info                  Show current runtime configuration
```

### 11.1 fluxa -dev: File Watcher

| OS | Backend | Mechanism |
|---|---|---|
| Linux | inotify | IN_CLOSE_WRITE \| IN_MOVED_TO |
| macOS / BSD | kqueue | EVFILT_VNODE NOTE_WRITE \| NOTE_ATTRIB |
| Others | select() | stat() mtime, 500ms interval |

### 11.2 IPC — Unix Socket

In `-prod` mode the runtime opens a Unix socket at `~/.fluxa/<pid>.sock`. The IPC server accepts JSON commands from `fluxa observe`, `fluxa set`, `fluxa logs`, and `fluxa status`. The socket is also used by `std.mcp` to expose Fluxa state as MCP tools.

---

## 12. FFI (C)

Integration with C is permitted exclusively inside `danger` blocks. Implemented via dlopen/libffi — zero static linking overhead.

```fluxa
import c libm

float result = 0.0
danger {
    result = libm.sqrt(16.0)
}
print(result)    // 4
print(err)       // nil — no error
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
int a = 0; int b = 0
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
    WarmProfile    warm;             // compact execution profile (8.7 KB max)
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
| ASTPool nodes | 4096 nodes | Parse arena — batch free at end |
| ASTPool strings | 64 KB | Interned string buffer |
| WarmProfile | 8.7 KB max | 32 functions × 276 bytes; zero malloc per call |
| WarmSlot | 1 byte/node | 3-bit type + 1-bit QJL guard + 4-bit obs counter |
| warm_local flag | 1 byte/ASTNode | Set by resolver; skips prst_pool_has for fn-local vars |

### 13.4 Warm Path — Sprint 11

#### Inspiration

TurboQuant (Google Research, ICLR 2026) applies two-stage quantization to KV cache vectors in large language models: a random orthogonal rotation (WHT/PolarQuant) concentrates vector energy and enables near-optimal scalar quantization per coordinate; a 1-bit QJL residual removes the inner-product bias introduced by the first stage. Result: 3-4 bits per value with near-zero distortion, applied once per forward pass.

The insight transferred to Fluxa: a function's execution state is a vector of types observed at each stack slot. Two executions of the same function with the same type sequence produce the same WHT signature. A 1-bit residual (the QJL guard) detects divergence. This is the first known application of this quantization technique to a language runtime execution profiler.

#### Three execution tiers

**Tier 0 — Cold (first 4 calls):** full AST walker. The resolver has already set `warm_local=1` on every `NODE_IDENTIFIER` inside a fn body (`in_func_depth > 0`, not `prst`), which skips `prst_pool_has` (O(n) strcmp scan) even in cold mode. The runtime records the observed ValType (3 bits) of each stack slot into the function's `WarmFunc`.

Observation caps at `WARM_OBS_LIMIT = 4` calls. After 4 calls: either promoted (Tier 1) or cold-locked permanently. Cold-locked functions have zero overhead — just the warm_local direct stack read.

**Tier 1 — Warm (stable_runs ≥ 2):** promoted functions skip ASTNode traversal entirely. Per `rt_get`:

1. Load `rt->current_wf` — pointer set once at `call_function` entry, zero hash cost inside the loop
2. Read `WarmSlot` (1 byte): `qjl_guard` bit + `observed_type` (3 bits)
3. Read `stack[resolved_offset]` (8 bytes)
4. QJL residual: if `warm_type_from_val_type(v.type) == ws->observed_type` → return. **9 bytes total.**
5. On type mismatch: QJL guard fires, `stable_runs` reset, function demoted to Tier 0

**Tier 2 — Hot (bytecode VM):** `while` and `if` loops compiled to 3-address register bytecode. Unchanged by Sprint 11.

#### Key implementation details

**`warm_local` flag (resolver):** Set at resolve time for every `NODE_IDENTIFIER` and `NODE_VAR_DECL` where `in_func_depth > 0` and `persistent == 0`. Script-body declarations (`in_func_depth == 0`) are never warm_local — the script body is not a function scope. `prst` declarations inside functions are also excluded — they must be read via the pool, not the stack.

**`current_wf` cache (runtime):** The O(1) hash (`warm_profile_get_func`) is called **once** at `call_function` entry and the result stored in `rt->current_wf`. All `rt_get` calls for that function frame use the cached pointer — zero hash overhead inside the hot loop.

**TCO trampoline fix:** When a tail call targets a different function (`pong` calling `ping`), the trampoline updates both `rt->current_fn` and `rt->current_wf` before continuing. Without this fix, the wrong WarmFunc slot would be used for the new target function.

**Block methods excluded:** `current_instance != NULL` in Block method frames → warm path disabled. Block methods use `inst->scope`, not the stack-slot path, so `warm_local` would be incorrect.

**Hash table (WarmProfile):** Open-addressing hash keyed by `(uintptr_t)fn_node` — the ASTNode pointer is stable across all calls to the same function. Capacity `WARM_FUNC_CAP = 32` (power of 2 for fast modulo). When full, `warm_profile_get_func` returns NULL — the function silently falls back to the warm_local direct stack read.

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
| 11 | ✅ | Warm Path execution tier: warm_local resolver flag; WarmProfile (8.7 KB) with WHT path signature + QJL 1-bit guard per slot; O(1) hash keyed by fn_node\*; observation capped at 4 calls; promoted reads touch 9B vs 418B+ cold. fib +21%, block calls +12%, PROJECT mode +23%. Zero regression on VM. First use of TurboQuant-inspired quantization in a language runtime. |
| 12 | 🔲 | Stdlib: std.sqlite, std.http, std.mqtt, std.serial, std.i2c, std.pid, std.flxgraph, std.infer, std.mcp. All opt-in via fluxa.toml [libs]. |

---

## 16. fluxa.toml — Complete Configuration

```toml
# fluxa.toml — optional config at project root

[project]
name  = "my_project"
entry = "main.flx"

[runtime]
gc_cap         = 1024   # GC table hard cap (static array, default 1024)
prst_cap       = 64     # PrstPool initial capacity (dynamic, default 64)
prst_graph_cap = 256    # PrstGraph initial capacity (dynamic, default 256)
# When to increase:
# prst_cap > 64        → programs with many prst variables (e.g. 500+)
# prst_graph_cap > 256 → many functions reading many prst vars
# gc_cap < 1024        → memory-constrained environments (e.g. simulated RP2040)

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

[libs.csv]
max_line_bytes = 1024   # max bytes per CSV line (default 1024)
max_fields     = 64     # max fields for csv.field (default 64)

[libs.json]
max_str_bytes  = 4096   # max JSON string size (default 4096)
```

*`prst_cap` and `prst_graph_cap` are INITIAL caps, not ceilings. Structures grow via realloc automatically. Setting the correct initial cap only improves allocation performance — it does not limit usage.*

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
| std.mqtt | IoT | libmosquitto | 🔲 planned | MQTT protocol. prst: broker config, auto-reconnect, active subscriptions. |
| std.serial | IoT | libserialport | 🔲 planned | UART/serial. Fundamental for RP2040. Stable since 2013. |
| std.i2c | Robotics | libgpiod | 🔲 planned | I2C protocol. IMU reading, encoders. prst state: accumulated calibration. |
| std.pid | Robotics | own ~300L | 🔲 planned | Pure C PID controller. State (integral, prev_error) is prst by nature. |
| std.sqlite | Database | SQLite 3 | 🔲 planned | Embedded database. Zero external deps. Works on RP2040. Sensor logging. |
| std.http | Network | mongoose | 🔲 planned | HTTP + WebSocket in single-file C. Embedded-friendly. Stable for years. |
| std.flxgraph | Visual | Raylib | 🔲 planned | Graphics window, 2D/3D, input. C99, zero deps, Raspberry Pi-compatible. |
| std.infer | AI | llama.cpp/ggml | 🔲 planned | Local inference of quantized models. prst context survives reloads. |
| std.mcp | Protocol | mongoose HTTP | 🔲 planned | Fluxa as MCP server. Exposes observe/set/apply/handover as MCP tools. |

### 17.4 Library Memory Model

Libraries manage their own memory. Fluxa does not serialize library internals — only opaque references as `prst dyn`. The dataset survives a reload because the lib keeps the pointer alive.

```fluxa
prst dyn data = csv.load("sales.csv")    // loaded once
// fluxa apply → data survives, new formula recalculates
float total = csv.sum(data, "revenue") * 1.1
```

*RP2040 limitation: prst dyn of libs is not serialized to Flash (HANDOVER_MODE_FLASH). On hardware with limited SRAM, the dataset exists only while the process is alive. In HANDOVER_MODE_MEMORY (x86/ARM64) the pointer is preserved normally.*

### 17.5 std.mcp — Fluxa as MCP Server

Primary use case: operator modifies a running agent without taking it down. The agent processes messages continuously. Via MCP, the operator triggers `fluxa/apply` with new code. Atomic Handover guarantees zero downtime.

| MCP Tool | Description |
|---|---|
| fluxa/observe | Reads current value of a prst var in real time |
| fluxa/set | Mutates a prst var without stopping execution (applied at next safe point) |
| fluxa/apply | Hot reload — prst state preserved via runtime_apply |
| fluxa/handover | Full Atomic Handover (5 steps, Dry Run included) |
| fluxa/status | Snapshot: cycle count, prst count, errors, mode |
| fluxa/logs | Last entries in err_stack |

---

## 18. Handover Latency — State Gap

In mission-critical systems the relevant question is not "how long does the gap last" — it is "what is the guaranteed worst case." This section documents the real state gap for each operating mode.

### 18.1 Gap Definition

The state gap is the interval between the last statement executed by the previous runtime and the first statement executed by the new runtime. During this interval no user code is executing.

### 18.2 Case 1 — fluxa apply (script swap)

The runtime executes until a safe point (call_depth==0 && danger_depth==0), stops completely, and the new script starts with the migrated prst pool.

| Component | Typical Time | Notes |
|---|---|---|
| Parse + Resolve of new script | ~1–5ms | Proportional to file size |
| PrstPool migration | ~1–50µs | Proportional to number of prst vars |
| Wait for safe point | 0 to 1 cycle | Worst case: 1 iteration of the longest loop |
| Total typical | ~2–10ms | Dominated by parse, not migration |

### 18.3 Case 2 — fluxa handover (Atomic Handover)

Steps 1–3 (Standby, Migration, Dry Run) execute with Runtime A active — zero gap. The real gap occurs only in Step 4 (Switchover).

| Component | Time | Notes |
|---|---|---|
| Steps 1–3 (Standby, Migration, Dry Run) | Zero | Runtime A continues active throughout |
| gc_collect_all() before swap | ~10–100µs | Proportional to objects in GC |
| Atomic pool swap (pointer) | ~nanoseconds | Pointer operation — submicrosecond |
| grace_period_ms (default: 100ms) | 100ms | Configurable. Use 0 for mission-critical. |
| Total without grace period | ~10–200µs | Worst case on modern hardware |
| Total with default grace period | ~100ms | Dominated entirely by grace period |

*Mission-critical configuration: To minimize the gap use `fluxa handover --grace 0` or `grace_period_ms = 0` in fluxa.toml. The swap itself is submicrosecond.*

### 18.4 RP2040 — Flash Mode

| Component | Typical Time | Notes |
|---|---|---|
| PrstPool serialization to flat buffer | ~100–500µs | Proportional to prst var count |
| Flash write (reserved sector) | ~1–5ms | Depends on snapshot size and Flash hardware |
| Firmware reboot | ~10–50ms | Bootloader + peripheral init |
| Deserialization + Dry Run | ~1–5ms | Full validation before taking control |
| **Total RP2040** | **~15–60ms** | Deterministic — same behavior every time |

*Consistency guarantee: In all modes, the runtime never starts with partially applied state. The gap is always from a valid previous state to a completely validated new state — no intermediate observable state exists.*

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
prst int p0 = 0; prst int p1 = 1    // ... 20 prst vars total

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
