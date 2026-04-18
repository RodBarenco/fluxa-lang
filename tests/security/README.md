# Fluxa Security Test Suite

**Threat Model:** *The Insider Out* — ex-employee or compromised third party
with knowledge of the system internals, old credentials, and flood tooling.

**Scope:** These tests validate the `FLUXA_SECURE=1` hardened build of the
Fluxa runtime. They are **not** run as part of the standard `make test-all`
suite — they require the `fluxa_secure` binary and simulate adversarial
conditions that would disrupt a normal development environment.

---

## Prerequisites

### 1. Build the hardened binary

```bash
make build-secure
# Produces: ./fluxa_secure
```

`FLUXA_SECURE=1` enables:
- **AC 1.2** — 50ms handshake timeout (standard build: 100ms)
- **AC 1.1** — Rate limiting: `invalid_burst` counter per rate window (1s)
- **AC 2.1** — RESCUE_MODE activates when > 100 invalid connections/sec
- **AC 2.2** — RESCUE_MODE auto-resets after 30s drain
- **AC 4.1** — Max 16 simultaneous connections (`IPC_MAX_CONNS`)
- **AC 4.2** — `active_conns` tracked; connection cap enforced before UID check

The standard `./fluxa` build has **none** of these — same binary size, zero
overhead. Security features are additive at compile time, not at runtime.

### 2. Install stress tools (for flood tests)

```bash
# For connection flood simulation
sudo apt install ncat socat

# Python 3 (for Slowloris simulation — already installed on most systems)
python3 --version
```

### 3. Understand the IPC socket model

Fluxa IPC uses **Unix domain sockets** (`/tmp/fluxa-<pid>.sock`, mode 0600).
This is intentional — it means:

- Network-level SYN/UDP floods **do not apply** (no TCP/IP exposure)
- The attack surface is **local processes** on the same host
- The realistic threat is: malicious process running as the same UID
  (compromised dependency, supply chain attack, insider script)
- The socket permission (0600) is the first defense — only the owner UID
  can connect at the filesystem level
- `FLUXA_SECURE` adds the second and third layers: rate limiting and
  connection caps that prevent resource exhaustion even by same-UID attackers

---

## Test Scenarios

### Scenario 1 — Handshake Timeout (AC 1.2)

**What it tests:** Connections that open the socket but never send data are
closed within 50ms (hardened) vs 100ms (standard). Prevents slow-connect
resource hold.

**Script:** `test_handshake_timeout.sh`

**Setup:**
```
[Attacker process]              [fluxa_secure -prod]
  connect()           ─────>    accept()
  (never sends)                 wait IPC_TIMEOUT_MS=50ms
                                close(client_fd)  ← aggressive teardown
```

**Expected result:**
- Connection held open ≤ 50ms then closed by server
- Runtime continues serving legitimate commands without delay
- No file descriptor leak

---

### Scenario 2 — Connection Flood / FD Exhaustion (AC 4.1)

**What it tests:** Attacker opens many simultaneous connections (Slowloris
style) to exhaust the server's file descriptor budget and prevent legitimate
commands from reaching the runtime.

**Script:** `test_fd_exhaustion.sh`

**Setup:**
```
[Attacker: 20 concurrent slow connections]
  for i in 0..19:
    socat UNIX:/tmp/fluxa-<pid>.sock -  ← opens but doesn't send

[Operator: tries to send fluxa status]
  fluxa_secure status  ← must succeed despite flood
```

**Expected result:**
- First 16 connections accepted (IPC_MAX_CONNS)
- Connections 17-20 dropped immediately (no FD hold)
- `fluxa_secure status` succeeds within 2 seconds
- `fluxa_secure logs` shows flood activity

---

### Scenario 3 — Invalid Packet Flood → RESCUE_MODE (AC 2.1)

**What it tests:** Attacker sends > 100 malformed/unauthenticated packets
per second. Runtime detects the flood, enters RESCUE_MODE, and logs the event.

**Script:** `test_rescue_mode.sh`

**Setup:**
```
[Attacker: rapid-fire invalid connections from wrong UID]
  while true:
    connect() → send garbage → close  (> 100/sec)

[Runtime]
  invalid_burst >= IPC_BURST_THRESHOLD (100)
  → rescue_mode = 1
  → stderr: "[fluxa] ipc: RESCUE_MODE activated"
```

**Note on Unix socket UID:** Since the socket is 0600, a different-UID
process cannot connect at the filesystem level. In practice, RESCUE_MODE
is triggered by same-UID processes sending invalid magic/opcode packets
at high rate. The test simulates this by sending packets with bad magic.

**Expected result:**
- RESCUE_MODE activates within 1 second of flood start
- `[fluxa] ipc: RESCUE_MODE activated` appears in stderr
- Legitimate commands still processed (UID check passes, burst from different counter)

---

### Scenario 4 — RESCUE_MODE Auto-Drain (AC 2.2)

**What it tests:** After RESCUE_MODE activates, it automatically clears after
`IPC_RESCUE_DRAIN_SEC` (30s) with no operator intervention needed.

**Script:** `test_rescue_drain.sh`

**Setup:**
```
1. Trigger RESCUE_MODE (via scenario 3)
2. Stop flood
3. Wait 30s
4. Verify: fluxa_secure status succeeds normally
5. Verify: stderr shows "[fluxa] ipc: RESCUE_MODE cleared after 30s drain"
```

**Expected result:**
- RESCUE_MODE clears automatically after 30s
- System returns to normal operation
- No manual reset required

---

### Scenario 5 — Maintenance Window Flood (AC 3.2 / Handover integrity)

**What it tests:** Attacker floods the IPC socket exactly during a `fluxa apply`
(Handover) operation. The handover must either complete atomically or roll back
to the previous state — never leave the system in a partial state.

**Script:** `test_handover_under_flood.sh`

**Setup:**
```
[T=0]  fluxa_secure run v1.flx -prod &   ← runtime starts
[T=1]  attacker flood starts             ← IPC flooded
[T=2]  fluxa_secure apply v2.flx         ← handover attempted
[T=3]  observe: runtime is on v1 or v2, never partial
[T=4]  attacker flood stops
```

**Expected result:**
- If handover completes: runtime runs v2, prst state preserved
- If handover fails (flood prevented ACK): runtime stays on v1
- Never: runtime in undefined state, crashed, or hanging

---

### Scenario 6 — Rate Limit Window Reset (AC 1.1)

**What it tests:** The invalid_burst counter resets every `IPC_RATE_WINDOW_SEC`
(1 second). A burst of 99 invalid connections in 1 second does NOT trigger
RESCUE_MODE. A burst of 101 does.

**Script:** `test_rate_limit.sh`

**Setup:**
```
Send 99 invalid packets in < 1s  →  no RESCUE_MODE
Wait 1s (window resets)
Send 99 more invalid packets     →  still no RESCUE_MODE
Send 100 invalid packets in < 1s →  RESCUE_MODE activated
```

**Expected result:**
- Sub-threshold bursts: no RESCUE_MODE, normal operation continues
- At-threshold burst: RESCUE_MODE activates, logged to stderr

---

## Running All Tests

```bash
# From the fluxa-lang root:
bash tests/security/run_security_tests.sh

# Run individual tests:
bash tests/security/test_handshake_timeout.sh
bash tests/security/test_fd_exhaustion.sh
bash tests/security/test_rescue_mode.sh
bash tests/security/test_rescue_drain.sh
bash tests/security/test_handover_under_flood.sh
bash tests/security/test_rate_limit.sh
```

---

## Definition of Done (DoD)

From the security spec, for this build to be considered production-ready:

| Criterion | Test | Status |
|---|---|---|
| Runtime does not freeze under connection flood | Scenario 2, 3 | `test_fd_exhaustion.sh` |
| Legitimate commands processed ≤ 2s under attack | Scenario 2 | `test_fd_exhaustion.sh` |
| RESCUE_MODE activates at threshold | Scenario 3 | `test_rescue_mode.sh` |
| RESCUE_MODE auto-clears after 30s | Scenario 4 | `test_rescue_drain.sh` |
| Handover is atomic under flood | Scenario 5 | `test_handover_under_flood.sh` |
| Rate window resets correctly | Scenario 6 | `test_rate_limit.sh` |
| Key generation correct + toml parsed | Scenarios 7-9 | `test_keygen.sh` |
| RESCUE_MODE sends no response to attacker | Scenario 10 | `test_silent_drop.sh` |
| Handshake timeout ≤ 50ms | Scenario 1 | `test_handshake_timeout.sh` |

---

## What FLUXA_SECURE Does NOT Protect Against

Being explicit about the threat model boundaries:

| Threat | Status | Reason |
|---|---|---|
| Network-level SYN/UDP flood | **Not applicable** | IPC is Unix socket, no TCP/IP exposure |
| SDR/radio signal injection | **Not applicable** | IPC has no wireless interface |
| Root-level local attacker | **Partial** | Root bypasses UID check; socket is 0600 |
| Kernel-level exploits | **Not in scope** | OS-level hardening (SELinux/AppArmor) |
| Crypto key theft | **Out of scope** | Covered by `std.crypto` + `prst` key rotation |
| Supply chain (dep compromise) | **Partial** | Same-UID protection applies |

---

## Architecture Notes

```
fluxa_secure -prod  (hardened binary)
│
├── Main thread: VM execution loop
│     └── Checks pending_set at safe points
│
└── IPC thread (ipc_server_thread)
      ├── select() with 200ms timeout
      ├── [FLUXA_SECURE] rate window reset every 1s
      ├── accept()
      ├── [FLUXA_SECURE] active_conns >= 16 → drop
      ├── check_peer_uid() → IPC_STATUS_ERR_AUTH
      ├── [FLUXA_SECURE] invalid_burst++ → RESCUE_MODE at 100/s
      └── dispatch() → PING / OBSERVE / SET / STATUS / LOGS / EXPLAIN
```

The security code adds **~100 lines** to `ipc_server.c` and **~20 bytes** to
the `IpcServer` struct. It has zero impact on the main VM execution thread —
all checks are in the IPC accept loop.

---

## Relevant Source Files

| File | What changed |
|---|---|
| `src/fluxa_ipc.h` | `IpcServer` struct: `invalid_burst`, `rescue_mode`, `active_conns`, `window_start` (all `#ifdef FLUXA_SECURE`). `IPC_TIMEOUT_MS`: 50ms secure / 100ms dev. Security constants: `IPC_MAX_CONNS`, `IPC_RATE_WINDOW_SEC`, `IPC_BURST_THRESHOLD`, `IPC_RESCUE_DRAIN_SEC` |
| `src/ipc_server.c` | `ipc_server_thread`: `#ifdef FLUXA_SECURE` selects hardened loop vs standard loop |
| `Makefile` | `build-secure` target: `FLUXA_SECURE=1`. `SECURE=1` variable for `make SECURE=1 build` |

---

## Scenario 10 — Silent Drop in RESCUE_MODE

**What it tests:** Once RESCUE_MODE activates, the server sends **no response**
to invalid connections. From the attacker's perspective the connection times
out — they cannot distinguish "detected and blocked" from "server busy" or
"packet lost". This prevents adaptive attacks where the attacker adjusts
strategy based on the error code they receive.

**Script:** `test_silent_drop.sh`

**Behavior comparison:**

```
Before RESCUE_MODE:   attacker sends garbage → gets IPC_STATUS_ERR_MAGIC
After  RESCUE_MODE:   attacker sends garbage → timeout (no bytes sent)
After  RESCUE_MODE:   operator sends status  → gets IPC_STATUS_OK
```

**Implementation:** Two paths lead to silent drop:
1. `ipc_recv_timed()` returns -1 (short packet) → `return` with no send
2. Valid UID + RESCUE_MODE active + bad magic peek → `close()` with no send

The audit log (`[fluxa] ipc: RESCUE_MODE activated`) is written to stderr
(operator-visible only) — never sent back over the socket.

**Expected result:**
- 10.1 Before RESCUE_MODE: response received (ERR_MAGIC or empty)
- 10.2 After  RESCUE_MODE: timeout — zero bytes from server
- 10.3 Operator `fluxa status` still works in RESCUE_MODE
- 10.4 Audit log on server stderr only, not exposed to attacker

---

## Scenario 7 — Key Generation (`fluxa keygen`)

**What it tests:** `fluxa_secure keygen --dir <path>` generates the correct
files with correct sizes and permissions.

**Script:** `test_keygen.sh`

**Setup:**
```
fluxa_secure keygen --dir /tmp/fluxa_test_keys
```

**Expected result:**
- `signing.key`         — 64 bytes, mode 0400 (Ed25519 private key)
- `signing.pub`         — 32 bytes, mode 0444 (Ed25519 public key)
- `signing.fingerprint` — 64 hex chars + newline, mode 0444
- `ipc_hmac.key`        — 32 bytes, mode 0400 (HMAC-SHA512 secret)
- All files non-empty, deterministically sized
- Each run generates a DIFFERENT keypair (randomness verified)

---

## Scenario 8 — `[security]` toml parsing

**What it tests:** `fluxa.toml [security]` section is parsed correctly.
Paths are read, mode is applied, key material is never accepted inline.

**Script:** `test_keygen.sh` (combined with Scenario 7)

**Cases:**
- `mode = "strict"` + missing key file → prod startup fails with clear error
- `mode = "warn"`   + valid key files  → prod starts, logs `security mode=warn`
- `mode = "off"`                       → prod starts, no security log
- Key path with inline quotes stripped correctly
- Unknown mode value → warning logged, mode stays `off`

---

## Scenario 9 — Key file permission enforcement

**What it tests:** `fluxa_security_check` warns when key files have
overly permissive permissions (world-readable).

**Script:** `test_keygen.sh` (combined)

**Setup:**
```bash
chmod 644 signing.key   # make world-readable
fluxa_secure run main.flx -prod
```

**Expected result:**
- `[fluxa] security WARNING: key file ... has loose permissions (mode 0644)`
- Runtime still starts (warning, not fatal)
- Operator is clearly informed of the security misconfiguration
