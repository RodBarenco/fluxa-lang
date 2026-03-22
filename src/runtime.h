/* runtime.h — Fluxa Runtime
 * Sprint 5: Runtime struct gains current_instance pointer (#40).
 * The hot path (while with ints) is unaffected: current_instance is NULL
 * outside of method calls, and the branch is CPU-predictable.
 */
#ifndef FLUXA_RUNTIME_H
#define FLUXA_RUNTIME_H

#include "ast.h"
#include "scope.h"
#include "block.h"

#define FLUXA_MAX_DEPTH  1000
#define FLUXA_STACK_SIZE 512

/* Issue #21: return signal */
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
    /* Sprint 5 — #40: current executing Block instance (NULL outside methods) */
    BlockInstance *current_instance;
} Runtime;

/* ── Public API ──────────────────────────────────────────────────────────── */
int runtime_exec(ASTNode *program);

#endif /* FLUXA_RUNTIME_H */
