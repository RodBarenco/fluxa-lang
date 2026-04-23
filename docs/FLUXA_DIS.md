# fluxa dis — Fluxa Program Disassembler

**v0.13.x — stable**

`fluxa dis` is a standalone static analysis command. It parses and resolves
a Fluxa program without executing it, then writes a human-readable report
file (`<program>.dis`) covering seven sections:

1. **AST structure** — every node: type, source line, `warm_local` flag, `resolved_offset`.
2. **Warm path forecast** — PROMOTABLE or COLD-LOCKED per function, cold bytes/read vs warm bytes/read.
3. **Hot path bytecode** — VM instructions for `while`/`if` bodies in warm functions.
4. **Call order** — call graph, recursive calls, mutual recursion (DFS), topological order.
5. **prst fork** — persistent variables, declared types, dependency graph.
6. **Execution paths** — per-function tier summary: Tier 0/1/2 eligibility, bytes/read, TCO.
7. **Statistics** — AST nodes, functions, WarmProfile budget, VM eligibility.
4. **Execution path summary** — per function: which tier it is likely to reach,
   memory touched per read (bytes), and the bytecode VM eligibility for hot paths.

---

## Usage

```bash
fluxa dis <file.flx>                   # writes <file>.dis in current dir
fluxa dis <file.flx> -proj <dir>       # reads fluxa.toml for prst context
fluxa dis <file.flx> -o <output.txt>   # explicit output path
fluxa dis <file.flx> --json            # machine-readable JSON output
```

The command always exits 0 unless the file fails to parse (exit 1).
It never executes user code. No IPC, no FFI, no file I/O beyond reading
the source and writing the output file.

---

## Output format — `<program>.dis`

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  fluxa dis — fib.flx
  Sprint 11 | v0.10
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

── 1. AST ───────────────────────────────────────────────────────

  fn fib(int n) int                              line 1
  ├─ PARAM      n            int   offset=0  warm_local=1
  ├─ IF                                          line 2
  │  ├─ BINARY  <=           int   n, 1
  │  └─ RETURN               int   n
  └─ RETURN                  int                 line 3
     └─ BINARY  +            int
        ├─ CALL fib          int   arg: BINARY - n 1
        └─ CALL fib          int   arg: BINARY - n 2

  [top-level]                                    line 5
  ├─ VAR_DECL   r            int   offset=1  warm_local=0
  │  └─ CALL    fib          int   arg: INT_LIT 32
  └─ FUNC_CALL  print        nil   arg: IDENT r

── 2. Warm Path Forecast ────────────────────────────────────────

  fn fib
    params:   n → int  (stable)
    locals:   (none beyond params)
    forecast: PROMOTABLE — all params and locals have single observed type
    tier:     1 (warm) after 2 stable calls
    cost:     9B/read  (WarmSlot 1B + stack 8B)
    note:     recursive — same fn_node* key across all frames ✓

── 3. prst Fork ─────────────────────────────────────────────────

  mode: SCRIPT  (no prst declarations)
  prst vars: 0
  dependencies: none

  If this file were run with -proj and prst vars were added:
    removing a prst var would atomically invalidate:
      → the variable itself
      → any fn that read it (registered in PrstGraph)
      → any prst var whose value depended on it

── 4. Execution Path Summary ────────────────────────────────────

  fn fib
    tier 0 (cold):   calls 1–4          418B/read (0 prst vars)
    tier 1 (warm):   calls 5+           9B/read   after promotion
    tier 2 (hot VM): not applicable     (no while/if compiled to bytecode)
    TCO:             yes — tail position detected at line 3

  [top-level]
    execution:       cold path (not inside fn scope, warm_local=0)
    prst mode:       SCRIPT — PrstPool not instantiated

── 5. Statistics ────────────────────────────────────────────────

  source lines:        10
  AST nodes:           14
  functions:           1   (1 promotable, 0 cold-locked, 0 block methods)
  Blocks:              0
  prst vars:           0
  warm candidates:     1 / 1  (100%)
  WarmProfile budget:  1 × 276B = 276B  (of 8704B max)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## prst Fork — detailed example

When `prst` vars and a `PrstGraph` are present (PROJECT mode):

```
── 3. prst Fork ─────────────────────────────────────────────────

  mode: PROJECT
  prst vars: 3

  prst int   tick    = 0      declared: main    line 1
  prst float temp    = 0.0    declared: Sensor  line 8
  prst bool  running = true   declared: main    line 2

  Dependency graph (who reads whom):
    tick    ← read by: update(), report()
    temp    ← read by: Sensor.read(), report()
    running ← read by: main loop

  Fork — if variable were removed:

    removing tick:
      → tick dies
      → update() invalidated (read tick)
      → report() invalidated (read tick)
      → 2 functions would restart from scratch on next reload

    removing temp:
      → temp dies
      → Sensor.read() invalidated
      → report() invalidated (also read temp)

    removing running:
      → running dies
      → main loop body invalidated (sole reader)
      → entire program execution interrupted atomically
```

---

## Warm Path Forecast — type inference rules

The static forecast uses conservative inference:

| Pattern | Forecast |
|---|---|
| All params typed `int` or `float`, no dyn locals | PROMOTABLE |
| Any param or local typed `dyn` | COLD-LOCKED (type not stable) |
| Function body accesses `prst` var | COLD-LOCKED (prst read goes through pool) |
| Function calls Block method | NOTE: callee is cold; caller unaffected |
| Recursive function | PROMOTABLE if base param is typed scalar |
| TCO target same function | PROMOTABLE — same WarmFunc slot |
| TCO target different function | each function profiled independently |
| > 32 functions in program | functions beyond slot 32 → cold fallback noted |
| > 256 locals in one function | slot wrap noted — may reduce promotion rate |

---

## Implementation notes

`fluxa dis` reuses the existing pipeline through the Resolver:

```
Lexer → Parser → Resolver → [dis output]
                             (no eval, no runtime)
```

The Resolver already computes:
- `resolved_offset` per node
- `warm_local` per identifier
- `in_func_depth` context
- `resolver_has_prst()` → SCRIPT vs PROJECT mode

The dis command reads the resolved AST and the PrstGraph (if PROJECT mode)
and formats them into the output file. No new runtime infrastructure needed.

Output file is written to the same directory as the input file, or to the
path specified with `-o`. If the output path is not writable, error to stderr
and exit 1.

---

## File naming

```
fib.flx          → fib.dis
tests/bench.flx  → tests/bench.dis
fluxa dis fib.flx -o report.txt  → report.txt
fluxa dis fib.flx --json         → fib.dis.json
```

---

## JSON output schema (`--json`)

```json
{
  "version": "0.10",
  "sprint": 11,
  "source": "fib.flx",
  "mode": "SCRIPT",
  "functions": [
    {
      "name": "fib",
      "line": 1,
      "return_type": "int",
      "params": [{"name": "n", "type": "int", "offset": 0, "warm_local": true}],
      "locals": [],
      "nodes": 8,
      "tco": true,
      "warm_forecast": "PROMOTABLE",
      "warm_tier_bytes": 9,
      "cold_tier_bytes": 18,
      "vm_eligible": false
    }
  ],
  "blocks": [],
  "prst_vars": [],
  "prst_graph": [],
  "stats": {
    "source_lines": 10,
    "ast_nodes": 14,
    "functions": 1,
    "warm_candidates": 1,
    "warm_profile_bytes": 276,
    "warm_profile_max": 8704
  }
}
```
