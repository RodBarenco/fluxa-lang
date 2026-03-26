/* runtime.h — Fluxa Runtime
 * Sprint 5: current_instance
 * Sprint 6: danger_depth, ErrStack, PrstGraph stub, GCTable stub
 */
#ifndef FLUXA_RUNTIME_H
#define FLUXA_RUNTIME_H

#include "ast.h"
#include "scope.h"
#include "block.h"
/* err.h is included transitively via scope.h */
#include "prst_graph.h"
#include "gc.h"

#define FLUXA_MAX_DEPTH  1000
#define FLUXA_STACK_SIZE 512

typedef struct {
    int   active;
    Value value;
} ReturnSignal;

/* ── Runtime state ───────────────────────────────────────────────────────── */
typedef struct Runtime {
    Scope          scope;
    Value          stack[FLUXA_STACK_SIZE];
    int            stack_size;
    int            had_error;
    int            call_depth;
    ReturnSignal   ret;

    /* Sprint 5 */
    BlockInstance *current_instance;   /* NULL outside methods              */

    /* Sprint 6 — danger/err */
    int            danger_depth;       /* 0 = fail-fast, 1 = inside danger  */
                                       /* danger is NOT nestable — parser    */
                                       /* rejects danger inside danger       */
    ErrStack       err_stack;          /* static, 32 entries, ring buffer    */

    /* Sprint 6 — GC stub */
    GCTable        gc;                 /* tracks heap allocs for future GC   */

    /* Sprint 6 — prst graph stub (Sprint 7 adds invalidation logic) */
    PrstGraph      prst_graph;
} Runtime;

/* ── Public API ──────────────────────────────────────────────────────────── */
int runtime_exec(ASTNode *program);

#endif /* FLUXA_RUNTIME_H */
