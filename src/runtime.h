/* runtime.h — Fluxa Runtime
 * Sprint 5: current_instance
 * Sprint 6: danger_depth, ErrStack, GCTable stub
 * Sprint 6.b: PrstPool (realloc, replaces PrstGraph stub), FFIRegistry
 */
#ifndef FLUXA_RUNTIME_H
#define FLUXA_RUNTIME_H

#include "ast.h"
#include "scope.h"
#include "block.h"
#include "gc.h"
#include "prst_pool.h"
#include "fluxa_ffi.h"

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
    BlockInstance *current_instance;

    /* Sprint 6 */
    int            danger_depth;    /* 0=fail-fast, 1=inside danger (no nesting) */
    ErrStack       err_stack;       /* 32 entries, ring buffer, static            */
    GCTable        gc;              /* stub — gc_alloc/gc_free wrap malloc/free   */

    /* Sprint 6.b */
    PrstPool       prst_pool;       /* dynamic realloc pool for prst variables    */
    FFIRegistry    ffi;             /* loaded C libraries via dlopen              */
} Runtime;

/* ── Public API ──────────────────────────────────────────────────────────── */
int runtime_exec(ASTNode *program);

#endif /* FLUXA_RUNTIME_H */
