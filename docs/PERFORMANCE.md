# Fluxa — Performance Baseline

## Sprint 5 — Object System & Instance Scopes

**Benchmark 1 (Global Hot Path):** 10 million iterations of a `while` loop (integer arithmetic).
**Benchmark 2 (Method Hot Path):** 10M iterations inside a Block method + 1M iterations of instance member access.

**Hardware Environment:**
- **CPU:** 13th Gen Intel® Core™ i5-13420H × 12
- **OS:** Pop!_OS 22.04 LTS
- **Compiler:** GCC 11.x (with `-O3`)

**Performance Results:**

| Runtime | Scenario | Average Time |
|---------|----------|--------------|
| **Fluxa 5.0** | **Global Loop (bench.flx)** | **~0.16s** |
| **Fluxa 5.0** | **Block Methods (bench_block.flx)** | **~0.38s** |

---

## Methodology

The benchmark is measured using the system `time` utility:
```bash
time ./fluxa run tests/bench_block.flx
```

- **Scenario 1:** Pure local loop inside a method (10M iterations). Uses **Bytecode VM**.
- **Scenario 2:** Instance field accumulation via method calls (1M iterations). Uses **AST Slow Path**.
- **Verification:** Both results (`49999995000000` and `1000000`) must be correct.

---

## Implemented Optimizations (Sprints 4 to 5)

| Step | Technique | Impact/Purpose |
|------|-----------|----------------|
| 1 | Register-Based VM | 3-address instructions (`R[a] = R[b] + R[c]`) |
| 2 | Unified Loop Compilation | Condition + Body compiled into a single contiguous Chunk |
| 3 | Pointer Fast-Paths | Inline `VAL_INT` checks skipping struct copies and function calls |
| 4 | **Method Isolation (S5)** | Block methods use isolated scopes (`parent=NULL`) to maintain VM offset integrity |
| 5 | **Memcpy Optimization (S5)** | Call stack only copies active slots instead of the full 512-slot stack |
| 6 | **Instance Context (S5)** | `rt->current_instance` pointer is a zero-overhead branch for global code |

---

## Hot Path Architecture (Sprint 5)

1. **HYBRID EXECUTION:** - **VM Path:** Local variables inside methods are resolved to stack offsets. If a loop only touches locals, it triggers the 0.16s-grade Bytecode VM.
   - **AST Path:** Accessing `inst.field` triggers the member access evaluator. While slower than the VM, it has been optimized to bypass global hash lookups.
   
2. **ZERO-REGRESSION GLOBAL SCOPE:** The `current_instance` pointer in the `Runtime` struct is checked only during symbol resolution. For standard global loops, this pointer is `NULL`, allowing the CPU to perfectly predict the branch and maintain the 0.16s baseline.

3. **EFFICIENT INSTANTIATION:** `typeof` creates a `BlockInstance` with an independent `Scope`. Memory movement during method calls (`call_function`) is now proportional to the number of active variables, not the maximum stack size.



---

## Expected Degradation Budget (Updated)

| Sprint | Feature | Est. Degradation | Status |
|--------|---------|-----------------|--------|
| 6 | danger / err | ~0% | Pending |
| 7 | Hot reload / prst | ~0% (Normal) | Pending |
| 9 | std.flxthread | ~0% (Per-thread) | Pending |
| 10 | Stdlib | ~5-10% | Pending |

**Current Status:** Performance is **ahead of schedule**. The object system overhead was lower than the 15% budgeted, thanks to the method/local variable isolation in the Resolver.

---

## Future Performance Hazards

- **Sprint 7 (Hot Reload):** Must ensure that `prst` (persistence) lookups only happen during the "reload" phase, never inside the `vm_run` execution loop.
- **Sprint 9 (Threading):** Avoiding mutex contention in the `BlockInstance` scope when multiple threads access the same object.


---

## Stdlib HTTP services — sub-core tail latency (libmicrohttpd)

Findings from a 3M-vector classification service (`std.libv` KNN + `std.wserver`)
benchmarked at 900 req/s under a ≤1-CPU / ≤350 MB budget. The lesson is that at a
fractional-CPU budget, **P99 is a topology problem, not a compute problem** —
compute (VKN3 KNN + HTTP) is sub-millisecond; the tail lived everywhere else, and
isolation testing (stripping one layer at a time) found it.

| Symptom | Cause | Fix |
|---|---|---|
| Bimodal P99 (~23 ms stragglers) | split-plane KNN bound collapses in 14-d | VKN3 per-node AABB + best-first (exact, ~80× worst case) |
| P99 ≈ CFS period (~100 ms), CPU not saturated | too many MHD threads burn the CFS quota on context switches | small thread pool (`workers = 2–4`) |
| Low median, ~40–100 ms tail only over a bridged network | MHD leaves Nagle on; peer delayed-ACK | TCP_NODELAY on every connection |
| Full-stack P99 ≫ single-instance P99 | reverse proxy CPU-bound at its limit (queueing) | give the proxy a larger CPU share |

Method that worked: reproduce the resource cap locally (cgroup `cpu.max`), drive
open-loop keep-alive load, and **isolate** — remove the proxy, remove the CPU
limit, swap one knob at a time — rather than tuning blind. The decisive datapoint
was identical images differing only by the proxy: ~1.5 ms without it, ~80 ms with
it, which located the bottleneck unambiguously.
