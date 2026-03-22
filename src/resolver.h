/* resolver.h — Name Resolution Pass for Fluxa
 * Sprint 4 Performance — Step 3
 *
 * Walks the AST once before execution and replaces string var_name
 * lookups with integer stack offsets. The runtime scope becomes a
 * flat Value array — zero hash, zero strcmp per variable access.
 *
 * Each scope level (program, function body) has its own SymTable.
 * After resolution, NODE_IDENTIFIER and NODE_ASSIGN nodes carry
 * an offset instead of a string name.
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
    struct SymTable *parent;   /* enclosing scope */
} SymTable;

static inline void symtable_init(SymTable *t, SymTable *parent) {
    t->count  = 0;
    t->parent = parent;
}

/* returns offset, or -1 if not found */
static inline int symtable_find(SymTable *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->syms[i].name, name) == 0)
            return t->syms[i].offset;
    if (t->parent) return symtable_find(t->parent, name);
    return -1;
}

/* returns offset of new or existing symbol */
static inline int symtable_declare(SymTable *t, const char *name) {
    int existing = symtable_find(t, name);
    if (existing >= 0) return existing;
    if (t->count >= SYM_MAX) return -1;
    strncpy(t->syms[t->count].name, name, 255);
    t->syms[t->count].name[255] = '\0';
    t->syms[t->count].offset    = t->count;
    return t->count++;
}

/* total slots needed in this scope */
static inline int symtable_size(SymTable *t) { return t->count; }

/* ── Resolver state ──────────────────────────────────────────────────────── */
typedef struct {
    SymTable *current;
    int       had_error;
    int       max_slots;  /* peak slot count seen */
} Resolver;

static void resolve_node(Resolver *r, ASTNode *node);

static inline void res_error(Resolver *r, const char *msg) {
    fprintf(stderr, "[fluxa] Resolver error: %s\n", msg);
    r->had_error = 1;
}

/* ── Resolution ──────────────────────────────────────────────────────────── */
static void resolve_expr(Resolver *r, ASTNode *node) {
    if (!node || r->had_error) return;
    switch (node->type) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STRING_LIT:
            break;

        case NODE_IDENTIFIER: {
            int off = symtable_find(r->current, node->as.str.value);
            if (off < 0) {
                /* builtins and functions — leave unresolved (offset = -1) */
                node->resolved_offset = -1;
            } else {
                node->resolved_offset = off;
            }
            break;
        }

        case NODE_BINARY_EXPR:
            resolve_expr(r, node->as.binary.left);
            resolve_expr(r, node->as.binary.right);
            break;

        case NODE_FUNC_CALL:
            for (int i = 0; i < node->as.list.count; i++)
                resolve_expr(r, node->as.list.children[i]);
            break;

        case NODE_ARR_ACCESS:
            resolve_expr(r, node->as.arr_access.index);
            break;

        default:
            resolve_node(r, node);
            break;
    }
}

static void resolve_node(Resolver *r, ASTNode *node) {
    if (!node || r->had_error) return;
    switch (node->type) {

        case NODE_PROGRAM:
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++)
                resolve_node(r, node->as.list.children[i]);
            break;

        case NODE_VAR_DECL: {
            resolve_expr(r, node->as.var_decl.initializer);
            int off = symtable_declare(r->current,
                                       node->as.var_decl.var_name);
            node->resolved_offset = off;
            if (r->current->count > r->max_slots)
                r->max_slots = r->current->count;
            break;
        }

        case NODE_ASSIGN: {
            resolve_expr(r, node->as.assign.value);
            int off = symtable_find(r->current, node->as.assign.var_name);
            if (off < 0) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "assignment to undeclared variable: %s",
                    node->as.assign.var_name);
                res_error(r, buf);
                return;
            }
            node->resolved_offset = off;
            break;
        }

        case NODE_IF:
            resolve_expr(r, node->as.if_stmt.condition);
            resolve_node(r, node->as.if_stmt.then_body);
            if (node->as.if_stmt.else_body)
                resolve_node(r, node->as.if_stmt.else_body);
            break;

        case NODE_WHILE:
            resolve_expr(r, node->as.while_stmt.condition);
            resolve_node(r, node->as.while_stmt.body);
            break;

        case NODE_FOR: {
            /* declare loop variable in current scope */
            int off = symtable_declare(r->current,
                                       node->as.for_stmt.var_name);
            node->resolved_offset = off;
            if (r->current->count > r->max_slots)
                r->max_slots = r->current->count;
            resolve_node(r, node->as.for_stmt.body);
            break;
        }

        case NODE_ARR_DECL: {
            for (int i = 0; i < node->as.arr_decl.size; i++)
                resolve_expr(r, node->as.arr_decl.elements[i]);
            /* arr elements stored as arr[0]..arr[N] — keep name-based for now */
            break;
        }

        case NODE_ARR_ASSIGN:
            resolve_expr(r, node->as.arr_assign.index);
            resolve_expr(r, node->as.arr_assign.value);
            break;

        case NODE_FUNC_DECL: {
            /* register function name in current scope */
            symtable_declare(r->current, node->as.func_decl.name);

            /* resolve body in new child scope */
            SymTable child;
            symtable_init(&child, r->current);
            SymTable *saved = r->current;
            r->current = &child;

            /* declare parameters */
            for (int i = 0; i < node->as.func_decl.param_count; i++) {
                int off = symtable_declare(&child,
                              node->as.func_decl.param_names[i]);
                node->resolved_offset = off; /* reuse for param base */
            }

            resolve_node(r, node->as.func_decl.body);

            if (child.count > r->max_slots)
                r->max_slots = child.count;

            r->current = saved;
            break;
        }

        case NODE_RETURN:
            if (node->as.ret.value)
                resolve_expr(r, node->as.ret.value);
            break;

        case NODE_FUNC_CALL:
            for (int i = 0; i < node->as.list.count; i++)
                resolve_expr(r, node->as.list.children[i]);
            break;

        default:
            resolve_expr(r, node);
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Run the resolver over a parsed program.
 * Returns max stack slots needed, or -1 on error.
 */
static inline int resolver_run(ASTNode *program) {
    SymTable global;
    symtable_init(&global, NULL);

    Resolver r;
    r.current   = &global;
    r.had_error = 0;
    r.max_slots = 0;

    resolve_node(&r, program);

    if (r.had_error) return -1;
    return r.max_slots;
}

#endif /* FLUXA_RESOLVER_H */