/* builtins.h — Fluxa Built-in Functions (Sprint 5, Issue #35) */
#ifndef FLUXA_BUILTINS_H
#define FLUXA_BUILTINS_H

#include "scope.h"
#include "ast.h"

/* Forward declaration — Runtime is defined in runtime.h */
struct Runtime;

/* Returns 1 if name is a builtin, 0 otherwise */
int builtin_is(const char *name);

/* Dispatch a builtin call. Returns val_nil() if name is unknown. */
/* eval_fn is a callback: eval_fn(rt, node) — avoids circular dependency */
typedef Value (*EvalFn)(struct Runtime *rt, ASTNode *node);

Value builtin_dispatch(struct Runtime *rt, ASTNode *call, EvalFn eval_fn);

#endif /* FLUXA_BUILTINS_H */
