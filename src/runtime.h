/* runtime.h — Fluxa Runtime
 * Sprint 5: current_instance
 * Sprint 6: danger_depth, ErrStack, GCTable stub
 * Sprint 6.b: PrstPool (realloc), FFIRegistry
 * Sprint 7: FluxaMode, real GC cap, real PrstPool+PrstGraph, toml config,
 *           runtime_explain for fluxa explain
 * Sprint 7.b: cycle_count, dry_run, runtime_is_safe_point, runtime_apply,
 *             ERR_HANDOVER, prst checksum+serialization
 */
#ifndef FLUXA_RUNTIME_H
#define FLUXA_RUNTIME_H

#include "ast.h"
#include "scope.h"
#include "block.h"
#include "gc.h"
#include "prst_pool.h"
#include "prst_graph.h"
#include "fluxa_ffi.h"
#include "toml_config.h"

#define FLUXA_MAX_DEPTH  500
#define FLUXA_STACK_SIZE 512

/* ── Execution mode ──────────────────────────────────────────────────────── */
/* Detected by resolver_has_prst() before any execution.
 * SCRIPT:  no prst found → lightweight path, PrstPool/Graph not active.
 * PROJECT: prst found → PrstPool + PrstGraph active, reload-capable.  */
typedef enum {
    FLUXA_MODE_SCRIPT,
    FLUXA_MODE_PROJECT,
} FluxaMode;

typedef struct {
    int      active;
    Value    value;
    /* TCO trampoline fields */
    int      tco_active;
    ASTNode *tco_fn;
    Value   *tco_args;
    int      tco_arg_count;
} ReturnSignal;

/* ── Runtime state ───────────────────────────────────────────────────────── */
typedef struct Runtime {
    Scope          scope;
    ScopeEntry    *global_table;   /* snapshot of top-level scope.table, synced after each top-level stmt */
    Value          stack[FLUXA_STACK_SIZE];
    int            stack_size;
    int            had_error;
    int            call_depth;
    ReturnSignal   ret;

    /* Sprint 5 */
    BlockInstance *current_instance;

    /* Sprint 6 */
    int            danger_depth;
    ErrStack       err_stack;
    GCTable        gc;

    /* Sprint 6.b */
    FFIRegistry    ffi;

    /* Sprint 7 */
    FluxaMode      mode;
    PrstPool       prst_pool;
    PrstGraph      prst_graph;
    FluxaConfig    config;

    /* Sprint 7.b */
    long           cycle_count;   /* incremented per top-level statement    */
    int            dry_run;       /* 1 = suppress all output (print, FFI)   */
    volatile int  *cancel_flag;   /* non-NULL in -dev: set to 1 to abort VM */
} Runtime;

/* ── Public API ──────────────────────────────────────────────────────────── */
int  runtime_exec(ASTNode *program);
int  runtime_exec_explain(ASTNode *program);
void runtime_explain(Runtime *rt);

/* Sprint 7.b */
/* Returns 1 when the runtime is at a safe point for handover/reload.
 * A safe point is: call_depth == 0 AND danger_depth == 0. */
static inline int runtime_is_safe_point(const Runtime *rt) {
    return rt->call_depth == 0 && rt->danger_depth == 0;
}

/* Apply a reload: re-parse and re-execute program, preserving prst state.
 * pool_in: existing PrstPool from the previous run (state to carry over).
 * Returns 0 on success, 1 on error. */
int runtime_apply(ASTNode *program, PrstPool *pool_in);

/* -dev mode: register a global cancel flag checked on every VM back-edge.
 * Set *flag = 1 from the watcher thread to break infinite loops.
 * Call with NULL to clear (after thread joins). */
void runtime_set_cancel_flag(volatile int *flag);

#endif /* FLUXA_RUNTIME_H */
