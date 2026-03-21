/* parser.h — Fluxa Parser
 * Consumes tokens from the Lexer and produces an AST.
 * Sprint 2: parses var declarations, assignments and binary expressions.
 * Sprint 2+: uses ASTPool for cache-friendly node allocation.
 */
#ifndef FLUXA_PARSER_H
#define FLUXA_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "pool.h"

/* ── Parser state ────────────────────────────────────────────────────────── */
typedef struct {
    Lexer    lexer;
    Token    current;
    Token    next;
    int      had_error;
    ASTPool *pool;      /* arena — owned by caller */
} Parser;

/* ── Public API ──────────────────────────────────────────────────────────── */
Parser   parser_new(const char *source, ASTPool *pool);
ASTNode *parser_parse(Parser *p);   /* returns NODE_PROGRAM or NULL on error */
void     parser_free(Parser *p);

#endif /* FLUXA_PARSER_H */
