/* runtime.h — Fluxa Runtime
 * Sprint 5: current_instance
 * Sprint 6: danger_depth, ErrStack, GCTable stub
 * Sprint 6.b: PrstPool (realloc), FFIRegistry
 * Sprint 7: FluxaMode, real GC cap, real PrstPool+PrstGraph, toml config,
 *           runtime_explain for fluxa explain
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
} Runtime;

/* ── Public API ──────────────────────────────────────────────────────────── */
int  runtime_exec(ASTNode *program);
int  runtime_exec_explain(ASTNode *program);
void runtime_explain(Runtime *rt);

#endif /* FLUXA_RUNTIME_H */
