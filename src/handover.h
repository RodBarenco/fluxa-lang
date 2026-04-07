/* handover.h — Fluxa Atomic Handover Protocol (Sprint 8)
 *
 * 5-step protocol for zero-downtime runtime replacement:
 *
 *   1. STANDBY      — Runtime B criado, programa parseado e resolvido
 *   2. MIGRATION    — PrstPool + PrstGraph serializados A → snapshot → B
 *   3. DRY_RUN      — Dry Run: B executes with dry_run=1
 *   4. SWITCHOVER   — Wait for safe point in A; atomic swap
 *   5. CLEANUP      — Runtime A destroyed after grace period
 *
 * Garantias de invariante:
 *   - Runtime A is NEVER modified during a handover attempt
 *   - Any failure in B keeps A active without corruption
 *   - dry_run suppresses stdout/FFI side-effects, not internal logic
 *   - ERR_HANDOVER gerado em A se B falhar em qualquer step
 *   - Multiple attempts do not accumulate invalid state
 *   - GC does not interfere: gc_collect_all() called before swap
 *   - danger does not mask critical failures: B err_stack is checked
 *
 * Versionamento (beta 0.001):
 *   v1.000 — first stable protocol version
 *   v1.xxx — compatible with v1.000 (mesmo major)
 *   v2.000 — breaking change; rejects snapshots v1.xxx
 *
 * RP2040 (264KB SRAM / 2MB Flash):
 *   Two parallel runtimes do not fit in SRAM.
 *   HANDOVER_MODE_FLASH: serialize → Flash → reboot → deserialize → dry_run
 *   Snapshot is flat binary without pointers — safe for Flash writes.
 */
#ifndef FLUXA_HANDOVER_H
#define FLUXA_HANDOVER_H

#include "pool.h"      /* ASTPool, ASTNode */
#include "runtime.h"   /* Runtime, PrstPool, PrstGraph, err.h, scope.h... */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ── Protocol version ─────────────────────────────────────────────────── */
#define FLUXA_HANDOVER_VERSION  1001u   /* v1.001 — wire format adds init_value */
#define FLUXA_HANDOVER_MAGIC    0xF10A8888u

/* ── Handover mode ────────────────────────────────────────────────────── */
typedef enum {
    HANDOVER_MODE_MEMORY,  /* dois runtimes em paralelo (x86/ARM64)   */
    HANDOVER_MODE_FLASH,   /* serialize→Flash→reboot→deserialize (RP2040) */
} HandoverMode;

/* ── Protocol state ─────────────────────────────────────────────────── */
typedef enum {
    HANDOVER_STATE_IDLE,
    HANDOVER_STATE_STANDBY,
    HANDOVER_STATE_MIGRATION,
    HANDOVER_STATE_DRY_RUN,
    HANDOVER_STATE_SWITCHOVER,
    HANDOVER_STATE_CLEANUP,
    HANDOVER_STATE_FAILED,
    HANDOVER_STATE_COMMITTED,
} HandoverState;

/* ── Result codes ────────────────────────────────────────────────── */
typedef enum {
    HANDOVER_OK = 0,
    HANDOVER_ERR_ALLOC,
    HANDOVER_ERR_PARSE,
    HANDOVER_ERR_RESOLVE,
    HANDOVER_ERR_SERIALIZE,
    HANDOVER_ERR_DESERIALIZE,
    HANDOVER_ERR_CHECKSUM,
    HANDOVER_ERR_DRY_RUN,
    HANDOVER_ERR_SAFE_POINT,
    HANDOVER_ERR_FLASH_WRITE,
    HANDOVER_ERR_FLASH_READ,
    HANDOVER_ERR_VERSION,
    HANDOVER_ERR_NULL_PROGRAM,
} HandoverResult;

/* ── Snapshot header ───────────────────────────────────────────────── */
/* Layout do buffer flat binary:
 *   [HandoverSnapshotHeader]
 *   [pool_size bytes  — PrstPool serializado]
 *   [graph_size bytes — PrstGraph serializado]
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pool_checksum;   /* FNV-32 do pool ANTES de serializar   */
    uint32_t graph_checksum;  /* FNV-32 do graph ANTES de serializar  */
    uint32_t pool_size;
    uint32_t graph_size;
    int32_t  pool_count;
    int32_t  graph_count;
    int32_t  cycle_count_a;   /* cycle_count de A no momento do snap  */
    uint8_t  _pad[4];
} HandoverSnapshotHeader;

/* ── Handover context ────────────────────────────────────────────────── */
typedef struct {
    HandoverMode   mode;
    HandoverState  state;
    HandoverResult last_result;

    Runtime *rt_a;      /* active — NEVER modified             */
    Runtime *rt_b;      /* candidate — discarded on failure     */
    ASTPool        *pool_b;
    ASTNode        *program_b;

    void          *snapshot;
    size_t         snapshot_size;

    int            safe_point_timeout_ms;  /* default 5000 */
    int            grace_period_ms;        /* default 100  */

    char           error_msg[512];
    long           rt_a_cycle_at_swap;

    /* Resulting pool after full handover — passed to runtime_apply */
    PrstPool       pool_after;
} HandoverCtx;

/* ── Human-readable strings ────────────────────────────────────────────────────── */
static inline const char *handover_state_str(HandoverState s) {
    switch (s) {
        case HANDOVER_STATE_IDLE:       return "IDLE";
        case HANDOVER_STATE_STANDBY:    return "STANDBY";
        case HANDOVER_STATE_MIGRATION:  return "MIGRATION";
        case HANDOVER_STATE_DRY_RUN:    return "DRY_RUN";
        case HANDOVER_STATE_SWITCHOVER: return "SWITCHOVER";
        case HANDOVER_STATE_CLEANUP:    return "CLEANUP";
        case HANDOVER_STATE_FAILED:     return "FAILED";
        case HANDOVER_STATE_COMMITTED:  return "COMMITTED";
    }
    return "UNKNOWN";
}

static inline const char *handover_result_str(HandoverResult r) {
    switch (r) {
        case HANDOVER_OK:               return "OK";
        case HANDOVER_ERR_ALLOC:        return "ERR_ALLOC";
        case HANDOVER_ERR_PARSE:        return "ERR_PARSE";
        case HANDOVER_ERR_RESOLVE:      return "ERR_RESOLVE";
        case HANDOVER_ERR_SERIALIZE:    return "ERR_SERIALIZE";
        case HANDOVER_ERR_DESERIALIZE:  return "ERR_DESERIALIZE";
        case HANDOVER_ERR_CHECKSUM:     return "ERR_CHECKSUM";
        case HANDOVER_ERR_DRY_RUN:      return "ERR_DRY_RUN";
        case HANDOVER_ERR_SAFE_POINT:   return "ERR_SAFE_POINT";
        case HANDOVER_ERR_FLASH_WRITE:  return "ERR_FLASH_WRITE";
        case HANDOVER_ERR_FLASH_READ:   return "ERR_FLASH_READ";
        case HANDOVER_ERR_VERSION:      return "ERR_VERSION";
        case HANDOVER_ERR_NULL_PROGRAM: return "ERR_NULL_PROGRAM";
    }
    return "UNKNOWN";
}

/* ── Version check ───────────────────────────────────────────────── */
static inline HandoverResult handover_check_version(uint32_t snap_ver) {
    uint32_t snap_major = snap_ver / 1000u;
    uint32_t cur_major  = FLUXA_HANDOVER_VERSION / 1000u;
    if (snap_major != cur_major)           return HANDOVER_ERR_VERSION;
    if (snap_ver > FLUXA_HANDOVER_VERSION) return HANDOVER_ERR_VERSION;
    return HANDOVER_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */
void           handover_ctx_init(HandoverCtx *ctx, Runtime *rt_a,
                                  HandoverMode mode);
void           handover_ctx_abort(HandoverCtx *ctx);
HandoverResult handover_execute(HandoverCtx *ctx, ASTNode *program_b,
                                 ASTPool *pool_b);
HandoverResult handover_serialize_state(HandoverCtx *ctx);
HandoverResult handover_deserialize_state(HandoverCtx *ctx);

HandoverResult handover_step1_standby(HandoverCtx *ctx, ASTNode *program_b,
                                       ASTPool *pool_b);
HandoverResult handover_step2_migrate(HandoverCtx *ctx);
HandoverResult handover_step3_dry_run(HandoverCtx *ctx);
HandoverResult handover_step4_switchover(HandoverCtx *ctx);
HandoverResult handover_step5_cleanup(HandoverCtx *ctx);

#endif /* FLUXA_HANDOVER_H */
