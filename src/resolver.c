/* resolver.c — Fluxa Name Resolution implementation
 * Issue #31: extracted from resolver.h (was header-only, caused ODR issues)
 * Sprint 5: Block declarations resolved in isolated child scopes.
 *           typeof instances registered as names in global scope.
 *           Member access (inst.field) left unresolved — handled at eval time.
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
    /* search only in current scope level (not parent) for redeclaration */
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

/* ── Expression resolution ───────────────────────────────────────────────── */
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
            /* Warm path: inside a function body, local variables are never
             * prst — mark so rt_get can skip prst_pool_has (O(n) scan). */
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
            /* Also resolve the array variable name itself — needed for fn params */
            {
                int off = symtable_find(r->current, node->as.arr_access.arr_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        /* Sprint 5: member access — leave unresolved, eval handles it */
        case NODE_MEMBER_ACCESS:
        case NODE_MEMBER_CALL:
            if (node->type == NODE_MEMBER_CALL) {
                for (int i = 0; i < node->as.member_call.arg_count; i++)
                    resolve_expr(r, node->as.member_call.args[i]);
            }
            node->resolved_offset = -1;
            break;

        /* Sprint 9.c: logical ! — right operand is NULL */
        /* Sprint 9.c: dyn literal — resolve each element */
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

/* ── Statement resolution ────────────────────────────────────────────────── */
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
            int off = symtable_declare(r->current, node->as.var_decl.var_name);
            node->resolved_offset = off;
            /* Warm path: local fn vars that are NOT prst can skip prst_pool_has */
            if (r->in_func_depth > 0 && !node->as.var_decl.persistent && off >= 0)
                node->warm_local = 1;
            if (r->current->count > r->max_slots)
                r->max_slots = r->current->count;
            break;
        }

        case NODE_ASSIGN: {
            resolve_expr(r, node->as.assign.value);
            int off = symtable_find(r->current, node->as.assign.var_name);
            if (off < 0) {
                /* may be an instance assign in a function body — leave -1 */
                node->resolved_offset = -1;
            } else {
                node->resolved_offset = off;
            }
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
            /* Resolve the iterable (arr/dyn name) before declaring loop var */
            int arr_off = symtable_find(r->current, node->as.for_stmt.arr_name);
            node->as.for_stmt.arr_resolved_offset = (arr_off >= 0) ? arr_off : -1;
            /* Declare the loop variable */
            int off = symtable_declare(r->current, node->as.for_stmt.var_name);
            node->resolved_offset = off;
            if (r->current->count > r->max_slots)
                r->max_slots = r->current->count;
            resolve_node(r, node->as.for_stmt.body);
            break;
        }

        case NODE_ARR_DECL: {
            if (node->as.arr_decl.default_init) {
                /* Sprint 6.b: scalar default — resolve just the one value */
                resolve_expr(r, node->as.arr_decl.default_value);
            } else {
                for (int i = 0; i < node->as.arr_decl.size; i++)
                    resolve_expr(r, node->as.arr_decl.elements[i]);
            }
            break;
        }

        case NODE_ARR_ASSIGN:
            resolve_expr(r, node->as.arr_assign.index);
            resolve_expr(r, node->as.arr_assign.value);
            /* Resolve array name for fn params */
            {
                int off = symtable_find(r->current, node->as.arr_assign.arr_name);
                node->resolved_offset = (off >= 0) ? off : -1;
            }
            break;

        case NODE_FUNC_DECL: {
            symtable_declare(r->current, node->as.func_decl.name);

            SymTable child;
            symtable_init(&child, r->current);
            SymTable *saved = r->current;
            r->current = &child;

            /* All parameters are local — never prst. Mark them warm_local
             * via in_func_depth so the body's NODE_IDENTIFIER nodes get the
             * flag set automatically. */
            r->in_func_depth++;
            for (int i = 0; i < node->as.func_decl.param_count; i++)
                symtable_declare(&child, node->as.func_decl.param_names[i]);

            resolve_node(r, node->as.func_decl.body);
            r->in_func_depth--;

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

        /* Sprint 5 — #39 ─────────────────────────────────────────────────── */

        case NODE_BLOCK_DECL: {
            /* Register the Block name in the global scope.
             * Resolve the body in an isolated child scope — members get their
             * own offsets, independent from the global stack. */
            symtable_declare(r->current, node->as.block_decl.name);

            SymTable child;
            symtable_init(&child, NULL);  /* no parent — isolated scope */
            SymTable *saved = r->current;
            r->current = &child;

            for (int i = 0; i < node->as.block_decl.count; i++) {
                ASTNode *member = node->as.block_decl.members[i];
                if (member->type == NODE_VAR_DECL) {
                    resolve_expr(r, member->as.var_decl.initializer);
                    int off = symtable_declare(&child, member->as.var_decl.var_name);
                    member->resolved_offset = off;
                } else if (member->type == NODE_FUNC_DECL) {
                    /* Method body resolved in a FULLY ISOLATED scope (parent=NULL).
                     * Block fields (total, x, etc.) are NOT resolved to stack offsets
                     * here — they live in the instance scope and are accessed at
                     * runtime via rt->current_instance. If we gave them the same
                     * offsets as the field symtable (child), both the field and a
                     * local var could get offset 0, causing the bytecode VM to
                     * overwrite the field with the local var value. */
                    symtable_declare(&child, member->as.func_decl.name);

                    SymTable method_scope;
                    symtable_init(&method_scope, NULL);  /* isolated — no parent */
                    SymTable *ms = r->current;
                    r->current = &method_scope;

                    for (int p = 0; p < member->as.func_decl.param_count; p++)
                        symtable_declare(&method_scope,
                                         member->as.func_decl.param_names[p]);

                    resolve_node(r, member->as.func_decl.body);
                    r->current = ms;
                }
            }

            r->current = saved;
            break;
        }

        case NODE_TYPEOF_INST: {
            /* Register the instance name in the current scope.
             * The resolver cannot validate that origin is a BlockDef (not an
             * instance) — that check happens at eval time for now. */
            symtable_declare(r->current, node->as.typeof_inst.inst_name);
            node->resolved_offset = -1;  /* instances live in block registry */
            break;
        }

        /* Sprint 5 — member assign resolved at eval time */
        case NODE_MEMBER_ASSIGN:
            resolve_expr(r, node->as.member_assign.value);
            node->resolved_offset = -1;
            break;

        case NODE_MEMBER_CALL:
            for (int i = 0; i < node->as.member_call.arg_count; i++)
                resolve_expr(r, node->as.member_call.args[i]);
            node->resolved_offset = -1;
            break;

        case NODE_MEMBER_ACCESS:
            node->resolved_offset = -1;
            break;

        /* Sprint 9.c bugfix — indexed member access/call as statements */
        case NODE_INDEXED_MEMBER_CALL:
            resolve_expr(r, node->as.indexed_member_call.index);
            for (int i = 0; i < node->as.indexed_member_call.arg_count; i++)
                resolve_expr(r, node->as.indexed_member_call.args[i]);
            {
                int off = symtable_find(r->current, node->as.indexed_member_call.dyn_name);
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

        /* Sprint 6 */
        case NODE_DANGER:
            resolve_node(r, node->as.danger_stmt.body);
            break;

        case NODE_FREE: {
            /* Resolve the variable name so runtime can find it via stack offset */
            int off = symtable_find(r->current, node->as.free_stmt.var_name);
            node->resolved_offset = (off >= 0) ? off : -1;
            break;
        }

        case NODE_IMPORT_STD:
            /* Standard library — no name resolution needed at parse time.
             * Runtime checks FluxaConfig.std_libs to confirm the lib was
             * declared in [libs] of fluxa.toml before registering it. */
            break;

        /* Sprint 6.b */
        case NODE_IMPORT_C:
            /* library loaded at runtime — nothing to resolve */
            break;

        case NODE_FFI_CALL:
            /* args resolved as expressions */
            for (int i = 0; i < node->as.ffi_call.arg_count; i++)
                resolve_expr(r, node->as.ffi_call.args[i]);
            node->resolved_offset = -1;
            break;

        default:
            resolve_expr(r, node);
            break;
    }
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

    resolve_node(&r, program);

    if (r.had_error) return -1;
    return r.max_slots;
}

/* ── resolver_has_prst ───────────────────────────────────────────────────── */
/* Recursive scan — returns 1 on first prst found anywhere in the AST.
 * Checks VAR_DECL and ARR_DECL with persistent=1.
 * Also scans inside Block declarations and function bodies. */
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
