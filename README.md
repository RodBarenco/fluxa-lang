# Fluxa-lang

This is the main source code repository for the Fluxa-lang programming language. It contains the interpreter, core toolchain, and base definitions.

### Why Fluxa-lang?

Fluxa-lang is in active development. The architecture is built to support the following core capabilities:

* **Embeddability:** Written in pure, dependency-light C99. The interpreter is designed to be easily embedded into larger host programs, utilizing memory arenas (`ASTPool`) for zero-fragmentation parsing and predictable cycle teardowns. Runtime behavior is configurable via `fluxa.toml`.

* **Predictability:** Fluxa-lang enforces a strict fail-fast execution model. Silent errors are by design non-existent. The `danger` block explicitly contains C FFI and runtime risks, gracefully accumulating errors in the `err` stack without crashing the host VM.

* **Prototypal Modularity:** State and behavior are grouped in zero-cost `Block` structures for fully isolated instantiation. Each `typeof` instance is independent with no shared state — no locks, no hidden coupling.

* **C Interoperability:** Built-in FFI via `libffi` allows calling compiled C libraries (e.g. `libm`) directly from Fluxa scripts inside `danger` blocks, with errors captured in the `err` stack.

* **State Persistence & Hot Reload:** Through the `prst` keyword, developers explicitly define which variables survive a reload. The runtime tracks dependencies via `PrstGraph` and preserves state via `PrstPool`. The `fluxa -dev` mode watches for file changes and reloads automatically. The Atomic Handover protocol (Sprint 8) will extend this to zero-downtime runtime replacement.

### Core Design Principles

Fluxa-lang is guided by five principles. Every language decision is anchored to at least one of them.

> Explicit > Implicit
>
> Simple > Complete
>
> Dynamic Runtime > Heavy Compilation
>
> Local Control > Global Magic
>
> Visible Error > Silent Error

### Where does it come from?

Fluxa-lang is designed and implemented in Rio de Janeiro, Brazil. It was born from a practical challenge in document processing and data analysis: the principles and rules for calculations change all the time. In these environments, having to halt the system and reinstantiate a complex web of states just to update a single formula is a frustrating and non-trivial bottleneck. Driven by curiosity and a "why not?" approach to memory management, Fluxa-lang was built to allow developers to hot-swap core logic on the fly, keeping the data and the engine running continuously.

### What's in a name?

"Fluxa-lang" refers to continuous flow. As a proper noun, it should be written in lower case with an initial capital: **Fluxa-lang**. Please avoid writing it as "FLUXA-LANG", as it is not an acronym.

### Quick Start

Currently, Fluxa-lang is built directly from source. Ensure you have a standard C99 compiler (GCC/Clang) and `make` installed. `libffi` is optional but recommended for C FFI support.
```bash
git clone https://github.com/RodBarenco/fluxa-lang.git
cd fluxa-lang
make build
./fluxa run tests/sprint7b.flx
```

**Automated Testing** — run the full PASS/FAIL test suite:
```bash
make test-runner
```

**Examples** — classic data structures and algorithms (Hash Maps, BST, Dijkstra, PageRank, and more):
```bash
make examples
```