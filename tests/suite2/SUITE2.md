# Fluxa Test Suite 2 — Edge Cases & Integration
**v0.13.3 | 7 sections | 70 cases**

**Status: All 7 sections passing (70/70).** Suite 2 tests the limits of the language under conditions that matter for real IoT and embedded deployments. Where Suite 1 validates that features work, Suite 2 validates that they *hold* under pressure — type collisions across handovers, GC under tight memory, hundreds of Block instances, deeply nested ownership chains, and realistic sensor loop patterns.

---

## Running

```bash
# Full suite
bash tests/suite2/run_suite2.sh

# Single section
bash tests/suite2/run_suite2.sh --section prst
bash tests/suite2/run_suite2.sh --section handover,gc

# Individual script (verbose output)
bash tests/suite2/s2_prst.sh
bash tests/suite2/s2_gc.sh --fluxa ./fluxa
```

The master runner exits `0` if all sections pass, `1` otherwise, making it `make`-friendly.

---

## Sections

### s2_prst.sh — Persistent State (10 cases)

Tests `prst` semantics under reload and handover conditions.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | `prst int` → `prst str` on handover | Type collision rejected (ERR_RELOAD) |
| 2 | Runtime mutation preserved across handover | `score=100`, v1 adds 50→150, v2 gets 150+1=151 |
| 3 | Three `prst` types (int, str, bool) across handover | All survive serialization |
| 4 | `prst dyn` with 5000 elements | Large payload, no OOM |
| 5 | `prst dyn` with mixed types (str, float, bool, int) | Heterogeneous serialization |
| 6 | 80 `prst` vars with `prst_cap=8` | PrstPool grows via realloc beyond initial cap |
| 7 | `prst dyn` containing Block instances | Block clones owned by dyn survive |
| 8 | Handover adding a new `prst` var | New var initializes correctly |
| 9 | Handover removing a `prst` var | No ghost state, remaining vars intact |
| 10 | 70 `prst` vars across handover | Exercises serialization at scale |

**Key invariant tested:** runtime mutations must survive handover but source edits must override. Implemented via `init_value` field in `PrstEntry` — the declared default at first run is serialized separately from the runtime value, so on reload the comparison detects source edits correctly.

---

### s2_handover.sh — Atomic Handover (10 cases)

Tests every outcome of the 5-step Atomic Handover protocol.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | Normal handover, prst mutation | Steps 1–5 complete, COMMITTED |
| 2 | Dry run failure — division by zero | ERR_DRY_RUN, Runtime A keeps control |
| 3 | Dry run failure — undefined variable | Handover aborted at step 3 |
| 4 | Parse error in new program | Fails at step 1 (Standby) |
| 5 | `prst int` → `prst str` type collision | Handover rejected |
| 6 | Add and remove `prst` simultaneously | `b` gone, `c` new, `a` preserved |
| 7 | 70 `prst` vars | Full serialization at scale |
| 8 | Large `prst dyn` (1000 elements) in pool | Migration with large payload |
| 9 | Mixed `prst` types (str, float, bool) | All types survive wire format |
| 10 | Runtime A output correct before handover | Output order validated |

---

### s2_gc.sh — Garbage Collector (10 cases)

Tests GC pin/unpin correctness and `free()` across all value types.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | 10k dyn created and freed in loop | No crash, no OOM |
| 2 | 1000 orphan dyn from fn calls | GC sweeps at safe points |
| 3 | `free(dyn)` with Block + arr + str inside | No double free |
| 4 | `prst dyn` across 100 danger safe points | Never collected (pin_count ≥ 1) |
| 5 | Live dyn after danger sweep | Pinned dyn intact, orphan freed |
| 6 | 500 create/free cycles | Stable memory over time |
| 7 | `free(arr)` | Stack-resident arr freed correctly |
| 8 | `free(str)` | Stack-resident str freed correctly |
| 9 | `free(prst x)` | Always a hard error (bypasses danger) |
| 10 | `dyn b = [a_dyn]` | dyn-in-dyn rejected at runtime |

**Key fix validated:** `NODE_FREE` now resolves via `resolved_offset` (set by resolver) before falling back to scope hash lookup, covering stack-resident variables that `HASH_FIND_STR` would miss.

---

### s2_dyn.sh — dyn Extreme Cases (10 cases)

Tests `dyn` at the boundaries of capacity, type safety, and ownership.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | `d[0]=1, d[99]=2, d[9999]=4` | Sparse auto-grow, nil gaps |
| 2 | `d[0]` reassigned int→str→bool→float | Type swap on same slot |
| 3 | 10,000 element dyn | Large capacity without crash |
| 4 | `dyn d = [arr]` then `arr[0] = 999` | arr deep copy — original independent |
| 5 | Block with arr field inserted into dyn | arr field isolated per clone |
| 6 | 3 clones, each mutated independently | All Block clones fully isolated |
| 7 | 100 Block instances inserted in loop | No aliasing across clones |
| 8 | `dyn d = []` then grow to 3 elements | Empty literal then grow |
| 9 | `dyn b = [a_dyn, 3]` | dyn-in-dyn rejected (literal path) |
| 10 | `dyn d = 8` | Parse error — bare value not allowed |

---

### s2_block.sh — Block + typeof (10 cases)

Tests Block isolation, method chains, and typeof semantics at scale.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | 200 typeof instances via dyn | All isolated, correct values |
| 2 | `add(10) → double() → add(5)` | Method chain: 10×2+5=25 |
| 3 | Instance mutated, root unchanged | typeof isolation |
| 4 | `typeof inst` (instance as source) | Rejected with error |
| 5 | Two typeof instances, same arr field | arr isolated per instance |
| 6 | Root Block method called 3 times | Root state accumulates correctly |
| 7 | 3 instances with prst id and name | Independent prst per instance |
| 8 | Block inserted in dyn, method called | Only clone mutated, original intact |
| 9 | Block method returns `kp * signal` | Computed return value correct |
| 10 | Block with prst int + count fields | Multi-field accumulator |

---

### s2_types_danger.sh — Type Enforcement + danger (10 cases)

Tests type errors in all contexts and danger edge cases.

| Case | Scenario | What it validates |
|---|---|---|
| 1 | `int x = 10; x = "wrong"` | Type error outside danger → abort |
| 2 | Same assignment inside `danger {}` | Captured in err, no abort |
| 3 | `1/0` inside danger | Division by zero captured |
| 4 | 40 errors in danger loop | err ring buffer (32 entries), no crash |
| 5 | `false && b` — right side skipped | Short-circuit && |
| 6 | `true \|\| b` — right side skipped | Short-circuit \|\| |
| 7 | Two sequential danger blocks | err cleared between each |
| 8 | `int arr v[3] = [1, "two", 3]` | arr type mismatch at element[1] |
| 9 | `int arr v[3]; v[0] = true` | arr assign type mismatch |
| 10 | `free(prst x)` inside danger | Hard error — bypasses danger capture |

---

### s2_embedded.sh — Resource Constraints (10 cases)

Simulates IoT hardware limits using `ulimit -v` and iteration counts. Cases 1–5 use memory limits. Cases 6–10 use behavioral patterns typical of embedded workloads.

| Case | Constraint | Scenario |
|---|---|---|
| 1 | 220 KB (`ulimit -v 225280`) | Basic loop — RP2040 simulation |
| 2 | 400 KB (`ulimit -v 409600`) | dyn with 100 elements — ESP32 |
| 3 | 220 KB | 3 `prst` vars with small gc/prst caps |
| 4 | 400 KB | Full handover — ESP32 sim |
| 5 | 400 KB | 10k create/free loop — GC prevents OOM |
| 6 | None | Sensor loop: `prst` accumulation over 100 ticks |
| 7 | None | State machine pattern using Block |
| 8 | None | `danger {}` inside tight loop, 1000 iterations |
| 9 | None | 3 dyn vars with `gc_cap=8` |
| 10 | None | Script mode: no PrstPool overhead |

Memory limit cases use `ulimit -v` which is available on Linux. On macOS/BSD where `ulimit -v` has no effect, cases 1–5 are automatically skipped with a `SKIP` marker (not counted as failures).

---

## Bugs Found and Fixed

Suite 2 uncovered three bugs in the runtime that were not caught by Suite 1:

### Bug: Handover prst reload using wrong comparison baseline

**Symptom:** `prst int score = 100` with `score` mutated to 150 at runtime. After handover to a new version (same declaration), v2 started at `100` instead of `150`.

**Root cause:** The reload heuristic compared the new source initializer (`100`) against the current runtime value (`150`). Since they differed, it assumed the user changed the source — and used `100`. It couldn't distinguish "user changed declaration" from "runtime mutated value".

**Fix:** Added `init_value` field to `PrstEntry` — the declared default at first run, serialized in the handover wire format alongside the runtime value. On reload, comparison is `new_src_init` vs `init_value` (the original default). If they match → runtime mutated → keep runtime value. If they differ → user edited source → use new source. Also added `dry_run` guard to `NODE_ASSIGN` pool sync so Dry Run mutations don't pollute the pool before the real run.

**Wire format version:** bumped from `1001` to `1001` (breaking change from `1000`).

### Bug: `free()` couldn't find stack-resident variables

**Symptom:** `free(d)`, `free(s)`, `free(v)` all returned "undefined variable" even for variables that were readable via `print`.

**Root cause:** `NODE_FREE` searched only `HASH_FIND_STR(rt->scope.table, ...)`. Top-level variables in script mode and function locals are stack-resident — their values live in `rt->stack[resolved_offset]`, not in the scope hash table.

**Fix:** Resolver now sets `resolved_offset` on `NODE_FREE` nodes (same as for `NODE_IDENTIFIER`). Runtime checks stack slot first, then Block instance scope, then current frame scope, then global table — matching `rt_get` lookup order.

### Bug: `free(prst x)` inside `danger {}` was silently ignored

**Symptom:** `free(prst counter)` inside `danger {}` returned no error.

**Root cause:** `rt_error()` inside a danger block pushes to `err_stack` without setting `had_error`. The prst check used `rt_error()` — so inside danger, the error was captured silently and execution continued.

**Fix:** `free(prst x)` now writes directly to `stderr` and sets `rt->had_error = 1`, bypassing the danger capture mechanism. This matches the documented invariant: "cannot free prst variable in any mode".

---

## Makefile Integration

```makefile
test-suite2:
	@bash tests/suite2/run_suite2.sh

test-suite2-prst:
	@bash tests/suite2/s2_prst.sh

test-suite2-handover:
	@bash tests/suite2/s2_handover.sh

test-suite2-gc:
	@bash tests/suite2/s2_gc.sh

test-suite2-dyn:
	@bash tests/suite2/s2_dyn.sh

test-suite2-block:
	@bash tests/suite2/s2_block.sh

test-suite2-types:
	@bash tests/suite2/s2_types_danger.sh

test-suite2-embedded:
	@bash tests/suite2/s2_embedded.sh

test-all: test test-suite2
```

---

## What Is Not Tested (by design)

Features not yet implemented are excluded rather than marked as SKIP:

- `prst Block` — Block instances as persistent state
- Cycle detection in PrstGraph — dependency cycles between prst vars
- `prst` inside Block methods (only `prst` at top-level and Block-field level)
- Docker-based torture tests (see `tests/integration/` for future container scenarios)
- `std.flxthread` concurrency — not yet implemented
