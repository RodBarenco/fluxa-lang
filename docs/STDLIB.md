# Fluxa Standard Library
**v0.16**

Reference documentation for all stdlib libs: `std.math`, `std.csv`, `std.json`, `std.json2`, `std.strings`, `std.time`, `std.flxthread`, `std.crypto`, `std.pid`, `std.sqlite`, `std.serial`, `std.i2c`, `std.httpc`, `std.https`, `std.mqtt`, `std.mcpc`, `std.mcps`, `std.websocket`, `std.http`, `std.mcp`, `std.graph`, `std.infer`, `std.zlib`, `std.fs`, `std.libv`, `std.libdsp`, `std.wserver`, `std.pg`.

---

## Design Principles

All stdlib libs share the same design contract:

**Opt-in by declaration.** A lib only exists at runtime if declared in `[libs]` of `fluxa.toml`. Without declaration, `import std <lib>` produces a clear error. No lib adds overhead to programs that don't use it.

**No `danger` required for pure computation.** Stdlib functions written in safe C and vetted for embedded use work outside `danger`. File I/O, network, and database calls require `danger {}` because they can fail for external reasons.

**Errors follow the standard model.** Outside `danger`: runtime error with line number, execution aborts. Inside `danger`: error captured in `err_stack`, execution continues.

**Buffers are bounded.** Every lib that touches external data has configurable buffer limits in `fluxa.toml`. No silent truncation — exceeding a limit produces a clear error.

**External resources use opaque int handles.** Libs that manage external resources (connections, servers, files) return `int` handles — positive integers that index into a fixed table inside the lib's C layer. Zero is always invalid. Handles are not pointers and carry no type information visible to Fluxa code.

**`dyn` cursors for in-process state.** Libs that manage state entirely within the Fluxa process (file cursors, DB result sets from SQLite, JSON documents, PID controllers) use `dyn` cursors — opaque `VAL_PTR` wrappers. Use `prst dyn cursor` to keep these alive across hot reloads. `dyn` cursors are valid in the main program scope. They are **never** valid as Block fields.

**No `dyn` inside Block.** Block fields must have a declared type (`int`, `float`, `str`, `bool`, `arr`). `dyn` is not a valid Block field type. Pass `dyn` cursors as arguments to Block methods if needed.

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
| `json2.free(doc)` | `nil` | Release document memory |

```fluxa
import std json2

danger {
    dyn doc = json2.parse("{\"sensor\":{\"temp\":22.5},\"readings\":[1,2,3]}")
    float temp = json2.get_float(doc, "sensor.temp")
    int first  = json2.get_int(doc, "readings[0]")
    int count  = json2.length(doc, "readings")
    json2.set_float(doc, "sensor.temp", 23.1)
    str updated = json2.stringify(doc)
    json2.free(doc)
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
| `ft.new("name", instance, "method")` | No | Spawn Block method as thread |
| `ft.resolve_all()` | Yes | Wait for all threads. Syncs prst pool. |
| `ft.active("name")` | No | True if thread is still running |
| `ft.thread_count()` | No | Number of active threads |

**Communication:**

| Function | Blocking? | Description |
|---|---|---|
| `ft.message("name", "method")` | No | Enqueue method call |
| `ft.message("name", "method", arg)` | No | Same, with one argument |
| `ft.await("name", "method")` | Yes | Enqueue + wait for return |
| `ft.await("name", "method", arg)` | Yes | Same, with one argument |

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
| `graph.init(w, h, title)` | `dyn` | Open window |
| `graph.close(win)` | `nil` | Close window |
| `graph.should_close(win)` | `bool` | Window close requested |
| `graph.begin_frame(win)` | `nil` | Begin draw frame |
| `graph.end_frame(win)` | `nil` | Present frame |
| `graph.clear(win, r, g, b)` | `nil` | Clear background (RGB 0–255) |
| `graph.fps(win)` | `int` | Current FPS |
| `graph.set_fps(win, fps)` | `nil` | Set target FPS |
| `graph.draw_rect(win, x, y, w, h, r, g, b)` | `nil` | Filled rectangle |
| `graph.draw_circle(win, x, y, radius, r, g, b)` | `nil` | Filled circle |
| `graph.draw_line(win, x1, y1, x2, y2, r, g, b)` | `nil` | Line |
| `graph.draw_text(win, text, x, y, size, r, g, b)` | `nil` | Text |
| `graph.key_pressed(win, key)` | `bool` | Key just pressed |
| `graph.key_down(win, key)` | `bool` | Key held |
| `graph.mouse_x(win)` | `int` | Mouse X |
| `graph.mouse_y(win)` | `int` | Mouse Y |
| `graph.mouse_pressed(win)` | `bool` | Left mouse button |
| `graph.dt(win)` | `float` | Delta time in seconds |
| `graph.version()` | `str` | Backend version |

---

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
| `fs.read(path)` | `str` | Read entire file |
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

### Manual mode — user controls workers

```fluxa
import std wserver
import std flxthread as ft

fn worker_loop(int srv) nil {
    while !ft.should_stop() {
        danger {
            int req = wserver.accept(srv, 100)
            if req != 0 {
                str method = wserver.req_method(req)
                if method == "GET"    { wserver.reply(req, 200, "ok") }
                if method == "POST"   { wserver.reply(req, 201, wserver.req_body(req)) }
                if method == "PUT"    { wserver.reply(req, 200, wserver.req_body(req)) }
                if method == "PATCH"  { wserver.reply(req, 200, wserver.req_body(req)) }
                if method == "DELETE" { wserver.reply(req, 204, "") }
            }
        }
    }
}

int srv = 0
danger { srv = wserver.serve(8080, false) }

Block w1 typeof Worker
Block w2 typeof Worker
ft.new("t1", w1, "run", srv)
ft.new("t2", w2, "run", srv)
ft.resolve_all()
wserver.stop(srv)
```

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
ft.new("t1", w1, "run", srv)
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

### Example — multi-threaded with std.flxthread

IO stays in functions with `danger`. Block holds pure state. Each worker fetches data independently using its own connection handle.

```fluxa
import std pg
import std flxthread as ft

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
    fn avg() float { return sum / count }
}

int c1 = 0
int c2 = 0
danger {
    c1 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
    c2 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
}

Block w1 typeof Worker
Block w2 typeof Worker
ft.new("t1", w1, "record")
ft.new("t2", w2, "record")

int tick = 0
while tick < 1000 {
    danger {
        str v1 = fetch(c1, "SELECT val FROM readings ORDER BY ts DESC LIMIT 1")
        str v2 = fetch(c2, "SELECT val FROM readings ORDER BY ts DESC LIMIT 1")
        ft.message("t1", "record", v1)
        ft.message("t2", "record", v2)
    }
    tick = tick + 1
}

ft.resolve_all()
pg.close(c1)
pg.close(c2)
```

### Notes

- `pg.free_result` must be called after every `pg.query` / `pg.query_params`.
- `pg.close` and `pg.free_result` are no-ops on invalid or already-freed handles — safe to call unconditionally.
- `pg.ping` uses `PQping` — does not open a full connection.
- `query_params` validates that `n ≤ arr.size` and each element is str before touching libpq.
- Out-of-bounds row/col in `get*` and `is_null` produces a runtime error with line number.
- Integration tests (real PostgreSQL in Docker): `bash tests/integration/pg/run.sh`

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

**std.wserver:**
- WebSocket upgrade (use `std.websocket` for WebSocket)
- HTTP/2

These limitations are intentional — predictable memory use on embedded hardware, not feature parity with desktop libraries.
