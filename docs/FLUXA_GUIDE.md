# How to Program in Fluxa

A step-by-step reference for writing correct Fluxa programs. Read this before writing any Fluxa code — especially the **Hard Rules** section. The mistakes listed there are the most common errors made by anyone (human or AI) new to the language.

---

## 1. The Mental Model

Fluxa's rules all flow from a small set of invariants. Internalize these seven before writing code:

1. **Unique ownership.** Every value has exactly one owner. No value is accessed outside its scope. There is no shared mutable state between functions — data flows through arguments and return values only.
2. **No global scope.** Variables declared at the top level belong to the program's execution context — they are not visible inside functions or Block methods. There is no free-variable capture, ever.
3. **Everything is passed.** Every value a function or method uses arrives as an argument or parameter. If a worker needs a server handle and a DB connection, they are parameters.
4. **No `dyn` inside a Block.** `dyn` lives at the program level and in functions — never as a Block field.
5. **No `danger` inside a Block.** Error containment belongs to global functions and the program level.
6. **`prst` is state that survives time.** A `prst` variable is simply marked to survive — hot reloads, Atomic Handover, runtime swap. It is how you make state explicit and durable; everything else dies and is reborn on each reload.
7. **`err` is a ring buffer; `danger` contains runtime-killing errors.** Division by zero, overflow, failed IO — anything that would otherwise abort the runtime is captured by `danger {}` into the `err` ring. That is why all IO goes inside `danger`.

A practical consequence of rules 1–3: if a variable "isn't found" inside a function, the fix is never "make it global" (there is no global) — it is "pass it as a parameter".

---

## 2. Variables

Every variable declaration starts with its type. No inference, ever.

```fluxa
int     x       = 10
float   pi      = 3.14159
str     name    = "fluxa"
bool    active  = true
```

**`prst` — persistent across reloads:**

```fluxa
prst int   counter = 0      // survives hot reload
prst float temp    = 0.0
prst str   label   = "init"
```

`prst` does not change ownership. It only marks the variable for serialization across reloads. A `prst` variable still has exactly one owner.

**`prst` is valid at the top level, inside a Block, and inside a function.** When declared inside a function, the value persists between calls to that function — like a static local variable in C.

```fluxa
fn counter() int {
    prst int count = 0
    count = count + 1
    return count
}

print(counter())   // 1
print(counter())   // 2
print(counter())   // 3
```

Useful for memoization, call counting, and accumulation without exposing state outside the function.

---

## 3. Arrays

Fixed-size, typed. Declare size at write time.

```fluxa
int arr   values[3]    = [1, 2, 3]
float arr temps[100]   = 0.0        // all 100 elements set to 0.0
bool arr  flags[64]    = false

values[0] = 9
print(values[1])
```

Out-of-bounds access produces a runtime error with line number — never silent.

---

## 4. dyn — Dynamic Array

Variable-size, heterogeneous. Each element carries a runtime type tag.

```fluxa
dyn events = [1, "hello", true, 3.14]
events[4] = 99          // auto-grows
int n = len(events)
for e in events { print(e) }
```

**`dyn` is for use at the program level and in functions. It is never a valid Block field.** See section 7.

---

## 5. dyn with Block instances

`dyn` can store Block instances alongside primitives. When a Block instance is placed into a `dyn`, the runtime creates a fully independent copy — the same isolation as `typeof`. The original Block and the element inside the `dyn` are completely separate: mutations to one never affect the other.

```fluxa
Block Sensor {
    prst float reading = 0.0
    fn set(float v) nil { reading = v }
    fn get() float { return reading }
}

Block a typeof Sensor
a.set(5.0)

dyn b = [a, 13, 0.55, "hello"]

b[0].set(99.0)

// a.get()    → 5.0   — a is unaffected
// b[0].get() → 99.0  — b[0] is an independent copy
```

`b[0]` is not a reference to `a`. It is a clone made at the moment `a` was placed into the `dyn`. From that point on, `b[0]` and `a` have no connection.

This works the same way with `typeof`:

```fluxa
Block c typeof Sensor    // independent copy of Sensor definition
Block d typeof Sensor    // another independent copy

dyn pool = [c, d]
pool[0].set(10.0)
pool[1].set(20.0)

// c.get()       → 0.0   — c unaffected
// d.get()       → 0.0   — d unaffected
// pool[0].get() → 10.0
// pool[1].get() → 20.0
```

**The isolation rule is unconditional.** There is no way to store a reference to a Block in a `dyn` — only independent copies. This is not a limitation, it is the ownership model: every value has exactly one owner, and placing a Block in a `dyn` transfers a copy to that slot.

---

## 6. Functions

Return type declared at the end. Everything passed as arguments — no implicit access to outer scope.

```fluxa
fn add(int a, int b) int {
    return a + b
}

fn greet(str name) nil {
    print("hello " + name)
}

int result = add(3, 4)
greet("fluxa")
```

**Functions do not see variables declared outside them.** Pass everything explicitly:

```fluxa
int x = 10

fn double(int n) int {
    return n + n    // n is the parameter — x is invisible here
}

int result = double(x)    // pass x explicitly
```

---

## 7. Control Flow

All control blocks require braces. No single-line shorthand.

```fluxa
// if / else
if x > 5 { print("high") } else { print("low") }

// while
int i = 0
while i < 3 {
    print(i)
    i = i + 1
}

// for x in arr / dyn
int arr nums[3] = [10, 20, 30]
for n in nums { print(n) }

dyn items = ["a", "b", "c"]
for item in items { print(item) }
```

---

## 8. Block — Encapsulated State and Behavior

Block groups state and behavior. It is not a class — there is no inheritance, no polymorphism, no implicit coupling.

```fluxa
Block Counter {
    prst int total = 0    // persists across reloads
    int   step    = 1     // valid non-prst field — lives for execution
    fn increment() nil { total = total + step }
    fn value() int     { return total }
}
```

**Create instances with `typeof`:**

```fluxa
Block c1 typeof Counter
Block c2 typeof Counter
c1.increment()    // c1.total == 1, c2.total == 0 — fully independent
```

### Hard Rules for Block

These are the most common mistakes. Read carefully.

**Rule 1 — No `dyn` as a Block field.**

`dyn` is not a valid Block field type. This is unconditional — there are no exceptions.

```fluxa
// WRONG — always an error
Block Worker {
    prst dyn conn = [0]      // ERROR
    dyn  buffer   = []       // ERROR
}

// CORRECT — typed fields only
Block Worker {
    prst int  conn_id = 0    // int handle to an external resource
    prst int  count   = 0
    prst float sum    = 0.0
    str  label = "worker"    // str is fine
}
```

If you need a `dyn` cursor (file handle, DB connection, model, etc.), keep it at the program level and pass it as an argument:

```fluxa
prst dyn sensor_cursor = csv.open("data.csv")   // lives at program level

Block Accumulator {
    prst int   count = 0
    prst float sum   = 0.0
    fn record(float v) nil {
        sum   = sum + v
        count = count + 1
    }
}

// pass cursor to function, which passes results to Block
fn read_batch(dyn cur, int n) dyn {
    danger {
        dyn chunk = csv.next(cur, n)
        return chunk
    }
    return []
}
```

For external resources like database connections that must be per-thread, use `int` handles (returned by `std.pg`, `std.wserver`):

```fluxa
Block Worker {
    prst int conn_id = 0    // CORRECT — int handle, not dyn cursor
    prst int count   = 0
    fn record(float v) nil { count = count + 1 }
}
```

**Rule 2 — No `danger` inside Block methods.**

`danger` captures errors into the `err` ring buffer. Inside a Block method, ownership of `err` is ambiguous. This is unconditional.

```fluxa
// WRONG — always an error
Block Worker {
    fn run(int conn) nil {
        danger {                    // ERROR
            int res = pg.query(conn, "SELECT 1")
        }
    }
}

// CORRECT — danger lives in a function, results passed to Block
fn fetch(int conn, str sql) str {
    danger {
        int res = pg.query(conn, sql)
        str val = ""
        if pg.rows(res) > 0 { val = pg.get(res, 0, 0) }
        pg.free_result(res)
        return val
    }
    return ""
}

Block Worker {
    prst int   count = 0
    prst float sum   = 0.0
    fn record(float v) nil {
        sum   = sum + v
        count = count + 1
    }
}
```

**Rule 3 — No Block declaration inside a Block.**

```fluxa
// WRONG
Block Outer {
    Block Inner {    // ERROR — not allowed
        int x = 0
    }
}
```

**Rule 4 — `typeof` only applies to defined Blocks, not instances.**

```fluxa
Block c1 typeof Counter    // ok
Block c2 typeof c1         // ERROR — c1 is an instance, not a definition
```

---

## 9. danger — Explicit Error Containment

`danger` isolates operations that can fail. Outside `danger`, any failure aborts execution immediately. Inside `danger`, errors are captured in `err`.

```fluxa
// outside danger — aborts on failure
float r = math.sqrt(-1.0)
// → [fluxa] Runtime error (line 1): sqrt of negative number

// inside danger — captured, execution continues
danger {
    float r = math.sqrt(-1.0)
}
if err != nil { print(err[0]) }
```

**`err` is a ring buffer of 32 entries:**

```fluxa
danger {
    float a = math.sqrt(-1.0)   // err[0]
    float b = 1.0 / 0.0         // err[0] (previous pushed to err[1])
}
print(err[0])    // most recent error
print(err[1])    // previous error
```

**`danger` is required for:**
- `import c` FFI calls
- File I/O (`csv.open`, `json.load`, `fs.read`, ...)
- Network and database operations (`pg.connect`, `pg.query`, `wserver.serve`, ...)
- Any operation that can fail for external reasons

**`danger` is not required for:**
- Pure math (`std.math`)
- String operations (`std.strings`)
- Time queries (`std.time`)
- Thread management (`std.flxthread`)

**`danger` is not permitted inside Block methods.** See section 7 Rule 2.

### Three hard-won `danger` rules

**Rule A — Never call a function that has its own `danger` from inside a `danger`.**

Nested `danger` — entering a callee's `danger {}` while already inside the caller's — corrupts the containment depth: the inner block's exit closes containment for the outer block too, and the remaining "protected" operations run bare. The symptom is an abort or silent misbehavior in code that *looks* wrapped.

```fluxa
// WRONG — helper's danger nests inside the worker's danger
fn read_row(int db) str {
    danger { ... }        // inner danger
    return ""
}

fn worker(int srv, int db) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            str row = read_row(db)     // ERROR CLASS: nested danger
        }
    }
}

// CORRECT — all fallible logic inline, one danger per iteration
fn worker(int srv, int db) nil {
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            danger {
                // query db, build reply — inline, single containment
            }
        }
    }
}
```

Helper functions with their own `danger` are fine — call them from *outside* any `danger`, as top-level orchestration.

**Rule B — `err` is one ring buffer shared by all threads. Never check `err != nil` inside a threaded function.**

Another thread's error lands in the same ring; a worker checking `err` will react to failures that are not its own. Inside any function that runs as a thread, communicate failure through **return values** (the `0` handle convention) and reserve `err != nil` checks for the top level of the program.

```fluxa
// top level — fine
danger { srv = wserver.serve(9999, false) }
if err != nil { print(err) }

// inside a worker — WRONG: err may hold another thread's error
// if err != nil { ... }
// CORRECT: if req != 0 { ... }   — decide from return values
```

**Rule C — `danger` swallows `undefined variable` errors.**

Because there is no global scope, a variable name that isn't a parameter is undefined inside a function — and inside `danger`, that error is *captured*, not printed. The block silently stops at the undefined reference. If a `danger` block "does nothing", the first suspect is a variable you forgot to pass as a parameter. Temporarily lift the code out of `danger` (or `print(err[0])` right after it) to surface the message.

---

## 10. Calling Standard Libraries

Enable in `fluxa.toml` first:

```toml
[libs]
std.math    = "1.0"
std.json    = "1.0"
std.pg      = "1.0"
std.wserver = "1.0"
```

Then import and use:

```fluxa
import std math
import std json
import std pg
import std wserver

float r = math.sqrt(16.0)   // no danger needed

// pg returns int handles — always use danger
int db = 0
danger { db = pg.connect("host=localhost dbname=mydb user=fluxa password=secret") }
if err != nil { print("connection failed") }

danger {
    int res = pg.query(db, "SELECT val FROM readings LIMIT 1")
    if pg.rows(res) > 0 {
        float v = pg.get_float(res, 0, 0)
        print(v)
    }
    pg.free_result(res)
    pg.close(db)
}
```

**Handle return convention for pg and wserver:**

- `pg.connect`, `pg.query`, `pg.query_params` return `int` > 0 on success, 0 on error
- `wserver.serve` returns `int` > 0 on success, 0 on error
- `wserver.accept` returns `int` > 0 when request received, 0 on timeout
- Always check inside `danger` — errors are pushed to `err`

---

## 11. Concurrency with std.flxthread

```fluxa
import std flxthread as ft
import std time

Block Worker {
    prst int count = 0
    fn run() nil {
        while !ft.should_stop() {
            count = count + 1
            time.sleep(10)
        }
    }
    fn get() int { return count }
}

Block w1 typeof Worker
Block w2 typeof Worker

ft.new("t1", w1, "run")
ft.new("t2", w2, "run")

time.sleep(100)
int n1 = ft.await("t1", "get")
int n2 = ft.await("t2", "get")
print(n1)
print(n2)

ft.stop("t1")
ft.stop("t2")
ft.resolve_all()
```

**Thread + IO pattern — danger lives in functions, not Block methods:**

```fluxa
import std pg
import std flxthread as ft

fn fetch(int conn) float {
    danger {
        int res = pg.query(conn, "SELECT val FROM sensor ORDER BY ts DESC LIMIT 1")
        float v = 0.0
        if pg.rows(res) > 0 { v = pg.get_float(res, 0, 0) }
        pg.free_result(res)
        return v
    }
    return 0.0
}

Block Worker {
    prst int   count = 0
    prst float last  = 0.0
    fn record(float v) nil {
        last  = v
        count = count + 1
    }
}

int conn = 0
danger { conn = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t") }

Block w1 typeof Worker
ft.new("t1", w1, "record")

int tick = 0
while tick < 100 {
    float v = fetch(conn)
    ft.message("t1", "record", v)
    tick = tick + 1
}

ft.stop("t1")
ft.resolve_all()
pg.close(conn)
```

**`ft.new` with arguments** — pass data to a global function thread:

```fluxa
fn process(int conn, int worker_id) nil {
    // each thread gets its own conn handle and id
    // danger lives here, in the function
}

ft.new("t1", "process", conn1, 1)
ft.new("t2", "process", conn2, 2)
ft.resolve_all()
```

**`ft.new` batch form (v0.21)** — spawn N identical workers in one call:

```fluxa
ft.new("w", 16, "worker", srv)   // spawns w1..w16, each running worker(srv)
```

The form is selected by the *type* of the second argument: `int` = batch count, `string` = single global-function thread, Block instance = method thread. A numeric *name* like `ft.new("w10", "worker", srv)` still works — `"w10"` is the name in the first slot. Count is bounded by `FLUXA_THREAD_MAX`; the same `max_msg_args` and arity checks apply.

Configure the limit in `fluxa.toml`:
```toml
[libs.flxthread]
max_msg_args = 2    # default 2, hard cap 8
```

### The threaded server-worker pattern

The architecture that holds up under load (validated at 900+ req/s):

1. **Open every external resource once, at the top level** — server socket, DB connections — each in its own `danger`, checked with `err != nil` (allowed here, and only here).
2. **Pass the handles to workers as arguments** (`int` handles — they work everywhere, including as values a Block method can receive).
3. **Workers loop on `accept` with a timeout**, decide from return values, and keep all fallible logic inline in one `danger` per request.

```fluxa
import std wserver
import std flxthread as ft

fn worker(int srv) nil {
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }   // 100 ms timeout
        if req != 0 {
            str method = wserver.req_method(req)
            str path   = wserver.req_path(req)
            // route, then reply — all inline, one danger per fallible section
            wserver.reply_json(req, 200, "{\"ok\":true}")
            free(method)
            free(path)
        }
    }
}

int srv = 0
danger { srv = wserver.serve(9999, false) }   // false: no built-in dispatcher
if err != nil { print(err) }

ft.new("w", 16, "worker", srv)

wserver.wait(srv)
wserver.stop(srv)
```

**Do not** use the dispatcher form (`serve(port, true)` handing requests to a named function) when workers also need their own connections — opening resources like `pg.connect` inside worker functions runs into the nested-`danger` rule (section 9, Rule A). Pre-connect at the top level; pass handles in.

---

## 12. prst with External Resources

There are two kinds of "resource" values, and they take opposite decisions on `prst`:

**In-process `dyn` cursors** (CSV cursor, SQLite DB, JSON document, PID controller, loaded model): these wrap state the runtime itself can carry across a hot reload. Use `prst dyn` at the program level.

```fluxa
prst dyn db      = sqlite.open("data.db")   // SQLite connection cursor
prst dyn cursor  = csv.open("log.csv")      // CSV file cursor
prst dyn model   = infer.load("model.gguf") // LLM model cursor
```

**OS-level `int` handles** (PostgreSQL connections, wserver sockets): use **plain `int`, not `prst`**, and (re)open them at startup.

```fluxa
int pg_conn = 0
int srv     = 0

danger { pg_conn = pg.connect("host=localhost dbname=mydb user=fluxa password=s3cr3t") }
danger { srv     = wserver.serve(8080, false) }
```

Why: a `prst int srv` makes the persistence layer restore the old handle value on restart and the runtime then tries to resume a socket the OS already reclaimed — the visible symptom is `Address already in use` (or a dead DB handle) after a restart. Sockets and server-side connections cannot survive the process; the correct durable thing is the *startup code that recreates them*, not the handle number.

**Never use `prst dyn` or `prst int` as Block fields for external resources.** Resource handles belong at the program level and are passed to functions and threads as arguments (section 11).

---

## 12.5 Memory in Long-Running Loops

Long-running workers — HTTP request handlers, IoT sensor loops, polling agents — never exit their `while` block. Without care, every per-iteration heap allocation lives until the process dies. Fluxa v0.19 makes the common cases automatic and gives you `free(x)` for the few that aren't.

### The mental model

Two value types carry heap data: `str` and `dyn`. Everything else (`int`, `float`, `bool`) is by-value.

For `dyn`, the GC invariant is: **every variable slot holds exactly one strong reference (pin) to its `dyn`**. Lib-returned dyns start unpinned (collectible orphans); storing one in a variable pins it, overwriting the variable unpins the old value. The GC is minimalist — it sweeps *unpinned* dyns at safe points only (loop back-edges, end of `danger`), and manual `free()` / lib release functions remain the preferred path for large or long-lived data. As of v0.21 this invariant holds identically in the tree evaluator and the bytecode VM, so the canonical chunked-cursor loop is safe as written:

```fluxa
dyn chunk = csv.next(cur, 1000)
while len(chunk) > 0 {          // reading chunk here is safe — the slot pins it
    // ... process ...
    chunk = csv.next(cur, 1000) // old chunk unpinned → swept; new one pinned
}
```

(Computing `int n = len(chunk)` once per iteration is still a fine micro-idiom — it just isn't required for correctness.)

**The runtime auto-releases heap data in every common pattern:**

```fluxa
fn worker(int srv) nil {
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            str method = wserver.req_method(req)  // slot reassign → previous freed
            str path   = wserver.req_path(req)    // slot reassign → previous freed

            if method == "POST" {
                if path == "/healthz" {           // "/healthz" literal: freed after compare
                    wserver.reply(req, 200, "ok")
                }
            }
            danger {
                dyn doc = json2.parse("{}")       // new dyn pinned; previous unpinned/swept
                json2.discard(doc)                // releases the parse tree
            }                                     // gc_sweep at danger-end collects orphans
        }
    }
}
```

Every reassignment of `method`, `path`, `doc` releases the prior value. Every literal passed to a lib call (`"/healthz"`, `"{}"`) is freed after the call returns. Every comparison with a literal (`path == "/healthz"`) releases the literal's temporary buffer. `json2.discard` releases the parse tree; the wrapper is collected automatically. **You never have to think about these.**

### When `free(x)` is required

Exactly one pattern requires explicit `free()`: **building intermediate values that you don't reassign**.

```fluxa
str p1 = strings.concat("{\"id\":\"", id)
str p2 = strings.concat(p1, "\",\"name\":\"")
str p3 = strings.concat(p2, name)
str p4 = strings.concat(p3, "\"}")
wserver.reply_json(req, 200, p4)
free(p1); free(p2); free(p3); free(p4)
```

Each `p1..p4` is a fresh `str` declaration — no reassignment releases its predecessor. Without `free()`, all four heap buffers stay alive until the worker returns (which it never does). The same pattern shows up when assembling SQL parameter arrays, JSON arrays, or anything you compose piece by piece.

### When `free(x)` does nothing

- After a reassignment: `str x = a; x = b; free(x)` — runtime already released the old `a` on reassignment; `free(x)` releases `b`.
- After scope ends: irrelevant — the runtime already released everything in the scope.
- On a `dyn` cursor: use the lib's release function (`json2.discard`, `csv.close`, `pg.free_result`, `sqlite.close`).
- On `prst` variables: `free()` rejects — the persistence layer owns them across reloads.
- On Block instance fields: `free()` rejects — the instance scope owns them.
- On arena strings (from `std.cache`): `free()` rejects — arenas are bulk-released via `arena_reset` / `arena_drop`.

### Where the runtime helps you

The runtime's automatic releases cover:

| Action | Released |
|---|---|
| `str x = ...; x = ...` | Old slot contents |
| `dyn d = ...; d = ...` | Old `dyn` (unpinned + swept) |
| `str arr p[N] = [...]; p = [...]` | Old array storage + element strings |
| `lib.fn("literal", x)` | The literal's transient copy |
| `if a == "literal" { }` | The literal's transient copy |
| `lib.fn(x, y)` as a statement (return discarded) | Owned return value |
| `len(s)`, `print(s)` | Transient string copies the read produced |
| End of `danger { }` | Unpinned dyns via `gc_sweep` |
| Function return | Everything in the function frame |

### Profile expectations

A well-written HTTP worker hitting 1k+ req/s should plateau at **20–40 MB resident** within seconds and stay there indefinitely. If your memory grows linearly with request count, check:

1. Did you `free()` the per-request intermediates (the `p1..pN` chain)?
2. Did you `discard` JSON documents and `free_result` query results?
3. Are you accumulating into a `prst dyn` collection by mistake? `prst` data persists across reloads — never use it as a transient buffer.

If the answers are all yes and memory still grows, `fluxa dis main.flx -proj .` will show the runtime forecast and identify any hot path that bypasses bytecode.

---

## 13. Modules

Organize code across files using `import live` and `import static`.

```fluxa
// main.flx
import live sensor        // loads live/sensor.flx
import static utils       // loads static/utils.flx

Block s typeof sensor.Sensor
s.set(3.14)
print(utils.double(7))
```

```fluxa
// live/sensor.flx — has prst state, designed for hot reload
Block Sensor {
    prst float reading = 0.0
    fn set(float v) nil { reading = v }
    fn get() float       { return reading }
}
```

```fluxa
// static/utils.flx — pure functions, no state
fn double(int n) int { return n + n }
```

**Rules:**
- Only `main.flx` can import modules — modules cannot import each other
- `live/` convention: modules with `prst` state
- `static/` convention: pure function modules

---

## 14. Common Mistakes Reference

A quick checklist of the most frequent errors:

| What you wrote | Why it's wrong | What to write instead |
|---|---|---|
| `Block W { prst dyn conn = [0] }` | `dyn` is not a valid Block field | `Block W { prst int conn_id = 0 }` |
| `Block W { fn run() nil { danger { ... } } }` | `danger` not allowed in Block methods | Move dangerous code to a function, pass result to Block |
| `Block W { Block Inner { ... } }` | No nested Block declarations | Separate Block definitions at the top level |
| `Block c2 typeof c1` where c1 is an instance | `typeof` only applies to Block definitions | `Block c2 typeof Counter` |

| `int res = pg.query(conn, sql)` outside `danger` | pg operations can fail | Wrap in `danger {}` |
| `if res != nil { ... }` for int handles | pg/wserver return `int`, not `dyn` | `if res != 0 { ... }` |
| Passing `dyn` cursor as Block field | `dyn` not valid in Block | Keep cursor at program level, pass as method arg |
| Calling a helper that contains `danger` from inside a `danger` | Nested containment closes early — remaining "protected" code runs bare | Inline the fallible logic; call helpers with `danger` only from outside any `danger` (§9 Rule A) |
| `if err != nil { ... }` inside a worker/thread function | `err` is one ring shared by all threads — you react to other threads' errors | Decide from return values (`req != 0`); check `err` only at top level (§9 Rule B) |
| A `danger` block that "does nothing" | It captured an `undefined variable` error — a name you forgot to pass as a parameter | Pass the variable explicitly; `print(err[0])` after the block to see the message (§9 Rule C) |
| `prst int srv` / `prst int conn` for sockets or DB connections | Persistence restores a dead OS handle on restart — `Address already in use` | Plain `int`, reopen at startup; `prst` is for in-process `dyn` cursors (§12) |
| `serve(port, true)` dispatcher + opening connections inside workers | Worker-side `pg.connect` hits the nested-`danger` rule | `serve(port, false)` + `ft.new("w", N, "worker", srv, db)` with pre-connected handles (§11) |
| Sixteen `ft.new("w1", ...)` … `ft.new("w16", ...)` lines | Verbose and error-prone | Batch form: `ft.new("w", 16, "worker", srv)` (§11) |
| Forgot `free(p1); free(p2); ...` on a `strings.concat` chain in a worker | Each intermediate accumulates over millions of iterations | Free each piece after `reply` — see §12.5 |
| `free(some_prst_var)` | `prst` belongs to the persistence layer, not the slot | Mutate the value; the pool tracks it |
| `free(s)` where `s` came from `cache.arena_str` | Arena strings are bulk-released | Use `cache.arena_reset(arena)` |
| `dyn doc = json2.parse(...)` then no `json2.discard(doc)` | The parse tree leaks even though the wrapper is GC'd | Always `json2.discard(doc)` inside the `danger` block |

