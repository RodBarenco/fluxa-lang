/* block.c — Fluxa Block system implementation (Sprint 5) */
#define _POSIX_C_SOURCE 200809L
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Global registries ───────────────────────────────────────────────────── */
BlockDef      *g_block_defs      = NULL;
BlockInstance *g_block_instances = NULL;

/* ── BlockDef ─────────────────────────────────────────────────────────────── */
BlockDef *block_def_register(const char *name, ASTNode *node) {
    /* if already registered, update */
    BlockDef *existing = block_def_find(name);
    if (existing) {
        existing->node = node;
        return existing;
    }
    BlockDef *def = (BlockDef*)calloc(1, sizeof(BlockDef));
    strncpy(def->name, name, sizeof(def->name) - 1);
    def->node = node;
    HASH_ADD_STR(g_block_defs, name, def);
    return def;
}

BlockDef *block_def_find(const char *name) {
    BlockDef *def = NULL;
    HASH_FIND_STR(g_block_defs, name, def);
    return def;
}

/* ── BlockInstance ────────────────────────────────────────────────────────── */
BlockInstance *block_inst_find(const char *name) {
    BlockInstance *inst = NULL;
    HASH_FIND_STR(g_block_instances, name, inst);
    return inst;
}

BlockInstance *block_inst_create(const char *inst_name, BlockDef *def,
                                  BlockInitFn init_fn, void *userdata,
                                  int is_root) {
    BlockInstance *old = block_inst_find(inst_name);
    if (old) {
        HASH_DEL(g_block_instances, old);
        scope_free(&old->scope);
        free(old);
    }

    BlockInstance *inst = (BlockInstance*)calloc(1, sizeof(BlockInstance));
    strncpy(inst->name, inst_name, sizeof(inst->name) - 1);
    inst->def     = def;
    inst->scope   = scope_new();
    inst->is_root = is_root;

    /* Run initializers from the Block definition — var_decl members only.
     * func_decl members are stored by name in the instance scope as VAL_FUNC.
     * This ensures complete isolation: each instance gets its own copy of
     * initial values evaluated fresh from the AST, not from any runtime state. */
    ASTNode *block_node = def->node;
    for (int i = 0; i < block_node->as.block_decl.count; i++) {
        ASTNode *member = block_node->as.block_decl.members[i];
        if (init_fn) init_fn(member, &inst->scope, userdata);
    }

    HASH_ADD_STR(g_block_instances, name, inst);
    return inst;
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */
void block_registry_free(void) {
    /* free instances */
    BlockInstance *inst, *itmp;
    HASH_ITER(hh, g_block_instances, inst, itmp) {
        HASH_DEL(g_block_instances, inst);
        scope_free(&inst->scope);
        free(inst);
    }
    g_block_instances = NULL;

    /* free defs */
    BlockDef *def, *dtmp;
    HASH_ITER(hh, g_block_defs, def, dtmp) {
        HASH_DEL(g_block_defs, def);
        free(def);
    }
    g_block_defs = NULL;
}

/* ── block_inst_clone — typeof-implicit for dyn insertion ─────────────────── */
/* Creates a new BlockInstance that is NOT in g_block_instances.
 * Copies current runtime state of src by copying each ScopeEntry value.
 * The clone is owned by the dyn — free with block_inst_free_unregistered(). */

static int g_clone_counter = 0;

BlockInstance *block_inst_clone(const BlockInstance *src) {
    if (!src) return NULL;

    BlockInstance *clone = (BlockInstance*)calloc(1, sizeof(BlockInstance));
    if (!clone) return NULL;

    /* Auto-generate a unique name — not added to registry */
    /* Truncate src name to ensure "_dynNNNN" suffix fits in 64-char field */
    snprintf(clone->name, sizeof(clone->name),
             "%.50s_dyn%d", src->name, g_clone_counter++);
    clone->def     = src->def;
    clone->scope   = scope_new();
    clone->is_root = 0;

    /* Copy each ScopeEntry value from src to clone.
     * scope_set handles strdup for strings — full deep copy of primitives.
     * VAL_ARR: create a fresh copy of the data array (owned=1).
     * VAL_FUNC: shared pointer to ASTNode — safe, AST is immutable. */
    ScopeEntry *entry, *tmp;
    HASH_ITER(hh, src->scope.table, entry, tmp) {
        Value v = entry->value;
        if (v.type == VAL_ARR && v.as.arr.data && v.as.arr.size > 0) {
            /* Deep copy array */
            Value *new_data = (Value*)malloc(
                sizeof(Value) * (size_t)v.as.arr.size);
            if (new_data) {
                for (int i = 0; i < v.as.arr.size; i++) {
                    new_data[i] = v.as.arr.data[i];
                    if (new_data[i].type == VAL_STRING && new_data[i].as.string)
                        new_data[i].as.string = strdup(new_data[i].as.string);
                }
                v = val_arr(new_data, v.as.arr.size); /* owned=1 */
            }
        }
        /* scope_set handles VAL_STRING strdup internally */
        scope_set(&clone->scope, entry->name, v);
    }

    return clone;
}

void block_inst_free_unregistered(BlockInstance *inst) {
    if (!inst) return;
    scope_free(&inst->scope);
    free(inst);
}
