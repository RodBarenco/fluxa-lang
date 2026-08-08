# Fluxa Standard Library
**v0.28**

Reference documentation for all stdlib libs: `std.math`, `std.csv`, `std.json`, `std.json2`, `std.strings`, `std.cache`, `std.time`, `std.flxthread`, `std.cabi`, `std.crypto`, `std.pid`, `std.sqlite`, `std.serial`, `std.i2c`, `std.httpc`, `std.https`, `std.mqtt`, `std.mcpc`, `std.mcps`, `std.websocket`, `std.http`, `std.mcp`, `std.graph`, `std.image`, `std.infer`, `std.zlib`, `std.fs`, `std.libv`, `std.libdsp`, `std.wserver`, `std.pg`, `std.sound`.

---

## Design Principles

All stdlib libs share the same design contract:

**Opt-in by declaration.** A lib only exists at runtime if declared in `[libs]` of `fluxa.toml`. Without declaration, `import std <lib>` produces a clear error. No lib adds overhead to programs that don't use it.

**No `danger` required for pure computation.** Stdlib functions written in safe C and vetted for embedded use work outside `danger`. File I/O, network, and database calls require `danger {}` because they can fail for external reasons.

**Errors follow the standard model.** Outside `danger`: runtime error with line number, execution aborts. Inside `danger`: error captured in `err_stack`, execution continues.

**Buffers are bounded.** Every lib that touches external data has configurable buffer limits in `fluxa.toml`. No silent truncation — exceeding a limit produces a clear error.

**External resources use opaque int handles.** Libs that manage external resources (connections, servers, files) return `int` handles — positive integers that index into a fixed table inside the lib's C layer. Zero is always invalid. Handles are not pointers and carry no type information visible to Fluxa code.

**`dyn` cursors for in-process state.** Libs that manage state entirely within the Fluxa process (file cursors, DB result sets from SQLite, JSON documents, PID controllers) use `dyn` cursors — opaque `VAL_PTR` wrappers. Use `prst dyn cursor` to keep these alive across hot reloads. `dyn` cursors are valid in the main program scope and inside function/method bodies. They are **never** valid as a Block *field declaration* (see the next note).

**`dyn` as a Block field vs inside a method.** Block fields must have a declared
type (`int`, `float`, `str`, `bool`, `arr`) — `dyn` is not a valid Block *field*
type. But `dyn` (and `danger`) work normally **inside a Block method**: a method can
open a `dyn` cursor, use it inside `danger`, and close it, updating typed fields. So
a persistence Block opens its SQLite/CSV cursor inside `load()`, not as a field.

---

## Memory Ownership Model

Most Fluxa programs do not need to think about memory: scoped declarations clean themselves up when their containing block ends, and the runtime handles owned heap data on every assignment. The model below matters only for **long-running hot loops** — typically HTTP workers, IoT sample loops, or any function that runs millions of iterations without returning.

### The two heap-allocating types

Only two value types carry heap data:

- **`str`** — every string value owns its own `char*` buffer.
- **`dyn`** — wraps an external resource or in-process collection. Tracked by the runtime GC.

All other types (`int`, `float`, `bool`) are by-value and free.

### Automatic releases — what you do not need to free

The runtime releases heap data automatically in all of these cases:

| Pattern | What's freed |
|---|---|
| `str x = "lit"; x = call()` (slot reassignment) | The previous `char*` in slot `x` |
| `dyn d = lib.parse(); d = lib.parse()` | The previous `dyn` is unpinned; sweeper collects it |
| `str arr p[N] = [...]; p = [...]` | The previous array storage and its element strings |
| `lib.fn("literal", x)` | The strdup of `"literal"` after `lib.fn` returns |
| `if s == "literal" { }` | The strdup of `"literal"` after the comparison |
| `lib.fn(doc, "k")` called as a statement | The owned string the lib returned |
| `len(s)`, `print(s)` on a string variable | The transient copy the read produced |
| End of `danger { }` block | All unpinned `dyn` objects via `gc_sweep` |
| Function return | All heap data in the function's scope and stack |

These releases were added in v0.19. Earlier versions leaked in each of these patterns. You can write idiomatic Fluxa without `free()` in most code.

### When to use `free()` explicitly

`free(x)` is **only required** when a long-running loop allocates heap data that is not covered by an automatic release. In practice this is exactly one pattern:

```fluxa
fn worker() nil {
    while !ft.should_stop() {
        str j1 = strings.concat("{\"id\":\"", id)
        str j2 = strings.concat(j1, "\",\"name\":\"")
        str j3 = strings.concat(j2, name)
        wserver.reply_json(req, 200, j3)
        free(j1); free(j2); free(j3)
    }
}
```

Each intermediate `j1`, `j2`, `j3` is declared with a fresh name on every iteration. The slot already exists from the first iteration onward, so reassignment would auto-release. But these are **new declarations**, not reassignments — the previous iteration's strings live on the heap and were already accumulated in earlier slots that no `=` overwrote. `free()` releases them deterministically.

**Rule of thumb:** if a heap value is built once and immediately consumed, the runtime handles it. If you build a chain of intermediate strings and the loop will run for hours, `free()` each one when you're done with it.

`free(x)` after the variable goes out of scope is harmless (no-op). `free(x)` twice on the same value is harmless (slot is nil'd after the first call).

### What `free()` cannot be used on

`free()` rejects:
- `prst` variables (state preservation across reloads owns the heap)
- Anything that's not a stack-resolved local

Attempting these produces a clear runtime error.

As of v0.23, `free(field)` on a Block instance field is **allowed**: it releases
the field's reference and sets it to `nil` (the next assignment revives it), matching
the behavior of a plain local. Earlier versions rejected it.

### Cursors and external resources

`dyn` cursors returned by libs (`csv.open`, `sqlite.open`, `pg.connect`, `json2.parse`) carry an external resource. Use the matching `close` / `discard` / `free_result` to release the resource, then let the runtime collect the wrapper. **Do not** call `free()` on a `dyn` cursor — use the lib's release function.

```fluxa
danger {
    dyn doc = json2.parse(body)
    // ...use doc...
    json2.discard(doc)        // releases the parse tree
    // The FluxaDyn wrapper is collected automatically when doc's slot is
    // reassigned or when the danger block ends.
}
```

---

## Enabling a Library

```toml
# fluxa.toml
[libs]
std.math = "1.0"
std.csv  = "1.0"
std.json = "1.0"
```

```fluxa
import std math
import std csv
import std json
```

---

## std.math

Pure math functions. No state, no `danger` required, no file I/O. Wraps `<math.h>` with Fluxa error semantics.

**Enable:**
```toml
[libs]
std.math = "1.0"
```

### Constants

```fluxa
float pi  = math.pi()   // 3.14159265358979323846
float e   = math.e()    // 2.71828182845904523536
float inf = math.inf()  // INFINITY
float nan = math.nan()  // NaN
```

### Roots and Powers

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.sqrt(x)` | float or int | float | Domain error if x < 0 |
| `math.cbrt(x)` | float or int | float | Cube root, works for negative |
| `math.pow(x, y)` | float or int | float | Domain error if x < 0 and y is non-integer |
| `math.hypot(x, y)` | float or int | float | √(x² + y²), no overflow |

### Logarithms and Exponentials

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.log(x)` | float or int | float | Natural log. Domain error if x ≤ 0 |
| `math.log2(x)` | float or int | float | Base-2 log. Domain error if x ≤ 0 |
| `math.log10(x)` | float or int | float | Base-10 log. Domain error if x ≤ 0 |
| `math.exp(x)` | float or int | float | eˣ |
| `math.exp2(x)` | float or int | float | 2ˣ |

### Trigonometry (radians)

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.sin(x)` | float or int | float | |
| `math.cos(x)` | float or int | float | |
| `math.tan(x)` | float or int | float | |
| `math.asin(x)` | float or int | float | Domain error if x ∉ [-1, 1] |
| `math.acos(x)` | float or int | float | Domain error if x ∉ [-1, 1] |
| `math.atan(x)` | float or int | float | |
| `math.atan2(y, x)` | float or int | float | Full-quadrant arc tangent |
| `math.sinh(x)` | float or int | float | |
| `math.cosh(x)` | float or int | float | |
| `math.tanh(x)` | float or int | float | |

### Rounding

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.floor(x)` | float or int | float | Round toward −∞ |
| `math.ceil(x)` | float or int | float | Round toward +∞ |
| `math.round(x)` | float or int | float | Round to nearest, ties away from 0 |
| `math.trunc(x)` | float or int | float | Round toward 0 |

### Utilities

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.abs(x)` | float or int | same type | Type-preserving: `abs(-3)` → int 3 |
| `math.min(a, b)` | float or int | same type | Type-preserving when both are int |
| `math.max(a, b)` | float or int | same type | Type-preserving when both are int |
| `math.clamp(v, lo, hi)` | float or int | same type | Error if lo > hi |
| `math.sign(x)` | float or int | int | Returns -1, 0, or 1 |
| `math.fmod(x, y)` | float or int | float | Remainder. Error if y == 0 |

### Conversion

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.to_int(x)` | float or int | int | Truncates toward zero |
| `math.to_float(x)` | float or int | float | |
| `math.deg_to_rad(x)` | float or int | float | Multiplies by π/180 |
| `math.rad_to_deg(x)` | float or int | float | Multiplies by 180/π |

### Predicates

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.is_nan(x)` | float or int | bool | True if x is NaN |
| `math.is_inf(x)` | float or int | bool | True if x is ±∞ |

### Approximate equality

```fluxa
bool ok = math.approx(0.1 + 0.2, 0.3)         // true (default epsilon 1e-9)
bool ok = math.approx(1.0, 1.001, 0.01)        // true (custom epsilon)
```

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.approx(a, b)` | float or int | bool | `\|a - b\| < 1e-9` |
| `math.approx(a, b, epsilon)` | float or int | bool | `\|a - b\| < epsilon`. Error if epsilon < 0 |

Use this instead of `==` for all float comparisons.

### Example

```fluxa
import std math

float kp     = 2.5
float signal = 4.0
float output = math.clamp(kp * signal, -100.0, 100.0)
print(output)  // 10.0

float angle_rad = math.deg_to_rad(45.0)
print(math.sin(angle_rad))  // ~0.707
```

---

## std.csv

CSV file processing. All file I/O requires `danger {}`. Pure string operations work outside `danger`.

**Enable:**
```toml
[libs]
std.csv = "1.0"

[libs.csv]
max_line_bytes = 1024   # max bytes per line (default 1024)
max_fields     = 64     # max fields per row (default 64)
```

### Data model

All functions return `dyn` of `str` — each element is one raw CSV line. Fields are extracted with `csv.field(row, idx)`.

```fluxa
dyn rows   = csv.load("data.csv")
str row    = rows[0]
str field0 = csv.field(row, 0)
int n      = csv.field_count(row)
```

### Mode A — Cursor (recommended for large files)

The cursor is a `dyn` wrapping a `FILE*`. Use `prst dyn cursor` in the main program scope to survive hot reloads. **Never use `dyn` cursor as a Block field.**

```fluxa
import std csv

prst dyn cursor = csv.open("data.csv")

danger {
    dyn chunk = csv.next(cursor, 1000)
    while len(chunk) > 0 {
        dyn data = csv.skip(chunk, 1)
        for row in data {
            str id   = csv.field(row, 0)
            str temp = csv.field(row, 1)
        }
        chunk = csv.next(cursor, 1000)
    }
    csv.close(cursor)
}
```

### Mode B — Chunk direct

```fluxa
import std csv

danger {
    dyn chunk = csv.chunk("data.csv", 500)
    for row in chunk {
        str temp = csv.field(row, 2)
    }
}
```

### Mode C — Load all

```fluxa
import std csv

danger {
    dyn all  = csv.load("config.csv")
    dyn data = csv.skip(all, 1)
    print(len(data))
}
```

### Function reference

**File operations (require `danger {}`):**

| Function | Returns | Description |
|---|---|---|
| `csv.open(str path)` | dyn cursor | Open file. Keep as `prst dyn` in main scope. |
| `csv.open(str path, str delim)` | dyn cursor | Open with custom delimiter. |
| `csv.next(dyn cursor, int n)` | dyn | Read next n lines. Empty dyn = EOF. |
| `csv.close(dyn cursor)` | nil | Close file and free cursor. |
| `csv.chunk(str path, int n)` | dyn | Open, read n lines from start, close. |
| `csv.chunk(str path, int n, int offset)` | dyn | Read n lines at byte offset. |
| `csv.load(str path)` | dyn | Load entire file as dyn of str. |
| `csv.save(dyn data, str path)` | nil | Write each element as a line. |

**String operations (no `danger` needed):**

| Function | Returns | Description |
|---|---|---|
| `csv.field(str row, int idx)` | str | Extract field (0-based). |
| `csv.field(str row, int idx, str delim)` | str | Extract with custom delimiter. |
| `csv.field_count(str row)` | int | Count fields in row. |
| `csv.field_count(str row, str delim)` | int | Count with custom delimiter. |
| `csv.skip(dyn chunk, int n)` | dyn | Return chunk without first n rows. |
| `csv.is_eof(dyn cursor)` | bool | True if cursor reached EOF. |

### IoT sensor loop pattern

```fluxa
import std csv

Block SensorLog {
    prst int   readings = 0
    prst float sum_temp = 0.0
    fn record(float t) nil {
        sum_temp = sum_temp + t
        readings = readings + 1
    }
    fn avg() float { return sum_temp / readings }
}

Block log typeof SensorLog
prst dyn cur = csv.open("sensors.csv")

fn process_chunk() nil {
    danger {
        dyn chunk = csv.next(cur, 100)
        dyn data  = csv.skip(chunk, 1)
        for row in data {
            str raw = csv.field(row, 1)
            log.readings = log.readings + 1
        }
    }
}
```

---

## std.json

JSON as strings — no parse tree, no intermediate data structures.

**Enable:**
```toml
[libs]
std.json = "1.0"

[libs.json]
max_str_bytes = 4096
```

### Building JSON objects

```fluxa
import std json

str obj = json.object()
obj = json.set(obj, "sensor_id", json.from_str("s001"))
obj = json.set(obj, "temp",      json.from_float(23.5))
obj = json.set(obj, "active",    json.from_bool(true))
obj = json.set(obj, "count",     json.from_int(42))
// {"sensor_id":"s001","temp":23.5,"active":true,"count":42}
```

### Extracting from JSON strings

```fluxa
str raw = "{\"temp\":23.5,\"unit\":\"celsius\",\"active\":true}"

float temp   = json.get_float(raw, "temp")
str   unit   = json.get_str(raw,   "unit")
bool  active = json.get_bool(raw,  "active")
bool  exists = json.has(raw, "temp")
```

### JSON arrays

```fluxa
str raw   = "[{\"id\":1},{\"id\":2},{\"id\":3}]"
dyn items = json.parse_array(raw)
int first_id = json.get_int(items[0], "id")
```

### File operations — cursor (require `danger {}`)

Use `prst dyn cur` in main scope. **Never as a Block field.**

```fluxa
prst dyn cur = json.open("readings.json")

danger {
    dyn chunk = json.next(cur, 200)
    while len(chunk) > 0 {
        for item in chunk {
            float t = json.get_float(item, "temp")
        }
        chunk = json.next(cur, 200)
    }
    json.close(cur)
}
```

### Function reference

| Function | Returns | Description |
|---|---|---|
| `json.object()` | str | Returns `"{}"` |
| `json.array()` | str | Returns `"[]"` |
| `json.set(str obj, str key, str val)` | str | Add or replace key |
| `json.from_str(str s)` | str | Fluxa str → JSON string literal |
| `json.from_float(float f)` | str | float → JSON number |
| `json.from_int(int n)` | str | int → JSON number |
| `json.from_bool(bool b)` | str | bool → JSON bool |
| `json.get_str(str json, str key)` | str | Extract string field |
| `json.get_float(str json, str key)` | float | Extract number as float |
| `json.get_int(str json, str key)` | int | Extract number as int |
| `json.get_bool(str json, str key)` | bool | Extract boolean |
| `json.has(str json, str key)` | bool | True if key exists |
| `json.parse_array(str raw)` | dyn | JSON array → dyn of str |
| `json.stringify(dyn data)` | str | dyn of str → JSON array |
| `json.valid(str raw)` | bool | Quick structural validation |
| `json.open(str path)` | dyn cursor | Open JSON array file |
| `json.next(dyn cursor, int n)` | dyn | Read next n objects |
| `json.close(dyn cursor)` | nil | Close file |
| `json.load(str path)` | str | Load entire file as str |
| `json.is_eof(dyn cursor)` | bool | True if EOF |

---

## std.json2 — Full DOM JSON Parser

Unlike `std.json` (streaming, no tree), `std.json2` parses the entire document into an in-memory tree and lets you navigate it with dot-path and array-index notation.

```toml
[libs]
std.json2 = "1.0"
```

Use `prst dyn doc` in main scope to keep the document across hot reloads. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `json2.parse(str)` | `dyn` | Parse JSON string → document cursor |
| `json2.load(path)` | `dyn` | Parse JSON file → document cursor |
| `json2.stringify(doc)` | `str` | Serialize document back to JSON |
| `json2.get(doc, path)` | `str` | Get value as string |
| `json2.get_int(doc, path)` | `int` | Get value as int |
| `json2.get_float(doc, path)` | `float` | Get value as float |
| `json2.get_bool(doc, path)` | `bool` | Get value as bool |
| `json2.has(doc, path)` | `bool` | Path exists in document |
| `json2.type(doc, path)` | `str` | Type: `"null"`, `"bool"`, `"int"`, `"float"`, `"str"`, `"array"`, `"object"` |
| `json2.length(doc, path)` | `int` | Element count at path |
| `json2.key(doc, path, i)` | `str` | i-th key of object at path |
| `json2.set(doc, path, val)` | `nil` | Set string value |
| `json2.set_int(doc, path, n)` | `nil` | Set int value |
| `json2.set_float(doc, path, f)` | `nil` | Set float value |
| `json2.set_bool(doc, path, b)` | `nil` | Set bool value |
| `json2.delete(doc, path)` | `nil` | Delete node |
| `json2.valid(doc)` | `bool` | Document parsed without error |
| `json2.error(doc)` | `str` | Parse error message |
| `json2.discard(doc)` | `nil` | Release the parsed document tree (see note below) |

> **Memory:** `json2.parse()` allocates a heap-resident document tree behind the `dyn` wrapper. The `dyn` itself is GC-managed (swept at `while` back-edges when unpinned), but the underlying tree is opaque to the GC and is only freed by `json2.discard(doc)`. **Always call `discard` at the end of the `danger` block that owns the document**, otherwise the tree leaks until process exit. The function is named `discard` (not `free`) because `free` is a reserved keyword in the Fluxa parser.

```fluxa
import std json2

danger {
    dyn doc = json2.parse("{\"sensor\":{\"temp\":22.5},\"readings\":[1,2,3]}")
    float temp = json2.get_float(doc, "sensor.temp")
    int first  = json2.get_int(doc, "readings[0]")
    int count  = json2.length(doc, "readings")
    json2.set_float(doc, "sensor.temp", 23.1)
    str updated = json2.stringify(doc)
    json2.discard(doc)
}
```

---

## std.strings

String manipulation. No `danger` required. All operations work on byte offsets.

**Enable:**
```toml
[libs]
std.strings = "1.0"

[libs.strings]
max_out_bytes = 8192
```

| Function | Returns | Description |
|---|---|---|
| `strings.split(str s, str delim)` | dyn | Split on delimiter. |
| `strings.join(dyn parts, str glue)` | str | Join with glue. |
| `strings.concat(a, b, ...)` | str | Concatenate any number of values. |
| `strings.slice(str s, int start, int end)` | str | Byte substring. Negative indices from end. |
| `strings.trim(str s)` | str | Remove leading/trailing whitespace. |
| `strings.find(str s, str sub)` | int | Offset of first occurrence, or -1. |
| `strings.replace(str s, str old, str new)` | str | Replace all occurrences. |
| `strings.starts_with(str s, str prefix)` | bool | |
| `strings.ends_with(str s, str suffix)` | bool | |
| `strings.contains(str s, str sub)` | bool | |
| `strings.count(str s, str sub)` | int | Count non-overlapping occurrences. |
| `strings.lower(str s)` | str | ASCII lowercase. |
| `strings.upper(str s)` | str | ASCII uppercase. |
| `strings.repeat(str s, int n)` | str | Repeat n times. |
| `strings.from_int(int n)` | str | int or float to string. |
| `strings.to_int(str s)` | int | Parse as integer (atol). Returns 0 if not parseable. |
| `strings.hash(str s)` | int | FNV-1a 32-bit hash. Useful for hash-table indexing. |

---

## std.cache — Sharded K/V Cache + Arena Allocator

Thread-safe key/value cache with sharded locks and a bump-pointer arena allocator. Designed for HTTP workers that need response memoization and short-lived per-request scratch storage without the malloc/free overhead of `strings.concat` chains.

Two independent subsystems share one library:
- **Cache** — 32 shards × 256 slots each = **8192 entries**. Per-shard `pthread_mutex` keeps contention bounded under heavy concurrency. Keys and values are owned copies (caller can free their inputs immediately after `cache.put`). When the 8-probe window in a shard fills, `cache.put` performs **random eviction** of one of the probed slots — recent writes always succeed.
- **Arena** — up to 64 concurrent arenas, each a linked list of 64 KB slabs with 8-byte aligned bump allocation. `arena_reset` returns the arena to one fresh slab in O(slabs); `arena_drop` releases everything. As of v0.23 `arena_str`/`arena_concat` return an ordinary refcounted string (a copy out of the arena), so the returned value follows the normal ownership rules; the arena's own memory is still bulk-released only by `arena_reset` or `arena_drop`. The arena remains the win for building many short-lived strings without per-piece `malloc`/`free`.

**Enable:**
```toml
[libs]
std.cache = "1.0"
```

The cache and arena tables are zero-init and lazily constructed on first call from any thread (`pthread_once`). No global state cost when the lib is enabled but unused.

### Cache functions

| Function | Returns | Description |
|---|---|---|
| `cache.put(str key, str value)` | nil | Insert, update, or evict-and-insert. Both strings are copied. |
| `cache.get(str key)` | str | Returns an owned copy of the value, or `""` if missing. Caller frees. |
| `cache.del(str key)` | bool | Remove the entry. Returns `true` if removed. |
| `cache.clear()` | nil | Empty all shards. |
| `cache.size()` | int | Total populated entries across all shards. |
| `cache.stats()` | str | Snapshot of put/get counters as `key=value ...` for diagnostics. |
| `cache.stats_reset()` | nil | Zero the counters. Typically called after warm-up. |

**Probe behavior.** Each shard holds 256 slots probed linearly up to 8 steps from the key's natural position. When all 8 probes are taken on a `put`, one of the probed slots is **randomly evicted** to make room for the new entry. Hot keys tend to displace cold ones over time. Each shard maintains its own xorshift32 PRNG state — no cross-thread coordination cost.

**Diagnostic output of `cache.stats()`:**
```
size=N capacity=8192 shards=32 probe=8
puts=N inserts=N updates=N evicts=N failures=N
gets=N hits=N misses=N hit_ratio=0.XXXX
```

Under healthy operation, `failures` stays at 0 (random eviction never fails). `evicts` rising while `hit_ratio` stays high indicates the working set is larger than capacity but recent traffic is still served — this is the expected steady state for HTTP caches with high-cardinality keys.

**Hash.** FNV-1a 32-bit — same as `strings.hash` — so a key produces the same shard whether you hash it manually or let the cache do it.

### Arena functions

| Function | Returns | Description |
|---|---|---|
| `cache.arena_new()` | int | Create a fresh arena. Returns handle `> 0` or 0 on table-full. |
| `cache.arena_str(int h, str src)` | str | Build `src` via the arena; returns an ordinary refcounted string (v0.23+). |
| `cache.arena_concat(int h, str a, str b)` | str | Concatenate into the arena. |
| `cache.arena_concat3(int h, a, b, c)` | str | Three-way concat. |
| `cache.arena_concat5(int h, a, b, c, d, e)` | str | Five-way concat (common for JSON building). |
| `cache.arena_reset(int h)` | nil | Free all slabs except the first; reset bump pointer. O(slabs). |
| `cache.arena_drop(int h)` | nil | Release the arena entirely. Handle becomes invalid. |

**Lifetime rule.** Strings returned by `arena_str` / `arena_concat*` live until the next `arena_reset` or `arena_drop` on that handle. **Do not** pass them to `free()` — `free()` rejects arena-allocated pointers (they were never `malloc`'d directly).

### Example — response cache

```fluxa
import std cache
import std strings
import std wserver

fn worker(int srv) nil {
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            str path = wserver.req_path(req)
            str id = strings.slice(path, 7, len(path))  // strip "/users/"
            str cached = cache.get(id)
            if cached != "" {
                wserver.reply_json(req, 200, cached)
            }
            if cached == "" {
                // ...build response from DB...
                str row = build_user_row(id)
                cache.put(id, row)              // cache stores its own copy
                wserver.reply_json(req, 200, row)
                free(row)
            }
            free(cached); free(id); free(path)
        }
    }
}
```

### Example — per-request arena (eliminates per-iter malloc/free chain)

```fluxa
import std cache
import std strings

fn worker(int srv) nil {
    int arena = cache.arena_new()
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            str id   = wserver.req_path(req)
            str name = "alice"
            // No free() needed for j; arena owns it until reset.
            str j = cache.arena_concat5(arena,
                "{\"id\":\"", id, "\",\"name\":\"", name, "\"}")
            wserver.reply_json(req, 200, j)
            cache.arena_reset(arena)            // O(slabs) wipe, ready for next
            free(id)
        }
    }
    cache.arena_drop(arena)
}
```

### When to use which

| Pattern | Use |
|---|---|
| Cross-request memoization (auth tokens, DB rows by ID) | Cache |
| Per-request response building (5–15 string pieces) | Arena |
| Both at once (cache a built response) | `arena_concat*` to build, `cache.put` to store (cache copies in) |
| One-off concat outside a hot loop | Plain `strings.concat` + `free` |

---

## std.time

Time functions. No `danger` required.

**Enable:**
```toml
[libs]
std.time = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `time.sleep(int ms)` | nil | Block for N milliseconds |
| `time.sleep_us(int us)` | nil | Block for N microseconds |
| `time.now_ms()` | int | Monotonic timestamp in ms |
| `time.now_us()` | int | Monotonic timestamp in µs |
| `time.ticks()` | int | Raw hardware tick counter |
| `time.elapsed_ms(int since)` | int | ms since prior `now_ms()`. Safe against wraparound. |
| `time.timeout(int start, int max_ms)` | bool | True if max_ms passed since start |
| `time.format(int ms)` | str | Human-readable UTC datetime |

---

## std.flxthread

Concurrency for Fluxa. Threads are isolated by default — Block instances have no shared state. No `danger` required for any `ft.*` call.

**Enable:**
```toml
[libs]
std.flxthread = "1.0"
std.time      = "1.0"
```

```fluxa
import std flxthread as ft
```

### Function reference

**Thread lifecycle:**

| Function | Blocking? | Description |
|---|---|---|
| `ft.new("name", fn_str)` | No | Spawn global function as thread |
| `ft.new("name", fn_str, arg1, ...)` | No | Spawn global function with arguments (up to `max_msg_args`) |
| `ft.new("prefix", n, fn_str[, arg1, ...])` | No | Batch: spawn `n` threads named `prefix1`..`prefixN`, each running `fn_str` with the given args |
| `ft.new("name", instance, "method")` | No | Spawn Block method as thread |
| `ft.resolve_all()` | Yes | Wait for all threads. Syncs prst pool. |
| `ft.active("name")` | No | True if thread is still running |
| `ft.thread_count()` | No | Number of active threads |

**Communication:**

| Function | Blocking? | Description |
|---|---|---|
| `ft.message("name", "method")` | No | Enqueue method call |
| `ft.message("name", "method", arg1, ...)` | No | Enqueue with arguments (up to `max_msg_args`) |
| `ft.await("name", "method")` | Yes | Enqueue + wait for return |
| `ft.await("name", "method", arg1, ...)` | Yes | Enqueue with arguments, wait for return |

**Stop control:**

| Function | Description |
|---|---|
| `ft.stop("name")` | Cooperative stop. Thread exits at next back-edge. |
| `ft.kill("name")` | Force stop. WARNING: held `ft.lock()` mutexes NOT released. |
| `ft.should_stop()` | True if stop was requested. Use in `while !ft.should_stop()`. |

**Shared state:**

| Function | Description |
|---|---|
| `ft.lock("var_name")` | Serialize access to a prst var. Main scope only — Block prst is isolated by design. |

**Configuration (`fluxa.toml`):**

```toml
[libs.flxthread]
max_msg_args = 2    # max arguments for ft.new/ft.message/ft.await
                    # default 2, range [1..8] (hard cap)
```

Exceeding `max_msg_args` pushes a clear error to `err`. Arity mismatch
(more args than function parameters) is caught at `ft.new` time.

**Batch spawn.** `ft.new("w", 16, "worker", srv)` spawns 16 global-function
threads named `w1`..`w16` (1-indexed), each running `worker(srv)` — a drop-in
replacement for sixteen `ft.new("w1", "worker", srv)` … `ft.new("w16", ...)`
lines. The batch form is selected purely by the **type** of the second argument:
an `int` (the count) means batch, a string means the single global-function
form, and a Block instance means the method form. A numeric *name* such as
`ft.new("w10", "worker", srv)` is therefore unaffected — `"w10"` is the name in
the first slot, the string `"worker"` is still the second argument, so it takes
the single form as before. The count must be in `1..FLUXA_THREAD_MAX`; the same
`max_msg_args` and arity checks apply to the trailing arguments.

```fluxa
// these two are equivalent:
ft.new("w1", "worker", srv)
ft.new("w2", "worker", srv)   // ... through w16
// ⇕
ft.new("w", 16, "worker", srv)
```

> **Thread cap:** the runtime tracks a compile-time hard limit on
> concurrently-active threads (`FLUXA_THREAD_MAX`, currently 64). This is
> generous for embedded targets but may be insufficient for HTTP servers
> dispatching one worker per CPU core × N. If you hit the cap, `ft.new`
> returns an error in `err` and the thread is not spawned.

### Full example

```fluxa
import std flxthread as ft
import std time

prst int contador = 0
ft.lock("contador")

fn incrementar() nil {
    contador = contador + 1
}

Block Enemy {
    prst int health = 100

    fn update() nil {
        while !ft.should_stop() {
            health = health - 1
            time.sleep(16)
        }
    }

    fn hit(int damage) nil { health = health - damage }
    fn get_health() int { return health }
}

Block e1 typeof Enemy
Block e2 typeof Enemy

ft.new("t1", e1, "update")
ft.new("t2", e2, "update")
ft.new("t3", "incrementar")

time.sleep(40)
ft.message("t1", "hit", 10)
int hp = ft.await("t1", "get_health")
print(hp)

ft.stop("t1")
ft.stop("t2")
ft.resolve_all()
print(contador)
```


---

## std.cabi — Deterministic Typed Host Bridge

`std.cabi` is the Fluxa-side endpoint of the stable Fluxa-lang C ABI. Its job is deliberately narrow: **exchange typed values between an external host and a Fluxa program**.

It is a communication bridge, not a persistence or runtime-state API.

### Contract

Only these semantic values may cross the C ABI v1 boundary:

- `int`
- `float`
- `bool`
- `str`
- homogeneous `int arr`
- homogeneous `float arr`
- homogeneous `bool arr`
- homogeneous `str arr`

The protocol does **not** transport `prst`, `dyn`, Blocks, pointers, native handles, GC state, VM state, AST nodes, handover state, snapshots, callbacks, or any other Fluxa runtime structure.

### Enable

Build-time (`fluxa.libs`):

```toml
[libs.build]
std.cabi = true
```

Runtime permission (`fluxa.toml`):

```toml
[libs]
std.cabi = "1.0"
```

Then:

```fluxa
import std cabi
```

There is no separate `build-cabi` workflow. When `std.cabi = true`, the normal build emits the host library together with the normal runtime.

### Deterministic clear-wire format

The clear frame is named `FXCB`, wire version 1. Multibyte numeric fields are explicitly little-endian; no C struct is copied to the wire.

| Fluxa value | Wire representation |
|---|---|
| `int` | signed 32-bit little-endian |
| `float` | IEEE-754 binary64, little-endian bit pattern |
| `bool` | one byte: `0` or `1` |
| `str` | byte length + UTF-8 bytes |
| `int arr` | element count + i32 elements |
| `float arr` | element count + binary64 elements |
| `bool arr` | element count + 0/1 bytes |
| `str arr` | element count + repeated byte-length + UTF-8 bytes |

`int` is fixed to i32 on the wire because Fluxa's internal C `long` differs between LP64 and LLP64 targets. The bridge therefore has identical bytes on Linux x64 and Windows x64.

The clear encoding is canonical: the same ordered value list produces the same `FXCB` bytes on every supported host architecture.

### Fluxa API

Request inspection:

| Function | Returns | Meaning |
|---|---|---|
| `cabi.version()` | `str` | bridge version |
| `cabi.count()` | `int` | number of input values |
| `cabi.type(index)` | `str` | `int`, `float`, `bool`, `str`, `arr<int>`, `arr<float>`, `arr<bool>`, or `arr<str>` |
| `cabi.read_int(index)` | `int` | read one integer |
| `cabi.read_float(index)` | `float` | read one float |
| `cabi.read_bool(index)` | `bool` | read one boolean |
| `cabi.read_str(index)` | `str` | read one string |
| `cabi.read_int_arr(index, destination)` | `int arr` | read one integer array |
| `cabi.read_float_arr(index, destination)` | `float arr` | read one float array |
| `cabi.read_bool_arr(index, destination)` | `bool arr` | read one boolean array |
| `cabi.read_str_arr(index, destination)` | `str arr` | read one string array |

Response construction:

| Function | Meaning |
|---|---|
| `cabi.response_reset()` | clear the current response |
| `cabi.write_int(value)` | append `int` |
| `cabi.write_float(value)` | append `float` |
| `cabi.write_bool(value)` | append `bool` |
| `cabi.write_str(value)` | append `str` |
| `cabi.write_int_arr(arr)` | append `int arr` |
| `cabi.write_float_arr(arr)` | append `float arr` |
| `cabi.write_bool_arr(arr)` | append `bool arr` |
| `cabi.write_str_arr(arr)` | append `str arr` |

Type mismatches fail with normal Fluxa error semantics. Because Fluxa-lang typed arrays have a fixed declared size, inbound array reads fill a caller-declared destination array in place. The destination size must exactly match the wire array size; otherwise the exchange fails cleanly.

Arrays are homogeneous by contract; a writer rejects an array containing an element of another type.

### Dispatcher example

```fluxa
import std cabi

fn cabi_dispatch() int {
    int id = cabi.read_int(0)
    float value = cabi.read_float(1)
    bool enabled = cabi.read_bool(2)
    str label = cabi.read_str(3)
    int arr points[3] = 0
    cabi.read_int_arr(4, points)

    cabi.response_reset()
    cabi.write_int(id)
    cabi.write_float(value)
    cabi.write_bool(enabled)
    cabi.write_str(label)
    cabi.write_int_arr(points)
    return 0
}
```

The dispatcher has no implicit C ABI state beyond the values of the currently active exchange.

### Host API

The public header is `src/cabi/fluxa_cabi.h`. It exposes an opaque `fluxa_cabi_runtime` and the deterministic message builder/reader:

```c
fluxa_cabi_message req;
fluxa_cabi_message_init(&req);

fluxa_cabi_add_int(&req, 42);
fluxa_cabi_add_float(&req, 3.5);
fluxa_cabi_add_bool(&req, 1);
fluxa_cabi_add_str(&req, "Fluxa", 5);

fluxa_cabi_view in = { req.data, req.size };
fluxa_cabi_view out;

fluxa_cabi_exchange(rt, &in, &out, &err);
```

Reader functions (`fluxa_cabi_get_int`, `fluxa_cabi_get_float`, etc.) decode the returned borrowed view without exposing Fluxa internals.

For end-to-end host integration examples in C, C++, Python, Java, JavaScript/Node.js, Go, C#, and Lua, see the dedicated **[C ABI Integration Guide](CABI_INTEGRATION.md)**.

### Optional secure envelope

Security is **outside** the typed value protocol. Enabling it never adds a new Fluxa value type and never changes the deterministic `FXCB` encoding.

When `std.cabi` and `std.crypto` are both enabled at build time, the C ABI can use a 32-byte shared key to wrap an `FXCB` frame in an authenticated `FXCS` envelope using XChaCha20-Poly1305 (libsodium):

```text
int / float / bool / str / arr
              │
              ▼
      deterministic FXCB
              │
      optional seal(key)
              ▼
       authenticated FXCS
```

`fluxa_cabi_seal()` and `fluxa_cabi_unseal()` are optional host helpers. `fluxa_cabi_security_available()` reports whether the current build contains the libsodium backend.

A fresh nonce is generated for every seal, so encrypted `FXCS` packets are intentionally non-deterministic. After successful authentication/decryption, the recovered `FXCB` bytes are exactly the canonical deterministic frame that was originally sealed.

Keys are host-side security material. They are not Fluxa values and do not enter `std.cabi`.

### Correctness test and bridge benchmark

The functional gate is intentionally separate from the performance benchmark:

```bash
make test-cabi
make bench-cabi
```

`make test-cabi` validates the Fluxa-side API, canonical wire encoding, real host
round-trip, and the optional secure envelope when available. `make bench-cabi`
runs a 10-second clear-wire throughput benchmark using one persistent runtime and
one host thread. Request frames are prebuilt outside the timed loop.

The benchmark has two 5-second phases:

- **READ:** host sends all eight supported wire tags; Fluxa decodes them and returns a boolean acknowledgement.
- **RESPONSE:** host sends a small integer trigger; Fluxa constructs and returns all eight supported wire tags.

First validated Linux x64 baseline:

| Phase | Throughput | Mean exchange time | Wire throughput |
|---|---:|---:|---:|
| READ | 380,578 exch/s | 2,627.6 ns/exch | 72.59 MiB/s |
| RESPONSE | 561,739 exch/s | 1,780.2 ns/exch | 102.32 MiB/s |
| Combined average | 471,158 exch/s | — | — |

Performance numbers are machine-specific and are recorded as a regression
baseline, not as a portability or ABI guarantee.

### Threading and ownership

Calls to one `fluxa_cabi_runtime` are serialized internally. `std.cabi` itself does not depend on `std.flxthread`.

A response view returned by `fluxa_cabi_exchange()` is borrowed and remains valid until the next exchange on that runtime or until the runtime is closed. `fluxa_cabi_message` buffers created by the host are owned by the host and released with `fluxa_cabi_message_free()`.

### Architectural boundary

```text
external host
 C / C++ / Go / Rust / Python / C#
                │
                │ int / float / bool / str / arr
                ▼
       deterministic C ABI
                │
                ▼
          Fluxa runtime
                │
                ▼
          Fluxa program
                │
                │ int / float / bool / str / arr
                ▼
          external host
```

The bridge transports values. **Fluxa runtime state stays inside Fluxa.**

---
## std.crypto

Cryptographic primitives via libsodium 1.0.18+. Dep: `apt install libsodium-dev`.

```toml
[libs]
std.crypto = "1.0"
```

Key material is stored in `int arr` (each byte as `VAL_INT [0..255]`) — `prst int arr key` survives hot reloads.

| Function | Signature | Description |
|---|---|---|
| `hash` | `hash(data: str\|arr) → dyn[32]` | BLAKE2b-256 |
| `to_hex` | `to_hex(arr) → str` | Encode as lowercase hex |
| `from_hex` | `from_hex(str) → dyn` | Decode hex to byte arr |
| `keygen` | `keygen() → dyn[32]` | Random 32-byte symmetric key |
| `nonce` | `nonce() → dyn[24]` | Random 24-byte nonce |
| `encrypt` | `encrypt(msg, key, nonce) → dyn` | XSalsa20-Poly1305 authenticated encryption |
| `decrypt` | `decrypt(cipher, key, nonce) → str` | Verify MAC then decrypt |
| `sign_keygen` | `sign_keygen(pk: int arr[32], sk: int arr[64]) → nil` | Ed25519 keypair |
| `sign` | `sign(msg, sk) → dyn` | Sign message |
| `sign_open` | `sign_open(signed, pk) → str` | Verify + extract |
| `kx_keygen` | `kx_keygen(pk: int arr[32], sk: int arr[32]) → nil` | Curve25519 keypair |
| `kx_client` | `kx_client(rx, tx, cpk, csk, spk) → nil` | Client session keys |
| `kx_server` | `kx_server(rx, tx, spk, ssk, cpk) → nil` | Server session keys |
| `compare` | `compare(a, b) → bool` | Constant-time comparison |
| `wipe` | `wipe(arr) → nil` | Securely zero arr |
| `version` | `version() → str` | libsodium version |

```fluxa
import std crypto

prst dyn session_key = crypto.keygen()

dyn nonce  = crypto.nonce()
dyn cipher = crypto.encrypt("secret payload", session_key, nonce)

danger {
    str plain = crypto.decrypt(cipher, session_key, nonce)
    print(plain)
}
```

---

## std.pid — PID Controller

Pure C99, zero deps. Embedded-friendly.

```toml
[libs]
std.pid = "1.0"
```

Use `prst dyn ctrl` in main scope to keep state across hot reloads. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `pid.new(kp, ki, kd)` | `dyn` | Create controller cursor |
| `pid.compute(ctrl, setpoint, pv)` | `float` | Compute output |
| `pid.reset(ctrl)` | `nil` | Zero integral and prev_error |
| `pid.set_limits(ctrl, min, max)` | `nil` | Clamp output. Enables anti-windup. |
| `pid.set_deadband(ctrl, band)` | `nil` | Ignore errors smaller than band |
| `pid.state(ctrl)` | `dyn` | [kp, ki, kd, integral, prev_error, out_min, out_max] |

```fluxa
import std pid

prst dyn heater = pid.new(2.0, 0.5, 0.1)
pid.set_limits(heater, 0.0, 100.0)
pid.set_deadband(heater, 0.2)

float duty = pid.compute(heater, 72.0, 68.5)
```

---

## std.sqlite — Embedded SQL

SQLite 3 wrapper. Dep: `apt install libsqlite3-dev`.

```toml
[libs]
std.sqlite = "1.0"
```

Use `prst dyn db` in main scope to keep the connection open across hot reloads. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `sqlite.open(path)` | `dyn` | Open (or create) database file |
| `sqlite.close(db)` | `nil` | Close connection. Double-close is a no-op. |
| `sqlite.exec(db, sql)` | `nil` | DDL or DML |
| `sqlite.query(db, sql)` | `dyn` | SELECT. Returns dyn of rows, each row a dyn. |
| `sqlite.last_insert_id(db)` | `int` | Row ID of last INSERT |
| `sqlite.changes(db)` | `int` | Rows affected by last DML |
| `sqlite.version()` | `str` | libsqlite3 version |

```fluxa
import std sqlite

danger {
    dyn db = sqlite.open("sensors.db")
    sqlite.exec(db, "CREATE TABLE IF NOT EXISTS readings (ts INTEGER, val REAL)")
    sqlite.exec(db, "INSERT INTO readings VALUES (1700000000, 23.5)")
    dyn rows = sqlite.query(db, "SELECT ts, val FROM readings LIMIT 5")
    int i = 0
    while i < len(rows) {
        dyn row = rows[i]
        print(row[0])
        print(row[1])
        i = i + 1
    }
    sqlite.close(db)
}
```

---

## std.serial — UART / Serial

UART via libserialport. Dep: `apt install libserialport-dev`.

```toml
[libs]
std.serial = "1.0"
```

Use `prst dyn port` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `serial.list()` | `dyn` | List available port names |
| `serial.open(port, baud)` | `dyn` | Open port at baud rate. 8N1 default. |
| `serial.close(port)` | `nil` | Close. Double-close is a no-op. |
| `serial.write(port, data)` | `int` | Write string. Returns bytes written. |
| `serial.read(port, max_bytes, timeout_ms)` | `str` | Read up to max_bytes with timeout. |
| `serial.readline(port, timeout_ms)` | `str` | Read until `\n` or timeout. |
| `serial.flush(port)` | `nil` | Flush TX/RX buffers. |
| `serial.bytes_available(port)` | `int` | Bytes in RX buffer (non-blocking). |

---

## std.i2c — I2C Protocol

I2C via Linux i2c-dev. No external library required. RP2040: use PICO_SDK directly (i2c-dev is Linux-only).

```toml
[libs]
std.i2c = "1.0"
```

Use `prst dyn bus` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `i2c.open(device, addr)` | `dyn` | Open I2C bus and set slave address |
| `i2c.close(bus)` | `nil` | Close. Double-close is a no-op. |
| `i2c.write(bus, data)` | `int` | Write int arr bytes. Returns bytes written. |
| `i2c.read(bus, nbytes)` | `dyn` | Read nbytes. Returns dyn of int (0–255). |
| `i2c.write_reg(bus, reg, value)` | `nil` | Write single byte to register. |
| `i2c.read_reg(bus, reg)` | `int` | Read single byte from register. |
| `i2c.read_reg16(bus, reg)` | `int` | Read 16-bit big-endian from register. |
| `i2c.scan(device)` | `dyn` | Scan bus. Returns dyn of int addresses. |

**Note:** I2C addresses are plain integers. `0x48` in C = `72` in Fluxa.

---

## std.httpc — HTTP Client (libcurl)

Dep: `apt install libcurl-dev`. All requests require `danger {}`.

```toml
[libs]
std.httpc = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `httpc.get(url)` | `dyn` | HTTP GET |
| `httpc.post(url, body)` | `dyn` | HTTP POST |
| `httpc.post_json(url, json)` | `dyn` | POST with `Content-Type: application/json` |
| `httpc.put(url, body)` | `dyn` | HTTP PUT |
| `httpc.delete(url)` | `dyn` | HTTP DELETE |
| `httpc.status(resp)` | `int` | HTTP status code |
| `httpc.body(resp)` | `str` | Response body |
| `httpc.ok(resp)` | `bool` | True if status 200–299 |

---

## std.https — HTTPS Client (TLS enforced)

Same API as `std.httpc`. Rejects plain `http://` URLs. Verifies server certificate and hostname.

```toml
[libs]
std.https = "1.0"
```

---

## std.mqtt — MQTT Client

Dep: `apt install libmosquitto-dev`. Use `prst dyn client` in main scope. **Never as a Block field.**

```toml
[libs]
std.mqtt = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `mqtt.connect(host, port, client_id)` | `dyn` | Connect to broker |
| `mqtt.connect_auth(host, port, id, user, pass)` | `dyn` | Connect with credentials |
| `mqtt.disconnect(cursor)` | `nil` | Disconnect and free |
| `mqtt.publish(cursor, topic, payload)` | `nil` | Publish QoS 0 |
| `mqtt.publish_qos(cursor, topic, payload, qos)` | `nil` | Publish with QoS |
| `mqtt.subscribe(cursor, topic)` | `nil` | Subscribe QoS 0 |
| `mqtt.subscribe_qos(cursor, topic, qos)` | `nil` | Subscribe with QoS |
| `mqtt.loop(cursor, timeout_ms)` | `nil` | Process one event |
| `mqtt.connected(cursor)` | `bool` | True if connection active |

---

## std.mcpc — MCP Client

MCP client over HTTP POST. Dep: `apt install libcurl-dev`.

```toml
[libs]
std.mcpc = "1.0"
```

Use `prst dyn server` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `mcpc.connect(url)` | `dyn` | Create cursor for MCP server |
| `mcpc.connect_auth(url, token)` | `dyn` | Create with Bearer token auth |
| `mcpc.list_tools(cursor)` | `dyn` | List tool names |
| `mcpc.call(cursor, tool, args_json)` | `str` | Call tool, return full JSON |
| `mcpc.call_text(cursor, tool, args_json)` | `str` | Call tool, return text only |
| `mcpc.disconnect(cursor)` | `nil` | Free cursor resources |

---

## std.mcps — MCP Client over HTTPS

Identical API to `std.mcpc` but forces TLS on all requests.

```toml
[libs]
std.mcps = "1.0"
```

---

## std.websocket — WebSocket Client

Two backends: pure C99 RFC 6455 (default, `ws://` only) or libwebsockets (`make FLUXA_WS_LWS=1`, adds `wss://`).

```toml
[libs]
std.websocket = "1.0"
```

Use `prst dyn conn` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `ws.connect(url)` | `dyn` | Connect to `ws://` |
| `ws.connect_tls(url)` | `dyn` | Force TLS (requires libwebsockets build) |
| `ws.send(conn, msg)` | `nil` | Send text frame |
| `ws.send_bin(conn, data)` | `nil` | Send binary frame |
| `ws.recv(conn, timeout_ms)` | `str` | Next message, `""` on timeout |
| `ws.poll(conn)` | `bool` | Message ready without blocking |
| `ws.close(conn)` | `nil` | Close connection |
| `ws.connected(conn)` | `bool` | Connection status |
| `ws.url(conn)` | `str` | Original URL |
| `ws.version()` | `str` | Backend version |

---

## std.http — HTTP Server + Client (mongoose 7.21)

Mongoose-backed. Embedded-friendly — runs on RP2040 Wi-Fi and ESP32. Uses `dyn` cursors (mongoose lifecycle model).

```toml
[libs]
std.http = "1.0"
```

**Server:**

| Function | Returns | Description |
|---|---|---|
| `http.serve(port)` | `dyn` | Start HTTP server |
| `http.serve_tls(port, cert, key)` | `dyn` | HTTPS server |
| `http.poll(server, timeout_ms)` | `dyn\|nil` | Wait for next request |
| `http.req_method(req)` | `str` | `"GET"`, `"POST"`, etc. |
| `http.req_path(req)` | `str` | Request URI path |
| `http.req_body(req)` | `str` | Request body |
| `http.req_header(req, name)` | `str` | Header value |
| `http.reply(req, status, body)` | `nil` | Send response |
| `http.reply_json(req, status, json)` | `nil` | Send JSON response |
| `http.stop(server)` | `nil` | Stop server |

**Client:**

| Function | Returns | Description |
|---|---|---|
| `http.get(url)` | `dyn` | HTTP GET |
| `http.post(url, body)` | `dyn` | HTTP POST |
| `http.post_json(url, json)` | `dyn` | POST with JSON content-type |
| `http.put(url, body)` | `dyn` | HTTP PUT |
| `http.delete(url)` | `dyn` | HTTP DELETE |
| `http.status(resp)` | `int` | HTTP status code |
| `http.body(resp)` | `str` | Response body |
| `http.ok(resp)` | `bool` | Status 200–299 |
| `http.version()` | `str` | `"mongoose/7.21"` |

```fluxa
import std http

danger {
    dyn srv = http.serve(8080)
    dyn req = http.poll(srv, 10000)
    if req != nil {
        str path = http.req_path(req)
        http.reply_json(req, 200, "{\"temp\":22.5}")
    }
    http.stop(srv)
}
```

---

## std.mcp — Fluxa as MCP Server

Exposes the Fluxa runtime as a Model Context Protocol server over HTTP. AI agents can discover and call Fluxa tools via JSON-RPC 2.0.

```toml
[libs]
std.http = "1.0"
std.mcp  = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `mcp.serve(port)` | `dyn` | Start MCP server |
| `mcp.poll(server, ms)` | `nil` | Process one request cycle |
| `mcp.stop(server)` | `nil` | Stop server |
| `mcp.version()` | `str` | Server version |

---

## std.graph — 2D/3D Graphics

Two backends: stub (default, zero deps, no-op rendering) or Raylib (`make FLUXA_GRAPH_RAYLIB=1`).

```toml
[libs]
std.graph = "1.0"
```

Use `prst dyn win` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `graph.init(w, h, title)` | `dyn` | Open window. **Needs `danger`** on the raylib backend — with no usable OpenGL driver it raises a catchable error rather than opening a broken window (Windows: see [WINDOWS.md](WINDOWS.md#virtual-machines-and-mesa3d)). |
| `graph.close(win)` | `nil` | Close window |
| `graph.should_close(win)` | `bool` | Window close requested |
| `graph.begin_frame(win)` | `nil` | Begin draw frame |
| `graph.end_frame(win)` | `nil` | Present frame |
| `graph.capture(win)` | `dyn` | Snapshot the current frame as an RGBA image buffer (feeds `std.image`). Release with `image.discard`. Stub returns a blank buffer of the logical size. |
| `graph.draw_image(win, img, x, y[, scale])` | `nil` | Draw an RGBA image buffer (from `std.image` or `graph.capture`) at (x,y). Optional `scale` (1.0 = original). The GPU texture is **cached** on the buffer and reused across frames — re-uploaded only when the pixels change — so drawing every frame is cheap. Completes the round trip (`image → graph`). |
| `graph.clear(win, r, g, b)` | `nil` | Clear background (RGB 0–255) |
| `graph.fps(win)` | `int` | Current FPS |
| `graph.set_fps(win, fps)` | `nil` | Set target FPS |
| `graph.fullscreen(win)` | `bool` | Toggle fullscreen; returns the new state. Raylib: `ToggleFullscreen()` (display switches to the window's resolution where the driver allows). Stub: tracks the flag. Key name `"F11"` is also available in `key_pressed`/`key_down`. |
| `graph.draw_rect(win, x, y, w, h, r, g, b)` | `nil` | Filled rectangle |
| `graph.draw_circle(win, x, y, radius, r, g, b)` | `nil` | Filled circle |
| `graph.draw_line(win, x1, y1, x2, y2, r, g, b)` | `nil` | Line |
| `graph.draw_text(win, text, x, y, size, r, g, b)` | `nil` | Text (built-in font) |
| `graph.load_font(win, path, size)` | `dyn` | Load TTF/OTF font at base size (1–512) |
| `graph.draw_text_font(win, font, text, x, y, size, r, g, b)` | `nil` | Text with a loaded font |
| `graph.text_width(win, font, text, size)` | `int` | Rendered text width in pixels |
| `graph.unload_font(win, font)` | `nil` | Free font (GPU texture in Raylib) |
| `graph.key_pressed(win, key)` | `bool` | Key just pressed |
| `graph.key_down(win, key)` | `bool` | Key held |
| `graph.mouse_x(win)` | `int` | Mouse X |
| `graph.mouse_y(win)` | `int` | Mouse Y |
| `graph.mouse_pressed(win)` | `bool` | Left mouse button |
| `graph.dt(win)` | `float` | Delta time in seconds |
| `graph.open_url(url)` | `bool` | Open a URL in the system's default browser. Only `http://`, `https://` and `mailto:` are accepted. The URL is passed to `exec` as a single argument with no shell, so it cannot carry a command. Works on both backends (no display needed). **Needs `danger`.** |
| `graph.version()` | `str` | Backend version |

### Custom fonts (TTF/OTF)

`graph.load_font` opens a TTF/OTF file and rasterizes a glyph atlas at the given
base size. The atlas covers **ASCII 32–126 plus Latin-1 160–255** — Portuguese and
Western European accented characters (`ã`, `ç`, `é`, …) render correctly; text is
passed as normal UTF-8 `str`.

The font is an opaque `dyn` cursor, same ownership pattern as the window: create
after `graph.init`, pass to workers/functions as an argument, release with
`graph.unload_font` before `graph.close`. Using a font after `unload_font` is an
"invalid font cursor" error, captured by `danger`.

```fluxa
import std graph

danger {
    dyn win  = graph.init(800, 600, "fonts")
    dyn font = graph.load_font(win, "assets/Roboto-Bold.ttf", 32)

    str title = "Colocação — 1º lugar"
    int tw = graph.text_width(win, font, title, 32)
    int cx = (800 - tw) / 2                       // center horizontally

    while !graph.should_close(win) {
        graph.begin_frame(win)
        graph.clear(win, 20, 20, 30)
        graph.draw_text_font(win, font, title, cx, 40, 32, 255, 255, 255)
        graph.end_frame(win)
    }

    graph.unload_font(win, font)
    graph.close(win)
}
if err != nil { print(err[0]) }
```

Notes:
- **Base size vs draw size.** The atlas is rasterized at the `load_font` size; the
  `draw_text_font`/`text_width` size scales it (bilinear filtering). For crisp
  text, load at the size you draw at — load one cursor per size if a screen mixes
  a title size and a body size.
- **Errors.** Missing file → "cannot open font file"; unsupported/corrupt file
  (Raylib backend) → "failed to load font"; size outside 1–512 → error. All
  captured by `danger`.
- **Frame-path discipline.** As with `draw_text`, never build the text with inline
  `strings.*` inside the frame loop — cache the strings (see FLUXA_GUIDE §5) and
  call `text_width` only when the text changes, not per frame.
- **Stub backend.** Validates the file exists, draws nothing, and `text_width`
  returns the deterministic approximation `len(text) * size * 6 / 10` — enough to
  test layout logic without a display.

---

## std.image — Encode / Decode / Transform Images

Two backends: stub (default, zero deps — buffer transforms work, `save`/`load`
report a clear "no codec" error) or the Raylib codec
(`make FLUXA_IMAGE_RAYLIB=1`), which bundles stb_image / stb_image_write and
encodes/decodes **PNG, JPG, BMP, TGA, and QOI**.

```toml
[libs]
std.image = "1.0"
```

`std.image` pairs with `std.graph`: `graph.capture(win)` returns a raw RGBA
buffer (a `dyn`), and `std.image` saves it, resizes it, or reads its size. The
two libs share a neutral 32-bit RGBA buffer format, so neither depends on the
other — a capture is just bytes. An image handle is an opaque `dyn` with the same
ownership pattern as a window or font cursor: pass it as an argument, release it
with `image.discard`. Because the release verb `free` is a reserved keyword, the
lib uses **`image.discard`**.

`image.save` and `image.load` touch the filesystem, so they **must** run inside a
`danger {}` block, exactly like `sqlite`/`csv`/`fs`. The output format is chosen
by the file **extension**.

**`load` vs `sload`.** Use `image.load` for files your program produced or fully
controls. Use `image.sload` for any image whose bytes are **not** under your
control — for example a card PNG dropped into a gallery folder, or a capture file
that could have been swapped between write and read. `sload` runs three checks
around the decode: it rejects files over `max_bytes` (default 24 MB — decompression
bombs), files whose first bytes are not a PNG or QOI signature (wrong/hostile
formats), and decoded images larger than `max_edge` per side (default 8192 px —
memory exhaustion). The two limits are optional trailing arguments: pass tighter
values when you know your own images are smaller (a game card is tens of KB and
~1024 px, so `sload(path, 262144, 1200)` rejects far more than the defaults). A
non-positive limit falls back to its default. These checks shrink the attack
surface but are **not** a guarantee against every decoder bug — the underlying
codec still parses the file — so treat `sload` as defense-in-depth, and still
validate/re-encode untrusted images server-side before redistributing them.

| Function | Returns | Description |
|---|---|---|
| `image.new(w, h)` | `dyn` | Blank RGBA buffer, all pixels transparent (zero) |
| `image.save(img, path)` | `bool` | Encode by extension (`.png`/`.jpg`/`.bmp`/`.tga`/`.qoi`). **Needs `danger`.** |
| `image.load(path)` | `dyn` | Decode a file into an RGBA buffer. **Needs `danger`.** |
| `image.sload(path[, max_bytes[, max_edge]])` | `dyn` | **Secure** decode of an untrusted file: validates size, magic bytes (PNG or QOI only), and dimensions around the decode, to shrink the attack surface of hostile images. Defaults: ≤24 MB, ≤8192 px per edge. Pass `max_bytes` / `max_edge` to tighten the bounds to what your own images should never exceed (e.g. `sload(p, 262144, 1200)` for game cards). Same result as `load` for a valid file within limits. **Needs `danger`.** |
| `image.resize(img, w, h)` | `nil` | Scale in place (Bicubic on the codec backend, nearest-neighbour on stub) |
| `image.blit(dst, src, x, y[, mask])` | `nil` | Compose `src` onto `dst` at (x,y), alpha-blended and clipped. Optional `mask` image gates the source by the mask's alpha (for clipped/rounded frames). Pure RGBA — both backends. |
| `image.width(img)` | `int` | Width in pixels |
| `image.height(img)` | `int` | Height in pixels |
| `image.set_text(path, key, text[, compress])` | `bool` | Embed a text field in an existing PNG as an `iTXt` chunk. `key` is a 1–79 char Latin-1 keyword, `text` is UTF-8. Without the 4th arg the text is stored uncompressed; pass a 4th arg to deflate it. **PNG only. Needs `danger`.** |
| `image.discard(img)` | `nil` | Release the buffer (idempotent; also releases a `graph.capture` handle) |
| `image.version()` | `str` | Backend version (reports whether the codec is present) |

### Capturing and exporting a frame

The canonical use — grab the current frame, shrink it to a thumbnail, and write
a PNG. On the stub backend the transforms run and `save` reports the missing
codec; with `FLUXA_IMAGE_RAYLIB=1` it writes a real file.

```fluxa
import std graph
import std image

danger {
    dyn win = graph.init(800, 600, "capture")
    graph.begin_frame(win)
    graph.clear(win, 10, 20, 40)
    graph.draw_rect(win, 100, 100, 200, 150, 240, 66, 66)
    graph.end_frame(win)

    dyn shot = graph.capture(win)          // RGBA snapshot of the frame
    image.resize(shot, 400, 300)           // scale down to a thumbnail
    image.save(shot, "card.png")           // encode by extension → PNG
    image.discard(shot)                    // release (not free — reserved word)
    graph.close(win)
}
if err != nil { print(err[0]) }
```

The same buffer can be written to more than one format before release — call
`image.save` with `card.png`, then `card.jpg`, then `card.bmp` on the same
handle. Any single `image.save` / `image.load` sits inside `danger`, with the
`if err != nil` decision right after, per the error-handling idiom.

### Composing a card and sealing metadata into it

The full Elite Card flow: capture the unique game frame, compose it inside a
pre-rendered frame image through a mask (so it takes the frame's clipped shape),
save, then seal the card's cryptographic proof into the PNG itself as `iTXt`.

```fluxa
import std graph
import std image

danger {
    dyn frame  = graph.capture(win)        // the run's unique frame
    image.resize(frame, 360, 200)          // fit the card's art window

    dyn card   = image.load("card_frame.png")   // pre-rendered border art
    dyn mask   = image.load("card_mask.png")     // alpha = where art shows
    image.blit(card, frame, 40, 92, mask)  // compose through the mask

    image.save(card, "elite_card.png")     // encode the composed card
    image.set_text("elite_card.png", "starfight-proof", proof_hex)  // seal proof

    image.discard(frame)
    image.discard(card)
    image.discard(mask)
}
if err != nil { print(err[0]) }
```

The `iTXt` chunk travels inside the PNG, so the proof survives copying, sharing,
and re-hosting the image — any PNG reader can read it back, and the card stays
verifiable offline. Pass a 4th argument to `image.set_text` to deflate long
proofs.

### The full round trip: graph ⇄ image

`graph.capture` and `graph.draw_image` are inverses, so pixels flow both ways:
`graph → image` (snapshot) and `image → graph` (draw). A card built with
`image.load` + `blit` can be shown in-game with `graph.draw_image`, and a live
frame can be captured, edited, and drawn back. `draw_image` caches the uploaded
GPU texture on the image buffer and reuses it across frames — re-uploading only
when the pixels change (`resize`/`blit` mark the buffer dirty) — so drawing a
card or HUD image every frame in the game loop stays cheap.

```fluxa
dyn card = image.load("elite_card.png")   // decode once
// ... in the render loop, every frame:
graph.draw_image(win, card, 220, 80)       // texture uploaded once, then reused
graph.draw_image(win, card, 40, 400, 0.4)  // same buffer, drawn small as a thumbnail
```

## std.infer — Local LLM Inference

Two backends: stub (default) or llama.cpp (`make FLUXA_INFER_LLAMA=1`).

```toml
[libs]
std.infer = "1.0"
```

Use `prst dyn model` in main scope. **Never as a Block field.**

| Function | Returns | Description |
|---|---|---|
| `infer.load(path)` | `dyn` | Load GGUF model |
| `infer.generate(model, prompt)` | `str` | Generate text (256 tokens) |
| `infer.generate_n(model, prompt, n)` | `str` | Generate max n tokens (1–4096) |
| `infer.unload(model)` | `nil` | Free model |
| `infer.loaded(model)` | `bool` | Load status |
| `infer.ctx_size(model)` | `int` | Context window size |
| `infer.model_name(model)` | `str` | Filename from path |
| `infer.version()` | `str` | Backend version |

---

## std.zlib — Compression

Dep: `apt install zlib1g-dev`. Compressed output is base64-encoded for safe `str` transport.

```toml
[libs]
std.zlib = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `zlib.compress(data)` | `str` | Deflate + base64 encode |
| `zlib.decompress(data)` | `str` | Base64 decode + inflate |
| `zlib.gzip(data)` | `str` | Gzip + base64 encode |
| `zlib.gunzip(data)` | `str` | Base64 decode + gunzip |
| `zlib.crc32(data)` | `int` | CRC-32 checksum |
| `zlib.adler32(data)` | `int` | Adler-32 checksum |
| `zlib.ratio(orig, comp)` | `float` | Compression ratio |
| `zlib.version()` | `str` | zlib version |

---

## std.fs — Filesystem

POSIX file and directory operations. Pure C99, zero deps.

```toml
[libs]
std.fs = "1.0"
```

| Function | Returns | Description |
|---|---|---|
| `fs.read(path)` | `str` | Read entire file (stops at first NUL — text only, not binary) |
| `fs.read_base64(path, max_bytes, type)` | `str` | **Secure** binary read as base64. Reads a file only if it is a regular file **inside the working directory**, no larger than `max_bytes`, and matches `type` (extension **and** magic bytes). `type` is one of `png`, `jpg`, `gif`, `webp`, `qoi`, `bmp`, `pdf`, `zip`, `xlsx`, `docx`, `mp3`, `wav`, `flac`, `ogg`, `mp4`, `webm`, `avi`, `csv`, `json`, `txt` — or `any` to skip the type check (dir-confinement and size still apply). Rejects path traversal, symlink escape, wrong-type files (e.g. a local DB asked for as `png`), and oversized files. Use to send a file over the network without the NUL-truncation of `read`. |
| `fs.write(path, data)` | `int` | Write file, return bytes written |
| `fs.append(path, data)` | `int` | Append to file |
| `fs.exists(path)` | `bool` | Check existence |
| `fs.delete(path)` | `bool` | Delete file |
| `fs.rename(src, dst)` | `bool` | Rename/move |
| `fs.copy(src, dst)` | `bool` | Copy file |
| `fs.size(path)` | `int` | File size in bytes (-1 if missing) |
| `fs.mkdir(path)` | `bool` | Create directory (including parents) |
| `fs.rmdir(path)` | `bool` | Remove empty directory |
| `fs.listdir(path)` | `dyn` | List filenames |
| `fs.isdir(path)` | `bool` | Is a directory? |
| `fs.isfile(path)` | `bool` | Is a regular file? |
| `fs.join(a, b)` | `str` | Join path components |
| `fs.basename(path)` | `str` | Filename part |
| `fs.dirname(path)` | `str` | Directory part |
| `fs.ext(path)` | `str` | Extension including dot |
| `fs.tempfile()` | `str` | Create and return path to temp file |

**Directory confinement across platforms.** `fs.read_base64` decides "inside the
working directory" from a canonical absolute path — `..` collapsed and every link
followed — so the check cannot be fooled by a path that merely looks contained.
POSIX gets that from `realpath()`. MinGW has no `realpath()`, and `_fullpath()`
is not a substitute (it collapses `..` only lexically and follows neither
symlinks nor NTFS junctions), so the Windows build asks the kernel for the file's
final name instead. Both paths therefore refuse the same escapes; on Windows the
comparison is additionally case-insensitive and accepts either separator, matching
how the filesystem behaves. See
[WINDOWS.md](WINDOWS.md#filesystem-path-resolution).

---

## std.libv — Vectors, Matrices, Tensors

Pure C99, zero deps. All storage is backed by `float arr` or `int arr`. Col-major storage.

```toml
[libs]
std.libv = "1.0"
```

### Initializers

| Expression | Size | Description |
|---|---|---|
| `libv.vec2` | 2 | 2D float vector, zeros |
| `libv.vec3` | 3 | 3D float vector, zeros |
| `libv.vec4` | 4 | 4D float vector, zeros |
| `libv.mat2` | 4 | 2×2 identity matrix |
| `libv.mat3` | 9 | 3×3 identity matrix |
| `libv.mat4` | 16 | 4×4 identity matrix |
| `libv.vec(n)` | n | N-vector, zeros |
| `libv.mat(r, c)` | r×c | r×c matrix, zeros |
| `libv.tens(d0, d1, ...)` | d0×d1×... | N-dimensional tensor, zeros |

### Vector operations (in-place unless scalar result)

| Function | Returns | Description |
|---|---|---|
| `libv.add(a, b)` | `nil` | a = a + b |
| `libv.sub(a, b)` | `nil` | a = a − b |
| `libv.scale(a, s)` | `nil` | a = a × scalar |
| `libv.normalize(a)` | `nil` | a = a / ‖a‖ |
| `libv.lerp(a, b, t)` | `nil` | a = mix(a, b, t) |
| `libv.cross(out, a, b)` | `nil` | out = a × b (vec3 only) |
| `libv.dot(a, b)` | `float` | dot product |
| `libv.norm(a)` | `float` | Euclidean length |
| `libv.angle(a, b)` | `float` | angle in radians |
| `libv.eq(a, b)` | `bool` | element-wise equality (ε = 1e-6) |
| `libv.shape(a)` | `int` | element count |
| `libv.fill(a, v)` | `nil` | set all elements to v |
| `libv.copy(dst, src)` | `nil` | copy src into dst |

### Matrix operations

| Function | Returns | Description |
|---|---|---|
| `libv.identity(m)` | `nil` | reset to identity |
| `libv.transpose(m)` | `nil` | in-place (square) |
| `libv.matmul(out, a, b)` | `nil` | out = a × b |
| `libv.det(m)` | `float` | determinant (2×2, 3×3, 4×4) |
| `libv.inverse(out, m)` | `nil` | out = m⁻¹ (2×2 only) |

### 3D transform helpers

| Function | Description |
|---|---|
| `libv.translate(m, tx, ty, tz)` | apply translation |
| `libv.rotate(m, angle_rad, ax, ay, az)` | rotate around axis |
| `libv.scale_mat(m, sx, sy, sz)` | apply scale |
| `libv.perspective(m, fov_rad, aspect, near, far)` | perspective projection |
| `libv.ortho(m, left, right, bottom, top, near, far)` | orthographic projection |
| `libv.lookat(m, eye, center, up)` | view matrix |

### Nearest-neighbor index — exact 14-d KNN (vector search)

A bundled k-nearest-neighbor index over a large reference set of pre-normalized
14-dimensional vectors, with **exact** Euclidean search. Built for
high-throughput classification (e.g. fraud scoring): the index is memory-mapped
read-only and shared across all worker threads, so a multi-worker server loads
it once and pays no per-request allocation.

The on-disk format is **VKN3**: every tree node carries a 14-d axis-aligned
bounding box (AABB) and the search is best-first with full box-distance pruning.
A split-plane bound prunes on a single dimension and collapses in 14-d, so
atypical ("off-manifold") queries used to scan most of the tree; the box bound
prunes across all 14 dimensions and visits children nearest-box-first, keeping
the result identical while cutting the worst case ~80× (typical query ~0.2 ms,
worst case ~0.3 ms over 3M references).

| Function | Returns | Description |
|---|---|---|
| `libv.kd_load()` | int | mmap + pre-touch the index. `0` = ok, `<0` = error. Path from `FLUXA_KD_INDEX` (default `kdtree.bin`). Call once at startup; only report `/ready` 2xx after it returns 0. |
| `libv.kd_ready()` | int | `1` if an index is loaded, else `0`. |
| `libv.kd_count(float arr q)` | int | Fraud-labeled points among the `k` nearest. `q` needs ≥14 dims. |
| `libv.kd_count(float arr q, int k)` | int | As above with explicit `k` (default 5). |
| `libv.kd_count(float arr q, int k, int budget)` | int | `budget` caps leaf visits; `<=0` = exact (or the env default below). |
| `libv.kd_score(float arr q[, int k[, int budget]])` | float | `kd_count / k` — score in `[0,1]`. |

**Building the index (offline).** `src/std/libv/build_index.c` reads a SoA
reference file (`int32 n`, `int32 dim=14`, then `n*14` floats, then `n` label
bytes) and writes the VKN3 tree:

```sh
gcc -O3 -ffast-math -Isrc/std/libv src/std/libv/build_index.c -o build_index -lm
./build_index refs.bin kdtree.bin     # ~262k nodes for 3M refs, ~103 MB
```

The format magic is `VKN3`; a stale `VKN2` index is rejected at load, so rebuild
after upgrading.

**`FLUXA_KD_BUDGET` (env, optional).** A deployment-level default leaf budget,
applied only when the script passes `budget<=0`. It caps the worst-case search
cost without changing the script. Leave it **unset** for exact search — VKN3 is
fast enough that the budget is no longer needed; set it only if you want a hard
ceiling at the cost of approximate results on pathological queries.

**Temporal helpers** (same module, used alongside the vectors):

| Function | Returns | Description |
|---|---|---|
| `libv.dow(int Y, int M, int D)` | int | Day of week, Mon=0 … Sun=6 (proleptic Gregorian). |
| `libv.daymin(int Y, int M, int D, int H, int Mi)` | int | Minutes since the civil epoch — subtract two results to get minutes between timestamps. |

---

## std.libdsp — DSP and Radar Math

Pure C99, zero deps. Requires `std.libv`. FFT: Cooley-Tukey in-place, power-of-2 only.

```toml
[libs]
std.libv   = "1.0"
std.libdsp = "1.0"
```

Interleaved complex layout: `[re0, im0, re1, im1, ...]`.

| Function | Returns | Description |
|---|---|---|
| `dsp.fft(signal)` | `nil` | Forward FFT in-place |
| `dsp.ifft(signal)` | `nil` | Inverse FFT in-place (normalized) |
| `dsp.zeros(signal)` | `nil` | Zero imaginary parts |
| `dsp.window(signal, type)` | `nil` | Apply window: `"hann"`, `"hamming"`, `"blackman"`, `"rect"` |
| `dsp.power(psd, signal)` | `nil` | Power spectrum |
| `dsp.magnitude(mag, signal)` | `nil` | Magnitude |
| `dsp.phase(ph, signal)` | `nil` | Phase |
| `dsp.fir(signal, h)` | `nil` | FIR filter |
| `dsp.iir(signal, b, a)` | `nil` | IIR filter direct form II |
| `dsp.matched_filter(signal, tmpl)` | `nil` | Cross-correlation with template |
| `dsp.stft(out, signal, win_size, hop)` | `nil` | Short-time Fourier Transform |
| `dsp.range_doppler(rd, signal, nrng, ndop)` | `nil` | 2D FFT range-Doppler map |
| `dsp.cfar(det, rd, guard, ref, threshold)` | `nil` | Cell-Averaging CFAR |
| `dsp.peak(signal)` | `int` | Index of maximum magnitude bin |
| `dsp.snr(signal, noise_floor)` | `float` | SNR in dB |
| `dsp.normalize(signal)` | `nil` | Scale so max absolute value = 1.0 |

---

## std.wserver — Resilient HTTP Server

Thread-pool HTTP server via libmicrohttpd. Designed for `std.flxthread` worker pattern.

**Dual-backend:** libmicrohttpd when available via `pkg-config`, stub with clear error otherwise.

Dep: `apt install libmicrohttpd-dev`

```toml
[libs]
std.wserver   = "1.0"
std.flxthread = "1.0"

[libs.wserver]
max_servers       = 4      # hard cap 32
max_requests      = 128    # hard cap 4096
max_body_bytes    = 65536  # hard cap 16MB
max_header_pairs  = 16     # hard cap 128
max_header_bytes  = 4096   # hard cap 65536
queue_depth       = 256    # hard cap 4096
workers           = 4      # manual mode (serve with false): MHD epoll thread-pool size
# Auto-scaling pool (serve with true):
min_threads       = 2      # hard cap 64
max_threads       = 16     # hard cap 256
scale_up_queue    = 4      # queue depth to trigger scale-up
scale_down_idle   = 10     # idle seconds to trigger scale-down
```

### Functions

| Function | Returns | Description |
|---|---|---|
| `wserver.serve(int port)` | int | Start server, manual mode. Returns handle > 0. |
| `wserver.serve(int port, bool auto)` | int | Start server. `auto=true` enables auto-scaling MHD thread pool. |
| `wserver.accept(int server, int timeout_ms)` | int | Wait for next request. Returns handle > 0, or 0 on timeout. Thread-safe. |
| `wserver.req_method(int req)` | str | `"GET"`, `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"`, `"HEAD"`, `"OPTIONS"` |
| `wserver.req_path(int req)` | str | Request URI path |
| `wserver.req_body(int req)` | str | Request body (POST/PUT/PATCH) |
| `wserver.req_header(int req, str name)` | str | Header value, `""` if absent |
| `wserver.reply(int req, int status, str body)` | nil | Send response. Invalidates req handle. |
| `wserver.reply_json(int req, int status, str json)` | nil | Send JSON (`Content-Type: application/json`). |
| `wserver.reply_headers(int req, int status, str body, str arr headers, int n)` | nil | Send with custom headers. `headers` is flat key/value pairs. `n` = pair count. |
| `wserver.connections(int server)` | int | Active connection count |
| `wserver.stop(int server)` | nil | Stop server. No-op on invalid handle. |
| `wserver.version()` | str | `"libmicrohttpd/x.x.x"` |

**Return value convention:** `serve` and `accept` return an `int` > 0 on success, 0 on failure/timeout. Always check inside `danger {}`.

**TCP_NODELAY.** Every accepted connection has Nagle disabled (via
`MHD_OPTION_NOTIFY_CONNECTION`). Without it, MHD's separate header/body writes can
stall on the peer's delayed-ACK timer — a low-median / ~40–100 ms-tail profile
that only appears over a real (bridged) network behind a reverse proxy, never on
loopback. On startup the server logs one self-verifying line so you can confirm
the running build inside a container (`docker logs … | grep wserver`):

```
[wserver] MHD started: thread_pool=<N>, TCP_NODELAY=on (build OK)
```

**`workers` (manual mode).** Sets the MHD epoll thread-pool size when you call
`serve(port, false)`. On a fractional-CPU deployment this is a real tail-latency
lever: too many runnable threads on a sub-core budget burn the CFS quota on
context switches and produce periodic throttle stalls (p99 jumps to roughly the
CFS period). A small pool — 2 to 4 — keeps p99 low; the default is 4.

### Manual mode — user controls workers

Worker functions own the request loop. Spawn one or more via `ft.new("name", "fn_name", arg)`. In long-running workers, release per-request heap strings explicitly so memory stays bounded indefinitely — see [Memory Ownership Model](#memory-ownership-model).

```fluxa
import std wserver
import std flxthread as ft

fn worker(int srv) nil {
    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            str method = wserver.req_method(req)
            str path   = wserver.req_path(req)

            if method == "GET" {
                wserver.reply(req, 200, "ok")
            }
            if method == "POST" {
                str body = wserver.req_body(req)
                wserver.reply(req, 201, body)
                free(body)                  // explicit: long-lived loop
            }
            if method == "DELETE" {
                wserver.reply(req, 204, "")
            }
            free(method); free(path)        // explicit: long-lived loop
        }
    }
}

int srv = 0
danger { srv = wserver.serve(8080, false) }
ft.new("w1", "worker", srv)
ft.new("w2", "worker", srv)
ft.new("w3", "worker", srv)
ft.new("w4", "worker", srv)
wserver.wait(srv)
wserver.stop(srv)
```

**Why `free` here.** The runtime auto-releases heap strings on slot reassignment and at scope end. In a worker loop that never returns, each `str method = wserver.req_method(req)` reassigns the slot, which IS auto-released. But intermediate strings built inside the request handler (`body`, response builders, JSON pieces) accumulate in the worker's frame until the function returns — which it never does. Calling `free()` after each use keeps the worker at constant memory regardless of uptime.

### Auto-scaling mode — lib manages MHD thread pool

With `auto=true`, the lib maintains a pool of MHD threads internally. The pool grows when the request queue fills and shrinks when threads are idle. The Fluxa accept/reply loop is identical in both modes.

```fluxa
import std wserver
import std flxthread as ft

fn worker_loop(int srv) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            if req != 0 {
                str method = wserver.req_method(req)
                if method == "GET"  { wserver.reply(req, 200, "ok") }
                if method == "POST" { wserver.reply(req, 201, wserver.req_body(req)) }
            }
        }
    }
}

int srv = 0
danger { srv = wserver.serve(8080, true) }

Block w1 typeof Worker
ft.new("t1", w1, "run")
ft.resolve_all()
wserver.stop(srv)
```

### reply_headers example

```fluxa
str arr headers[4] = ["Content-Type", "text/html", "X-Frame-Options", "DENY"]
wserver.reply_headers(req, 200, "<h1>ok</h1>", headers, 2)
```

### Comparison with std.http

| | std.http (mongoose) | std.wserver (libmicrohttpd) |
|---|---|---|
| Backend | mongoose 7.21 (vendored) | libmicrohttpd (system) |
| Handle type | dyn cursor | int opaque handle |
| Connection model | Single-threaded poll | Thread-per-connection pool |
| Best for | Embedded, low connection count | Server workloads, high concurrency |
| Auto-scaling | No | Yes (`serve(port, true)`) |
| TLS | ✅ via mongoose | ✅ via GnuTLS backend |
| Dep | None (vendored) | `libmicrohttpd-dev` |

---

## std.pg — PostgreSQL Client

PostgreSQL client via libpq. Parameterized queries prevent SQL injection.

**Dual-backend:** libpq when available via `pkg-config`, stub with clear error otherwise.

Dep: `apt install libpq-dev`

```toml
[libs]
std.pg = "1.0"

[libs.pg]
max_connections = 16     # hard cap 256
max_results     = 64     # hard cap 1024
max_cell_bytes  = 4096   # hard cap 65536
max_param_bytes = 1024   # hard cap 65536
max_params      = 16     # hard cap 64
```

### Functions

| Function | Returns | Description |
|---|---|---|
| `pg.connect(str connstr)` | int | Connect. Returns handle > 0, or 0 on failure. |
| `pg.close(int conn)` | nil | Close connection. No-op on invalid handle. |
| `pg.exec(int conn, str sql)` | nil | DDL or DML |
| `pg.query(int conn, str sql)` | int | SELECT. Returns result handle > 0, or 0 on failure. |
| `pg.query_params(int conn, str sql, str arr params, int n)` | int | Parameterized query. `params` is a str arr, `n` is how many elements to use. |
| `pg.rows(int result)` | int | Row count |
| `pg.cols(int result)` | int | Column count |
| `pg.col_name(int result, int col)` | str | Column name (0-indexed) |
| `pg.get(int result, int row, int col)` | str | Value as string. `""` if NULL. |
| `pg.get_int(int result, int row, int col)` | int | Value as int. `0` if NULL. |
| `pg.get_float(int result, int row, int col)` | float | Value as float. `0.0` if NULL. |
| `pg.get_bool(int result, int row, int col)` | bool | Value as bool. |
| `pg.is_null(int result, int row, int col)` | bool | True if cell is NULL. |
| `pg.free_result(int result)` | nil | Release result. No-op on invalid handle. |
| `pg.last_error(int conn)` | str | Last error from libpq. |
| `pg.ping(str connstr)` | bool | Check server reachable (no full connect). |
| `pg.version(int conn)` | str | Server version string. |
| `pg.version()` | str | libpq version (no conn needed). |

**Return value convention:** `connect`, `query`, `query_params` return `int` > 0 on success, 0 on error. Always check inside `danger {}`.

### Connection string format

```
"host=localhost port=5432 dbname=mydb user=fluxa password=secret connect_timeout=5"
"postgresql://fluxa:secret@localhost/mydb"
```

### Example — basic query

```fluxa
import std pg

int db = 0
danger { db = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t") }

danger {
    pg.exec(db, "CREATE TABLE IF NOT EXISTS readings (ts BIGINT, val REAL)")
    pg.exec(db, "INSERT INTO readings VALUES (1700000000, 23.5)")

    int res = pg.query(db, "SELECT ts, val FROM readings ORDER BY ts DESC LIMIT 5")
    int i = 0
    while i < pg.rows(res) {
        int   ts  = pg.get_int(res, i, 0)
        float val = pg.get_float(res, i, 1)
        print(ts)
        print(val)
        i = i + 1
    }
    pg.free_result(res)
    pg.close(db)
}
```

### Example — parameterized query (SQL injection safe)

```fluxa
import std pg

int db = 0
danger { db = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t") }

danger {
    str arr params[2] = ["sensor-01", "23.5"]
    int res = pg.query_params(db,
        "SELECT ts FROM readings WHERE sensor_id = $1 AND val > $2::real",
        params, 2)
    int n = pg.rows(res)
    print(n)
    pg.free_result(res)
    pg.close(db)
}
```

### Example — multi-threaded poll loop

IO stays in functions with `danger`. Each worker holds its own connection. `free()` after the consumer releases the result string so memory stays flat over millions of iterations.

```fluxa
import std pg
import std flxthread as ft

fn fetch(int conn, str sql) str {
    str val = ""
    danger {
        int res = pg.query(conn, sql)
        if pg.rows(res) > 0 { val = pg.get(res, 0, 0) }
        pg.free_result(res)
    }
    return val
}

fn poller(int conn) nil {
    while !ft.should_stop() {
        str v = fetch(conn, "SELECT val FROM readings ORDER BY ts DESC LIMIT 1")
        // ...record v into shared state via ft.message or a sharded cache...
        free(v)
    }
}

int c1 = 0
int c2 = 0
danger {
    c1 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
    c2 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
}
ft.new("p1", "poller", c1)
ft.new("p2", "poller", c2)
ft.resolve_all()
pg.close(c1); pg.close(c2)
```

### Example — HTTP + DB worker (the production pattern)

This is the canonical pattern for HTTP services backed by Postgres. Each worker has its own connection, accepts requests with `wserver.accept`, runs parameterized queries with `pg.query_params`, and releases per-request strings explicitly. Memory stays bounded at runtime — typically tens of MB regardless of throughput.

```fluxa
import std wserver
import std pg
import std strings
import std flxthread as ft

fn worker(int srv) nil {
    str dsn = "host=postgres port=5432 dbname=app user=app password=secret connect_timeout=1 sslmode=disable"
    int conn = 0
    danger { conn = pg.connect(dsn) }
    if conn == 0 { print("worker: pg connect failed"); return }

    while !ft.should_stop() {
        int req = 0
        danger { req = wserver.accept(srv, 100) }
        if req != 0 {
            str method = wserver.req_method(req)
            str path   = wserver.req_path(req)

            if method == "GET" {
                bool is_user = strings.starts_with(path, "/users/")
                if is_user {
                    str id = strings.slice(path, 7, len(path))
                    str out_uid = ""
                    str out_nm  = ""
                    int out_rows = -1
                    danger {
                        str arr params[1] = [id]
                        int res = pg.query_params(conn,
                            "SELECT id::text, name FROM users WHERE id = $1::uuid",
                            params, 1)
                        out_rows = pg.rows(res)
                        if out_rows > 0 {
                            out_uid = pg.get(res, 0, 0)
                            out_nm  = pg.get(res, 0, 1)
                        }
                        pg.free_result(res)
                    }
                    if out_rows > 0 {
                        str p1 = strings.concat("{\"id\":\"", out_uid)
                        str p2 = strings.concat(p1, "\",\"name\":\"")
                        str p3 = strings.concat(p2, out_nm)
                        str row = strings.concat(p3, "\"}")
                        wserver.reply_json(req, 200, row)
                        free(p1); free(p2); free(p3); free(row)
                    }
                    if out_rows == 0 {
                        wserver.reply_json(req, 404, "{\"error\":\"not found\"}")
                    }
                    free(out_uid); free(out_nm); free(id)
                }
                if !is_user {
                    wserver.reply_json(req, 404, "{\"error\":\"not found\"}")
                }
            }
            free(method); free(path)
        }
    }

    pg.close(conn)
}

int srv = 0
danger { srv = wserver.serve(8080, false) }
ft.new("w01", "worker", srv); ft.new("w02", "worker", srv)
ft.new("w03", "worker", srv); ft.new("w04", "worker", srv)
ft.new("w05", "worker", srv); ft.new("w06", "worker", srv)
ft.new("w07", "worker", srv); ft.new("w08", "worker", srv)
wserver.wait(srv)
wserver.stop(srv)
```

**Memory profile under load:** baseline (workers idle) ~12 MB, under 1k+ req/s ~30–40 MB, falls back to ~28 MB when traffic stops. Stable across hours of operation.

### Notes

- `pg.free_result` must be called after every `pg.query` / `pg.query_params`.
- `pg.close` and `pg.free_result` are no-ops on invalid or already-freed handles — safe to call unconditionally.
- `pg.ping` uses `PQping` — does not open a full connection.
- `query_params` validates that `n ≤ arr.size` and each element is str before touching libpq.
- Out-of-bounds row/col in `get*` and `is_null` produces a runtime error with line number.
- Integration tests (real PostgreSQL in Docker): `bash tests/integration/pg/run.sh`

---

## std.sound — Audio Playback and Tones

Audio playback (wav/mp3/flac) and sine tone generation.

**Dual-backend:** stub (default, zero deps, no audio device — tracks
engine/sound state so program logic is fully testable headless) or
miniaudio (`make FLUXA_SOUND_MINIAUDIO=1`, requires `vendor/miniaudio.h`
from https://github.com/mackron/miniaudio). miniaudio resolves the OS
audio subsystem at runtime: ALSA/PulseAudio/JACK (Linux), WASAPI
(Windows), CoreAudio (macOS), sndio (BSD), AAudio/OpenSL (Android).
Not for bare-metal targets (RP2040/ESP32) — there set `std.sound = false`
in `fluxa.libs` (zero code size) or use the stub.

```toml
[libs]
std.sound = "1.0"
```

**Design:** opaque `int` handles (same pattern as `std.wserver`) — no
`dyn` cursors, so Block methods can receive engine/sound handles as
plain `int` parameters. Limits: 4 engines, 64 sounds per engine.

Use plain `int eng` for the engine handle — **not `prst int`** (like
wserver, the runtime would attempt to restore a dead OS handle after
restart). All `load` calls (file IO) belong inside `danger {}`.

| Function | Returns | Description |
|---|---|---|
| `sound.init()` | `int` | Create engine, returns handle |
| `sound.close(eng)` | `nil` | Close engine, frees all its sounds |
| `sound.load(eng, path)` | `int` | Load wav/mp3/flac, returns sound handle |
| `sound.unload(eng, h)` | `nil` | Free a loaded sound |
| `sound.play(eng, h)` | `bool` | Play from the start |
| `sound.stop(eng, h)` | `nil` | Stop and rewind |
| `sound.pause(eng, h)` | `nil` | Stop, keep position |
| `sound.resume(eng, h)` | `nil` | Continue from paused position |
| `sound.is_playing(eng, h)` | `bool` | Playback state |
| `sound.volume(eng, h, v)` | `nil` | Per-sound volume, `float`\|`int` 0.0–1.0 |
| `sound.tone(eng, freq_hz, ms)` | `bool` | Sine beep, 1–20000 Hz, ≤10000 ms |
| `sound.version()` | `str` | Backend version |

**Backend differences:** `tone()` blocks for the duration on miniaudio,
returns immediately on the stub. Stub `load()` only checks the file is
readable (no decode); stub `is_playing` reflects the play/stop/pause
state machine, not real playback position.

```fluxa
import std sound
import std time

int eng = sound.init()

danger {
    int alarm = sound.load(eng, "alarm.wav")
    sound.volume(eng, alarm, 0.8)
    sound.play(eng, alarm)

    while sound.is_playing(eng, alarm) {
        time.sleep(50)
    }

    sound.tone(eng, 880, 200)     // confirmation beep
    sound.unload(eng, alarm)
}
if err != nil {
    print(err)
}

sound.close(eng)
```

---

## Buffer Configuration Reference

```toml
[libs.csv]
max_line_bytes = 1024
max_fields     = 64

[libs.json]
max_str_bytes  = 4096

[libs.pg]
max_connections = 16
max_results     = 64
max_cell_bytes  = 4096
max_param_bytes = 1024
max_params      = 16

[libs.wserver]
max_servers       = 4
max_requests      = 128
max_body_bytes    = 65536
max_header_pairs  = 16
max_header_bytes  = 4096
queue_depth       = 256
min_threads       = 2
max_threads       = 16
scale_up_queue    = 4
scale_down_idle   = 10

[ffi]
str_buf_size = 1024
```

**RP2040 recommended (264 KB SRAM):**
```toml
[libs.csv]
max_line_bytes = 256
max_fields     = 16

[libs.json]
max_str_bytes  = 512

[libs.pg]
max_connections = 2
max_results     = 8
max_cell_bytes  = 512
max_params      = 4

[ffi]
str_buf_size = 64
```

---

## What Is Not Supported (v1.0)

**std.csv:**
- Multiline fields (quoted fields spanning multiple lines)
- Automatic type detection (all fields are `str`)
- Encoding beyond ASCII/UTF-8 passthrough

**std.strings:**
- Unicode-aware indexing (all operations work on bytes, not codepoints)
- Regex pattern matching (coming in `std.regex`)
- `trim_left` / `trim_right`

**std.json:**
- JSON path expressions (`json.get("a.b.c")`) — use `std.json2`
- Number formatting control (uses `%g`)
- Unicode escape sequences (`\uXXXX`)
- In-place mutation (always returns new `str`)

**std.pg:**
- Prepared statements (use `query_params` instead)
- Async queries
- COPY protocol

**std.cache:**
- Eviction policy. When all 8 probe slots in a shard are taken, `put` is a silent no-op. Keep working set well below 1024.
- TTL. Entries live until `del`, `clear`, or process exit.
- Iteration. No `cache.keys()` or `cache.foreach` — by design; iteration on a sharded table during concurrent writes is unsafe.
- Persistence across reload. The cache is process-local and rebuilt on every restart. Use `prst dyn` cursors for state that must survive reloads.
- `free()` on arena strings. Arena strings are not `malloc`'d directly — only `arena_reset` / `arena_drop` releases them.

**std.wserver:**
- WebSocket upgrade (use `std.websocket` for WebSocket)
- HTTP/2

These limitations are intentional — predictable memory use on embedded hardware, not feature parity with desktop libraries.
