/* parser.h — Fluxa Parser
 * Sprint 5: Block declaration and typeof instance parsing (#37)
 */
#ifndef FLUXA_PARSER_H
#define FLUXA_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "pool.h"

typedef struct {
    Lexer    lexer;
    Token    current;
    Token    next;
    int      had_error;
    ASTPool *pool;
} Parser;

Parser   parser_new(const char *source, ASTPool *pool);
ASTNode *parser_parse(Parser *p);
void     parser_free(Parser *p);

#endif /* FLUXA_PARSER_H */
