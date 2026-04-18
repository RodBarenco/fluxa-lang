/* resolver.c — Fluxa Name Resolution implementation
 * Issue #31: extracted from resolver.h (was header-only, caused ODR issues)
 * Sprint 5: Block declarations resolved in isolated child scopes.
 *           typeof instances registered as names in global scope.
 *           Member access (inst.field) left unresolved — handled at eval time.
 * Issue #151: resolve_node made iterative via explicit work-stack to prevent
 *             stack overflow on large programs (Dijkstra, deeply nested Blocks).
 */
#define _POSIX_C_SOURCE 200809L
#include "resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SymTable ────────────────────────────────────────────────────────────── */
void symtable_init(SymTable *t, SymTable *parent) {
    t->count  = 0;
    t->parent = parent;
}

int symtable_find(SymTable *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->syms[i].name, name) == 0)
            return t->syms[i].offset;
    if (t->parent) return symtable_find(t->parent, name);
    return -1;
}

int symtable_declare(SymTable *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->syms[i].name, name) == 0)
            return t->syms[i].offset;
    if (t->count >= SYM_MAX) return -1;
    strncpy(t->syms[t->count].name, name, 255);
    t->syms[t->count].name[255] = '\0';
    t->syms[t->count].offset    = t->count;
    return t->count++;
}

int symtable_size(SymTable *t) { return t->count; }

/* ── Internal forward declarations ───────────────────────────────────────── */
static void resolve_node(Resolver *r, ASTNode *node);

/* ── Expression resolution (recursive — bounded by expr depth, not stmt nesting) */
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
            node->resolved_offset = (off >= 0) ? off : -1;
            if (r->in_func_depth > 0 && off >= 0)
                node->warm_local = 1;
            break;
        }

        case NODE_BINARY_EXPR:
            resolve_expr(r, node->as.binary.left);
            if (node->as.binary.right)
                resolve_expr(r, node->as.binary.right);
            break;

        case NODE_FUNC_CALL:
            for (int i = 0; i < node->as.list.count; i++)
                resolve_expr(r, node->as.list.children[i]);
            break;

        case NODE_ARR_ACCESS:
            resolve_expr(r, node->as.arr_access.index);
            {
                int off = symtable_find(r->current, node->as.arr_access.arr_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_MEMBER_ACCESS:
        case NODE_MEMBER_CALL:
            if (node->type == NODE_MEMBER_CALL) {
                for (int i = 0; i < node->as.member_call.arg_count; i++)
                    resolve_expr(r, node->as.member_call.args[i]);
            }
            node->resolved_offset = -1;
            break;

        case NODE_DYN_LIT:
            for (int i = 0; i < node->as.dyn_lit.count; i++)
                resolve_expr(r, node->as.dyn_lit.elements[i]);
            node->resolved_offset = -1;
            break;

        case NODE_DYN_ACCESS:
            resolve_expr(r, node->as.dyn_access.index);
            {
                int off = symtable_find(r->current, node->as.dyn_access.dyn_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_DYN_ASSIGN:
            resolve_expr(r, node->as.dyn_assign.index);
            resolve_expr(r, node->as.dyn_assign.value);
            {
                int off = symtable_find(r->current, node->as.dyn_assign.dyn_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_INDEXED_MEMBER_ACCESS:
            resolve_expr(r, node->as.indexed_member_access.index);
            {
                int off = symtable_find(r->current, node->as.indexed_member_access.dyn_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_INDEXED_MEMBER_CALL:
            resolve_expr(r, node->as.indexed_member_call.index);
            for (int i = 0; i < node->as.indexed_member_call.arg_count; i++)
                resolve_expr(r, node->as.indexed_member_call.args[i]);
            {
                int off = symtable_find(r->current, node->as.indexed_member_call.dyn_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        default:
            resolve_node(r, node);
            break;
    }
}

/* ── Explicit work-stack for statement resolution ────────────────────────── *
 * Each WorkItem carries the node to process and the SymTable scope active   *
 * when it was pushed. This eliminates C-stack growth from stmt nesting.    */

#define WORK_STACK_CAP 4096

typedef struct {
    ASTNode  *node;
    SymTable *scope;        /* r->current when this item was pushed        */
    int       in_func;      /* r->in_func_depth when pushed                */
} WorkItem;

typedef struct {
    WorkItem items[WORK_STACK_CAP];
    int      top;
} WorkStack;

static void ws_push(WorkStack *ws, ASTNode *node, SymTable *scope, int in_func) {
    if (!node || ws->top >= WORK_STACK_CAP) return;
    ws->items[ws->top].node     = node;
    ws->items[ws->top].scope    = scope;
    ws->items[ws->top].in_func  = in_func;
    ws->top++;
}

/* Push children in REVERSE order so they are processed left-to-right */
static void ws_push_list(WorkStack *ws, ASTNode **children, int count,
                          SymTable *scope, int in_func) {
    for (int i = count - 1; i >= 0; i--)
        ws_push(ws, children[i], scope, in_func);
}

/* ── Statement resolution — iterative ───────────────────────────────────── */
static void resolve_stmts(Resolver *r, ASTNode *root) {
    WorkStack *ws = (WorkStack *)malloc(sizeof(WorkStack));
    if (!ws) return;
    ws->top = 0;

    /* We need heap-allocated SymTables for child scopes created during
     * iteration (function bodies, Block methods). Keep a simple freelist. */
#define SCOPE_POOL_CAP 256
    SymTable *scope_pool = (SymTable *)calloc(SCOPE_POOL_CAP, sizeof(SymTable));
    int       scope_used = 0;
    if (!scope_pool) { free(ws); return; }

#define ALLOC_SCOPE(parent) \
    (scope_used < SCOPE_POOL_CAP \
        ? (symtable_init(&scope_pool[scope_used], (parent)), &scope_pool[scope_used++]) \
        : (r->had_error = 1, (SymTable*)NULL))

    ws_push(ws, root, r->current, r->in_func_depth);

    while (ws->top > 0 && !r->had_error) {
        WorkItem wi = ws->items[--ws->top];
        ASTNode  *n = wi.node;
        if (!n) continue;

        /* Restore scope context for this item */
        r->current      = wi.scope;
        r->in_func_depth = wi.in_func;

        switch (n->type) {

        case NODE_PROGRAM:
        case NODE_BLOCK_STMT:
            /* Push children in reverse so first child is processed first */
            ws_push_list(ws, n->as.list.children, n->as.list.count,
                         wi.scope, wi.in_func);
            break;

        case NODE_VAR_DECL: {
            resolve_expr(r, n->as.var_decl.initializer);
            int off = symtable_declare(wi.scope, n->as.var_decl.var_name);
            n->resolved_offset = off;
            if (wi.in_func > 0 && !n->as.var_decl.persistent && off >= 0)
                n->warm_local = 1;
            if (wi.scope->count > r->max_slots)
                r->max_slots = wi.scope->count;
            break;
        }

        case NODE_ASSIGN: {
            resolve_expr(r, n->as.assign.value);
            int off = symtable_find(wi.scope, n->as.assign.var_name);
            n->resolved_offset = (off >= 0) ? off : -1;
            break;
        }

        case NODE_IF:
            resolve_expr(r, n->as.if_stmt.condition);
            /* Push else first (processed last), then then-body */
            if (n->as.if_stmt.else_body)
                ws_push(ws, n->as.if_stmt.else_body, wi.scope, wi.in_func);
            ws_push(ws, n->as.if_stmt.then_body, wi.scope, wi.in_func);
            break;

        case NODE_WHILE:
            resolve_expr(r, n->as.while_stmt.condition);
            ws_push(ws, n->as.while_stmt.body, wi.scope, wi.in_func);
            break;

        case NODE_FOR: {
            int arr_off = symtable_find(wi.scope, n->as.for_stmt.arr_name);
            n->as.for_stmt.arr_resolved_offset = (arr_off >= 0) ? arr_off : -1;
            int off = symtable_declare(wi.scope, n->as.for_stmt.var_name);
            n->resolved_offset = off;
            if (wi.scope->count > r->max_slots)
                r->max_slots = wi.scope->count;
            ws_push(ws, n->as.for_stmt.body, wi.scope, wi.in_func);
            break;
        }

        case NODE_ARR_DECL: {
            if (n->as.arr_decl.default_init) {
                resolve_expr(r, n->as.arr_decl.default_value);
            } else {
                for (int i = 0; i < n->as.arr_decl.size; i++)
                    resolve_expr(r, n->as.arr_decl.elements[i]);
            }
            break;
        }

        case NODE_ARR_ASSIGN:
            resolve_expr(r, n->as.arr_assign.index);
            resolve_expr(r, n->as.arr_assign.value);
            {
                int off = symtable_find(wi.scope, n->as.arr_assign.arr_name);
                n->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_FUNC_DECL: {
            symtable_declare(wi.scope, n->as.func_decl.name);

            SymTable *child = ALLOC_SCOPE(wi.scope);
            if (!child) break;

            int new_func_depth = wi.in_func + 1;
            for (int i = 0; i < n->as.func_decl.param_count; i++)
                symtable_declare(child, n->as.func_decl.param_names[i]);

            /* Push body with new child scope and incremented func depth.
             * After the body, we need to check max_slots — push a sentinel? 
             * Simpler: update max_slots after body processes, using child.count.
             * Since we can't run code "after" an async push, we track it by
             * recording child pointer. But child is in pool so it's stable.
             * max_slots will be updated when VAR_DECL nodes inside run. */
            ws_push(ws, n->as.func_decl.body, child, new_func_depth);
            break;
        }

        case NODE_RETURN:
            if (n->as.ret.value)
                resolve_expr(r, n->as.ret.value);
            break;

        case NODE_FUNC_CALL:
            for (int i = 0; i < n->as.list.count; i++)
                resolve_expr(r, n->as.list.children[i]);
            break;

        case NODE_BLOCK_DECL: {
            symtable_declare(wi.scope, n->as.block_decl.name);

            SymTable *child = ALLOC_SCOPE(NULL);  /* isolated — no parent */
            if (!child) break;

            for (int i = 0; i < n->as.block_decl.count; i++) {
                ASTNode *member = n->as.block_decl.members[i];
                if (!member) continue;

                if (member->type == NODE_VAR_DECL) {
                    resolve_expr(r, member->as.var_decl.initializer);
                    int off = symtable_declare(child, member->as.var_decl.var_name);
                    member->resolved_offset = off;
                } else if (member->type == NODE_FUNC_DECL) {
                    symtable_declare(child, member->as.func_decl.name);

                    /* Method body: fully isolated scope (parent=NULL) */
                    SymTable *method_scope = ALLOC_SCOPE(NULL);
                    if (!method_scope) break;

                    for (int p = 0; p < member->as.func_decl.param_count; p++)
                        symtable_declare(method_scope,
                                         member->as.func_decl.param_names[p]);

                    /* Push method body with isolated scope, in_func=1 */
                    ws_push(ws, member->as.func_decl.body, method_scope, 1);
                }
            }
            break;
        }

        case NODE_TYPEOF_INST:
            symtable_declare(wi.scope, n->as.typeof_inst.inst_name);
            n->resolved_offset = -1;
            break;

        case NODE_MEMBER_ASSIGN:
            resolve_expr(r, n->as.member_assign.value);
            n->resolved_offset = -1;
            break;

        case NODE_MEMBER_CALL:
            for (int i = 0; i < n->as.member_call.arg_count; i++)
                resolve_expr(r, n->as.member_call.args[i]);
            n->resolved_offset = -1;
            break;

        case NODE_MEMBER_ACCESS:
            n->resolved_offset = -1;
            break;

        case NODE_INDEXED_MEMBER_CALL:
            resolve_expr(r, n->as.indexed_member_call.index);
            for (int i = 0; i < n->as.indexed_member_call.arg_count; i++)
                resolve_expr(r, n->as.indexed_member_call.args[i]);
            {
                int off = symtable_find(wi.scope, n->as.indexed_member_call.dyn_name);
                n->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_INDEXED_MEMBER_ACCESS:
            resolve_expr(r, n->as.indexed_member_access.index);
            {
                int off = symtable_find(wi.scope, n->as.indexed_member_access.dyn_name);
                n->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_DANGER:
            ws_push(ws, n->as.danger_stmt.body, wi.scope, wi.in_func);
            break;

        case NODE_FREE: {
            int off = symtable_find(wi.scope, n->as.free_stmt.var_name);
            n->resolved_offset = (off >= 0) ? off : -1;
            break;
        }

        case NODE_IMPORT_STD:
        case NODE_IMPORT_C:
            break;

        case NODE_FFI_CALL:
            for (int i = 0; i < n->as.ffi_call.arg_count; i++)
                resolve_expr(r, n->as.ffi_call.args[i]);
            n->resolved_offset = -1;
            break;

        default:
            resolve_expr(r, n);
            break;
        }
    }

    free(ws);
    free(scope_pool);
#undef ALLOC_SCOPE
#undef SCOPE_POOL_CAP
}

/* ── resolve_node: thin wrapper — pushes to iterative engine ─────────────── */
static void resolve_node(Resolver *r, ASTNode *node) {
    /* Called from resolve_expr default case for statement nodes embedded in
     * expressions. Since expressions are already bounded in depth, this is
     * safe — but we still dispatch through the iterative path to handle
     * any statement lists correctly. */
    if (!node || r->had_error) return;
    resolve_stmts(r, node);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int resolver_run(ASTNode *program) {
    SymTable global;
    symtable_init(&global, NULL);

    Resolver r;
    r.current       = &global;
    r.had_error     = 0;
    r.max_slots     = 0;
    r.in_func_depth = 0;

    resolve_stmts(&r, program);

    if (r.had_error) return -1;
    return r.max_slots;
}

/* ── resolver_has_prst ───────────────────────────────────────────────────── */
static int has_prst_node(ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case NODE_VAR_DECL:
            if (node->as.var_decl.persistent) return 1;
            break;
        case NODE_ARR_DECL:
            if (node->as.arr_decl.persistent) return 1;
            break;
        case NODE_BLOCK_DECL:
            for (int i = 0; i < node->as.block_decl.count; i++)
                if (has_prst_node(node->as.block_decl.members[i])) return 1;
            break;
        case NODE_FUNC_DECL:
            if (has_prst_node(node->as.func_decl.body)) return 1;
            break;
        case NODE_PROGRAM:
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++)
                if (has_prst_node(node->as.list.children[i])) return 1;
            break;
        case NODE_IF:
            if (has_prst_node(node->as.if_stmt.then_body)) return 1;
            if (has_prst_node(node->as.if_stmt.else_body)) return 1;
            break;
        case NODE_WHILE:
            if (has_prst_node(node->as.while_stmt.body)) return 1;
            break;
        case NODE_FOR:
            if (has_prst_node(node->as.for_stmt.body)) return 1;
            break;
        case NODE_DANGER:
            if (has_prst_node(node->as.danger_stmt.body)) return 1;
            break;
        default:
            break;
    }
    return 0;
}

int resolver_has_prst(ASTNode *program) {
    return has_prst_node(program);
}
