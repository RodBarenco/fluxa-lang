# Fluxa Windows minimal

Build on Linux with MinGW-w64:

```sh
make build-windows
```

On native Windows, use an **MSYS2 MinGW64 shell**. The Makefile detects
`OS=Windows_NT`, switches the compiler to `gcc` and looks for dependencies
under `/mingw64`:

```sh
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-raylib \
  mingw-w64-x86_64-zlib \
  mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-libsodium \
  mingw-w64-x86_64-curl

make build-windows-essential
```

No prefix overrides are needed with the standard MSYS2 MinGW64 installation.
Run from the MinGW64 shell, not from the MSYS shell or Windows Command Prompt.

Named library profiles produce separate executables:

```sh
make build-windows-graph
make build-windows-image
make build-windows-strings
make build-windows-json2
make build-windows-fs
make build-windows-time
make build-windows-sound
make build-windows-sqlite
make build-windows-crypto
make build-windows-httpc
make build-windows-https
```

The result is `fluxa.exe`, a console executable supporting:

```text
fluxa.exe run <file.flx>
fluxa.exe explain <file.flx>
```

## Included standard libraries

The Windows profile uses an explicit allowlist in the Makefile. It does not
inherit libraries discovered by the host `pkg-config`.

- `std.math`
- `std.csv`
- `std.json`
- `std.json2`
- `std.strings`
- `std.pid`
- `std.libdsp` (native C backend)
- `std.image` (stub backend)
- `std.infer` (stub backend)

Additional named profiles:

- `graph`: functional Raylib backend. Requires Raylib compiled for MinGW.
- `image`: functional Raylib codec plus zlib metadata support. Requires Raylib
  and zlib compiled for MinGW.
- `fs`: native Windows/MinGW filesystem operations.
- `time`: MinGW clock and sleep implementation. Winpthreads is linked
  statically, so no `libwinpthread-1.dll` needs to be shipped.
- `sound`: functional miniaudio/WASAPI backend using the vendored
  `vendor/miniaudio.h`. Winpthreads is linked statically.

Stub backends expose their normal API but report that the optional native
backend is unavailable for operations which require it.

## Excluded standard libraries

These libraries are not compiled into the minimal profile:

- POSIX or pthread based: `std.time`, `std.fs`, `std.flxthread`, `std.cache`,
  `std.websocket`, `std.wserver`, `std.sound`
- Linux specific: `std.i2c`
- Need a Windows-target dependency build: `std.crypto`, `std.sqlite`,
  `std.serial`, `std.zlib`, `std.httpc`, `std.https`, `std.mqtt`, `std.mcpc`,
  `std.mcps`, `std.pg`
- Need additional Windows port or validation: `std.libv`, `std.http`,
  `std.mcp`, `std.graph`

Attempting to import an excluded library produces the existing
"not compiled in" runtime error.

## External Windows dependencies

Raylib, zlib, SQLite, libsodium and curl must be compiled for the Windows
target. Native Linux packages are intentionally rejected.

```sh
make build-windows-graph WINDOWS_RAYLIB_PREFIX=/opt/mingw-raylib
make build-windows-image \
  WINDOWS_RAYLIB_PREFIX=/opt/mingw-raylib \
  WINDOWS_ZLIB_PREFIX=/opt/mingw-zlib
make build-windows-sqlite WINDOWS_SQLITE_PREFIX=/opt/mingw-sqlite
make build-windows-crypto WINDOWS_SODIUM_PREFIX=/opt/mingw-sodium
make build-windows-httpc WINDOWS_CURL_PREFIX=/opt/mingw-curl
make build-windows-https WINDOWS_CURL_PREFIX=/opt/mingw-curl
```

Each prefix must contain `include/` and `lib/`. For a static curl build, pass
its complete dependency list:

```sh
make build-windows-https \
  WINDOWS_CURL_PREFIX=/opt/mingw-curl \
  WINDOWS_CURL_LDFLAGS="-L/opt/mingw-curl/lib -lcurl ..."
```

After all three dependencies are available, this creates one runtime carrying
the entire requested set:

```sh
make build-windows-essential \
  WINDOWS_RAYLIB_PREFIX=/opt/mingw-raylib \
  WINDOWS_ZLIB_PREFIX=/opt/mingw-zlib \
  WINDOWS_SQLITE_PREFIX=/opt/mingw-sqlite \
  WINDOWS_SODIUM_PREFIX=/opt/mingw-sodium \
  WINDOWS_CURL_PREFIX=/opt/mingw-curl
```

## Excluded runtime services

The minimal entrypoint intentionally excludes `-dev`, `-prod`, Unix IPC,
observe/set/logs/status, atomic handover, runtime binary update, native Fluxa
threads and C FFI. Those services depend on POSIX process, socket, watcher or
threading APIs and require a separate Windows backend.
