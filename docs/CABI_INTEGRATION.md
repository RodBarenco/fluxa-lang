# Fluxa C ABI Integration Guide

This document contains host-language integration examples for the Fluxa-lang C ABI.

The standard-library reference remains in [`STDLIB.md`](STDLIB.md). This guide focuses only on how external languages call `libfluxa_cabi` / `fluxa_cabi.dll`, send typed values into Fluxa, and read typed values returned by Fluxa.

The examples assume this Fluxa dispatcher:

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

All examples send:

```text
42
3.5
true
"Fluxa"
[10, 20, 30]
```

and expect:

```text
id      = 42
value   = 3.5
enabled = true
label   = Fluxa
points  = [10, 20, 30]
```

The public native header is:

```text
src/cabi/fluxa_cabi.h
```

Typical native library names are:

```text
Linux:   libfluxa_cabi.so
macOS:   libfluxa_cabi.dylib
Windows: fluxa_cabi.dll
```

---

## C

C uses the ABI directly.

```c
#include <stdint.h>
#include <stdio.h>
#include "fluxa_cabi.h"

int main(void) {
    fluxa_cabi_runtime *rt = NULL;
    fluxa_cabi_error err = {0};

    fluxa_cabi_config cfg = {
        .struct_size = sizeof(fluxa_cabi_config),
        .abi_version = FLUXA_CABI_ABI_VERSION,
        .entry_path = "main.flx",
        .config_path = "fluxa.toml",
        .dispatch_fn = NULL,
        .max_frame_bytes = 0,
        .flags = 0
    };

    if (fluxa_cabi_open(&cfg, &rt, &err) != FLUXA_CABI_OK) {
        fprintf(stderr, "open failed: %s\n", err.message);
        return 1;
    }

    fluxa_cabi_message req;
    fluxa_cabi_message_init(&req);

    int32_t points[] = {10, 20, 30};

    fluxa_cabi_add_int(&req, 42);
    fluxa_cabi_add_float(&req, 3.5);
    fluxa_cabi_add_bool(&req, 1);
    fluxa_cabi_add_str(&req, "Fluxa", 5);
    fluxa_cabi_add_int_arr(&req, points, 3);

    fluxa_cabi_view in = {req.data, req.size};
    fluxa_cabi_view out;

    if (fluxa_cabi_exchange(rt, &in, &out, &err) != FLUXA_CABI_OK) {
        fprintf(stderr, "exchange failed: %s\n", err.message);
        fluxa_cabi_message_free(&req);
        fluxa_cabi_close(rt);
        return 1;
    }

    int32_t id;
    double value;
    int enabled;
    fluxa_cabi_str_view label;
    uint32_t count;

    fluxa_cabi_get_int(&out, 0, &id);
    fluxa_cabi_get_float(&out, 1, &value);
    fluxa_cabi_get_bool(&out, 2, &enabled);
    fluxa_cabi_get_str(&out, 3, &label);
    fluxa_cabi_get_arr_count(&out, 4, &count);

    printf("id      = %d\n", id);
    printf("value   = %.1f\n", value);
    printf("enabled = %s\n", enabled ? "true" : "false");
    printf("label   = %.*s\n", (int)label.size, label.data);

    printf("points  = [");
    for (uint32_t i = 0; i < count; ++i) {
        int32_t point;
        fluxa_cabi_get_int_arr_value(&out, 4, i, &point);
        if (i) printf(", ");
        printf("%d", point);
    }
    printf("]\n");

    fluxa_cabi_message_free(&req);
    fluxa_cabi_close(rt);
    return 0;
}
```

Typical Linux build:

```bash
cc host.c \
  -I/path/to/fluxa/src/cabi \
  -L/path/to/fluxa \
  -lfluxa_cabi \
  -Wl,-rpath,/path/to/fluxa \
  -o host
```

---

## C++

The public header can be included directly from C++.

```cpp
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "fluxa_cabi.h"

int main() {
    fluxa_cabi_runtime *rt = nullptr;
    fluxa_cabi_error err{};

    fluxa_cabi_config cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = FLUXA_CABI_ABI_VERSION;
    cfg.entry_path = "main.flx";
    cfg.config_path = "fluxa.toml";

    if (fluxa_cabi_open(&cfg, &rt, &err) != FLUXA_CABI_OK) {
        std::cerr << err.message << '\n';
        return 1;
    }

    fluxa_cabi_message req;
    fluxa_cabi_message_init(&req);

    std::vector<int32_t> points{10, 20, 30};

    fluxa_cabi_add_int(&req, 42);
    fluxa_cabi_add_float(&req, 3.5);
    fluxa_cabi_add_bool(&req, 1);
    fluxa_cabi_add_str(&req, "Fluxa", 5);
    fluxa_cabi_add_int_arr(
        &req,
        points.data(),
        static_cast<uint32_t>(points.size())
    );

    fluxa_cabi_view in{req.data, req.size};
    fluxa_cabi_view out{};

    if (fluxa_cabi_exchange(rt, &in, &out, &err) != FLUXA_CABI_OK) {
        std::cerr << err.message << '\n';
        return 1;
    }

    int32_t id;
    double value;
    int enabled;
    fluxa_cabi_str_view label;
    uint32_t count;

    fluxa_cabi_get_int(&out, 0, &id);
    fluxa_cabi_get_float(&out, 1, &value);
    fluxa_cabi_get_bool(&out, 2, &enabled);
    fluxa_cabi_get_str(&out, 3, &label);
    fluxa_cabi_get_arr_count(&out, 4, &count);

    std::vector<int32_t> returned_points(count);
    for (uint32_t i = 0; i < count; ++i) {
        fluxa_cabi_get_int_arr_value(&out, 4, i, &returned_points[i]);
    }

    std::cout << "id      = " << id << '\n';
    std::cout << "value   = " << value << '\n';
    std::cout << "enabled = " << std::boolalpha << (enabled != 0) << '\n';
    std::cout << "label   = " << std::string(label.data, label.size) << '\n';

    std::cout << "points  = [";
    for (std::size_t i = 0; i < returned_points.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << returned_points[i];
    }
    std::cout << "]\n";

    fluxa_cabi_message_free(&req);
    fluxa_cabi_close(rt);
}
```

---

## Python

Python can use the standard `ctypes` module.

```python
import ctypes
from ctypes import (
    Structure,
    POINTER,
    byref,
    c_char,
    c_char_p,
    c_double,
    c_int,
    c_int32,
    c_uint32,
    c_void_p,
)

lib = ctypes.CDLL("./libfluxa_cabi.so")


class FluxaError(Structure):
    _fields_ = [
        ("code", c_uint32),
        ("line", c_int32),
        ("context", c_char * 128),
        ("message", c_char * 1024),
    ]


class FluxaMessage(Structure):
    _fields_ = [
        ("data", c_void_p),
        ("size", c_uint32),
        ("capacity", c_uint32),
    ]


class FluxaView(Structure):
    _fields_ = [
        ("data", c_void_p),
        ("size", c_uint32),
    ]


class FluxaStrView(Structure):
    _fields_ = [
        ("data", c_void_p),
        ("size", c_uint32),
    ]


class FluxaConfig(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("abi_version", c_uint32),
        ("entry_path", c_char_p),
        ("config_path", c_char_p),
        ("dispatch_fn", c_char_p),
        ("max_frame_bytes", c_uint32),
        ("flags", c_uint32),
    ]


FLUXA_CABI_OK = 0
FLUXA_CABI_ABI_VERSION = 1 << 16

lib.fluxa_cabi_open.argtypes = [
    POINTER(FluxaConfig),
    POINTER(c_void_p),
    POINTER(FluxaError),
]

lib.fluxa_cabi_message_init.argtypes = [POINTER(FluxaMessage)]
lib.fluxa_cabi_message_free.argtypes = [POINTER(FluxaMessage)]

lib.fluxa_cabi_add_int.argtypes = [POINTER(FluxaMessage), c_int32]
lib.fluxa_cabi_add_float.argtypes = [POINTER(FluxaMessage), c_double]
lib.fluxa_cabi_add_bool.argtypes = [POINTER(FluxaMessage), c_int]
lib.fluxa_cabi_add_str.argtypes = [
    POINTER(FluxaMessage),
    c_char_p,
    c_uint32,
]
lib.fluxa_cabi_add_int_arr.argtypes = [
    POINTER(FluxaMessage),
    POINTER(c_int32),
    c_uint32,
]

lib.fluxa_cabi_exchange.argtypes = [
    c_void_p,
    POINTER(FluxaView),
    POINTER(FluxaView),
    POINTER(FluxaError),
]

lib.fluxa_cabi_get_int.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    POINTER(c_int32),
]
lib.fluxa_cabi_get_float.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    POINTER(c_double),
]
lib.fluxa_cabi_get_bool.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    POINTER(c_int),
]
lib.fluxa_cabi_get_str.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    POINTER(FluxaStrView),
]
lib.fluxa_cabi_get_arr_count.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    POINTER(c_uint32),
]
lib.fluxa_cabi_get_int_arr_value.argtypes = [
    POINTER(FluxaView),
    c_uint32,
    c_uint32,
    POINTER(c_int32),
]

cfg = FluxaConfig(
    ctypes.sizeof(FluxaConfig),
    FLUXA_CABI_ABI_VERSION,
    b"main.flx",
    b"fluxa.toml",
    None,
    0,
    0,
)

runtime = c_void_p()
error = FluxaError()

if lib.fluxa_cabi_open(byref(cfg), byref(runtime), byref(error)) != FLUXA_CABI_OK:
    raise RuntimeError(bytes(error.message).split(b"\0", 1)[0].decode())

request = FluxaMessage()
lib.fluxa_cabi_message_init(byref(request))

points = (c_int32 * 3)(10, 20, 30)

lib.fluxa_cabi_add_int(byref(request), 42)
lib.fluxa_cabi_add_float(byref(request), 3.5)
lib.fluxa_cabi_add_bool(byref(request), 1)
lib.fluxa_cabi_add_str(byref(request), b"Fluxa", 5)
lib.fluxa_cabi_add_int_arr(byref(request), points, 3)

input_view = FluxaView(request.data, request.size)
output_view = FluxaView()

rc = lib.fluxa_cabi_exchange(
    runtime,
    byref(input_view),
    byref(output_view),
    byref(error),
)

if rc != FLUXA_CABI_OK:
    raise RuntimeError(bytes(error.message).split(b"\0", 1)[0].decode())

id_value = c_int32()
float_value = c_double()
enabled = c_int()
label = FluxaStrView()
point_count = c_uint32()

lib.fluxa_cabi_get_int(byref(output_view), 0, byref(id_value))
lib.fluxa_cabi_get_float(byref(output_view), 1, byref(float_value))
lib.fluxa_cabi_get_bool(byref(output_view), 2, byref(enabled))
lib.fluxa_cabi_get_str(byref(output_view), 3, byref(label))
lib.fluxa_cabi_get_arr_count(byref(output_view), 4, byref(point_count))

label_value = ctypes.string_at(label.data, label.size).decode("utf-8")

returned_points = []
for i in range(point_count.value):
    point = c_int32()
    lib.fluxa_cabi_get_int_arr_value(
        byref(output_view),
        4,
        i,
        byref(point),
    )
    returned_points.append(point.value)

print("id      =", id_value.value)
print("value   =", float_value.value)
print("enabled =", bool(enabled.value))
print("label   =", label_value)
print("points  =", returned_points)

lib.fluxa_cabi_message_free(byref(request))
lib.fluxa_cabi_close(runtime)
```

On Windows:

```python
lib = ctypes.CDLL("fluxa_cabi.dll")
```

---

## Java

Java can access the C ABI through JNA.

```java
import com.sun.jna.*;
import com.sun.jna.ptr.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class FluxaExample {

    public interface Fluxa extends Library {
        Fluxa INSTANCE = Native.load("fluxa_cabi", Fluxa.class);

        class Error extends Structure {
            public int code;
            public int line;
            public byte[] context = new byte[128];
            public byte[] message = new byte[1024];

            @Override
            protected List<String> getFieldOrder() {
                return Arrays.asList("code", "line", "context", "message");
            }
        }

        class Message extends Structure {
            public Pointer data;
            public int size;
            public int capacity;

            @Override
            protected List<String> getFieldOrder() {
                return Arrays.asList("data", "size", "capacity");
            }
        }

        class View extends Structure {
            public Pointer data;
            public int size;

            @Override
            protected List<String> getFieldOrder() {
                return Arrays.asList("data", "size");
            }
        }

        class StrView extends Structure {
            public Pointer data;
            public int size;

            @Override
            protected List<String> getFieldOrder() {
                return Arrays.asList("data", "size");
            }
        }

        class Config extends Structure {
            public int struct_size;
            public int abi_version;
            public String entry_path;
            public String config_path;
            public String dispatch_fn;
            public int max_frame_bytes;
            public int flags;

            @Override
            protected List<String> getFieldOrder() {
                return Arrays.asList(
                    "struct_size",
                    "abi_version",
                    "entry_path",
                    "config_path",
                    "dispatch_fn",
                    "max_frame_bytes",
                    "flags"
                );
            }
        }

        int fluxa_cabi_open(Config config, PointerByReference runtime, Error error);
        void fluxa_cabi_close(Pointer runtime);

        void fluxa_cabi_message_init(Message message);
        void fluxa_cabi_message_free(Message message);

        int fluxa_cabi_add_int(Message message, int value);
        int fluxa_cabi_add_float(Message message, double value);
        int fluxa_cabi_add_bool(Message message, int value);
        int fluxa_cabi_add_str(Message message, byte[] data, int size);
        int fluxa_cabi_add_int_arr(Message message, int[] values, int count);

        int fluxa_cabi_exchange(Pointer runtime, View request, View response, Error error);

        int fluxa_cabi_get_int(View view, int index, IntByReference value);
        int fluxa_cabi_get_float(View view, int index, DoubleByReference value);
        int fluxa_cabi_get_bool(View view, int index, IntByReference value);
        int fluxa_cabi_get_str(View view, int index, StrView value);
        int fluxa_cabi_get_arr_count(View view, int index, IntByReference count);
        int fluxa_cabi_get_int_arr_value(
            View view,
            int index,
            int element,
            IntByReference value
        );
    }

    public static void main(String[] args) {
        Fluxa f = Fluxa.INSTANCE;

        Fluxa.Config cfg = new Fluxa.Config();
        cfg.struct_size = cfg.size();
        cfg.abi_version = 1 << 16;
        cfg.entry_path = "main.flx";
        cfg.config_path = "fluxa.toml";

        PointerByReference runtimeRef = new PointerByReference();
        Fluxa.Error error = new Fluxa.Error();

        int rc = f.fluxa_cabi_open(cfg, runtimeRef, error);
        if (rc != 0) {
            throw new RuntimeException(Native.toString(error.message));
        }

        Pointer runtime = runtimeRef.getValue();

        Fluxa.Message request = new Fluxa.Message();
        f.fluxa_cabi_message_init(request);

        byte[] label = "Fluxa".getBytes(StandardCharsets.UTF_8);

        f.fluxa_cabi_add_int(request, 42);
        f.fluxa_cabi_add_float(request, 3.5);
        f.fluxa_cabi_add_bool(request, 1);
        f.fluxa_cabi_add_str(request, label, label.length);
        f.fluxa_cabi_add_int_arr(request, new int[]{10, 20, 30}, 3);

        request.read();

        Fluxa.View input = new Fluxa.View();
        input.data = request.data;
        input.size = request.size;

        Fluxa.View output = new Fluxa.View();

        rc = f.fluxa_cabi_exchange(runtime, input, output, error);
        if (rc != 0) {
            throw new RuntimeException(Native.toString(error.message));
        }

        IntByReference id = new IntByReference();
        DoubleByReference value = new DoubleByReference();
        IntByReference enabled = new IntByReference();
        Fluxa.StrView resultLabel = new Fluxa.StrView();
        IntByReference count = new IntByReference();

        f.fluxa_cabi_get_int(output, 0, id);
        f.fluxa_cabi_get_float(output, 1, value);
        f.fluxa_cabi_get_bool(output, 2, enabled);
        f.fluxa_cabi_get_str(output, 3, resultLabel);
        f.fluxa_cabi_get_arr_count(output, 4, count);

        resultLabel.read();

        String labelText = new String(
            resultLabel.data.getByteArray(0, resultLabel.size),
            StandardCharsets.UTF_8
        );

        List<Integer> points = new ArrayList<>();

        for (int i = 0; i < count.getValue(); ++i) {
            IntByReference point = new IntByReference();
            f.fluxa_cabi_get_int_arr_value(output, 4, i, point);
            points.add(point.getValue());
        }

        System.out.println("id      = " + id.getValue());
        System.out.println("value   = " + value.getValue());
        System.out.println("enabled = " + (enabled.getValue() != 0));
        System.out.println("label   = " + labelText);
        System.out.println("points  = " + points);

        f.fluxa_cabi_message_free(request);
        f.fluxa_cabi_close(runtime);
    }
}
```

---

## JavaScript / Node.js

Node.js can bind the C ABI through a native FFI package such as `ffi-napi`, together with `ref-napi` and `ref-struct-di`.

```javascript
const ffi = require("ffi-napi");
const ref = require("ref-napi");
const Struct = require("ref-struct-di")(ref);

const voidPtr = ref.refType(ref.types.void);
const uint32 = ref.types.uint32;
const int32 = ref.types.int32;

const FluxaError = Struct({
  code: uint32,
  line: int32,
  context: ref.types.char.array(128),
  message: ref.types.char.array(1024),
});

const FluxaMessage = Struct({
  data: voidPtr,
  size: uint32,
  capacity: uint32,
});

const FluxaView = Struct({
  data: voidPtr,
  size: uint32,
});

const FluxaStrView = Struct({
  data: voidPtr,
  size: uint32,
});

const FluxaConfig = Struct({
  struct_size: uint32,
  abi_version: uint32,
  entry_path: ref.types.CString,
  config_path: ref.types.CString,
  dispatch_fn: ref.types.CString,
  max_frame_bytes: uint32,
  flags: uint32,
});

const lib = ffi.Library("./libfluxa_cabi", {
  fluxa_cabi_open: [
    "int",
    [
      ref.refType(FluxaConfig),
      ref.refType(voidPtr),
      ref.refType(FluxaError),
    ],
  ],
  fluxa_cabi_close: ["void", [voidPtr]],

  fluxa_cabi_message_init: ["void", [ref.refType(FluxaMessage)]],
  fluxa_cabi_message_free: ["void", [ref.refType(FluxaMessage)]],

  fluxa_cabi_add_int: ["int", [ref.refType(FluxaMessage), "int32"]],
  fluxa_cabi_add_float: ["int", [ref.refType(FluxaMessage), "double"]],
  fluxa_cabi_add_bool: ["int", [ref.refType(FluxaMessage), "int"]],
  fluxa_cabi_add_str: [
    "int",
    [ref.refType(FluxaMessage), voidPtr, "uint32"],
  ],
  fluxa_cabi_add_int_arr: [
    "int",
    [ref.refType(FluxaMessage), voidPtr, "uint32"],
  ],

  fluxa_cabi_exchange: [
    "int",
    [
      voidPtr,
      ref.refType(FluxaView),
      ref.refType(FluxaView),
      ref.refType(FluxaError),
    ],
  ],

  fluxa_cabi_get_int: [
    "int",
    [ref.refType(FluxaView), "uint32", ref.refType(ref.types.int32)],
  ],
  fluxa_cabi_get_float: [
    "int",
    [ref.refType(FluxaView), "uint32", ref.refType(ref.types.double)],
  ],
  fluxa_cabi_get_bool: [
    "int",
    [ref.refType(FluxaView), "uint32", ref.refType(ref.types.int)],
  ],
  fluxa_cabi_get_str: [
    "int",
    [ref.refType(FluxaView), "uint32", ref.refType(FluxaStrView)],
  ],
  fluxa_cabi_get_arr_count: [
    "int",
    [ref.refType(FluxaView), "uint32", ref.refType(ref.types.uint32)],
  ],
  fluxa_cabi_get_int_arr_value: [
    "int",
    [
      ref.refType(FluxaView),
      "uint32",
      "uint32",
      ref.refType(ref.types.int32),
    ],
  ],
});

const cfg = new FluxaConfig();
cfg.struct_size = FluxaConfig.size;
cfg.abi_version = 1 << 16;
cfg.entry_path = "main.flx";
cfg.config_path = "fluxa.toml";
cfg.dispatch_fn = null;

const runtime = ref.alloc(voidPtr);
const error = new FluxaError();

let rc = lib.fluxa_cabi_open(cfg.ref(), runtime, error.ref());
if (rc !== 0) {
  throw new Error("fluxa_cabi_open failed");
}

const request = new FluxaMessage();
lib.fluxa_cabi_message_init(request.ref());

lib.fluxa_cabi_add_int(request.ref(), 42);
lib.fluxa_cabi_add_float(request.ref(), 3.5);
lib.fluxa_cabi_add_bool(request.ref(), 1);

const label = Buffer.from("Fluxa", "utf8");
lib.fluxa_cabi_add_str(request.ref(), label, label.length);

const points = Buffer.alloc(12);
points.writeInt32LE(10, 0);
points.writeInt32LE(20, 4);
points.writeInt32LE(30, 8);

lib.fluxa_cabi_add_int_arr(request.ref(), points, 3);

request.read();

const input = new FluxaView();
input.data = request.data;
input.size = request.size;

const output = new FluxaView();

rc = lib.fluxa_cabi_exchange(
  runtime.deref(),
  input.ref(),
  output.ref(),
  error.ref()
);

if (rc !== 0) {
  throw new Error("fluxa_cabi_exchange failed");
}

const id = ref.alloc(ref.types.int32);
const value = ref.alloc(ref.types.double);
const enabled = ref.alloc(ref.types.int);
const resultLabel = new FluxaStrView();
const count = ref.alloc(ref.types.uint32);

lib.fluxa_cabi_get_int(output.ref(), 0, id);
lib.fluxa_cabi_get_float(output.ref(), 1, value);
lib.fluxa_cabi_get_bool(output.ref(), 2, enabled);
lib.fluxa_cabi_get_str(output.ref(), 3, resultLabel.ref());
lib.fluxa_cabi_get_arr_count(output.ref(), 4, count);

resultLabel.read();

const labelText = resultLabel.data
  .reinterpret(resultLabel.size)
  .toString("utf8");

const returnedPoints = [];

for (let i = 0; i < count.deref(); ++i) {
  const point = ref.alloc(ref.types.int32);

  lib.fluxa_cabi_get_int_arr_value(
    output.ref(),
    4,
    i,
    point
  );

  returnedPoints.push(point.deref());
}

console.log("id      =", id.deref());
console.log("value   =", value.deref());
console.log("enabled =", enabled.deref() !== 0);
console.log("label   =", labelText);
console.log("points  =", returnedPoints);

lib.fluxa_cabi_message_free(request.ref());
lib.fluxa_cabi_close(runtime.deref());
```

---

## Go

Go can call the C ABI through `cgo`.

```go
package main

/*
#cgo CFLAGS: -I/path/to/fluxa/src/cabi
#cgo LDFLAGS: -L/path/to/fluxa -lfluxa_cabi

#include <stdlib.h>
#include "fluxa_cabi.h"
*/
import "C"

import (
	"fmt"
	"unsafe"
)

func main() {
	entry := C.CString("main.flx")
	configPath := C.CString("fluxa.toml")

	defer C.free(unsafe.Pointer(entry))
	defer C.free(unsafe.Pointer(configPath))

	var runtime *C.fluxa_cabi_runtime
	var cabiErr C.fluxa_cabi_error

	cfg := C.fluxa_cabi_config{
		struct_size:     C.uint32_t(C.sizeof_fluxa_cabi_config),
		abi_version:     C.FLUXA_CABI_ABI_VERSION,
		entry_path:      entry,
		config_path:     configPath,
		dispatch_fn:     nil,
		max_frame_bytes: 0,
		flags:           0,
	}

	if C.fluxa_cabi_open(
		&cfg,
		&runtime,
		&cabiErr,
	) != C.FLUXA_CABI_OK {
		panic("fluxa_cabi_open failed")
	}

	defer C.fluxa_cabi_close(runtime)

	var request C.fluxa_cabi_message
	C.fluxa_cabi_message_init(&request)
	defer C.fluxa_cabi_message_free(&request)

	label := C.CString("Fluxa")
	defer C.free(unsafe.Pointer(label))

	points := []C.int32_t{10, 20, 30}

	C.fluxa_cabi_add_int(&request, C.int32_t(42))
	C.fluxa_cabi_add_float(&request, C.double(3.5))
	C.fluxa_cabi_add_bool(&request, 1)
	C.fluxa_cabi_add_str(&request, label, 5)

	C.fluxa_cabi_add_int_arr(
		&request,
		&points[0],
		C.uint32_t(len(points)),
	)

	input := C.fluxa_cabi_view{
		data: request.data,
		size: request.size,
	}

	var output C.fluxa_cabi_view

	if C.fluxa_cabi_exchange(
		runtime,
		&input,
		&output,
		&cabiErr,
	) != C.FLUXA_CABI_OK {
		panic("fluxa_cabi_exchange failed")
	}

	var id C.int32_t
	var value C.double
	var enabled C.int
	var resultLabel C.fluxa_cabi_str_view
	var count C.uint32_t

	C.fluxa_cabi_get_int(&output, 0, &id)
	C.fluxa_cabi_get_float(&output, 1, &value)
	C.fluxa_cabi_get_bool(&output, 2, &enabled)
	C.fluxa_cabi_get_str(&output, 3, &resultLabel)
	C.fluxa_cabi_get_arr_count(&output, 4, &count)

	labelText := C.GoStringN(
		resultLabel.data,
		C.int(resultLabel.size),
	)

	returnedPoints := make([]int32, int(count))

	for i := C.uint32_t(0); i < count; i++ {
		var point C.int32_t

		C.fluxa_cabi_get_int_arr_value(
			&output,
			4,
			i,
			&point,
		)

		returnedPoints[int(i)] = int32(point)
	}

	fmt.Println("id      =", int32(id))
	fmt.Println("value   =", float64(value))
	fmt.Println("enabled =", enabled != 0)
	fmt.Println("label   =", labelText)
	fmt.Println("points  =", returnedPoints)
}
```

Typical Linux execution:

```bash
LD_LIBRARY_PATH=/path/to/fluxa go run .
```

---

## C#

C# can call the shared library directly through P/Invoke.

```csharp
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

class FluxaExample
{
    const int FLUXA_CABI_OK = 0;
    const uint FLUXA_CABI_ABI_VERSION = 1u << 16;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    struct FluxaConfig
    {
        public uint struct_size;
        public uint abi_version;

        [MarshalAs(UnmanagedType.LPStr)]
        public string entry_path;

        [MarshalAs(UnmanagedType.LPStr)]
        public string config_path;

        public IntPtr dispatch_fn;
        public uint max_frame_bytes;
        public uint flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct FluxaMessage
    {
        public IntPtr data;
        public uint size;
        public uint capacity;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct FluxaView
    {
        public IntPtr data;
        public uint size;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct FluxaStrView
    {
        public IntPtr data;
        public uint size;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    struct FluxaError
    {
        public uint code;
        public int line;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string context;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 1024)]
        public string message;
    }

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_open(
        ref FluxaConfig config,
        out IntPtr runtime,
        ref FluxaError error
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern void fluxa_cabi_close(IntPtr runtime);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern void fluxa_cabi_message_init(ref FluxaMessage message);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern void fluxa_cabi_message_free(ref FluxaMessage message);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_add_int(ref FluxaMessage message, int value);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_add_float(ref FluxaMessage message, double value);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_add_bool(ref FluxaMessage message, int value);

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_add_str(
        ref FluxaMessage message,
        byte[] data,
        uint size
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_add_int_arr(
        ref FluxaMessage message,
        int[] values,
        uint count
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_exchange(
        IntPtr runtime,
        ref FluxaView request,
        out FluxaView response,
        ref FluxaError error
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_int(
        ref FluxaView view,
        uint index,
        out int value
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_float(
        ref FluxaView view,
        uint index,
        out double value
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_bool(
        ref FluxaView view,
        uint index,
        out int value
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_str(
        ref FluxaView view,
        uint index,
        out FluxaStrView value
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_arr_count(
        ref FluxaView view,
        uint index,
        out uint count
    );

    [DllImport("fluxa_cabi", CallingConvention = CallingConvention.Cdecl)]
    static extern int fluxa_cabi_get_int_arr_value(
        ref FluxaView view,
        uint index,
        uint element,
        out int value
    );

    static void Main()
    {
        FluxaConfig cfg = new FluxaConfig
        {
            struct_size = (uint)Marshal.SizeOf<FluxaConfig>(),
            abi_version = FLUXA_CABI_ABI_VERSION,
            entry_path = "main.flx",
            config_path = "fluxa.toml",
            dispatch_fn = IntPtr.Zero,
            max_frame_bytes = 0,
            flags = 0
        };

        FluxaError error = new FluxaError();

        if (fluxa_cabi_open(ref cfg, out IntPtr runtime, ref error)
            != FLUXA_CABI_OK)
        {
            throw new Exception(error.message);
        }

        FluxaMessage request = new FluxaMessage();
        fluxa_cabi_message_init(ref request);

        byte[] label = Encoding.UTF8.GetBytes("Fluxa");

        fluxa_cabi_add_int(ref request, 42);
        fluxa_cabi_add_float(ref request, 3.5);
        fluxa_cabi_add_bool(ref request, 1);
        fluxa_cabi_add_str(ref request, label, (uint)label.Length);
        fluxa_cabi_add_int_arr(ref request, new[] {10, 20, 30}, 3);

        FluxaView input = new FluxaView
        {
            data = request.data,
            size = request.size
        };

        if (fluxa_cabi_exchange(
            runtime,
            ref input,
            out FluxaView output,
            ref error
        ) != FLUXA_CABI_OK)
        {
            throw new Exception(error.message);
        }

        fluxa_cabi_get_int(ref output, 0, out int id);
        fluxa_cabi_get_float(ref output, 1, out double value);
        fluxa_cabi_get_bool(ref output, 2, out int enabled);
        fluxa_cabi_get_str(ref output, 3, out FluxaStrView resultLabel);
        fluxa_cabi_get_arr_count(ref output, 4, out uint count);

        byte[] labelBytes = new byte[resultLabel.size];
        Marshal.Copy(
            resultLabel.data,
            labelBytes,
            0,
            (int)resultLabel.size
        );

        string labelText = Encoding.UTF8.GetString(labelBytes);

        List<int> returnedPoints = new List<int>();

        for (uint i = 0; i < count; ++i)
        {
            fluxa_cabi_get_int_arr_value(
                ref output,
                4,
                i,
                out int point
            );

            returnedPoints.Add(point);
        }

        Console.WriteLine($"id      = {id}");
        Console.WriteLine($"value   = {value}");
        Console.WriteLine($"enabled = {enabled != 0}");
        Console.WriteLine($"label   = {labelText}");
        Console.WriteLine(
            $"points  = [{string.Join(", ", returnedPoints)}]"
        );

        fluxa_cabi_message_free(ref request);
        fluxa_cabi_close(runtime);
    }
}
```

---

## Lua / LuaJIT

LuaJIT can access the C ABI through its built-in FFI module.

```lua
local ffi = require("ffi")

ffi.cdef[[
typedef struct fluxa_cabi_runtime fluxa_cabi_runtime;

typedef struct {
    uint32_t code;
    int32_t line;
    char context[128];
    char message[1024];
} fluxa_cabi_error;

typedef struct {
    void *data;
    uint32_t size;
    uint32_t capacity;
} fluxa_cabi_message;

typedef struct {
    const void *data;
    uint32_t size;
} fluxa_cabi_view;

typedef struct {
    const char *data;
    uint32_t size;
} fluxa_cabi_str_view;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *entry_path;
    const char *config_path;
    const char *dispatch_fn;
    uint32_t max_frame_bytes;
    uint32_t flags;
} fluxa_cabi_config;

int fluxa_cabi_open(
    const fluxa_cabi_config *config,
    fluxa_cabi_runtime **out_runtime,
    fluxa_cabi_error *error
);

void fluxa_cabi_close(fluxa_cabi_runtime *runtime);

void fluxa_cabi_message_init(fluxa_cabi_message *message);
void fluxa_cabi_message_free(fluxa_cabi_message *message);

int fluxa_cabi_add_int(fluxa_cabi_message *message, int32_t value);
int fluxa_cabi_add_float(fluxa_cabi_message *message, double value);
int fluxa_cabi_add_bool(fluxa_cabi_message *message, int value);

int fluxa_cabi_add_str(
    fluxa_cabi_message *message,
    const char *data,
    uint32_t size
);

int fluxa_cabi_add_int_arr(
    fluxa_cabi_message *message,
    const int32_t *values,
    uint32_t count
);

int fluxa_cabi_exchange(
    fluxa_cabi_runtime *runtime,
    const fluxa_cabi_view *request,
    fluxa_cabi_view *response,
    fluxa_cabi_error *error
);

int fluxa_cabi_get_int(
    const fluxa_cabi_view *view,
    uint32_t index,
    int32_t *out
);

int fluxa_cabi_get_float(
    const fluxa_cabi_view *view,
    uint32_t index,
    double *out
);

int fluxa_cabi_get_bool(
    const fluxa_cabi_view *view,
    uint32_t index,
    int *out
);

int fluxa_cabi_get_str(
    const fluxa_cabi_view *view,
    uint32_t index,
    fluxa_cabi_str_view *out
);

int fluxa_cabi_get_arr_count(
    const fluxa_cabi_view *view,
    uint32_t index,
    uint32_t *out_count
);

int fluxa_cabi_get_int_arr_value(
    const fluxa_cabi_view *view,
    uint32_t index,
    uint32_t element,
    int32_t *out
);
]]

local fluxa = ffi.load("./libfluxa_cabi.so")

local cfg = ffi.new("fluxa_cabi_config")
cfg.struct_size = ffi.sizeof(cfg)
cfg.abi_version = bit.lshift(1, 16)
cfg.entry_path = "main.flx"
cfg.config_path = "fluxa.toml"

local runtime = ffi.new("fluxa_cabi_runtime *[1]")
local err = ffi.new("fluxa_cabi_error")

if fluxa.fluxa_cabi_open(cfg, runtime, err) ~= 0 then
    error(ffi.string(err.message))
end

local request = ffi.new("fluxa_cabi_message")
fluxa.fluxa_cabi_message_init(request)

local points = ffi.new(
    "int32_t[3]",
    {10, 20, 30}
)

fluxa.fluxa_cabi_add_int(request, 42)
fluxa.fluxa_cabi_add_float(request, 3.5)
fluxa.fluxa_cabi_add_bool(request, 1)
fluxa.fluxa_cabi_add_str(request, "Fluxa", 5)
fluxa.fluxa_cabi_add_int_arr(request, points, 3)

local input = ffi.new("fluxa_cabi_view")
input.data = request.data
input.size = request.size

local output = ffi.new("fluxa_cabi_view")

if fluxa.fluxa_cabi_exchange(
    runtime[0],
    input,
    output,
    err
) ~= 0 then
    error(ffi.string(err.message))
end

local id = ffi.new("int32_t[1]")
local value = ffi.new("double[1]")
local enabled = ffi.new("int[1]")
local label = ffi.new("fluxa_cabi_str_view")
local count = ffi.new("uint32_t[1]")

fluxa.fluxa_cabi_get_int(output, 0, id)
fluxa.fluxa_cabi_get_float(output, 1, value)
fluxa.fluxa_cabi_get_bool(output, 2, enabled)
fluxa.fluxa_cabi_get_str(output, 3, label)
fluxa.fluxa_cabi_get_arr_count(output, 4, count)

local labelText = ffi.string(
    label.data,
    label.size
)

local returnedPoints = {}

for i = 0, tonumber(count[0]) - 1 do
    local point = ffi.new("int32_t[1]")

    fluxa.fluxa_cabi_get_int_arr_value(
        output,
        4,
        i,
        point
    )

    returnedPoints[#returnedPoints + 1] =
        tonumber(point[0])
end

print("id      =", tonumber(id[0]))
print("value   =", tonumber(value[0]))
print("enabled =", enabled[0] ~= 0)
print("label   =", labelText)
print(
    "points  = [" ..
    table.concat(returnedPoints, ", ") ..
    "]"
)

fluxa.fluxa_cabi_message_free(request)
fluxa.fluxa_cabi_close(runtime[0])
```

On Windows:

```lua
local fluxa = ffi.load("fluxa_cabi.dll")
```

---

## Host-language type mapping

| Fluxa C ABI | C | C++ | Python | Java | JavaScript | Go | C# | LuaJIT |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `int` | `int32_t` | `int32_t` | `c_int32` | `int` | `int32` | `int32` | `int` | `int32_t` |
| `float` | `double` | `double` | `c_double` | `double` | `double` | `float64` | `double` | `double` |
| `bool` | `int` | `bool/int` | `c_int` | `int/boolean` | `int/boolean` | `bool` | `bool/int` | boolean |
| `str` | pointer + size | `std::string` wrapper | bytes/str | `String` | Buffer/String | string | string | Lua string |
| `int arr` | `int32_t[]` | `std::vector<int32_t>` | ctypes array/list | `int[]` | Buffer/array | `[]int32` | `int[]` | table/cdata |
| `float arr` | `double[]` | `std::vector<double>` | ctypes array/list | `double[]` | Buffer/array | `[]float64` | `double[]` | table/cdata |
| `bool arr` | byte array | container/wrapper | ctypes array/list | byte/int array | Buffer/array | `[]byte` | `byte[]` | table/cdata |
| `str arr` | string views | string wrapper | list of str | `String[]` wrapper | string array | `[]string` wrapper | `string[]` wrapper | string table |

The host-language representation may differ, but the encoded `FXCB` frame is identical.

---

## Ownership reminder

The response returned by:

```c
fluxa_cabi_exchange()
```

is a borrowed view.

It remains valid until:

- the next exchange on the same `fluxa_cabi_runtime`, or
- the runtime is closed.

If the host needs to retain returned strings or arrays beyond the current exchange, copy them into host-owned memory.

Host-created `fluxa_cabi_message` buffers are released with:

```c
fluxa_cabi_message_free()
```

---

## Secure exchanges

The host-language binding does not change the secure-envelope model.

The application may use:

```c
fluxa_cabi_seal()
fluxa_cabi_unseal()
fluxa_cabi_exchange_sealed()
```

with a 32-byte shared key when the current build includes the libsodium security backend.

Security wraps the deterministic `FXCB` message. It does not add any new Fluxa value type and does not expose the key to the Fluxa program.
