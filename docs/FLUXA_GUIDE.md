# How to Program in Fluxa

A step-by-step reference for writing correct Fluxa programs. Read this before writing
any Fluxa code — especially the Block section (§8), the `danger` rules (§9), and the
memory model (§12.5). The mistakes collected in §14 are the most common errors made
by anyone (human or AI) new to the language.

---

## 1. The Mental Model

Fluxa's rules all flow from a small set of invariants. Internalize these seven before writing code:

1. **Unique ownership.** Every value has exactly one owner. No value is accessed outside its scope. There is no shared mutable state between functions — data flows through arguments and return values only.
2. **No global scope.** Variables declared at the top level belong to the program's execution context — they are not visible inside functions or Block methods. There is no free-variable capture, ever.
3. **Everything is passed.** Every value a function or method uses arrives as an argument or parameter. If a worker needs a server handle and a DB connection, they are parameters.
4. **No `dyn` as a Block field.** `dyn` lives at the program level and inside functions and methods — but never as a *field declaration* in a Block body. Inside a Block **method**, `dyn` works normally (see section 7).
5. **`danger` is not a Block field declaration.** You cannot write a loose `danger` statement in a Block body (only typed fields and `fn` methods are allowed there). But `danger` **works inside a Block method** — that is the idiomatic place for a Block's fallible IO (see section 7).
6. **`prst` is state that survives time.** A `prst` variable is simply marked to survive — hot reloads, Atomic Handover, runtime swap. It is how you make state explicit and durable; everything else dies and is reborn on each reload.
7. **`err` is a ring buffer; `danger` is intentional containment.** Division by zero, overflow, failed IO — anything that would otherwise abort the runtime is captured by `danger {}` into the `err` ring. `danger` is not a safety net you sprinkle around: it is you declaring *"this may fail and I will handle it"*, and the handling is the `if err != nil` that closes the block. That is why all IO goes inside `danger` — and why every `danger` is followed by a decision.

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
    print("hello ", name)
}

int result = add(3, 4)
greet("fluxa")
```

`print` takes multiple arguments of any type, separated by commas, and writes them
in order (`+` is arithmetic — it does not join strings):

```fluxa
int   n = 42
float f = 3.14
print("count: ", n, " ratio: ", f)   // → count:  42  ratio:  3.14
```

To build a single `str` value instead of printing pieces, use `strings.concat`.

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

A Block groups related state and the methods that operate on it. It is not a class:
there is no inheritance, no polymorphism, no implicit coupling. Think of it as a
named record with functions attached.

```fluxa
Block Counter {
    prst int total = 0    // a field — persists across reloads
    int   step    = 1     // a field — lives for this execution
    fn increment() nil { total = total + step }
    fn value() int     { return total }
}
```

A Block has two kinds of members: **fields** (the state) and **methods** (the
behavior). Almost everything worth knowing about Block comes down to how these two
differ — fields are restricted to concrete types, while methods are ordinary Fluxa
code. The sections below cover instances, fields, methods, and the pattern that ties
them together.

### Block instances

A Block declaration is a template. You create working instances from it with
`typeof`:

```fluxa
Block c1 typeof Counter
Block c2 typeof Counter
c1.increment()    // c1.total == 1, c2.total == 0 — fully independent
```

Each instance is a complete, isolated copy — its own fields, unconnected to any
other instance. Mutating `c1` never touches `c2`. Two rules govern instantiation:

- **`typeof` applies to a defined Block, never to an instance.**

  ```fluxa
  Block c1 typeof Counter    // ok — Counter is a definition
  Block c2 typeof c1         // ERROR — c1 is an instance
  ```

- **No nested Block.** A Block cannot declare another Block inside it. Define Blocks
  at the top level and compose them through methods and arguments.

  ```fluxa
  Block Outer {
      Block Inner { int x = 0 }    // ERROR — no nesting
  }
  ```

### Block fields

A field must have a concrete declared type: `int`, `float`, `str`, `bool`, `char`,
or `arr`. That is the whole rule. `dyn` is not a valid field type, and statements
(like a loose `danger`) cannot appear in the field area — a Block body holds field
declarations and `fn` methods, nothing else.

```fluxa
// WRONG — these are not valid fields
Block Worker {
    dyn  buffer   = []       // ERROR — dyn is not a field type
    prst dyn conn = [0]      // ERROR — same reason
    danger { int y = 5 }     // ERROR — statements don't belong in the field area
}

// RIGHT — concrete typed fields
Block Worker {
    prst int   conn_id = 0     // an int handle to an external resource
    prst int   count   = 0
    prst float sum     = 0.0
    str        label   = "worker"
}
```

The reason is straightforward: fields are the Block's persistent shape, and the
runtime needs a known concrete type for each one — to serialize `prst` fields across
reloads, and to give every instance an independent copy. A `dyn` is an opaque,
GC-tracked handle with no fixed shape to store, so it lives elsewhere: at the program
level, or as a local inside a method (next).

Fields may be `prst` or not: `prst int total = 0` survives hot reloads; `int step =
1` lives for the current execution and resets on reload. Both are valid.

### Block methods

A method body is ordinary Fluxa. Anything you can write in a function, you can write
in a method — local variables, `dyn` cursors, `danger` blocks, `if err != nil`
checks. In particular, **`dyn` and `danger` work inside methods**, and a method is
the right home for a Block that owns fallible IO: it opens a resource inside
`danger`, works with it, writes results into the Block's typed fields, and closes by
checking `err`.

```fluxa
Block Store {
    prst int count = 0

    fn load() nil {
        danger {
            dyn db   = sqlite.open("data.db")            // dyn: fine inside a method
            dyn rows = sqlite.query(db, "SELECT v FROM readings")
            count = len(rows)                            // write to a typed field
            sqlite.close(db)
        }
        if err != nil { print(err[0]) }                  // handle it, still in the method
    }

    fn get() int { return count }
}

Block s typeof Store
s.load()
```

The `dyn` cursors here (`db`, `rows`) are method locals, not fields — exactly where
they belong. The `err` check sits right after the `danger`, still inside the method,
and decides what to do.

### The idiomatic pattern

The division of labor is the whole idea: **fields hold concrete state; methods do the
work** that reads and updates it, including any `dyn`/IO logic. State is encapsulated;
you mutate and read it through methods. A persistence Block opens its cursor inside
`load()`, keeps a plain `int count` as its visible state, and exposes `get()` — the
caller never sees the `dyn`.

One performance corollary follows directly. When a method does repeated work over its
own state, **keep the loop inside the method** rather than writing an outer function
that calls a one-step method thousands of times. A loop that only touches locals
inside a method runs on the fast bytecode path; calling a method per iteration pays
the call boundary every turn — and, if the instance is passed as an argument, an
instance copy every turn.

```fluxa
// IDIOMATIC — one call; the loop runs inside on the fast path
Block Simulation {
    prst int total = 0
    fn run(int n) nil {
        int i = 0
        while i < n {
            total = total + i
            i = i + 1
        }
    }
    fn result() int { return total }
}

Block sim typeof Simulation
sim.run(1000)

// AVOID — thousands of method dispatches for the same work
fn run_outside(int n) nil {
    int i = 0
    while i < n { sim.bump()  i = i + 1 }
}
```

Give the Block a method that does the whole batch (`run(n)`, `add_batch(n)`) and call
it once. Don't expose a single-step mutator to be hammered from outside.

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

**`err` is a ring buffer of 32 entries.** Verified behavior: `err` is cleared before
each `danger`, and execution **stops at the first error** inside the block — the code
after the failing line does not run, so you normally see one error per block:

```fluxa
danger {
    float a = math.sqrt(-1.0)   // ERROR — the block stops here
    float b = 1.0 / 0.0         // does NOT execute
}
print(err[0])    // "math.sqrt (line N): sqrt of negative number"
print(err[1])    // nil — only one error in this block
```

Higher indices (`err[1]`, `err[2]`, …) hold earlier errors when several accumulate
from different contexts before you check; when the ring fills, the oldest entry is
pushed out first. Each `danger` closes with a decision — the idiom is
`if err != nil { ... }`.

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

**`danger` works inside Block methods** — it is the idiomatic place for a Block's fallible IO. What is not allowed is a loose `danger` as a Block field. See section 7 Rule 2.

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

**Thread + IO pattern — in a thread, keep `err` handling in the worker, decide by return value:**

The reason here is the shared error ring (§9 Rule B), not a Block restriction:
`err` is one ring across all threads, so a thread function reads its result from the
return value and never checks `err`. (`danger` itself is fine inside Block methods —
see §8; the constraint below is specifically about threads sharing `err`.)

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

For `str` (v0.23+), the buffer is **refcounted and immutable**: reading a string —
from a variable, an array element, a `dyn` element, or a Block field — shares the
buffer and adds a reference (O(1), no copy); `free`, reassignment, and function
return drop a reference; the heap is released at zero. Writing a string always
produces a new buffer. You rarely think about this: the effect is simply that reads
are cheap and any `free`/reassignment only ever affects your own name.

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

### Reading array and `dyn` elements (v0.23+)

`str`, `arr`, and `dyn` are **pointers** — the variable slot holds a pointer, the
data lives on the heap. Copying a large string or array on every read would be
ruinous, so Fluxa never copies the bytes. Strings are **immutable** (any change
produces a new buffer), so as of v0.23 the runtime shares the buffer safely on
read and counts how many names point at it: reading adds a name, `free` /
reassignment / function return removes one, and the heap is released only when the
last name lets go. A read is therefore **O(1)** — the pointer cost — regardless of
string size.

The practical result: reading an element gives you your own reference, and you can
`free` it or reassign it without touching the element.

```fluxa
str arr names[3] = ""
names[0] = "Alpha"
str x = names[0]     // x shares names[0]'s buffer, O(1), with its own reference
free(x)              // removes x's name; names[0] is intact
print(names[0])      // → "Alpha"
```

```fluxa
str y = names[0]
y = names[1]         // reassigning releases y's old reference; names[0] untouched
print(names[0])      // → "Alpha"
```

All the reading patterns are now equally safe and equally cheap — `for-in`, passing
the element directly as an argument (`f(arr[i])`), or binding to a variable. Pick
whichever reads best; the old `strings.concat(arr[i], "")` copy trick is no longer
needed. This applies identically to `dyn` elements from `csv`/`sqlite` rows.

One thing worth knowing for hot paths: reading a large string still hands the caller
a reference in O(1), but *building* a new string (any `strings.*` call) allocates a
fresh buffer in O(n). When a loop only needs to pass a value along, pass the element
directly rather than rebuilding it.

### When `free(x)` does nothing

- After a reassignment: `str x = a; x = b; free(x)` — runtime already released the old `a` on reassignment; `free(x)` releases `b`.
- After scope ends: irrelevant — the runtime already released everything in the scope.
- On a `dyn` cursor: use the lib's release function (`json2.discard`, `csv.close`, `pg.free_result`, `sqlite.close`).
- On `prst` variables: `free()` rejects — the persistence layer owns them across reloads.
- On a Block instance field (v0.23+): `free(field)` releases the field's reference and sets it to `nil`; the next assignment revives it. (Earlier versions rejected this.)
- On arena strings (from `std.cache`): `free()` is harmless — as of v0.23 `arena_str`/`arena_concat` return an ordinary refcounted copy, so freeing it just drops that copy; the arena itself is still bulk-released via `arena_reset` / `arena_drop`.

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
| `len(s)`, `print(s)` | Any temporary reference the read took (v0.23: O(1), no copy) |
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
- **v0.22 — Block singletons across modules:** a Block declared in a module
  is addressable as `mod.Block.method(args)` and `mod.Block.field` (read and
  write) from `main.flx`, from other modules' code, and from inside Block
  methods anywhere. Inside its own module, plain `Block.method(...)`
  references resolve too (the parser mangles the owner). State lives in the
  definition — no `typeof` needed for the singleton pattern:

  ```fluxa
  // static/vault.flx
  Block Vault {
      int best = 0
      fn bump(int v) nil { if v > best { best = v } }
      fn get_best() int { return best }
  }
  fn bump_twice(int v) nil {
      Vault.bump(v)          // same-module reference: works
      Vault.bump(v + 1)
  }

  // main.flx
  import static vault
  vault.Vault.bump(55)
  print(vault.Vault.get_best())   // 55
  ```
- `live/` convention: modules with `prst` state
- `static/` convention: pure function modules

### When a large program hits "aborting due to resolver errors"

As a program grows across many modules, the resolver's scope pool can fill up.
The resolver allocates one lexical scope per **top-level function, per Block, and
per Block method** — and *only* those. `if`/`while` bodies do **not** consume a
scope (verified: 256 sequential `while`s resolve fine under the default). The pool
is sized by `scope_cap` (default 256). When a program needs more scopes than that,
name resolution aborts with **"aborting due to resolver errors"** — cleanly, with
no crash and no partial state, but also with no symbol name, because the failure
is capacity, not a bad reference. The tell is that the error appears only after
adding *more* code (another module, more methods), and each individual module
resolves fine on its own. Note that `fluxa dis` only parses and will **not**
surface this — validate a large program with `fluxa run`, which runs the resolver.

The count that matters is therefore:

```
scopes = (top-level functions) + (Blocks) + (Block methods)
```

The fix is not to shrink the program but to raise the pool in `fluxa.toml`:

```toml
[runtime]
scope_cap = 1024   # room for the resolver's scopes; raise as you grow
```

`scope_cap` has a floor of 256 (it can never be weaker than the default) and a
nominal maximum of 65536. **On memory:** the pool is allocated once per resolver
run and freed as soon as resolution finishes — before your program executes — so
its cost is transient (milliseconds), not part of your program's steady-state
footprint. The allocation is also lazy: only the scopes actually used touch
physical pages, so a generous `scope_cap` you don't fill costs nothing in RSS
(measured: a 1000-function program runs at the same ~10 MB resident whether
`scope_cap` is 256 or 8192). The caveat is the *virtual* reservation: each scope
slot is ~130 KB, so `scope_cap = 1024` reserves ~130 MB of address space and
`scope_cap = 65536` reserves ~8 GB. On a normal (overcommit) Linux host the
unused reservation is harmless, but on a target without overcommit — or a
memory-constrained embedded build — an oversized `scope_cap` can make the pool
allocation fail (the resolver then aborts gracefully). Set `scope_cap` a
comfortable margin above your real scope count, not to the maximum.

A quick checklist of the most frequent errors, grouped by topic. Each row links to
the section that explains it in full.

**Block — fields vs methods (§8)**

| What you wrote | Why it's wrong | What to write instead |
|---|---|---|
| `Block W { prst dyn conn = [0] }` | `dyn` is not a valid field type — fields are concrete types only | `prst int conn_id = 0` as the field, or open the `dyn` cursor inside a method |
| `Block W { danger { ... } }` at field level | A Block body holds only field declarations and `fn` methods | Put the `danger` **inside a method** — the idiomatic home for a Block's fallible IO |
| `Block Outer { Block Inner { ... } }` | No nested Block declarations | Define Blocks at the top level; compose via methods and arguments |
| `Block c2 typeof c1` where c1 is an instance | `typeof` applies to a Block definition, not an instance | `Block c2 typeof Counter` |

**Memory (§12.5)**

| What you wrote | Why it's wrong | What to write instead |
|---|---|---|
| Dropped `free(p1); free(p2); …` on a `strings.concat` chain in a worker | Each intermediate accumulates over millions of iterations | Free each piece after the reply |
| `dyn doc = json2.parse(...)` with no `json2.discard(doc)` | The parse tree leaks even though the wrapper is GC'd | `json2.discard(doc)` inside the same `danger` block |
| `free(some_prst_var)` | `prst` belongs to the persistence layer, not the slot | Assign a replacement value; the pool tracks it |
| Treating a `prst dyn` collection as a transient buffer | `prst` persists across reloads — it never shrinks on its own | Use a non-`prst` `dyn` for per-iteration scratch |

**danger and error handling (§9)**

| What you wrote | Why it's wrong | What to write instead |
|---|---|---|
| Calling a helper that contains `danger` from inside a `danger` | Nested containment closes early — the remaining "protected" code runs bare | Inline the fallible logic; call helpers that use `danger` only from outside any `danger` (Rule A) |
| `if err != nil { ... }` inside a worker/thread function | `err` is one ring shared by all threads — you react to other threads' errors | Decide from return values (`req != 0`); check `err` only at the top level (Rule B) |
| A `danger` block that "does nothing" | It captured an `undefined variable` error — a name you forgot to pass as a parameter | Pass the variable explicitly; `print(err[0])` after the block to see the message (Rule C) |
| A `danger` with no `if err != nil` after it | The failure was contained but never checked — the program runs on inconsistent state | Close every `danger` with a decision on `err` |

**Libraries, handles, and concurrency (§10–§12)**

| What you wrote | Why it's wrong | What to write instead |
|---|---|---|
| `int res = pg.query(conn, sql)` outside `danger` | pg operations can fail | Wrap the call in `danger {}` |
| `if res != nil { ... }` for an int handle | pg/wserver return `int`, not `dyn` | `if res != 0 { ... }` |
| `prst int srv` / `prst int conn` for a socket or DB connection | Persistence restores a dead OS handle on restart — `Address already in use` | Plain `int`, reopened at startup; `prst` is for in-process `dyn` cursors |
| `serve(port, true)` dispatcher + opening connections inside workers | Worker-side `pg.connect` hits the nested-`danger` rule | `serve(port, false)` + `ft.new("w", N, "worker", srv, db)` with pre-connected handles |
| Sixteen separate `ft.new("w1", ...)` … `ft.new("w16", ...)` lines | Verbose and error-prone | Batch form: `ft.new("w", 16, "worker", srv)` |

