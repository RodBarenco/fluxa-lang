# How to Program in Fluxa

A step-by-step reference for writing correct Fluxa programs. Read this before writing any Fluxa code — especially the **Hard Rules** section. The mistakes listed there are the most common errors made by anyone (human or AI) new to the language.

---

## 1. The Mental Model

Fluxa has three organizing ideas that everything else flows from:

**Unique ownership.** Every value has exactly one owner. There is no shared mutable state between functions. Data flows through arguments and return values — nothing is shared implicitly.

**No global scope.** Variables declared at the top level of a program belong to that program's execution context — they are not accessible from inside functions or methods unless passed explicitly. Functions see only their parameters.

**State that survives is explicitly marked.** Only `prst` variables survive hot reloads. Everything else dies and is reborn on each reload. This is a feature — it makes the boundary between ephemeral and persistent state visible in code.

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

Configure the limit in `fluxa.toml`:
```toml
[libs.flxthread]
max_msg_args = 2    # default 2, hard cap 8
```

---

## 12. prst with External Resources

External resources (connections, cursors, file handles) should be `prst` so they survive hot reloads in dev mode.

**`dyn` cursors** (in-process state — CSV cursor, SQLite DB, JSON document, PID controller): use `prst dyn` at the program level.

```fluxa
prst dyn db      = sqlite.open("data.db")   // SQLite connection cursor
prst dyn cursor  = csv.open("log.csv")      // CSV file cursor
prst dyn model   = infer.load("model.gguf") // LLM model cursor
```

**`int` handles** (external resources — PostgreSQL, wserver): use `prst int` at the program level.

```fluxa
prst int pg_conn = 0
prst int srv     = 0

danger { pg_conn = pg.connect("host=localhost dbname=mydb user=fluxa password=s3cr3t") }
danger { srv     = wserver.serve(8080) }
```

**Never use `prst dyn` or `prst int` as Block fields for external resources.** External resource handles belong at the program level.

---

## 12.5 Memory in Long-Running Loops

Fluxa's GC sweeps `dyn` objects at every `while` back-edge when their pin count is zero. **String values (`VAL_STRING`) are not GC-managed**: a `str` is freed when the scope that owns it is released, which only happens when the surrounding function returns.

This matters for HTTP servers and other long-running workers. Inside

```fluxa
fn worker(int srv) nil {
    while !ft.should_stop() {
        // ... per-request work that allocates strings ...
    }
}
```

every `str` assigned inside the loop lives in the worker's scope until the function returns — which it never does. Each iteration's strings accumulate.

**Mitigations within the current runtime:**

1. **Use `json2.discard(doc)`** at the end of every `danger` block that calls `json2.parse(...)`. The `dyn` wrapper is GC-managed but the underlying parse tree is opaque and must be released explicitly.
2. **Build response bodies with multi-step `strings.concat`** chains. Each step allocates one string; the previous step's intermediate falls out of usage but the trailing `wserver.reply_json(req, status, body)` only needs the final string.
3. **Cache aggressively** when possible. A `Block` field `prst str arr` honors deep-copy semantics on write and proper free-on-overwrite via the prst pool.
4. **Accept a residual leak.** glibc's malloc consolidates arenas under memory pressure, producing a bounded steady state under sustained load. In practice this is ~50 MB residual per 100 RPS over 10 minutes of benchmarking.

A documented gap exists in the runtime: there is no `free(x)` built-in that releases an arbitrary `str` slot, and no automatic free on `rt_set` overwrite. Adding either would let workers reach near-zero residual memory. This is tracked as a runtime enhancement; see the issue tracker.

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

