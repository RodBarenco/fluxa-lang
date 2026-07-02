# wserver: optional direct-socket backend ([libs.wserver] direct = 1)

## What this is
An opt-in backend for `std.wserver` where `accept()`/`reply()` do raw HTTP/1.1
socket I/O **on the calling worker thread** — no MHD epoll pool, no cross-thread
queue, no condvar handoff. Removes the cross-thread scheduling latency that
dominates the p99 tail on a fractional core. The `.flx` script is unchanged:
the existing worker model (`serve(false)` + `ft.new` workers + `accept` loop)
maps directly onto it.

## Opt-in / default
- `fluxa.toml` → `[libs.wserver] direct = 1` enables it.
- **Default is 0** → the MHD path is used, byte-for-byte unchanged.
- Only applies to the plain worker model (`!inline_mode && !auto_scale`).

## Files changed
- `src/std/wserver/fluxa_std_wserver.h` — the direct backend (new functions:
  WsRd per-connection buffered reader, ws_http_read_request / ws_http_write_response,
  ws_direct_accept / ws_direct_send, listen-socket setup in ws_start_server,
  cleanup in ws_stop_server, branches in the accept dispatch and ws_finish_reply).
- `src/toml_config.h` — new field `wserver_direct` + parse of the `direct` key.

## Safety / orthogonality (verified)
- All direct paths are guarded by `srv->direct` / `req->direct` / `ws_direct`.
  With `direct = 0` none are taken → the MHD path is identical to before.
- `req->direct` (calloc => 0 for MHD requests; set to 1 only in ws_direct_accept)
  is the discriminator — NOT a file-descriptor sentinel. This fixes a latent
  collision: the MHD handler calloc's WsRequest, so `conn_fd` would be 0, and a
  `conn_fd >= 0` test would wrongly route MHD requests through the direct sender.
- Zero references to prst / PrstPool / PrstGraph / handover / dry_run / serialize
  in the wserver code → fully orthogonal to hot reload and the swap stages.
- `accept()` returns an int handle (or 0) and never pushes to `err` or touches
  `danger_depth` — identical contract to the MHD accept, so `danger { accept }`
  behaves the same. wserver is `import std`, not `import c`.
- Listen socket uses SO_REUSEADDR (+ SO_REUSEPORT where available), matching
  MHD's MHD_OPTION_LISTENING_ADDRESS_REUSE → same rebind semantics on reload.
- SIGPIPE is ignored (server-standard) only when a direct server starts.

## HTTP behavior
- HTTP/1.1 request parser (request line + headers + Content-Length body),
  response writer, and keep-alive. Keep-alive lets nginx reuse upstream
  connections (configure `keepalive` + `proxy_http_version 1.1` +
  `proxy_set_header Connection ""` in nginx to exploit it).
- Pipelining is handled: bytes received beyond the current request are kept in
  the per-connection WsRd buffer and consumed on the next accept() — no data
  loss and no worker hang on a pipelined client.

## Validation done in-sandbox (no Docker/k6 here)
- Build: clean, zero warnings (-std=c99 -Wall -Wextra -pedantic).
- direct=0 (MHD) regression: correctness + 2000-concurrent burst OK.
- direct=1: correctness (POST /fraud-score, GET /ready, bad JSON, 404),
  2000-concurrent bursts (0 failures), sequential keep-alive (nginx-style) 7/7,
  pipelined keep-alive 9/9, fd-leak check flat (4 → 4 fds over 2000 requests).
- Latency itself can only be confirmed on your k6 — this REMOVES the handoff
  (the diagnosed cause) rather than adding a mechanism.

## Suggested commit message
    wserver: optional direct-socket backend (no MHD handoff)

    Add [libs.wserver] direct=1: accept()/reply() do HTTP/1.1 socket I/O on
    the worker thread, eliminating the MHD epoll->queue->worker cross-thread
    handoff that dominates p99 on a fractional core. Opt-in; default 0 keeps
    the MHD path unchanged. Per-connection buffered reader handles keep-alive
    and pipelining. Orthogonal to prst/handover; danger/CLI contracts preserved.

---

# libv: FLUXA_KD_BUDGET — deployment-level KNN leaf budget (the real p99 fix)

## The measurement (what profiling showed)
Per-request handler cost: parse+extract 22µs, vector build 9µs, response 2µs —
and the KNN is ~99% of the request. The exact search cost is **bimodal**:
- near-manifold queries (typical traffic): 0.24 ms, ~650 leaves visited
- off-manifold queries (atypical transactions): up to **23 ms**, scanning ~76k
  of 125k leaves (~60% of the 3M points) — KD pruning collapses in 14-d.
A handful of these stragglers per second saturates a 0.45-core quota and
generates the queueing tail (p99 ~150 ms). This is why no threading change
(suspend/resume, direct sockets) moved the number.

## The fix
`FLUXA_KD_BUDGET=<n>`: read once at `libv.kd_load()`; applied as the leaf
budget **only when the script passes budget<=0**. Unset/0 = exact, as before.
The .flx script is unchanged (`kd_count(q,5,0)` stays); the cap is deployment
config (compose/Dockerfile env), like any engine tuning knob.

## Measured trade-off (400 queries/regime, this index)
- budget=2048: near-manifold **100.0% identical** to exact (count and decision);
  worst case capped at ~0.6 ms instead of 23 ms. Real test payloads (t0/t2)
  give identical counts exact-vs-2048.
- Off-manifold answers differ from exact (~66% decision agreement there).
  If the official test data is generator-sampled (near-manifold), detection
  score impact is zero; verify with the official TESTDATA before relying on it.

## Files changed
- `src/std/libv/fluxa_std_libv.h` — g_vknn_env_budget + getenv at kd_load +
  `if (budget <= 0) budget = g_vknn_env_budget;` in kd_count/kd_score.

## Deploy (Rinha kit)
    environment:
      - FLUXA_KD_INDEX=/app/kdtree.bin
      - FLUXA_KD_BUDGET=2048

## Suggested commit message
    libv: FLUXA_KD_BUDGET deployment default for kd_count/kd_score

    Profiling showed exact 14-d KNN is bimodal: 0.24ms near the data manifold
    but up to 23ms off it (pruning collapses), and these stragglers generate
    the entire p99 queueing tail under a fractional-CPU quota. The env var
    caps the leaf budget when the script passes 0, leaving scripts unchanged;
    budget=2048 is bit-identical to exact on typical traffic while capping
    the worst case at ~0.6ms.

---

# vknn v3 (VKN3): per-node AABB + best-first search — exact KNN, 80x worst case

## What the dotnet reference does (fksegundo/rinha-dotnet, analyzed)
Learned partition tree + **per-node bounding boxes** with box-distance pruning,
best-first traversal, AVX2 SoA leaf scan, FD-passing Rust LB, raw HTTP parser,
precomputed responses. The structural element is the **box bound**: a KD
split-plane bound uses ONE dimension and is nearly useless in 14-d.

## What changed in vknn.h
- Format VKN3: every VkNode carries int16 mn[14]/mx[14] (AABB of its subtree),
  80 B/node (~21 MB of nodes; index file 89 → 103 MB).
- Search is iterative best-first: children visited nearest-box-first, far child
  stacked with its bound and re-checked against the kth distance on pop; whole
  subtrees prune on the full 14-d box distance. EXACT results, same answers.
- `vk_count_stats(ix,q,k,budget,&leaves)` exposes leaves visited; budget (and
  FLUXA_KD_BUDGET) still cap leaf visits, but are no longer needed.
- `build_index.c` added (src/std/libv/): refs.bin (SoA: header, n*14 floats,
  n labels) -> VKN3. The old kdtree.bin (VKN2) is rejected at load (magic).

## Measured (3M refs, 400 queries/regime, this sandbox)
- near-manifold exact: 0.243 -> 0.204 ms (leaves 649 -> 197)
- off-manifold exact:  23.1  -> 0.290 ms (leaves 76,286 -> 356)  [80x]
- in-Fluxa kd_count: fixed 1.05 -> 0.065 ms; varied 3.56 -> 0.129 ms
- t0/t2 responses unchanged; full server suite passes (correctness, keep-alive,
  pipelining, 2000-burst x2, no fd leak).

## Deploy notes
- REBUILD THE INDEX:
      gcc -std=c99 -O2 -Wall -Wextra src/std/libv/build_index.c \
          -Isrc/std/libv -o build_index -lm
      ./build_index refs.bin kdtree.bin
- FLUXA_KD_BUDGET: leave UNSET (pure exact; no detection-score risk).
- RSS: ~118 MB warm (103 MB index). Give each API >=160 MB in compose
  (350 MB total budget: 160+160+30 for nginx fits).

## Suggested commit message
    libv/vknn: VKN3 index with per-node AABBs + best-first exact search

    A split-plane bound prunes on one dimension and collapses in 14-d:
    off-manifold exact queries scanned ~60% of the 3M points (23 ms) and
    generated the entire p99 tail. Per-node bounding boxes + best-first
    traversal keep the search exact while cutting the worst case 80x
    (0.29 ms); typical queries also improve (0.20 ms). Adds build_index.c
    (VKN3 builder) and vk_count_stats; budget/FLUXA_KD_BUDGET remain as an
    optional cap but are no longer required.

---

# THE TAIL FIX: thread count on a fractional core ([libs.wserver] workers)

## Reproduced locally (cgroup v2, cpu.max=45000/100000 = 0.45 core, open-loop keep-alive loadgen)
After VKN3 made the KNN cheap, k6 p99 barely moved (75 -> 71 ms). Reproducing
the 0.45-core quota locally showed the tail is NOT the KNN and NOT nginx — it is
**CFS throttle stalls driven by thread-count thrash** in the server itself
(p50 was already 0.46 ms; the tail was periodic ~tens-of-ms freezes).

Sweep at 450 req/s under the 0.45-core cap (MHD path, exact VKN3):
| workers | server threads | p99    | p99.9  |
|--------:|---------------:|-------:|-------:|
| 8 (old) | ~24            | 42.5ms | 141ms  |
| 1       | 4              | 2.6ms  | 38ms   |
| 4       | ~9             | 1.1ms  | 30ms   |
| **2**   | 5              | 0.68ms | 3.2ms  |
| **3**   | ~7             | 0.70ms | 8.0ms  |

At the full 900 req/s on a SINGLE 0.45-core instance, workers=3 holds
p50=0.40 / p99=0.72 / p99.9=8.8 / max=18 ms, 0 errors over 18k requests.

## Why
On a 0.45-core quota you can run at most ~0.45 threads' worth of CPU at once.
~24 runnable threads (MHD epoll pool + ft workers) burn the 45ms/100ms quota on
context switches and wakeups; once the quota is exhausted the whole process is
frozen until the next period -> tens-of-ms latency spikes. A small pool (2-3)
removes the thrash: the quota covers the actual work, no freeze, sub-ms p99.

## The fix (no code change)
Rinha kit fluxa.toml:
    [libs.wserver]
    workers = 3        # was 8; 2 is equally good on p99, 3 trims p99.9
Direct-socket mode stays OFF (it binds one worker per connection and starves
when connections > workers — 50% errors under load; epoll handles many conns
on few threads, which is what we want here).

## Combined result
VKN3 (exact KNN, no off-manifold stragglers) + workers=3 (no CFS thrash):
single 0.45-core instance, 900 req/s, **p99 ~0.7 ms** (was ~70 ms). With the
kit's 2 instances behind nginx (~450 req/s each) there is ample headroom.
Optional extra: enable nginx upstream keep-alive (proxy_http_version 1.1;
proxy_set_header Connection ""; upstream { keepalive 32; }).
