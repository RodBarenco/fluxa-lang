# Fluxa runtime for Windows

This directory contains the native Windows entrypoint, compatibility layer,
dependency preparation, and Windows-specific tests for **Fluxa Lang**.

Fluxa Lang and Fluxa Builder are separate projects:

- this repository builds and tests the Fluxa language runtime;
- Fluxa Builder consumes a verified packaged-runtime build and creates the
  final application launcher, FLXPKG, metadata, archives, and installers.

Building `fluxa-runtime.exe` here does not package an application. Conversely,
Fluxa Builder does not compile or reimplement the Fluxa language runtime.

## Supported runtime

The Windows runtime is a native x86-64 PE console executable. It supports:

```text
fluxa-runtime.exe run <file.flx>
fluxa-runtime.exe explain <file.flx>
fluxa-runtime.exe runtime info
```

The Windows entrypoint deliberately excludes Unix runtime services such as
Unix IPC, atomic handover, live runtime replacement, native Fluxa threads, and
C FFI.

The essential profile includes functional implementations of:

```toml
std.graph   = "1.0"
std.image   = "1.0"
std.strings = "1.0"
std.sqlite  = "1.0"
std.sound   = "1.0"
std.crypto  = "1.0"
std.json2   = "1.0"
std.fs      = "1.0"
std.httpc   = "1.0"
std.https   = "1.0"
```

It also retains the pure-C libraries from the Windows minimal profile:
`std.math`, `std.csv`, `std.json`, `std.pid`, and `std.libdsp`.

## Build environment

Use an **MSYS2 MinGW64 shell**, not the MSYS shell, Command Prompt, or a plain
PowerShell session.

Install the required packages:

```sh
pacman -S --needed \
  git make python \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-zlib \
  mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-libsodium \
  mingw-w64-x86_64-curl
```

Python 3 is required by `scripts/gen_lib_registry.py`. Git is required only
when the pinned static Raylib source is not present in `.deps`.

## Build targets and generated files

Run these commands from the repository root in an MSYS2 MinGW64 shell:

| Command | Generated file | Purpose |
| --- | --- | --- |
| `make build-windows-essential` | `fluxa-essential.exe` | Dynamic development runtime. It requires the corresponding MSYS2 third-party DLLs and is not a standalone distribution artifact. |
| `make build-windows-essential-static` | `fluxa-runtime.exe` | Public standalone Fluxa Lang runtime. Use it to run or explain `.flx` files on Windows. |
| `make build-windows-packaged` | `fluxa-runtime.exe` | Private standalone runtime for Fluxa Builder. Builder must register it with schema-v2 `runtime.json`; users must not execute it directly. |
| `make windows-test` | `fluxa-runtime.exe` | Rebuilds the public standalone runtime and runs the Windows dependency, language, and standard-library test gate. |

The two static build targets intentionally use the same output filename.
Running one after the other replaces the previous `fluxa-runtime.exe`. Check
which variant is present with:

```powershell
.\fluxa-runtime.exe runtime info
```

`Packaged: false` identifies the public Fluxa Lang runtime.
`Packaged: true` identifies the private Fluxa Builder runtime.

Generated executables are build artifacts in the repository root and are not
committed. Distribution folders, archives, `runtime.json`, application
launchers, and installers are produced or assembled by Fluxa Builder, not by
these Fluxa Lang Make targets.

## Shared-library development build

```sh
make build-windows-essential
```

This creates `fluxa-essential.exe`. It is useful for development inside the
MSYS2 environment, but it links the MSYS2 Raylib, curl, SQLite, libsodium, and
zlib packages dynamically. Copying this executable alone to another directory
is not a supported distribution.

Named development profiles remain available:

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

## Standalone runtime

```sh
make build-windows-essential-static
```

This creates `fluxa-runtime.exe` with all third-party libraries linked into the
PE. The executable may import Windows system DLLs, but must not import MinGW,
Raylib, curl, SQLite, libsodium, OpenSSL, zlib, or winpthreads DLLs.

Raylib needs special treatment. The MSYS2 `libraylib.a` can reference the
shared GLFW ABI and is therefore unsuitable for a truly standalone runtime.
`platform/windows/build-raylib-static.sh` fetches and verifies:

```text
raylib version: 6.0
commit: dbc56a87da87d973a9c5baa4e7438a9d20121d28
backend: PLATFORM_DESKTOP_GLFW
GLFW linkage: internal static module (USE_EXTERNAL_GLFW=FALSE)
graphics API: GRAPHICS_API_OPENGL_33
library type: STATIC
```

The GLFW module is compiled into `libraylib.a`; it does not add a
`glfw3.dll` runtime dependency. The backend and graphics API remain
overridable for development builds:

```sh
WINDOWS_RAYLIB_PLATFORM=PLATFORM_DESKTOP_WIN32 \
WINDOWS_RAYLIB_GRAPHICS=GRAPHICS_API_OPENGL_21 \
make build-windows-essential-static
```

The pinned source and generated prefix live under `.deps/` and are not
committed. Override their locations with:

```sh
WINDOWS_RAYLIB_SOURCE_DIR=/path/to/source
WINDOWS_RAYLIB_STATIC_PREFIX=/path/to/prefix
```

## Packaged runtime for Fluxa Builder

```sh
make build-windows-packaged
```

This creates the same standalone `fluxa-runtime.exe`, additionally compiled
with `FLUXA_PACKAGED_RUNTIME=1`.

The packaged runtime:

- allows the offline `runtime info` identity probe;
- accepts the authenticated `__fluxa_builder_run_v1` private launcher command;
- rejects public `run`, `explain`, and unrelated commands with exit code 126.

Fluxa Builder registers this binary with schema-v2 runtime metadata and places
it in an application as `.fluxa-runtime.exe`. The executable visible to the
end user is the Builder launcher, not this private runtime. Refer to the Fluxa
Builder repository for FLXPKG, launcher, persistence, export, signing, archive,
and installer behavior.

## Tests

Run the native Windows gate:

```sh
make windows-test
```

The target builds the public standalone runtime and verifies:

- only Windows system DLLs are imported;
- core functions, arrays, loops, Blocks, and `danger`;
- `std.strings` and `std.json2`;
- real Windows filesystem operations;
- real SQLite create/insert/query operations;
- real libsodium hashing;
- Raylib, image codec, and miniaudio backends;
- PNG encode and decode.

Network tests are opt-in because they require external connectivity:

```sh
FLUXA_WINDOWS_NETWORK_TESTS=1 make windows-test
```

On Windows, curl is configured to request the native CA store. A custom PEM
bundle can be selected with `CURL_CA_BUNDLE`.

The test fixtures and native PowerShell runner are under
`platform/windows/tests/`.

## Virtual machines and Mesa3D

`std.graph` requires a working OpenGL driver. Enabling VirtualBox 3D
acceleration is not sufficient when the Windows guest still reports
`Microsoft Basic Display Adapter` or `VirtualBox VESA BIOS`. Typical failures
are:

```text
GLAD: Cannot load OpenGL extensions
WGL: The driver does not appear to support OpenGL
```

Install the matching VirtualBox Guest Additions and use the `VBoxSVGA`
controller first. If the guest still has no usable OpenGL driver, Mesa3D can
provide an application-local fallback. Do not replace the system
`C:\Windows\System32\opengl32.dll`.

The validated fallback used the community
[`mesa-dist-win`](https://github.com/pal1000/mesa-dist-win/releases) 26.1.3
`release-mingw` archive:

```text
SHA-256: 80d5add64254c839b4c784bdab6a2b504e448675604b0fe54a9bce3c543303a7
```

These x64 files were placed beside `fluxa-runtime.exe`:

```text
dxil.dll
libgallium_wgl.dll
opengl32.dll
opengl32sw.dll
fluxa-runtime.exe.local
```

`opengl32sw.dll` is a second copy of Mesa's `opengl32.dll`.
`fluxa-runtime.exe.local` is a non-empty text file that enables
application-local DLL redirection for this executable name. If the runtime is
renamed, the `.local` filename must be renamed to match, for example
`fluxa.exe.local`.

This configuration was validated with both Mesa renderers:

```text
Renderer: D3D12 (Microsoft Basic Render Driver)
Renderer: llvmpipe (LLVM ..., 256 bits)
GLAD: OpenGL extensions loaded successfully
FBO: Framebuffer object created successfully
```

Mesa normally selects a renderer automatically. To force CPU rendering for
diagnosis:

```powershell
$env:GALLIUM_DRIVER = "llvmpipe"
$env:LIBGL_ALWAYS_SOFTWARE = "true"
.\fluxa-runtime.exe run main.flx
```

Mesa is an optional companion distribution, not linked into
`fluxa-runtime.exe`. It substantially increases the application size and
software rendering can be slow. Download Windows binaries from a trusted
release, verify the published checksum, preserve the accompanying licenses,
and prefer per-application deployment. Fluxa Builder must explicitly include
these companion files when producing a VM-compatible application bundle.

## HTTPS trust

`std.https` always verifies the peer certificate and hostname. The Windows
build enables libcurl's native-CA option. Environments using a private proxy or
private certificate authority can set:

```powershell
$env:CURL_CA_BUNDLE = "C:\path\to\organization-ca-bundle.pem"
```

Disabling TLS verification is not supported.
