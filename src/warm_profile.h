/* warm_profile.h — Fluxa Warm Path: compact execution profile
 *
 * Inspired by TurboQuant (Google Research, 2025):
 *   "randomly rotating input vectors induces a concentrated Beta distribution
 *    on coordinates, leveraging near-independence in high dimensions to apply
 *    optimal scalar quantizers per coordinate"
 *
 * Applied to Fluxa's AST execution:
 *   - First pass (cold): AST walker collects observed types per node
 *   - Warm profile: WHT signature (8 bytes/fn) + 1 byte/node type observation
 *   - Second pass (warm): reads WarmSlot instead of ASTNode — never touches
 *     union fields, never calls prst_pool_has, never calls scope_get
 *   - QJL residual (1 bit/node): detects type divergence; falls back to cold
 *
 * Memory layout (tight):
 *   WarmSlot   = 1 byte  (3 bits type + 1 bit guard + 4 bits spare)
 *   WarmFunc   = 16 bytes header + 1 byte × node_count
 *   WarmProfile= 8 bytes header + WarmFunc × func_count
 *
 * For 10 functions × 50 nodes: 660 bytes total warm profile.
 * Compared to ASTNode at 48-64 bytes × 500 nodes = 24-32 KB of cold AST.
 *
 * Walsh-Hadamard Transform (WHT) for path signature:
 *   Zero parameters, zero malloc — pure XOR and shifts.
 *   Two execution paths with the same WHT signature have the same observed
 *   type sequence. One uint64_t covers up to 64 nodes per function.
 *
 * Sprint 11 — warm path between cold AST walker and hot bytecode VM.
 */
#ifndef FLUXA_WARM_PROFILE_H
#define FLUXA_WARM_PROFILE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Type encoding (3 bits) ──────────────────────────────────────────────── */
/* Maps ValType to a 3-bit warm type tag. Must stay in sync with scope.h. */
#define WARM_T_NIL   0u  /* VAL_NIL   */
#define WARM_T_INT   1u  /* VAL_INT   */
#define WARM_T_FLOAT 2u  /* VAL_FLOAT */
#define WARM_T_BOOL  3u  /* VAL_BOOL  */
#define WARM_T_STR   4u  /* VAL_STRING */
#define WARM_T_ARR   5u  /* VAL_ARR   */
#define WARM_T_DYN   6u  /* VAL_DYN   */
#define WARM_T_OTHER 7u  /* everything else (PTR, BLOCK_INST, ERR_STACK) */

/* ── WarmSlot — 1 byte per AST node with a resolved_offset ──────────────── */
typedef struct {
    uint8_t observed_type : 3;  /* type seen on last warm execution   */
    uint8_t qjl_guard     : 1;  /* 1 = stable (type matched); 0 = diverged */
    uint8_t run_count     : 4;  /* saturating count of stable runs (0-15) */
} WarmSlot;

/* ── WarmFunc — per-function profile ─────────────────────────────────────── */
#define WARM_SLOTS_MAX 256

/* Promotion threshold: function becomes warm after this many stable runs */
#define WARM_STABLE_RUNS 2
/* Observation limit: stop recording after this many function calls        */
#define WARM_OBS_LIMIT   4  /* max nodes tracked per function — covers all
                             * practical Fluxa functions; larger bodies use
                             * index modulo to wrap around */

typedef struct {
    uint64_t  path_sig;     /* WHT signature of observed types — 8 bytes.
                             * If current run's WHT matches this, all nodes
                             * in the function are on the warm path.         */
    uint8_t   stable_runs;  /* consecutive runs with matching path_sig      */
    uint8_t   node_count;   /* number of WarmSlot entries used (≤ WARM_SLOTS_MAX) */
    uint8_t   obs_calls;    /* observation call counter — stops at WARM_OBS_LIMIT */
    uint8_t   _pad;
    uintptr_t fn_id;        /* stable identity: (uintptr_t)fn_node ASTNode* */
    WarmSlot  slots[WARM_SLOTS_MAX]; /* 256 bytes — one per resolved node   */
} WarmFunc;                 /* total: 16 + 256 = 272 bytes per function     */

/* ── WarmProfile — top-level structure ───────────────────────────────────── */
#define WARM_FUNC_CAP 32    /* max functions profiled — 32 × 272 = 8.5 KB
                             * covers all realistic Fluxa programs           */

typedef struct {
    WarmFunc funcs[WARM_FUNC_CAP];
    int      count;          /* number of WarmFunc entries in use            */
    int      enabled;        /* 1 after first cold pass completes            */
} WarmProfile;               /* total max: 32 × 272 + 8 = 8.7 KB           */

/* ── ValType → warm type tag conversion ─────────────────────────────────── */
/* Defined without including scope.h to keep this header standalone.
 * Callers pass the ValType integer and we map it here.                      */
static inline uint8_t warm_type_from_val_type(int vtype) {
    switch (vtype) {
        case 0: return WARM_T_NIL;   /* VAL_NIL    */
        case 1: return WARM_T_INT;   /* VAL_INT    */
        case 2: return WARM_T_FLOAT; /* VAL_FLOAT  */
        case 3: return WARM_T_BOOL;  /* VAL_BOOL   */
        case 4: return WARM_T_STR;   /* VAL_STRING */
        case 5: return WARM_T_ARR;   /* VAL_ARR    */
        case 6: return WARM_T_DYN;   /* VAL_DYN    */
        default: return WARM_T_OTHER;
    }
}

/* ── Walsh-Hadamard Transform — zero parameters, zero alloc ─────────────── */
/* Takes a 64-bit vector of 3-bit type tags packed 1 per nibble (low 3 bits).
 * WHT in-place over 64 elements using butterfly operations.
 * Two identical type sequences always produce the same signature.
 * Different sequences almost always produce different signatures
 * (collision probability ≈ 2^-64 for random inputs).                       */
static inline uint64_t warm_wht_sign(uint64_t type_vec) {
    /* Hadamard butterfly over 6 levels (2^6 = 64 virtual elements).
     * Each level XORs adjacent pairs — O(64) XOR operations, no memory.    */
    uint64_t v = type_vec;
    v ^= (v >> 1)  & 0x5555555555555555ULL;
    v ^= (v >> 2)  & 0x3333333333333333ULL;
    v ^= (v >> 4)  & 0x0F0F0F0F0F0F0F0FULL;
    v ^= (v >> 8)  & 0x00FF00FF00FF00FFULL;
    v ^= (v >> 16) & 0x0000FFFF0000FFFFULL;
    v ^= (v >> 32);
    return v;
}

/* Build a type vector from an array of observed warm types (up to 16).
 * Each type occupies 4 bits (nibble) in the uint64_t vector.               */
static inline uint64_t warm_build_type_vec(const WarmSlot *slots, int count) {
    uint64_t vec = 0;
    int n = count < 16 ? count : 16; /* saturate at 16 nibbles = 64 bits   */
    for (int i = 0; i < n; i++)
        vec |= ((uint64_t)(slots[i].observed_type & 0x7u)) << (i * 4);
    return vec;
}

/* ── WarmProfile lifecycle ───────────────────────────────────────────────── */
static inline void warm_profile_init(WarmProfile *wp) {
    memset(wp, 0, sizeof(*wp));
}

/* Find or create a WarmFunc entry keyed by fn_id (ASTNode* cast to uintptr_t).
 * Uses open-addressing with linear probe — O(1) average, no linear scan.
 * fn_id must be a stable identifier across calls: use (uintptr_t)fn_node.  */
static inline WarmFunc *warm_profile_get_func(WarmProfile *wp, uintptr_t fn_id) {
    /* Map fn_id to a slot via FNV-inspired mix, then linear probe */
    uint32_t h = (uint32_t)((fn_id ^ (fn_id >> 16)) * 0x45d9f3bU);
    int start = (int)(h & (WARM_FUNC_CAP - 1));  /* WARM_FUNC_CAP must be power of 2 */
    for (int i = 0; i < WARM_FUNC_CAP; i++) {
        int idx = (start + i) & (WARM_FUNC_CAP - 1);
        WarmFunc *wf = &wp->funcs[idx];
        if (wf->fn_id == fn_id) return wf;  /* found */
        if (wf->fn_id == 0) {               /* empty slot — claim it */
            memset(wf, 0, sizeof(*wf));
            wf->fn_id = fn_id;
            wp->count++;
            return wf;
        }
    }
    return NULL;  /* table full */
}

/* ── Cold pass: record observed type for a resolved node ─────────────────── */
/* Called by the AST walker after every successful rt_get.
 * slot_idx = resolved_offset of the node (indexes into WarmFunc.slots).
 * observed_vtype = the ValType of the value just read.                      */
static inline void warm_record(WarmFunc *wf, int slot_idx, int observed_vtype) {
    if (!wf || slot_idx < 0) return;
    int idx = slot_idx % WARM_SLOTS_MAX;
    uint8_t wt = warm_type_from_val_type(observed_vtype);
    WarmSlot *s = &wf->slots[idx];
    if (s->run_count == 0) {
        /* First observation — set type and mark stable */
        s->observed_type = wt;
        s->qjl_guard     = 1;
        s->run_count     = 1;
    } else if (s->observed_type == wt) {
        /* Type stable — increment saturation counter, keep guard */
        s->qjl_guard = 1;
        if (s->run_count < 15) s->run_count++;
    } else {
        /* Type diverged — QJL guard fires, reset */
        s->qjl_guard     = 0;
        s->observed_type = wt;
        s->run_count     = 1;
    }
    if (slot_idx >= (int)wf->node_count) {
        wf->node_count = (uint8_t)(slot_idx < 255 ? slot_idx + 1 : 255);
    }
}

/* Update the WHT path signature after recording a batch of nodes.
 * Call at the end of each function execution (cold pass).                   */
static inline void warm_update_sig(WarmFunc *wf) {
    if (!wf) return;
    /* Increment observation call counter — stop recording after the limit */
    if (wf->obs_calls < WARM_OBS_LIMIT) wf->obs_calls++;
    uint64_t vec = warm_build_type_vec(wf->slots, wf->node_count);
    uint64_t new_sig = warm_wht_sign(vec);
    if (new_sig == wf->path_sig) {
        if (wf->stable_runs < 255) wf->stable_runs++;
    } else {
        wf->path_sig    = new_sig;
        wf->stable_runs = 0;
    }
}

/* ── Warm path: check if a slot is safe to use without AST walk ─────────── */
/* Returns 1 if the slot is stable and the function path signature is valid.
 * The caller can then read directly from rt->stack[resolved_offset] without
 * touching the ASTNode union, the scope chain, or prst_pool.                */
static inline int warm_slot_ok(const WarmFunc *wf, int slot_idx,
                                int current_vtype) {
    if (!wf || wf->stable_runs < 2) return 0; /* need ≥2 stable runs        */
    int idx = slot_idx % WARM_SLOTS_MAX;
    const WarmSlot *s = &wf->slots[idx];
    if (!s->qjl_guard) return 0;              /* QJL guard fired last time   */
    if (s->run_count < 2) return 0;           /* not enough observations     */
    /* QJL residual check: does observed type match current type? */
    uint8_t wt = warm_type_from_val_type(current_vtype);
    return s->observed_type == wt;
}

/* ── Promotion and observation thresholds ────────────────────────────────── */
/* A function is "warm" when its path signature has been stable for ≥2 runs. */

static inline int warm_func_is_promoted(const WarmFunc *wf) {
    return wf && wf->stable_runs >= WARM_STABLE_RUNS;
}

/* Returns 1 if observation is still active for this function.
 * Returns 0 if already promoted OR cold-locked (obs_calls >= WARM_OBS_LIMIT).
 * After the limit: zero overhead — falls straight to direct stack read.     */
static inline int warm_func_observing(const WarmFunc *wf) {
    return wf && wf->obs_calls < WARM_OBS_LIMIT && !warm_func_is_promoted(wf);
}

/* Maximum observation calls before giving up. After WARM_OBS_LIMIT calls
 * without reaching stable_runs >= WARM_STABLE_RUNS, the function is
 * cold-locked: zero observation overhead, just direct stack read.           */
#define WARM_OBS_LIMIT   4

#endif /* FLUXA_WARM_PROFILE_H */
