/* resolver.h — Name Resolution Pass for Fluxa (Sprint 4 / Sprint 5)
 * Issue #31: moved implementation to resolver.c — header is declarations only.
 *
 * Walks the AST once before execution and replaces string var_name
 * lookups with integer stack offsets. After resolution, NODE_IDENTIFIER
 * and NODE_ASSIGN nodes carry an offset instead of a string name.
 */
#ifndef FLUXA_RESOLVER_H
#define FLUXA_RESOLVER_H

#include "ast.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Symbol table ────────────────────────────────────────────────────────── */
#define SYM_MAX 512

typedef struct {
    char name[256];
    int  offset;
} Symbol;

typedef struct SymTable {
    Symbol          syms[SYM_MAX];
    int             count;
    struct SymTable *parent;
} SymTable;

void symtable_init(SymTable *t, SymTable *parent);
int  symtable_find(SymTable *t, const char *name);
int  symtable_declare(SymTable *t, const char *name);
int  symtable_size(SymTable *t);

/* ── Resolver state ──────────────────────────────────────────────────────── */
typedef struct {
    SymTable *current;
    int       had_error;
    int       max_slots;
} Resolver;

/* ── Public API ──────────────────────────────────────────────────────────── */
/* Run the resolver over a parsed program.
 * Returns max stack slots needed, or -1 on error. */
int resolver_run(ASTNode *program);

#endif /* FLUXA_RESOLVER_H */
