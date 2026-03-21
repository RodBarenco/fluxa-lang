# Fluxa-lang
This is the main source code repository for the Fluxa-lang programming language. It contains the interpreter, core toolchain, and base definitions.

### Why Fluxa-lang?
Fluxa-lang is in early active development (v0.1). The architecture is being built to support the following core capabilities:

* **Embeddability (Active):** Written in pure, dependency-light C99. The interpreter is designed to be easily embedded into larger host programs, utilizing memory arenas (`ASTPool`) for zero-fragmentation parsing and predictable cycle teardowns.
* **Predictability (Active):** Fluxa-lang enforces a strict fail-fast execution model. Silent errors are by design non-existent. Future updates will introduce `danger` blocks to explicitly contain C FFI and I/O risks.
* **State Persistence (Roadmap):** Through the planned `prst` keyword, developers will explicitly define which variables survive a hot-reload, allowing runtime logic updates without resetting the host application's state.
* **Prototypal Modularity (Roadmap):** Bypassing complex object-oriented overhead, state and behavior will be grouped in zero-cost `Block` structures for isolated instantiation.

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
Fluxa-lang is designed and implemented in Rio de Janeiro, Brazil. It was born from a practical challenge in document processing and data analysis: the principles and rules for calculations change all the time. In these environments, having to halt the system and reinstantiate a complex web of states just to update a single formula is a frustrating and non-trivial bottleneck. Driven by curiosity and a "why not?" approach to memory management, Fluxa-lang was built to allow developers to hot-swap core logic principles on the fly, keeping the data and the engine running seamlessly.

### What's in a name?
"Fluxa-lang" refers to continuous flow. As a proper noun, it should be written in lower case with an initial capital: "Fluxa-lang". Please avoid writing it as "FLUXA-LANG", as it is not an acronym.

### Quick Start
Currently, Fluxa-lang is built directly from source. Ensure you have a standard C99 compiler (GCC/Clang) and `make` installed.

```bash
git clone [https://github.com/RodBarenco/fluxa-lang.git](https://github.com/RodBarenco/fluxa-lang.git)
cd fluxa-lang
make build
./fluxa run tests/sprint2.flx