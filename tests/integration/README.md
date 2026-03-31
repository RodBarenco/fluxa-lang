# Fluxa — Integration Tests

Simulation-based validation of the **Atomic Handover Protocol** (Sprint 8).

These tests go beyond unit tests: they simulate real deployment conditions —
IoT containers with persistent volumes, process crashes mid-handover, corrupted
snapshots, and watchdog resets — to confirm that the handover protocol is safe
for use in production edge environments.

---

## Structure

```
tests/integration/
├── run_all.sh              ← Master runner (both scenarios)
├── Dockerfile              ← Multi-stage image: builds fluxa + runs tests
├── docker-compose.yml      ← Container orchestration with persistent NVS volume
├── scenario1/
│   └── run.sh              ← Scenario 1: Simple IoT (normal handover conditions)
├── scenario2/
│   └── run.sh              ← Scenario 2: Drone / Critical Edge (fault injection)
└── fixtures/
    ├── v1/main.flx         ← Version 1: 3 prst vars (x=100, modo=1, ciclo=0)
    ├── v2/main.flx         ← Version 2: same vars + new prst speed=2
    └── v2_remove_var/      ← Version 2 variant: removes 'modo' to test GC
```

---

## Quick Start

```bash
# Build the binary first
make

# Run both scenarios
make test-integration

# Run a single scenario
make test-integration-s1   # Scenario 1 only
make test-integration-s2   # Scenario 2 only

# Verbose output (shows individual asserts)
./tests/integration/run_all.sh --fluxa ./fluxa --verbose

# Full suite: unit tests + integration tests
make test-all
```

---

## Docker

The Docker setup simulates a real IoT deployment: the Fluxa runtime runs inside
a container, and the NVS (Non-Volatile Storage) is a named Docker volume that
survives container restarts — mimicking Flash memory on embedded hardware.

```
┌─────────────────────────────────┐
│  Container: fluxa-sim           │
│  (Ubuntu 24.04 + fluxa binary)  │
│                                 │
│  run_all.sh                     │
│    └── scenario1/run.sh         │
│    └── scenario2/run.sh         │
└──────────────┬──────────────────┘
               │ volume mount
               ▼
┌─────────────────────────────────┐
│  Docker Volume: fluxa-nvs       │
│  Mount path: /nvs               │
│                                 │
│  Simulates Flash / NVS memory.  │
│  Persists across container      │
│  restarts — used in Scenario 1  │
│  Case 3 to validate that prst   │
│  state survives a full restart. │
└─────────────────────────────────┘
```

```bash
cd tests/integration

# Build the simulation image (compiles fluxa inside Docker)
docker-compose build

# Run all scenarios
docker-compose run --rm fluxa-sim

# Run with verbose output
docker-compose run --rm fluxa-sim --verbose

# Run a single scenario
docker-compose run --rm fluxa-sim --scenario 1
docker-compose run --rm fluxa-sim --scenario 2

# Test NVS persistence across restarts:
# Run twice — the second run uses the same named volume
docker-compose run --rm fluxa-sim --scenario 1
docker-compose run --rm fluxa-sim --scenario 1

# Reset NVS state (wipe the volume)
docker-compose down -v
```

---

## Scenarios

### Scenario 1 — Simple IoT

**Goal:** Validate that the Atomic Handover works correctly under normal
conditions — the kind of environment you would find in a simple IoT device
running a sensor loop.

**Environment:**
- Fluxa runtime (local or containerized)
- Persistent NVS volume mounted at `/nvs` (simulates Flash)
- Two Fluxa programs: `v1/main.flx` (running) and `v2/main.flx` (incoming)

**Execution flow:**
1. Start with v1 — initializes `x=100`, `modo=1`, `ciclo=0`
2. Runtime mutates `ciclo` to simulate real execution
3. Trigger handover: `fluxa handover v1/main.flx v2/main.flx`
4. Validate state after handover
5. Simulate container restart
6. Validate state survives the restart

#### Case 1 — Normal handover

Validates the happy path: the full 5-step protocol completes, prst variables
are preserved from Runtime A into Runtime B, and new variables from v2 are
initialized from source.

| Assertion | Expected |
|-----------|----------|
| `x` after handover | `100` (preserved from Runtime A) |
| `modo` after handover | `1` (preserved from Runtime A) |
| `ciclo` after handover | `1` (mutated value preserved) |
| `speed` in v2 | `2` (new var, initialized from source) |
| Handover state | `COMMITTED` |

#### Case 2 — Variable removal

Validates that removing a `prst` variable between versions triggers correct
GC behavior: the removed variable's memory is freed, remaining variables are
unaffected, and no invalid pointer is accessed after the handover.

`v2_remove_var/main.flx` drops `modo` but keeps `x` and `ciclo`. After
handover, `modo` must not appear in the pool, and the runtime must remain
fully operational.

| Assertion | Expected |
|-----------|----------|
| `x` present after removal of `modo` | `100` |
| `ciclo` present after removal of `modo` | `0` |
| Internal handover protocol suite | `ALL PASS` (no dangling pointers) |

#### Case 3 — Restart after handover

Validates NVS persistence: the prst state serialized during handover must
survive a full container restart and be correctly deserialized on cold boot.
This maps to the RP2040 `HANDOVER_MODE_FLASH` path where state is written to
Flash before rebooting into the new firmware.

| Assertion | Expected |
|-----------|----------|
| `test-reload` (3 successive applies) | `ALL PASS` |
| `fluxa explain` lists prst vars | `x`, `modo`, `ciclo`, `speed` visible |
| NVS checkpoint file created | exists at `$NVS_DIR/prst_snapshot.bin` |
| `x` on cold-start re-execution | `100` |

---

### Scenario 2 — Drone / Critical Edge (Fault Injection)

**Goal:** Validate runtime resilience under the worst possible conditions —
process crashes mid-handover, corrupted binary snapshots, watchdog resets.
This scenario enforces the global safety criterion: the system must **never**
start with a partially-applied state.

**Environment:**
- Fluxa runtime
- Shell-based fault injection (no external tools required)
- Python3 for crafting malformed binary snapshots

**Execution flow:**
1. Start with v1 and initialize prst state
2. Begin handover to v2
3. Inject a fault at a critical point
4. Restart / recover
5. Validate that state is consistent — either fully old or fully new, never mixed

#### Case 1 — Kill during handover

Injects `SIGKILL` into the handover process ~20ms after launch. Because the
5-step protocol takes ~5–20ms end-to-end, the kill can land at any step
(Standby, Migration, Dry Run, Switchover, or Cleanup). Runtime A must remain
untouched in all cases.

On a real device, a power cut or watchdog expiry can happen at any moment
during the upgrade sequence. The invariant — Runtime A is never modified
during a handover attempt — must hold unconditionally.

| Assertion | Expected |
|-----------|----------|
| Runtime executes v1 after kill | operational, correct output |
| Internal handover protocol suite after kill | `ALL PASS` |

#### Case 2 — Memory corruption (checksum poisoning)

Crafts two malformed binary snapshots and verifies the deserializer rejects
both without crashing:

- **Case 2a:** correct snapshot size but wrong `magic` (`0xDEADBEEF` instead
  of `0xF10A8888`) and invalid FNV-32 checksums for pool and graph.
- **Case 2b:** a snapshot truncated to 8 bytes — valid enough to look like a
  header start but missing all data.

This exercises the `handover_deserialize_state()` validation path:
`magic` check → version check → checksum verification. A corrupted snapshot
must be detected and discarded; the runtime must not dereference any pointer
derived from untrusted data.

| Assertion | Expected |
|-----------|----------|
| Runtime rejects snapshot with bad magic | no crash, no state applied |
| Internal handover suite after corruption attempt | `ALL PASS` |
| Runtime handles 8-byte truncated snapshot | no `SIGSEGV` / `SIGABRT` |

#### Case 3 — Watchdog reset

Sends `SIGTERM` to a Fluxa process running a 100,000-iteration loop. Unlike
`SIGKILL` (Case 1), `SIGTERM` allows partial cleanup — this tests that
whatever cleanup happens does not leave the prst pool in an inconsistent state
for the next cold start.

The key property validated here is **determinism**: the same input must always
produce the same output after recovery. Non-deterministic output signals state
contamination between runs.

| Assertion | Expected |
|-----------|----------|
| Runtime produces output after SIGTERM | non-empty |
| Two consecutive runs produce identical output | `run1 == run2` |

#### Case 4 — Partial write (truncated snapshot)

Simulates Flash write interruptions by creating snapshots truncated at 5
different byte offsets (4, 8, 16, 24, 36 bytes). Each truncation point
corresponds to a different field boundary in `HandoverSnapshotHeader`:

| Truncation | Cuts off at |
|------------|-------------|
| 4 bytes    | after `magic` |
| 8 bytes    | after `version` |
| 16 bytes   | after both checksums |
| 24 bytes   | after `pool_size` + `graph_size` |
| 36 bytes   | after counts + `cycle_count_a` |

In each case, the snapshot declares `pool_size=256` and `graph_size=64` that
are never actually delivered. The deserializer must detect the mismatch and
abort — not attempt to read beyond the buffer.

| Assertion | Expected |
|-----------|----------|
| No truncation causes `SIGSEGV` (exit 139) | all exit codes != 139 |
| No truncation causes `SIGABRT` (exit 134) | all exit codes != 134 |
| Internal handover suite after all truncations | `ALL PASS` |

---

## Global Acceptance Criterion

The Sprint 8 handover is considered **safe for critical edge use** only if:

- Every prst variable that should survive a handover does survive
- No crash occurs under any fault injection condition
- No inconsistent state is ever observable (partial old + partial new)
- Recovery after any fault is deterministic: same input produces same output
- The internal `test-handover` suite passes after every fault injection test

If any test generates an unexpected value, a crash signal, or non-deterministic
output — **the Atomic Handover is not safe for production.**

---

## Dependencies

- `bash` >= 4.0
- `python3` (Scenario 2, Case 2 — binary snapshot construction)
- `fluxa` binary (`make`)

No external tools required to run locally. Docker is optional.

---

## CI Integration

```yaml
# .github/workflows/fluxa.yml
- name: Build
  run: make

- name: Unit tests
  run: ./tests/run_tests.sh ./fluxa

- name: Integration tests
  run: make test-integration
```
