# Fluxa Standard Library
**v0.10 | Sprint 10.c**

Reference documentation for all stdlib libs implemented: `std.math`, `std.csv`, `std.json`, `std.strings`, `std.time`, `std.flxthread`.

---

## Design Principles

All stdlib libs share the same design contract:

**Opt-in by declaration.** A lib only exists at runtime if it is declared in `[libs]` of `fluxa.toml`. Without declaration, `import std <lib>` produces a clear error. No lib adds any overhead to programs that don't use it.

**No `danger` required (unlike `import c`).** Stdlib functions are written in safe C and vetted for embedded use. File I/O functions are the exception — they require `danger {}` because file operations can fail. Pure computation functions (math, field parsing, JSON extraction) work outside `danger`.

**Errors follow the standard model.** Outside `danger`: runtime error with line number, execution aborts. Inside `danger`: error captured in `err_stack`, execution continues. No special error handling API.

**Buffers are bounded.** Every lib that touches external data has configurable buffer limits in `fluxa.toml`. No silent truncation — exceeding a limit produces a clear error.

**All data is `str` or `dyn` of `str`.** No intermediate parse trees, no hidden heap allocations, no complex ownership chains. JSON and CSV data flows through the runtime as plain strings.

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

The `import std <lib>` statement validates at runtime that the lib was declared in `[libs]`. It does not load anything — registration is implicit through the config flags. The statement is idiomatic documentation: it makes it clear to any reader which libs a file depends on.

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

### Error handling

Domain errors produce a runtime error at the offending line:

```fluxa
// outside danger — aborts
float bad = math.sqrt(-1.0)
// [fluxa] Runtime error (line 2): math.sqrt (line 2): sqrt of negative number

// inside danger — captured in err_stack
danger {
    float bad = math.sqrt(-1.0)
}
// execution continues, err[0] has the message
```

### Approximate equality

```fluxa
bool ok = math.approx(0.1 + 0.2, 0.3)         // true (default epsilon 1e-9)
bool ok = math.approx(1.0, 1.001, 0.01)        // true (custom epsilon)
bool ok = math.approx(1.0, 2.0)                // false
```

| Function | Arguments | Returns | Notes |
|---|---|---|---|
| `math.approx(a, b)` | float or int | bool | `\|a - b\| < 1e-9` |
| `math.approx(a, b, epsilon)` | float or int | bool | `\|a - b\| < epsilon`. Error if epsilon < 0 |

Use this instead of `==` for all float comparisons. `0.1 + 0.2 == 0.3` is false in floating point arithmetic on every platform.

### Example

```fluxa
import std math

// PID output calculation
float kp     = 2.5
float signal = 4.0
float output = math.clamp(kp * signal, -100.0, 100.0)
print(output)  // 10.0 (within clamp range)

// Angle conversion
float angle_deg = 45.0
float angle_rad = math.deg_to_rad(angle_deg)
float sine      = math.sin(angle_rad)
print(sine)    // ~0.707
```

---

## std.csv

CSV file processing. Three usage modes for different memory profiles. All file I/O requires `danger {}`. Pure string operations (`csv.field`, `csv.field_count`) work outside `danger`.

**Enable:**
```toml
[libs]
std.csv = "1.0"

[libs.csv]
max_line_bytes = 1024   # max bytes per line (default 1024)
max_fields     = 64     # max fields for csv.field (default 64)
```

### Data model

All functions return `dyn` of `str` — each element is one raw CSV line as a string. Fields are extracted with `csv.field(row, idx)`. Lines are returned as-is (no unquoting in v1.0).

```fluxa
dyn rows   = csv.load("data.csv")
str row    = rows[0]                    // "sensor_id,temp,humidity"
str field0 = csv.field(row, 0)         // "sensor_id"
str field1 = csv.field(row, 1)         // "temp"
int n      = csv.field_count(row)      // 3
```

### Mode A — Cursor (recommended for large files)

The cursor is a `VAL_PTR` wrapping a `FILE*`. It survives hot reload as `prst dyn`. In `HANDOVER_MODE_FLASH` (RP2040 reboot) the file pointer is invalid after restart — close and reopen after reload.

```fluxa
import std csv

prst dyn cursor = csv.open("data.csv")

danger {
    dyn chunk = csv.next(cursor, 1000)
    while len(chunk) > 0 {
        dyn data = csv.skip(chunk, 1)   // skip header on first chunk
        for row in data {
            str id   = csv.field(row, 0)
            str temp = csv.field(row, 1)
        }
        chunk = csv.next(cursor, 1000)
    }
    csv.close(cursor)
}
```

### Mode B — Chunk direct (small to medium files)

Reopens the file on each call. Simple, predictable, O(n) per call. Best for files where you process one chunk at a time and don't need to resume.

```fluxa
import std csv

danger {
    dyn chunk = csv.chunk("data.csv", 500)
    for row in chunk {
        str temp = csv.field(row, 2)
    }
}
```

### Mode C — Load all (files that fit in memory)

```fluxa
import std csv

danger {
    dyn all  = csv.load("config.csv")
    dyn data = csv.skip(all, 1)        // skip header
    print(len(data))
}
```

### Field parsing — FSM with quoted field support

`csv.field` uses a finite state machine that correctly handles:
- Fields containing the delimiter inside double quotes: `"hello, world"`
- Escaped quotes inside quoted fields: `"say ""hello"""`
- Custom delimiters via the optional third argument

```fluxa
// Standard comma-separated
str f = csv.field("a,b,c", 1)                  // "b"

// TSV (tab-separated)
str f = csv.field("a	b	c", 1, "	")          // "b"

// Semicolon (European Excel)
str f = csv.field("a;b;c", 1, ";")             // "b"

// Quoted field with embedded comma
str f = csv.field('a,"hello, world",c', 1)     // "hello, world"
```

### Function reference

**File operations (require `danger {}`):**

| Function | Returns | Description |
|---|---|---|
| `csv.open(str path)` | dyn cursor | Open file with default delimiter (`,`). Keep as `prst dyn`. |
| `csv.open(str path, str delim)` | dyn cursor | Open file with custom delimiter (`"\t"`, `";"`, etc.). |
| `csv.next(dyn cursor, int n)` | dyn | Read next n lines. Empty dyn = EOF. |
| `csv.close(dyn cursor)` | nil | Close file and free cursor memory. |
| `csv.chunk(str path, int n)` | dyn | Reopen file, read n lines from start. |
| `csv.chunk(str path, int n, int offset)` | dyn | Read n lines starting at byte offset. |
| `csv.load(str path)` | dyn | Load entire file as dyn of str. |
| `csv.save(dyn data, str path)` | nil | Write each element as a line. |

**String operations (no `danger` needed):**

| Function | Returns | Description |
|---|---|---|
| `csv.field(str row, int idx)` | str | Extract field at index (0-based). |
| `csv.field(str row, int idx, str delim)` | str | Extract with custom delimiter. |
| `csv.field_count(str row)` | int | Count fields in row. |
| `csv.field_count(str row, str delim)` | int | Count with custom delimiter. |
| `csv.skip(dyn chunk, int n)` | dyn | Return chunk without first n rows. |
| `csv.is_eof(dyn cursor)` | bool | True if cursor reached end of file. |

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

danger {
    dyn chunk = csv.next(cur, 100)
    dyn data  = csv.skip(chunk, 1)     // skip header
    for row in data {
        str raw_temp = csv.field(row, 1)
        // convert str to float via math — not shown, use prst accumulation
        log.readings = log.readings + 1
    }
}
```

---

## std.json

JSON as strings — no parse tree, no intermediate data structures. Build JSON objects with `json.set()`. Extract values with `json.get_*()`. Entire JSON lives as a Fluxa `str`.

**Enable:**
```toml
[libs]
std.json = "1.0"

[libs.json]
max_str_bytes = 4096    # max JSON string size (default 4096)
```

### Data model

JSON is always `str`. The only compound structure Fluxa exposes from JSON is `dyn` of `str` (from `json.parse_array`), where each element is one JSON object or value as a string.

This means:
- Flat JSON objects: use `json.get_*` directly on the string
- JSON arrays: use `json.parse_array` to get a `dyn` of `str`, then extract from each element
- Nested JSON: extract the nested object as a `str`, then `json.get_*` on that string

### Building JSON objects

```fluxa
import std json

str obj = json.object()                        // "{}"
obj = json.set(obj, "sensor_id", json.from_str("s001"))
obj = json.set(obj, "temp",      json.from_float(23.5))
obj = json.set(obj, "active",    json.from_bool(true))
obj = json.set(obj, "count",     json.from_int(42))
// {"sensor_id":"s001","temp":23.5,"active":true,"count":42}
print(obj)
```

### Extracting from JSON strings

```fluxa
import std json

str raw = "{\"temp\":23.5,\"unit\":\"celsius\",\"active\":true}"

float temp   = json.get_float(raw, "temp")    // 23.5
str   unit   = json.get_str(raw,   "unit")    // "celsius"
bool  active = json.get_bool(raw,  "active")  // true
bool  exists = json.has(raw, "temp")          // true
bool  valid  = json.valid(raw)                // true
```

### JSON arrays

```fluxa
import std json

str raw   = "[{\"id\":1},{\"id\":2},{\"id\":3}]"
dyn items = json.parse_array(raw)       // dyn of 3 str elements
print(len(items))                       // 3

int first_id = json.get_int(items[0], "id")   // 1
int last_id  = json.get_int(items[2], "id")   // 3
```

### Serializing dyn to JSON array

```fluxa
import std json

str a = json.from_int(1)
str b = json.from_str("hello")
str c = json.from_bool(true)
dyn d = [a, b, c]
str out = json.stringify(d)             // [1,"hello",true]
```

### File operations (require `danger {}`)

#### Mode A — cursor (large JSON array files)

```fluxa
import std json

prst dyn cur = json.open("readings.json")

danger {
    dyn chunk = json.next(cur, 200)     // 200 JSON objects per chunk
    while len(chunk) > 0 {
        for item in chunk {
            float t = json.get_float(item, "temp")
        }
        chunk = json.next(cur, 200)
    }
    json.close(cur)
}
```

#### Mode B/C — load

```fluxa
import std json

danger {
    str raw   = json.load("config.json")
    float kp  = json.get_float(raw, "kp")
    float ki  = json.get_float(raw, "ki")
}
```

### Function reference

**Object construction:**

| Function | Returns | Description |
|---|---|---|
| `json.object()` | str | Returns `"{}"` |
| `json.array()` | str | Returns `"[]"` |
| `json.set(str obj, str key, str val)` | str | Add or replace key. val must be a valid JSON value string. |

**Type conversion (Fluxa → JSON string):**

| Function | Returns | Description |
|---|---|---|
| `json.from_str(str s)` | str | `"hello"` → `"\"hello\""` |
| `json.from_float(float f)` | str | `23.5` → `"23.5"` |
| `json.from_int(int n)` | str | `42` → `"42"` |
| `json.from_bool(bool b)` | str | `true` → `"true"` |

**Extraction (JSON string → Fluxa value):**

| Function | Returns | Description |
|---|---|---|
| `json.get_str(str json, str key)` | str | Extract string field |
| `json.get_float(str json, str key)` | float | Extract number as float |
| `json.get_int(str json, str key)` | int | Extract number as int |
| `json.get_bool(str json, str key)` | bool | Extract boolean |
| `json.has(str json, str key)` | bool | True if key exists |

**Parse and serialize:**

| Function | Returns | Description |
|---|---|---|
| `json.parse_array(str raw)` | dyn | JSON array → dyn of str elements |
| `json.stringify(dyn data)` | str | dyn of str → JSON array string |
| `json.valid(str raw)` | bool | Quick structural validation |

**File operations (require `danger {}`):**

| Function | Returns | Description |
|---|---|---|
| `json.open(str path)` | dyn cursor | Open file, return cursor |
| `json.next(dyn cursor, int n)` | dyn | Read next n JSON objects. Empty = EOF. |
| `json.close(dyn cursor)` | nil | Close file and free cursor |
| `json.load(str path)` | str | Load entire file as one str |
| `json.is_eof(dyn cursor)` | bool | True if cursor reached EOF |

### MQTT telemetry pattern

```fluxa
import std json

Block Sensor {
    prst float temp     = 0.0
    prst float humidity = 0.0
    prst int   tick     = 0
    fn read(float t, float h) nil {
        temp     = t
        humidity = h
        tick     = tick + 1
    }
    fn payload() str {
        str obj = json.object()
        obj = json.set(obj, "temp",     json.from_float(temp))
        obj = json.set(obj, "humidity", json.from_float(humidity))
        obj = json.set(obj, "tick",     json.from_int(tick))
        return obj
    }
}

Block s typeof Sensor
s.read(23.5, 60.0)
str msg = s.payload()
// {"temp":23.5,"humidity":60,"tick":1}
print(msg)
```

---

## std.strings

String manipulation functions. No `danger` required — all operations are pure computation. No regex, no Unicode-aware indexing — all operations work on byte offsets.

**Enable:**
```toml
[libs]
std.strings = "1.0"

[libs.strings]
max_out_bytes = 8192    # max output string size (default 8192)
```

**Note:** `import std strings` uses `str` as the namespace prefix — `strings.split(...)`, `strings.upper(...)` etc. `str` is a built-in type keyword in Fluxa, but it is also valid as a namespace prefix in expressions.

### Function reference

| Function | Returns | Description |
|---|---|---|
| `strings.split(str s, str delim)` | dyn | Split `s` on `delim`. Returns dyn of str. Empty delim splits into individual bytes. |
| `strings.join(dyn parts, str glue)` | str | Join elements of dyn with `glue` between each. |
| `strings.concat(a, b, ...)` | str | Concatenate any number of values (int, float, bool, str) into one string. |
| `strings.slice(str s, int start, int end)` | str | Byte substring. Negative indices count from end. `end` is exclusive. |
| `strings.trim(str s)` | str | Remove leading and trailing whitespace. |
| `strings.find(str s, str sub)` | int | Byte offset of first occurrence of `sub`, or `-1` if not found. |
| `strings.replace(str s, str old, str new)` | str | Replace all occurrences of `old` with `new`. |
| `strings.starts_with(str s, str prefix)` | bool | True if `s` begins with `prefix`. |
| `strings.ends_with(str s, str suffix)` | bool | True if `s` ends with `suffix`. |
| `strings.contains(str s, str sub)` | bool | True if `sub` appears anywhere in `s`. |
| `strings.count(str s, str sub)` | int | Count non-overlapping occurrences of `sub` in `s`. |
| `strings.lower(str s)` | str | ASCII lowercase. |
| `strings.upper(str s)` | str | ASCII uppercase. |
| `strings.repeat(str s, int n)` | str | Repeat `s` `n` times. Returns `""` if `n <= 0`. |
| `strings.from_int(int n)` | str | Convert int or float to its string representation. |
| `strings.to_int(str s)` | int | Parse str as integer (via `atol`). Returns 0 if not parseable. |

### Examples

```fluxa
import std strings

// Parsing a sensor reading string
str raw    = "  sensor_01: 23.5 degC  "
str clean  = strings.trim(raw)                       // "sensor_01: 23.5 degC"
dyn parts  = strings.split(clean, ": ")             // ["sensor_01", "23.5 degC"]
str id     = parts[0]                            // "sensor_01"
str val    = strings.slice(parts[1], 0, 4)          // "23.5"

// Building a log line
str prefix = strings.upper("warn")                   // "WARN"
bool ok    = strings.starts_with(id, "sensor")      // true
int  n     = strings.count(clean, " ")              // 3

// CSV field splitting
dyn row    = strings.split("alice,30,engineer", ",")
str name   = row[0]   // "alice"
str age    = row[1]   // "30"
```

### Combining with std.csv and std.json

```fluxa
import std csv
import std strings

danger {
    dyn rows = csv.load("data.csv")
    dyn data = csv.skip(rows, 1)
    for row in data {
        str id    = csv.field(row, 0)
        str clean = strings.trim(id)
        if strings.starts_with(clean, "sensor") {
            str upper_id = strings.upper(clean)
        }
    }
}
```


---

## std.time

Time functions. No `danger` required. Platform-aware: uses `clock_gettime`/`nanosleep` on Linux/macOS, native hardware timers on RP2040 and ESP32.

**Enable:**
```toml
[libs]
std.time = "1.0"
```

### Function reference

| Function | Returns | Description |
|---|---|---|
| `time.sleep(int ms)` | nil | Block current thread for N milliseconds |
| `time.sleep_us(int us)` | nil | Block current thread for N microseconds |
| `time.now_ms()` | int | Monotonic timestamp in milliseconds |
| `time.now_us()` | int | Monotonic timestamp in microseconds |
| `time.ticks()` | int | Raw hardware tick counter (platform-native resolution) |
| `time.elapsed_ms(int since)` | int | Milliseconds since a prior `now_ms()` call. Safe against wraparound. |
| `time.timeout(int start, int max_ms)` | bool | True if at least `max_ms` have passed since `start` |
| `time.format(int ms)` | str | Human-readable UTC datetime: `"2025-01-15 14:32:01.123"`. On embedded targets without RTC: elapsed time `"00:01:23.456"` |

### The three loop patterns

`std.time` is designed around three documented loop patterns that also determine how `std.flxthread` mailbox drain interacts with the thread:

```fluxa
import std time

// Pattern 1 — with sleep (IoT, game loops)
// Mailbox drains at sleep frequency. Predictable latency.
while active {
    readings = sensor.read()
    time.sleep(16)   // ← drain + GC safe point here
}

// Pattern 2 — hot loop (DSP, control, computation)
// No sleep. Back-edge drain is O(1) — just one load + branch.
while i < 10000 {
    sum = sum + data[i]
    i = i + 1
    // ← drain check every iteration, negligible overhead
}

// Pattern 3 — polling loop (maximum responsiveness)
// timeout drives exit. Responds to stop signal immediately.
int t0 = time.now_ms()
while !time.timeout(t0, 5000) {
    process_next()
    // ← exits within one iteration of stop signal
}
```

### Example

```fluxa
import std time

int t0     = time.now_ms()
time.sleep(100)
int dt     = time.elapsed_ms(t0)   // ~100
bool late  = time.timeout(t0, 50)  // true
str  ts    = time.format(t0)       // "2025-01-15 14:32:01.100"
print(dt)
```

---

## std.flxthread

Concurrency for Fluxa. Threads are isolated by default — Block instances have no shared state, no locks required. The only shared resource is `prst` global vars, protected explicitly via `ft.lock()`.

No `danger` required for any `ft.*` call. Not available on embedded targets (`FLUXA_EMBEDDED`).

**Enable:**
```toml
[libs]
std.flxthread = "1.0"
std.time      = "1.0"   # required for time.sleep in thread loops
```

**Import with alias:**
```fluxa
import std flxthread as ft
```

### Model

```
Thread A (main)          Thread B (t1 — e1.update)
─────────────────        ─────────────────────────
ft.new("t1", e1, "update")
                         while health > 0 {
                             health = health - 1
ft.message("t1","hit",10)── → mailbox enqueued
                             time.sleep(16)
                         }  ← back-edge: drain mailbox
                             ↳ hit(10) called on e1
int hp = ft.await("t1","get_health")
                         ← get_health() called, reply sent
         ← hp received
ft.resolve_all()         thread finishes
                         ← joined
```

### Function reference

**Thread lifecycle:**

| Function | Blocking? | Description |
|---|---|---|
| `ft.new("name", fn_str)` | No | Spawn global function as thread |
| `ft.new("name", instance, "method")` | No | Spawn Block method as thread |
| `ft.resolve_all()` | Yes | Wait for all threads to finish. Syncs prst pool to main runtime. |
| `ft.active("name")` | No | True if thread is still running |
| `ft.thread_count()` | No | Number of active threads |

**Communication:**

| Function | Blocking? | Description |
|---|---|---|
| `ft.message("name", "method")` | No | Enqueue method call. Thread drains at back-edge. |
| `ft.message("name", "method", arg)` | No | Same, with one argument |
| `ft.await("name", "method")` | Yes | Enqueue + wait for return value |
| `ft.await("name", "method", arg)` | Yes | Same, with one argument |

**Stop control:**

| Function | Description |
|---|---|
| `ft.stop("name")` | Request cooperative stop. Thread exits at next back-edge. |
| `ft.kill("name")` | Force stop. Marks thread dead immediately. Pending `ft.await` calls return nil. **WARNING:** `ft.lock()` mutexes held by the killed thread are NOT released. |
| `ft.should_stop()` | Called inside a thread. Returns true if stop was requested. Use in `while !ft.should_stop()` loops. |

**Shared state:**

| Function | Description |
|---|---|
| `ft.lock("var_name")` | Register a prst global var with a mutex. All accesses from any thread are automatically serialized. Only meaningful for prst global scope — Block prst is isolated by design. |

### Stop patterns

```fluxa
// ft.stop — cooperative. Thread exits at next while back-edge.
ft.stop("t1")
ft.resolve_all()

// ft.should_stop — idiomatic for clean shutdown with resource cleanup.
Block Worker {
    fn run() nil {
        while !ft.should_stop() {
            process()
            time.sleep(10)
        }
        cleanup()   // always runs before thread dies
        print("shutdown complete")
    }
}

// ft.kill — forced. Use when ft.stop would never be observed.
ft.kill("t1")       // marks dead immediately
// do NOT call ft.resolve_all() after ft.kill on that thread

// ft.stop + timeout → ft.kill pattern (graceful first, force fallback)
ft.stop("t1")
time.sleep(500)
if ft.active("t1") {
    ft.kill("t1")
}
```

### When to use global fn threads vs Block method threads

| Use case | Recommended | Reason |
|---|---|---|
| Simple work, no loops | Global fn thread | Less boilerplate |
| Loops that mutate prst | **Block method thread** | Block scope isolates vars correctly |
| Mailbox / `ft.await` | Block method thread | Methods needed for dispatch |
| `ft.stop` / `ft.should_stop` | Either | Both patterns supported |

> **Implementation note:** `while` loops compile to the bytecode VM, which
> accesses prst variables by name via scope — independent of stack slot
> assignments. Local fn variables (`int i`) use stack slots that overlap with
> global prst slots in the resolver numbering, but this does not cause
> aliasing because the VM separates the two lookups. For prst + loop patterns,
> Block method threads are the idiomatic choice in Fluxa.

### Decision table

| Scenario | Use | Why |
|---|---|---|
| Loop with `time.sleep` | `ft.stop()` | Thread observes stop at next back-edge |
| Hot loop, no sleep | `ft.stop()` | Back-edge runs every iteration, stops fast |
| Thread stuck in long operation | `ft.kill()` | `ft.stop()` won't be observed |
| Holding `ft.lock()` | `ft.stop()` + timeout → `ft.kill()` | Avoid leaving mutex inconsistent |
| Application shutdown | `ft.stop()` all → `ft.kill()` remaining | Graceful first |

### Full example (from the design spec)

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

### Notes

- `ft.message` and `ft.await` map to **real declared methods** on the Block. If the method doesn't exist, it's a runtime error.
- Each thread gets its own `Runtime` clone — stack and scope are fully isolated. `prst` global vars are shared via the prst pool and synced on every `NODE_ASSIGN`.
- Mailbox drain happens at the **while back-edge** — the same safe point used by the GC. Fast path: O(1) when no messages.
- `ft.resolve_all()` syncs the prst pool back to the main runtime's stack after all threads finish.

---

## Buffer Configuration Reference

All limits are configurable in `fluxa.toml`. Defaults are conservative and safe for embedded targets.

```toml
[libs.csv]
max_line_bytes = 1024   # bytes per line — increase for wide CSVs
max_fields     = 64     # fields per row — used by csv.field internally

[libs.json]
max_str_bytes  = 4096   # max size of a JSON str value or loaded file

[ffi]
str_buf_size   = 1024   # writable char* buffer allocated per pointer arg (default 1024)
                        # range: 64–65536. Applies to all char* output params in FFI calls.
```

**RP2040 recommended (264 KB SRAM):**
```toml
[libs.csv]
max_line_bytes = 256
max_fields     = 16

[libs.json]
max_str_bytes  = 512

[ffi]
str_buf_size   = 64
```

**ESP32 recommended (520 KB SRAM):**
```toml
[libs.csv]
max_line_bytes = 512
max_fields     = 32

[libs.json]
max_str_bytes  = 2048

[ffi]
str_buf_size   = 512
```

---

## What Is Not Supported (v1.0)

**std.csv:**
- Multiline fields (quoted fields spanning multiple lines)
- Automatic type detection (all fields are `str` — convert manually)
- Streaming iterator with random access
- Encoding beyond ASCII/UTF-8 passthrough

Quoted fields with embedded commas (`"hello, world"`) and escaped quotes (`"say ""hello"""`) **are supported** via the FSM parser in `csv.field`.

**std.strings:**
- Unicode-aware indexing (all operations work on bytes, not codepoints)
- Regex pattern matching (coming in `std.regex`)
- `trim_left` / `trim_right` (use `strings.trim` for both sides)
- `replace_first` (use `strings.find` + `strings.slice` + string concatenation)
- `pad_left` / `pad_right`

**std.json:**
- Deeply nested object construction (flatten before building)
- JSON path expressions (`json.get("a.b.c")`)
- Number formatting control (uses `%g` internally)
- Unicode escape sequences in strings (`\uXXXX`)
- In-place mutation of JSON objects (always returns new `str`)

These limitations are intentional — the goal is predictable memory use on embedded hardware, not feature parity with desktop JSON/CSV libraries.

## std.crypto

**Sprint 12.a** — Cryptographic primitives via libsodium 1.0.18+. All functions follow the standard error model: errors abort outside `danger`, are captured in `err[]` inside `danger`. Key material is stored in `int arr` (each byte as `VAL_INT [0..255]`), so `prst int arr key` survives hot reloads.

**Dependency:** libsodium must be installed (`apt install libsodium-dev` or equivalent). Auto-detected via `pkg-config`.

```toml
[libs]
std.crypto = "1.0"
```

```fluxa
import std crypto
```

### Hash

| Function | Signature | Description |
|---|---|---|
| `hash` | `hash(data: str\|arr) → dyn[32]` | BLAKE2b-256 hash. No secret key. Deterministic. |
| `to_hex` | `to_hex(arr) → str` | Encode byte arr as lowercase hex string. |
| `from_hex` | `from_hex(str) → dyn` | Decode hex string to byte arr. Error on invalid hex. |

### Symmetric Encryption (XSalsa20-Poly1305)

| Function | Signature | Description |
|---|---|---|
| `keygen` | `keygen() → dyn[32]` | Random 32-byte symmetric key. Use with `encrypt`/`decrypt`. |
| `nonce` | `nonce() → dyn[24]` | Random 24-byte nonce. **Generate fresh per message.** |
| `encrypt` | `encrypt(msg, key, nonce) → dyn` | Authenticated encryption. Output = 16-byte MAC + ciphertext. |
| `decrypt` | `decrypt(cipher, key, nonce) → str` | Verify MAC then decrypt. Fails on authentication error. |

### Signing (Ed25519)

| Function | Signature | Description |
|---|---|---|
| `sign_keygen` | `sign_keygen(pk: int arr[32], sk: int arr[64]) → nil` | Generate Ed25519 keypair. Writes into existing arr args. |
| `sign` | `sign(msg, sk) → dyn` | Sign message. Output = 64-byte signature + message. |
| `sign_open` | `sign_open(signed, pk) → str` | Verify signature and extract message. Fails on invalid sig. |

### Key Exchange (Curve25519)

| Function | Signature | Description |
|---|---|---|
| `kx_keygen` | `kx_keygen(pk: int arr[32], sk: int arr[32]) → nil` | Curve25519 keypair for key exchange. |
| `kx_client` | `kx_client(rx, tx, cpk, csk, spk) → nil` | Client-side session keys. rx/tx: int arr[32]. |
| `kx_server` | `kx_server(rx, tx, spk, ssk, cpk) → nil` | Server-side session keys. rx/tx: int arr[32]. |

### Utilities

| Function | Signature | Description |
|---|---|---|
| `compare` | `compare(a, b) → bool` | Constant-time comparison. Safe against timing attacks. |
| `wipe` | `wipe(arr) → nil` | Securely zero arr in place. Compiler-barrier safe. |
| `version` | `version() → str` | libsodium version string. |

### Example — Encrypt/Decrypt

```fluxa
import std crypto

// Key generation — store as prst to survive reloads
prst dyn session_key = crypto.keygen()

// Encrypt a message
dyn nonce  = crypto.nonce()                              // fresh nonce per message
dyn cipher = crypto.encrypt("secret payload", session_key, nonce)

// Decrypt
str plain = crypto.decrypt(cipher, session_key, nonce)
print(plain)    // secret payload
```

### Example — Ed25519 Signature

```fluxa
import std crypto

// Generate keypair into pre-sized arrs
int arr pk[32] = 0
int arr sk[64] = 0
crypto.sign_keygen(pk, sk)

// Sign
dyn signed = crypto.sign("data to sign", sk)

// Verify and extract — fails with err if signature invalid
danger {
    str msg = crypto.sign_open(signed, pk)
    print(msg)    // data to sign
}
```

### Example — Key Exchange (Curve25519)

```fluxa
import std crypto

// Server generates keypair
int arr spk[32] = 0; int arr ssk[32] = 0
crypto.kx_keygen(spk, ssk)

// Client generates keypair
int arr cpk[32] = 0; int arr csk[32] = 0
crypto.kx_keygen(cpk, csk)

// Client computes session keys
int arr crx[32] = 0; int arr ctx[32] = 0
crypto.kx_client(crx, ctx, cpk, csk, spk)

// Server computes session keys
int arr srx[32] = 0; int arr stx[32] = 0
crypto.kx_server(srx, stx, spk, ssk, cpk)

// crx == stx and ctx == srx (symmetric session keys established)
```

### Notes

- `keygen()`, `nonce()`, `sign_keygen()`, `kx_keygen()` use `randombytes_buf()` — cryptographically secure random, seeded by the OS.
- `compare()` uses `sodium_memcmp()` — constant time regardless of input content. Use for MAC and signature comparison.
- `wipe()` zeros the arr elements (type preserved as VAL_INT) with a compiler barrier. Call on key material after use.
- On RP2040 with libsodium-minimal: all functions work. `randombytes` uses hardware RNG if available, otherwise system entropy.
- `prst dyn key` survives hot reloads. The key material lives in the PrstPool and is serialized in Handover snapshots.

---

## std.pid — PID Controller

Pure C99, zero external dependencies. Embedded-friendly (RP2040, ESP32).

**Declaration:**
```toml
[libs]
std.pid = "1.0"
```

**State:** The controller state (integral, prev_error, gains, limits) lives inside a heap-allocated struct wrapped in a `dyn` cursor. Use `prst dyn ctrl` so state survives hot reloads — this is the correct pattern for PID in a control loop.

### Functions

| Function | Returns | Description |
|---|---|---|
| `pid.new(kp, ki, kd)` | `dyn` | Create controller cursor. kp/ki/kd are float gains. |
| `pid.compute(ctrl, setpoint, pv)` | `float` | Compute output. setpoint = desired value, pv = measured value. |
| `pid.reset(ctrl)` | `nil` | Zero integral and prev_error. Call on mode switch or startup. |
| `pid.set_limits(ctrl, min, max)` | `nil` | Clamp output to [min, max]. Enables anti-windup. min must be < max. |
| `pid.set_deadband(ctrl, band)` | `nil` | Ignore errors smaller than band (treat as zero). Reduces chatter. |
| `pid.state(ctrl)` | `dyn` | Returns [kp, ki, kd, integral, prev_error, out_min, out_max]. |

**Anti-windup:** When `set_limits` is configured, `compute` back-calculates the integral when the output is clamped — preventing the integral from growing unbounded during saturation.

### Example

```fluxa
import std pid

prst dyn heater = pid.new(2.0, 0.5, 0.1)
pid.set_limits(heater, 0.0, 100.0)    // output: 0–100% duty cycle
pid.set_deadband(heater, 0.2)          // ignore error < 0.2°C

float setpoint = 72.0
float temperature = 68.5              // from sensor

float duty = pid.compute(heater, setpoint, temperature)
// → duty ≈ 7.0 (kp * 3.5 + accumulated integral)
```

---

## std.sqlite — Embedded SQL

SQLite 3 wrapper. Requires `libsqlite3-dev`. Works on RP2040 with filesystem support.

**Declaration:**
```toml
[libs]
std.sqlite = "1.0"
```

**State:** DB connection is a `dyn` cursor wrapping a `sqlite3*`. Use `prst dyn db` to keep the connection open across hot reloads.

### Functions

| Function | Returns | Description |
|---|---|---|
| `sqlite.open(path)` | `dyn` | Open (or create) a SQLite database file. |
| `sqlite.close(db)` | `nil` | Close connection. Double-close is a no-op. |
| `sqlite.exec(db, sql)` | `nil` | Execute DDL or DML (CREATE, INSERT, UPDATE, DELETE). |
| `sqlite.query(db, sql)` | `dyn` | Execute SELECT. Returns dyn of rows, each row a dyn of values. |
| `sqlite.last_insert_id(db)` | `int` | Row ID of the last INSERT. |
| `sqlite.changes(db)` | `int` | Rows affected by the last DML statement. |
| `sqlite.version()` | `str` | libsqlite3 version string (e.g. "3.45.1"). |

### Example

```fluxa
import std sqlite

danger {
    dyn db = sqlite.open("sensors.db")
    sqlite.exec(db, "CREATE TABLE IF NOT EXISTS readings (ts INTEGER, val REAL)")
    sqlite.exec(db, "INSERT INTO readings VALUES (1700000000, 23.5)")

    dyn rows = sqlite.query(db, "SELECT ts, val FROM readings ORDER BY ts DESC LIMIT 5")
    int i = 0
    while i < len(rows) {
        dyn row = rows[i]
        print(row[0])    // ts (int)
        print(row[1])    // val (float)
        i = i + 1
    }
    sqlite.close(db)
}
```

---

## std.serial — UART / Serial

UART communication via libserialport. Requires `libserialport-dev`. Fundamental for RP2040/ESP32 debug and device communication.

**Declaration:**
```toml
[libs]
std.serial = "1.0"
```

**State:** Port cursor wraps an `sp_port*`. Use `prst dyn port` to keep the port open across hot reloads.

### Functions

| Function | Returns | Description |
|---|---|---|
| `serial.list()` | `dyn` | List available port names as strings (e.g. ["/dev/ttyUSB0"]). |
| `serial.open(port, baud)` | `dyn` | Open port at given baud rate. 8N1 by default. |
| `serial.close(port)` | `nil` | Close port. Double-close is a no-op. |
| `serial.write(port, data)` | `int` | Write string to port. Returns bytes written. |
| `serial.read(port, max_bytes, timeout_ms)` | `str` | Read up to max_bytes with timeout. Returns what arrived. |
| `serial.readline(port, timeout_ms)` | `str` | Read until `\n` or timeout. |
| `serial.flush(port)` | `nil` | Flush TX and RX buffers. |
| `serial.bytes_available(port)` | `int` | Bytes waiting in RX buffer (non-blocking). |

### Example

```fluxa
import std serial

prst dyn port = serial.list()    // list on first load

danger {
    dyn p = serial.open("/dev/ttyUSB0", 115200)
    serial.write(p, "AT\r\n")
    str resp = serial.readline(p, 500)
    print(resp)
    serial.close(p)
}
```

---

## std.i2c — I2C Protocol

I2C via Linux i2c-dev kernel interface. No external library required — i2c-dev is part of the Linux kernel. For RP2040: use PICO_SDK `hardware_i2c` directly (i2c-dev is Linux-only; the lib returns a clear error on non-Linux).

**Declaration:**
```toml
[libs]
std.i2c = "1.0"
```

**State:** Bus cursor wraps an `I2cBus` struct (fd + addr). Use `prst dyn bus` to keep the handle open across hot reloads.

### Functions

| Function | Returns | Description |
|---|---|---|
| `i2c.open(device, addr)` | `dyn` | Open I2C bus and set slave address. e.g. `i2c.open("/dev/i2c-1", 72)` |
| `i2c.close(bus)` | `nil` | Close bus handle. Double-close is a no-op. |
| `i2c.write(bus, data)` | `int` | Write int arr bytes to device. Returns bytes written. |
| `i2c.read(bus, nbytes)` | `dyn` | Read nbytes. Returns dyn of int (each element 0–255). |
| `i2c.write_reg(bus, reg, value)` | `nil` | Write single byte to register. |
| `i2c.read_reg(bus, reg)` | `int` | Read single byte from register. |
| `i2c.read_reg16(bus, reg)` | `int` | Read 16-bit big-endian value from register (common for sensors). |
| `i2c.scan(device)` | `dyn` | Scan bus for responding addresses. Returns dyn of int addresses. |

**Note:** I2C addresses in Fluxa are plain integers. `0x48` in C = `72` in Fluxa (no hex literal support).

### Example

```fluxa
import std i2c

// ADS1115 ADC at address 72 (0x48) on /dev/i2c-1
prst dyn adc = i2c.open("/dev/i2c-1", 72)

danger {
    // Read conversion register (register 0)
    int raw = i2c.read_reg16(adc, 0)
    float voltage = raw * 0.0001875    // ADS1115 at ±6.144V, 16-bit
    print(voltage)
}
```

---

## std.http — HTTP Client

HTTP client via libcurl. Requires `libcurl-dev`. Works on Linux/macOS; embedded targets need network stack support.

**Declaration:**
```toml
[libs]
std.http = "1.0"
```

All requests must be inside `danger {}` — network I/O can fail for external reasons.

### Functions

| Function | Returns | Description |
|---|---|---|
| `http.get(url)` | `dyn` | HTTP GET. Returns response dyn. |
| `http.post(url, body)` | `dyn` | HTTP POST with form-encoded body. |
| `http.post_json(url, json)` | `dyn` | HTTP POST with `Content-Type: application/json`. |
| `http.put(url, body)` | `dyn` | HTTP PUT. |
| `http.delete(url)` | `dyn` | HTTP DELETE. |
| `http.status(resp)` | `int` | HTTP status code (200, 404, ...). |
| `http.body(resp)` | `str` | Response body as string. |
| `http.ok(resp)` | `bool` | True if status 200–299. |

Response dyn layout: `[status:int, body:str, ok:bool]`.

### Example

```fluxa
import std http

danger {
    dyn r = http.get("https://api.example.com/data")
    if http.ok(r) {
        str data = http.body(r)
        print(data)
    }
}
```

---

## std.mqtt — MQTT Client

MQTT publish/subscribe via libmosquitto. Requires `libmosquitto-dev`. Fundamental for IoT sensor telemetry and device control.

**Declaration:**
```toml
[libs]
std.mqtt = "1.0"
```

**State:** Connection cursor wraps a `mosquitto*`. Use `prst dyn client` to keep the connection alive across hot reloads.

### Functions

| Function | Returns | Description |
|---|---|---|
| `mqtt.connect(host, port, client_id)` | `dyn` | Connect to broker. |
| `mqtt.connect_auth(host, port, id, user, pass)` | `dyn` | Connect with credentials. |
| `mqtt.disconnect(cursor)` | `nil` | Disconnect and free. Double-disconnect is a no-op. |
| `mqtt.publish(cursor, topic, payload)` | `nil` | Publish QoS 0. |
| `mqtt.publish_qos(cursor, topic, payload, qos)` | `nil` | Publish with QoS 0/1/2. |
| `mqtt.subscribe(cursor, topic)` | `nil` | Subscribe QoS 0. |
| `mqtt.subscribe_qos(cursor, topic, qos)` | `nil` | Subscribe with QoS 0/1/2. |
| `mqtt.loop(cursor, timeout_ms)` | `nil` | Process one event (call in sensor loop). |
| `mqtt.connected(cursor)` | `bool` | True if connection is active. |

### Example

```fluxa
import std mqtt

prst dyn broker = mqtt.connect("mqtt.example.com", 1883, "sensor-01")

danger {
    if mqtt.connected(broker) {
        mqtt.publish(broker, "sensors/temp", "23.5")
        mqtt.loop(broker, 10)
    }
}
```

---

## std.mcp — Model Context Protocol Client

MCP client for calling AI tool servers (Claude, filesystem, databases). Uses JSON-RPC 2.0 over HTTP POST. Requires `libcurl-dev`.

**Declaration:**
```toml
[libs]
std.mcp = "1.0"
```

**State:** Server cursor is lazy — `connect` doesn't open a TCP connection, it just stores the URL. Use `prst dyn server` to keep the cursor across hot reloads.

### Functions

| Function | Returns | Description |
|---|---|---|
| `mcp.connect(url)` | `dyn` | Create cursor for MCP server at url. |
| `mcp.connect_auth(url, token)` | `dyn` | Create cursor with Bearer token auth. |
| `mcp.list_tools(cursor)` | `dyn` | List tool names as `dyn` of `str`. |
| `mcp.call(cursor, tool, args_json)` | `str` | Call tool, return full JSON result. |
| `mcp.call_text(cursor, tool, args_json)` | `str` | Call tool, return text content only. |
| `mcp.disconnect(cursor)` | `nil` | Free cursor resources. |

### Example

```fluxa
import std mcp

prst dyn claude = mcp.connect("http://localhost:3000")

danger {
    dyn tools = mcp.list_tools(claude)
    int i = 0
    while i < len(tools) {
        print(tools[i])
        i = i + 1
    }
    str result = mcp.call_text(claude, "read_file", "{\"path\":\"/etc/hostname\"}")
    print(result)
}
```

---

## std.libv — Vectors, Matrices, Tensors

Pure C99, zero external dependencies. GLM-inspired API. Works on RP2040 and ESP32.

All storage is backed by `float arr` or `int arr` — no new types introduced. Operations add shape semantics on top of existing Fluxa arrays. Col-major storage (same as GLSL/OpenGL). In-place operations by default.

**Declaration:**
```toml
[libs]
std.libv = "1.0"
```

### Initializers

| Expression | Size | Description |
|---|---|---|
| `libv.vec2` | 2 | 2D float vector, zeros |
| `libv.vec3` | 3 | 3D float vector, zeros |
| `libv.vec4` | 4 | 4D / RGBA float vector, zeros |
| `libv.ivec2` | 2 | 2D int vector, zeros |
| `libv.ivec3` | 3 | 3D int vector, zeros |
| `libv.mat2` | 4 | 2×2 identity matrix |
| `libv.mat3` | 9 | 3×3 identity matrix |
| `libv.mat4` | 16 | 4×4 identity matrix (shader standard) |
| `libv.vec(n)` | n | N-vector, zeros |
| `libv.mat(r, c)` | r×c | r×c matrix, zeros |
| `libv.tens(d0, d1, ...)` | d0×d1×... | N-dimensional tensor, zeros |

### Vector operations

All modify the first argument **in-place** unless the operation inherently returns a scalar.

| Function | Returns | Description |
|---|---|---|
| `libv.add(a, b)` | `nil` | a = a + b |
| `libv.sub(a, b)` | `nil` | a = a − b |
| `libv.scale(a, s)` | `nil` | a = a × scalar |
| `libv.negate(a)` | `nil` | a = −a |
| `libv.normalize(a)` | `nil` | a = a / ‖a‖ |
| `libv.lerp(a, b, t)` | `nil` | a = mix(a, b, t) |
| `libv.cross(out, a, b)` | `nil` | out = a × b (vec3 only, caller allocates out) |
| `libv.dot(a, b)` | `float` | dot product |
| `libv.norm(a)` | `float` | Euclidean length |
| `libv.angle(a, b)` | `float` | angle in radians |
| `libv.eq(a, b)` | `bool` | element-wise equality (ε = 1e-6) |
| `libv.shape(a)` | `int` | element count |
| `libv.fill(a, v)` | `nil` | set all elements to scalar v |
| `libv.copy(dst, src)` | `nil` | copy src into dst |

Shape mismatch produces a runtime error:
```
libv.add (line 5): shape mismatch (3 != 4)
```

### Matrix operations

| Function | Returns | Description |
|---|---|---|
| `libv.identity(m)` | `nil` | reset to identity in-place |
| `libv.transpose(m)` | `nil` | in-place (square matrices) |
| `libv.matmul(out, a, b)` | `nil` | out = a × b (caller allocates out) |
| `libv.det(m)` | `float` | determinant (2×2, 3×3, 4×4) |
| `libv.inverse(out, m)` | `nil` | out = m⁻¹ (2×2 only; higher dims in std.libdsp) |

### 3D transform helpers (shader / Raylib workflow)

All write into the matrix `m` in-place (mat4 required).

| Function | Description |
|---|---|
| `libv.translate(m, tx, ty, tz)` | apply translation |
| `libv.rotate(m, angle_rad, ax, ay, az)` | rotate around axis |
| `libv.scale_mat(m, sx, sy, sz)` | apply scale |
| `libv.perspective(m, fov_rad, aspect, near, far)` | perspective projection |
| `libv.ortho(m, left, right, bottom, top, near, far)` | orthographic projection |
| `libv.lookat(m, eye, center, up)` | view matrix (eye/center/up are vec3) |

### Tensor operations

| Function | Returns | Description |
|---|---|---|
| `libv.tens_add(t, t2)` | `nil` | element-wise add in-place |
| `libv.tens_scale(t, s)` | `nil` | scalar multiply in-place |
| `libv.tens_slice(out, t, idx)` | `nil` | extract slice along first axis |

### Example

```fluxa
import std libv

// 3D transform pipeline
float arr model[16] = libv.mat4
float arr view[16]  = libv.mat4
float arr proj[16]  = libv.mat4

libv.translate(model, 1.0, 0.0, 0.0)
libv.rotate(model, 0.785, 0.0, 1.0, 0.0)   // 45 degrees around Y

float arr eye[3]    = libv.vec3
float arr center[3] = libv.vec3
float arr up[3]     = libv.vec3
libv.fill(eye, 0.0)
eye[2] = 5.0          // camera at z=5
libv.lookat(view, eye, center, up)

libv.perspective(proj, 1.047, 1.777, 0.1, 100.0)  // 60 deg FOV, 16:9

// Dot product
float arr a[3] = libv.vec3
float arr b[3] = libv.vec3
a[0] = 1.0; b[0] = 0.5; b[1] = 0.5
float d = libv.dot(a, b)

// PID weights as prst tensor (survives hot reloads)
prst float arr weights[27] = libv.tens(3, 3, 3)
```

### Notes

- `mat2/mat3/mat4` initialize to the **identity matrix**. All other initializers produce zeros.
- Storage is flat col-major: `mat4[col*4 + row]`.
- `prst float arr` works exactly like any other `prst arr` — flat storage serializes through handover.
- `libv.inverse` supports only 2×2 currently. Full 3×3/4×4 inverse (LU decomposition) will be in `std.libdsp`.
