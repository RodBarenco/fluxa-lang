/* parser.h — Fluxa Parser
 * Consumes tokens from the Lexer and produces an AST.
 * Sprint 1: parses top-level function calls with literal arguments.
 */
#ifndef FLUXA_PARSER_H
#define FLUXA_PARSER_H

#include "lexer.h"
#include "ast.h"

/* ── Parser state ────────────────────────────────────────────────────────── */
typedef struct {
    Lexer   lexer;
    Token   current;
    Token   next;
    int     had_error;
} Parser;

/* ── Public API ──────────────────────────────────────────────────────────── */
Parser   parser_new(const char *source);
ASTNode *parser_parse(Parser *p);   /* returns NODE_PROGRAM or NULL on error */
void     parser_free(Parser *p);

#endif /* FLUXA_PARSER_H */
