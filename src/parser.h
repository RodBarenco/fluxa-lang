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
    int      expr_depth;   /* recursion depth guard — reset to 0 by parser_new */
    int      stmt_depth;   /* statement block nesting depth guard */
    /* v0.15: module namespace support
     * ns: active namespace for this parse pass ("sensor", or "" for main).
     * imported: namespaces registered from `import live/static X` in main.
     * imported_count: number of registered namespaces. */
    char  ns[64];
    char  imported[32][64];
    int   imported_count;
    /* Names declared at top-level in the current module parse pass.
     * Used to mangle references inside fn bodies. */
    char  module_decls[256][64];
    int   module_decl_count;
    /* v0.23: depth of function/method bodies currently being parsed.
     * Module top-level decls (depth 0) get namespace-mangled; locals declared
     * INSIDE a function body (depth > 0) must NOT be mangled — they are frame
     * locals, private to the call, and mangling their name desynchronizes a
     * dyn's type/identity binding from its later index read (bug J). */
    int   fn_body_depth;
} Parser;

Parser   parser_new(const char *source, ASTPool *pool);
/* v0.15: parse module source `source` under namespace `ns`, appending
 * top-level declarations (mangled) into `program`.
 * Returns 0 on success, -1 on parse error. */
int      parser_parse_module(Parser *main_p, ASTNode *program,
                              const char *ns, const char *source);
ASTNode *parser_parse(Parser *p);
void     parser_free(Parser *p);

#endif /* FLUXA_PARSER_H */
