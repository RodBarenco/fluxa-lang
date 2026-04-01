/* handover.c — Fluxa Atomic Handover Protocol (Sprint 8)
 *
 * Implementa os 5 steps do protocolo:
 *   1. standby    — aloca B, resolve novo programa
 *   2. migration  — serializa A → snapshot → desserializa em B
 *   3. dry_run    — Dry Run: B executes with dry_run=1
 *   4. switchover — wait for safe point in A; atomic pool swap
 *   5. cleanup    — destroy temporary B, grace period
 *
 * Core invariant: rt_a is NEVER modified during any step.
 */
#define _POSIX_C_SOURCE 200809L
#include "handover.h"
#include "runtime.h"
#include "resolver.h"
#include "block.h"
#include "gc.h"
#include "scope.h"
#include "fluxa_ffi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── clock helper ────────────────────────────────────────────────────────── */
static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* ── fail with full rollback ────────────────────────────────────────────── */
static void ctx_fail(HandoverCtx *ctx, HandoverResult r, const char *detail) {
    ctx->last_result = r;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "[handover:%s] %s — %s",
             handover_state_str(ctx->state),
             handover_result_str(r),
             detail ? detail : "");
    fprintf(stderr, "%s\n", ctx->error_msg);
    /* ERR_HANDOVER em rt_a para que o caller possa inspecionar */
    if (ctx->rt_a)
        errstack_push(&ctx->rt_a->err_stack, ERR_HANDOVER,
                      ctx->error_msg, "<handover>", 0);
    handover_ctx_abort(ctx);
}

/* ── Public API ─────────────────────────────────────────────────────────── */
void handover_ctx_init(HandoverCtx *ctx, struct Runtime *rt_a,
                        HandoverMode mode) {
    memset(ctx, 0, sizeof(HandoverCtx));
    ctx->mode                  = mode;
    ctx->state                 = HANDOVER_STATE_IDLE;
    ctx->last_result           = HANDOVER_OK;
    ctx->rt_a                  = rt_a;
    ctx->safe_point_timeout_ms = 5000;
    ctx->grace_period_ms       = 100;
    ctx->pool_after.entries    = NULL;
    ctx->pool_after.count      = 0;
    ctx->pool_after.cap        = 0;
}

void handover_ctx_abort(HandoverCtx *ctx) {
    /* Free snapshot */
    if (ctx->snapshot) { free(ctx->snapshot); ctx->snapshot = NULL; }
    ctx->snapshot_size = 0;

    /* Destroy rt_b completely — rt_a remains intact */
    if (ctx->rt_b) {
        /* Clear qualquer estado parcial que rt_b possa ter */
        scope_free(&ctx->rt_b->scope);
        scope_table_free(&ctx->rt_b->global_table);
        /* block_registry_free() is global — only call if B was executed */
        gc_collect_all(&ctx->rt_b->gc);
        if (ctx->rt_b->prst_pool.entries) prst_pool_free(&ctx->rt_b->prst_pool);
        prst_graph_free(&ctx->rt_b->prst_graph);
        ffi_registry_free(&ctx->rt_b->ffi);
        free(ctx->rt_b);
        ctx->rt_b = NULL;
    }
    /* pool_b (ASTPool) is managed by caller — do not free here */
    ctx->program_b = NULL;
    ctx->state     = HANDOVER_STATE_FAILED;
}

/* ── Step 1: Standby ────────────────────────────────────────────────────── */
HandoverResult handover_step1_standby(HandoverCtx *ctx, ASTNode *program_b,
                                       ASTPool *pool_b) {
    ctx->state = HANDOVER_STATE_STANDBY;
    fprintf(stderr, "[handover] step 1: standby\n");

    if (!program_b) {
        ctx_fail(ctx, HANDOVER_ERR_NULL_PROGRAM, "program_b is NULL");
        return HANDOVER_ERR_NULL_PROGRAM;
    }

    /* Resolver pass no novo programa */
    int slots = resolver_run(program_b);
    if (slots < 0) {
        ctx_fail(ctx, HANDOVER_ERR_RESOLVE, "resolver failed on new program");
        return HANDOVER_ERR_RESOLVE;
    }

    /* Allocate rt_b — zero-init */
    ctx->rt_b = (Runtime *)calloc(1, sizeof(Runtime));
    if (!ctx->rt_b) {
        ctx_fail(ctx, HANDOVER_ERR_ALLOC, "calloc Runtime B failed");
        return HANDOVER_ERR_ALLOC;
    }

    ctx->pool_b    = pool_b;
    ctx->program_b = program_b;

    fprintf(stderr, "[handover] step 1: standby OK (slots=%d)\n", slots);
    return HANDOVER_OK;
}

/* ── Serialization ────────────────────────────────────────────────────────── */
HandoverResult handover_serialize_state(HandoverCtx *ctx) {
    Runtime *rt_a = ctx->rt_a;

    /* Pre-serialization checksums — integrity reference */
    uint32_t pool_cs  = prst_pool_checksum(&rt_a->prst_pool);
    uint32_t graph_cs = prst_graph_checksum(&rt_a->prst_graph);

    void  *pool_buf  = NULL; size_t pool_sz  = 0;
    void  *graph_buf = NULL; size_t graph_sz = 0;

    if (!prst_pool_serialize(&rt_a->prst_pool, &pool_buf, &pool_sz)) {
        return HANDOVER_ERR_SERIALIZE;
    }
    if (!prst_graph_serialize(&rt_a->prst_graph, &graph_buf, &graph_sz)) {
        free(pool_buf);
        return HANDOVER_ERR_SERIALIZE;
    }

    size_t total = sizeof(HandoverSnapshotHeader) + pool_sz + graph_sz;
    char *snap = (char *)malloc(total);
    if (!snap) {
        free(pool_buf); free(graph_buf);
        return HANDOVER_ERR_ALLOC;
    }

    HandoverSnapshotHeader *hdr = (HandoverSnapshotHeader *)snap;
    hdr->magic          = FLUXA_HANDOVER_MAGIC;
    hdr->version        = FLUXA_HANDOVER_VERSION;
    hdr->pool_checksum  = pool_cs;
    hdr->graph_checksum = graph_cs;
    hdr->pool_size      = (uint32_t)pool_sz;
    hdr->graph_size     = (uint32_t)graph_sz;
    hdr->pool_count     = (int32_t)rt_a->prst_pool.count;
    hdr->graph_count    = (int32_t)rt_a->prst_graph.count;
    hdr->cycle_count_a  = (int32_t)rt_a->cycle_count;
    memset(hdr->_pad, 0, sizeof(hdr->_pad));

    char *w = snap + sizeof(HandoverSnapshotHeader);
    if (pool_sz  > 0) { memcpy(w, pool_buf,  pool_sz);  w += pool_sz; }
    if (graph_sz > 0) { memcpy(w, graph_buf, graph_sz); }

    free(pool_buf); free(graph_buf);

    if (ctx->snapshot) free(ctx->snapshot);
    ctx->snapshot      = snap;
    ctx->snapshot_size = total;
    return HANDOVER_OK;
}

/* ── Deserialization ─────────────────────────────────────────────────────── */
HandoverResult handover_deserialize_state(HandoverCtx *ctx) {
    if (!ctx->snapshot || ctx->snapshot_size < sizeof(HandoverSnapshotHeader))
        return HANDOVER_ERR_DESERIALIZE;

    const HandoverSnapshotHeader *hdr =
        (const HandoverSnapshotHeader *)ctx->snapshot;

    if (hdr->magic != FLUXA_HANDOVER_MAGIC)
        return HANDOVER_ERR_DESERIALIZE;

    HandoverResult vr = handover_check_version(hdr->version);
    if (vr != HANDOVER_OK) return vr;

    const char *data = (const char *)ctx->snapshot
                       + sizeof(HandoverSnapshotHeader);

    /* Desserializa pool em rt_b */
    if (hdr->pool_size > 0) {
        if (!prst_pool_deserialize(&ctx->rt_b->prst_pool,
                                    data, (size_t)hdr->pool_size))
            return HANDOVER_ERR_DESERIALIZE;
        uint32_t cs = prst_pool_checksum(&ctx->rt_b->prst_pool);
        if (cs != hdr->pool_checksum)
            return HANDOVER_ERR_CHECKSUM;
    } else {
        prst_pool_init(&ctx->rt_b->prst_pool);
    }
    data += hdr->pool_size;

    /* Desserializa grafo em rt_b */
    if (hdr->graph_size > 0) {
        if (!prst_graph_deserialize(&ctx->rt_b->prst_graph,
                                     data, (size_t)hdr->graph_size))
            return HANDOVER_ERR_DESERIALIZE;
        uint32_t cs = prst_graph_checksum(&ctx->rt_b->prst_graph);
        if (cs != hdr->graph_checksum)
            return HANDOVER_ERR_CHECKSUM;
    } else {
        prst_graph_init(&ctx->rt_b->prst_graph);
    }

    return HANDOVER_OK;
}

/* ── Step 2: Migration ──────────────────────────────────────────────────── */
HandoverResult handover_step2_migrate(HandoverCtx *ctx) {
    ctx->state = HANDOVER_STATE_MIGRATION;
    fprintf(stderr, "[handover] step 2: migration (pool=%d, graph=%d)\n",
            ctx->rt_a->prst_pool.count, ctx->rt_a->prst_graph.count);

    HandoverResult r = handover_serialize_state(ctx);
    if (r != HANDOVER_OK) { ctx_fail(ctx, r, "serialize"); return r; }

    r = handover_deserialize_state(ctx);
    if (r != HANDOVER_OK) { ctx_fail(ctx, r, "deserialize"); return r; }

    fprintf(stderr, "[handover] step 2: migration OK (%zu bytes)\n",
            ctx->snapshot_size);
    return HANDOVER_OK;
}

/* ── Step 3: Dry Run ─────────────────────────────────── */
HandoverResult handover_step3_dry_run(HandoverCtx *ctx) {
    ctx->state = HANDOVER_STATE_DRY_RUN;
    fprintf(stderr, "[handover] step 3: Dry Run (dry_run=1)\n");

    Runtime *rt_b = ctx->rt_b;

    /* Initialize rt_b — Dry Run */
    rt_b->scope            = scope_new();
    rt_b->global_table     = NULL;
    rt_b->stack_size       = 0;
    rt_b->had_error        = 0;
    rt_b->call_depth       = 0;
    rt_b->ret.active       = 0;
    rt_b->ret.tco_active   = 0;
    rt_b->ret.tco_fn       = NULL;
    rt_b->ret.tco_args     = NULL;
    rt_b->ret.value        = val_nil();
    rt_b->current_instance = NULL;
    rt_b->danger_depth     = 0;
    rt_b->cycle_count      = 0;
    rt_b->dry_run          = 1;
    rt_b->current_line     = 0;    /* ← Dry Run: suppress output */
    rt_b->cancel_flag      = NULL;
    rt_b->mode             = FLUXA_MODE_PROJECT;
    rt_b->config           = ctx->rt_a->config;
    errstack_clear(&rt_b->err_stack);
    gc_init(&rt_b->gc, rt_b->config.gc_cap);
    ffi_registry_init(&rt_b->ffi);
    /* prst_pool and prst_graph already populated by step 2 */

    int result = runtime_exec_with_rt(rt_b, ctx->program_b);

    /* Any error — including danger — aborts the handover */
    int failed = (result != 0) || rt_b->had_error || (rt_b->err_stack.count > 0);

    /* Cleanup of B execution state (escopo, GC, FFI, blocks) */
    scope_free(&rt_b->scope);
    scope_table_free(&rt_b->global_table);
    block_registry_free();
    gc_collect_all(&rt_b->gc);
    ffi_registry_free(&rt_b->ffi);
    /* prst_pool and prst_graph SURVIVE — used by step 4 */

    if (failed) {
        char buf[512] = "dry run failed";
        if (rt_b->err_stack.count > 0) {
            const ErrEntry *e = errstack_get(&rt_b->err_stack, 0);
            if (e) snprintf(buf, sizeof(buf), "dry run: %s", e->message);
        }
        ctx_fail(ctx, HANDOVER_ERR_DRY_RUN, buf);
        return HANDOVER_ERR_DRY_RUN;
    }

    fprintf(stderr,
            "[handover] step 3: Dry Run OK (B cycle=%ld)\n",
            rt_b->cycle_count);
    return HANDOVER_OK;
}

/* ── Step 4: Switchover ─────────────────────────────────────────────────── */
HandoverResult handover_step4_switchover(HandoverCtx *ctx) {
    ctx->state = HANDOVER_STATE_SWITCHOVER;
    fprintf(stderr,
            "[handover] step 4: switchover (timeout=%dms)\n",
            ctx->safe_point_timeout_ms);

    Runtime *rt_a = ctx->rt_a;

    /* Wait for safe point in A with timeout */
    long deadline = ms_now() + ctx->safe_point_timeout_ms;
    while (!runtime_is_safe_point(rt_a)) {
        if (ms_now() >= deadline) {
            ctx_fail(ctx, HANDOVER_ERR_SAFE_POINT,
                     "timeout waiting for safe point in Runtime A");
            return HANDOVER_ERR_SAFE_POINT;
        }
        struct timespec ts_poll; ts_poll.tv_sec = 0; ts_poll.tv_nsec = 500000L;
        nanosleep(&ts_poll, NULL);
    }

    ctx->rt_a_cycle_at_swap = rt_a->cycle_count;

    /* Atomic swap: transfer B pool to pool_after.
     * Caller uses pool_after for the next runtime_apply(). */
    ctx->pool_after           = ctx->rt_b->prst_pool;
    /* Zero pointer in rt_b to avoid double-free */
    ctx->rt_b->prst_pool.entries = NULL;
    ctx->rt_b->prst_pool.count   = 0;
    ctx->rt_b->prst_pool.cap     = 0;

    fprintf(stderr,
            "[handover] step 4: switchover OK (A cycle=%ld)\n",
            ctx->rt_a_cycle_at_swap);
    return HANDOVER_OK;
}

/* ── Step 5: Cleanup ────────────────────────────────────────────────────── */
HandoverResult handover_step5_cleanup(HandoverCtx *ctx) {
    ctx->state = HANDOVER_STATE_CLEANUP;
    fprintf(stderr, "[handover] step 5: cleanup (grace=%dms)\n",
            ctx->grace_period_ms);

    if (ctx->grace_period_ms > 0) {
        struct timespec ts_grace;
        ts_grace.tv_sec  = ctx->grace_period_ms / 1000;
        ts_grace.tv_nsec = (long)(ctx->grace_period_ms % 1000) * 1000000L;
        nanosleep(&ts_grace, NULL);
    }

    /* Free snapshot */
    if (ctx->snapshot) { free(ctx->snapshot); ctx->snapshot = NULL; }
    ctx->snapshot_size = 0;

    /* Destroy rt_b — prst_pool was already transferred in step 4 */
    if (ctx->rt_b) {
        /* prst_graph still needs to be freed */
        prst_graph_free(&ctx->rt_b->prst_graph);
        free(ctx->rt_b);
        ctx->rt_b = NULL;
    }

    ctx->state       = HANDOVER_STATE_COMMITTED;
    ctx->last_result = HANDOVER_OK;
    fprintf(stderr, "[handover] step 5: cleanup OK — handover COMMITTED\n");
    return HANDOVER_OK;
}

/* ── Full protocol ──────────────────────────────────────────────────── */
HandoverResult handover_execute(HandoverCtx *ctx, ASTNode *program_b,
                                 ASTPool *pool_b) {
    HandoverResult r;

    r = handover_step1_standby(ctx, program_b, pool_b);
    if (r != HANDOVER_OK) return r;

    r = handover_step2_migrate(ctx);
    if (r != HANDOVER_OK) return r;

    r = handover_step3_dry_run(ctx);
    if (r != HANDOVER_OK) return r;

    r = handover_step4_switchover(ctx);
    if (r != HANDOVER_OK) return r;

    r = handover_step5_cleanup(ctx);
    return r;
}
