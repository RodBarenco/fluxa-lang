# Fluxa — Performance Baseline

## Sprint 4.c

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
- **Compiler:** GCC 11.x (with `-O2`)

**Performance Results:**

| Runtime       | Average Time | Result Consistency |
|---------------|--------------|--------------------|
| **Fluxa 4.c** | **~0.16s**   | ±0.005s            |

---

## Methodology

The benchmark is measured using the system `time` utility:
```bash
time ./fluxa run tests/bench.flx
```

- **Execution:** Average of 5 consecutive runs.
- **Verification:** Expected output `49999995000000` must be consistent.

---

## Implemented Optimizations (Sprints 4 to 4.c)

| Step | Technique | Impact/Purpose |
|------|-----------|----------------|
| 1 | `-O2` C Optimization | Initial baseline reduction (3.3s → 1.1s) |
| 2 | Name Resolution | Pre-resolving symbols to integer stack offsets, eliminating `uthash` from `eval` |
| 3 | Loop Fast Path | O(1) seed/sync mechanism for loop variables (scope↔stack bridge) |
| 4 | Inline Cache | `ScopeEntry*` cached per instruction, bypassing repeated hash lookups |
| 5 | Computed Gotos | GCC extension (`&&LABEL`) to minimize branch misprediction |
| 4.c-1 | Register-Based VM | 3-address instructions, completely replacing `VMStack` push/pops |
| 4.c-2 | Unified Loop Compilation | Condition and body compiled into a single contiguous `Chunk` |
| 4.c-3 | Register-Mapped Stack | VM registers (`R[a]`) act as direct pointers to `rt->stack` |
| 4.c-4 | Pointer Fast-Paths | Inline `VAL_INT` checks in the VM loop, eliminating struct copying and function call overhead |

---

## Hot Path Architecture

1. **NAME RESOLUTION:** The Resolver converts all local variable lookups from string keys into direct integer offsets (`resolved_offset`).
2. **UNIFIED COMPILATION:** When the interpreter hits a `NODE_WHILE`, it completely bypasses the AST walker. Both the condition and the body are compiled into a single bytecode `Chunk`.
3. **ZERO-COST BINDING:** The VM is invoked passing a pointer to the current `rt->stack`. The VM's registers (`R`) natively alias this array. There is no seeding, no synchronization, and no memory moving.
4. **DOUBLE DISPATCH ELIMINATION:** Inside the `vm_run` hot loop, arithmetic (`OP_ADD`, `OP_SUB`) and comparisons (`OP_LT`) check for integer operands inline. If true, the operation is performed natively by the CPU via pointers, skipping structural copies (16 bytes) and helper function calls.
5. **HASH TABLE BYPASS:** The `uthash` scope system is accessed **zero times** during the 10M iterations.

---

## Expected Degradation Budget

| Sprint | Feature | Est. Degradation | Technical Reason |
|--------|---------|-----------------|------------------|
| 5 | Blocks / typeof | ~10-15% | Instance-level scopes — stack frames will need to track active instance context. |
| 6 | danger / err | ~0% | Flag-based wrappers — zero impact on execution hot path. |
| 7 | Hot reload / prst | ~0% (Normal) | Dependency graph is consulted only on file change events. |
| 9 | std.flxthread | ~0% (Per-thread) | Each thread maintains an isolated `Runtime` instance. |
| 10 | Stdlib | ~5-10% | Calls to C-based stdlib functions introduce slow-path transitions. |

**Total Budgeted Overhead:** ~20-30%
**Target Performance (Post-Sprint 10):** < 0.21s on current hardware.

---

## Future Performance Hazards

- **Sprint 5 (Blocks):** Fast path currently assumes a flat global/local scope. Method calls will require frame redirection to the instance scope.
- **Sprint 7 (Hot Reload):** Lazy dependency resolution is mandatory. Any per-iteration graph check would destroy the 0.16s baseline.
- **Sprint 9 (Threading):** Thread-local Runtimes are required to avoid global lock contention during execution.