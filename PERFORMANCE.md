# Fluxa — Performance Baseline

## Sprint 4.b — Final Result

**Benchmark:** 10 million iterations of a `while` loop with integer arithmetic.

```fluxa
int i = 0
int total = 0
while i < 10000000 {
    total = total + i
    i = i + 1
}
print(total)
```

**Hardware Environment:**
- **CPU:** 13th Gen Intel® Core™ i5-13420H × 12
- **OS:** Pop!_OS 22.04 LTS
- **Compiler:** GCC 11.x (with `-O3 -march=native`)

**Performance Results:**

| Runtime       | Average Time | Result Consistency |
|---------------|--------------|--------------------|
| **Fluxa 4.b** | **~0.25s** | ±0.005s            |

---

## Methodology

The benchmark is measured using the system `time` utility:

```bash
time ./fluxa run tests/bench.flx
```

- **Execution:** Average of 5 consecutive runs.
- **Verification:** Expected output `49999995000000` must be consistent.

---

## Implemented Optimizations (Sprints 4 & 4.b)

| Step | Technique | Impact/Purpose |
|-------|---------|-------|
| 1 | `-O3` C Optimization | Initial baseline reduction (3.3s → 1.1s) |
| 2 | Bytecode VM | Flat instruction array for hot paths |
| 3 | Inline Caching | Caching `ScopeEntry*` to avoid repeated lookups |
| 4 | Name Resolution | Pre-resolving symbols to eliminate string hashes in `eval` |
| 5 | Loop Fast Path | $O(1)$ seed/sync mechanism for loop variables |
| 6 | Computed Gotos | GCC extension to minimize branch misprediction |
| 4.b-1 | Pre-computed `seed_vars` | Moves AST traversal from runtime to the Resolver pass |
| 4.b-2 | Direct Stack Access | VM uses `rt->stack[offset]` directly, bypassing the scope system |

---

## Hot Path Architecture

1. **NODE_WHILE Detection:** The Resolver has already computed `seed_vars[]` — a flat list of `(name, offset)`.
2. **SEEDING $O(1)$:** For each entry in `seed_vars`, the system performs `scope[name] = stack[offset]` once before the loop.
3. **BYTECODE LOOP (Computed Gotos):**
   - `OP_LOAD`:  `stack[offset]` → VM stack (direct memory access).
   - `OP_STORE`: VM stack → `stack[offset]` (direct memory access).
4. **SYNC $O(1)$:** Final state is written back to the scope only after the loop terminates.

**Hash Table Access:** The `uthash` system is accessed **zero times** during the 10M iterations.

---

## Expected Degradation Budget

| Sprint | Feature | Est. Degradation | Technical Reason |
|--------|---------------|---------------------|--------|
| 5 | Blocks / typeof | ~10-15% | Instance-level scopes — `seed_vars` needs to track active instance context. |
| 6 | danger / err | ~0% | Flag-based wrappers — zero impact on execution hot path. |
| 7 | Hot reload / prst | ~0% (Normal) | Dependency graph is consulted only on file change events. |
| 9 | std.flxthread | ~0% (Per-thread) | Each thread maintains an isolated `Runtime` instance. |
| 10 | Stdlib | ~5-10% | Calls to C-based stdlib functions introduce slow-path transitions. |

**Total Budgeted Overhead:** ~20-30%
**Target Performance (Post-Sprint 10):** < 0.35s on current hardware.

---

## Future Performance Hazards

- **Sprint 5 (Blocks):** Fast path currently assumes a flat global/local scope. Method calls will require `scope_ptr` redirection to the instance scope.
- **Sprint 7 (Hot Reload):** Lazy dependency resolution is mandatory. Any per-iteration graph check would destroy the 0.25s baseline.
- **Sprint 9 (Threading):** Thread-local Runtimes are required to avoid global lock contention during execution.


#