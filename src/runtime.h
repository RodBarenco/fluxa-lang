/* runtime.h — Fluxa Runtime
 * Walks the AST and executes it.
 * Sprint 1: executes print() with string / int / float / bool arguments.
 */
#ifndef FLUXA_RUNTIME_H
#define FLUXA_RUNTIME_H

#include "ast.h"

/* ── Public API ──────────────────────────────────────────────────────────── */
int runtime_exec(ASTNode *program);  /* returns 0 on success, 1 on error */

#endif /* FLUXA_RUNTIME_H */
