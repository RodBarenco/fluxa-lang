/* block.h — Fluxa Block system (Sprint 5)
 * BlockDef  = molde (Block Foo { ... })
 * BlockInstance = instância com escopo próprio (Block raiz ou typeof)
 */
#ifndef FLUXA_BLOCK_H
#define FLUXA_BLOCK_H

#include "scope.h"
#include "ast.h"
#include "../vendor/uthash.h"

/* ── BlockDef — the template ─────────────────────────────────────────────── */
typedef struct BlockDef {
    char      name[64];
    ASTNode  *node;           /* NODE_BLOCK_DECL — source of truth for init */
    UT_hash_handle hh;
} BlockDef;

/* ── BlockInstance — a live instance with its own scope ──────────────────── */
typedef struct BlockInstance {
    char            name[64];
    BlockDef       *def;      /* never NULL, never points to another instance */
    Scope           scope;
    int             is_root;  /* 1 = auto-created root (same name as BlockDef) */
    UT_hash_handle  hh;
} BlockInstance;

/* ── Global registries ───────────────────────────────────────────────────── */
/* These are managed by the runtime — declared extern here, defined in block.c */
extern BlockDef      *g_block_defs;       /* uthash table of BlockDef      */
extern BlockInstance *g_block_instances;  /* uthash table of BlockInstance */

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Register a BlockDef (called when NODE_BLOCK_DECL is evaluated) */
BlockDef *block_def_register(const char *name, ASTNode *node);

/* Find a BlockDef by name — NULL if not found */
BlockDef *block_def_find(const char *name);

/* Find a BlockInstance by name — NULL if not found */
BlockInstance *block_inst_find(const char *name);

/* Create a BlockInstance from a BlockDef.
 * exec_init_fn is a callback: exec_init_fn(node, scope, userdata)
 * Used to run var_decl initializers without circular deps. */
typedef void (*BlockInitFn)(ASTNode *member, Scope *scope, void *userdata);

BlockInstance *block_inst_create(const char *inst_name, BlockDef *def,
                                  BlockInitFn init_fn, void *userdata,
                                  int is_root);

/* Free all global state — called at runtime shutdown */
void block_registry_free(void);

#endif /* FLUXA_BLOCK_H */
