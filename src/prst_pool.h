/* prst_pool.h — Persistent Variable Pool (Sprint 7)
 *
 * Sprint 6.b: dynamic array, realloc ×2, basic get/set.
 * Sprint 7:   real semantics:
 *   - Type collision detection: same name + different type → error
 *   - Reload semantics: on re-declaration of existing prst, value is
 *     restored from pool instead of re-evaluated from AST initializer
 *   - Only active in FLUXA_MODE_PROJECT (prst present in source)
 *   - In FLUXA_MODE_SCRIPT: prst declarations emit a warning, pool unused
 */
#ifndef FLUXA_PRST_POOL_H
#define FLUXA_PRST_POOL_H

#include "scope.h"
#include "err.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PRST_POOL_INIT_CAP 64

typedef struct {
    char    name[256];
    Value   value;
    ValType declared_type;
    int     stack_offset;   /* resolved_offset of the VAR_DECL node, -1 if unknown */
} PrstEntry;

typedef struct {
    PrstEntry *entries;
    int        count;
    int        cap;
} PrstPool;

static inline void prst_pool_init(PrstPool *p) {
    p->entries = (PrstEntry *)malloc(sizeof(PrstEntry) * PRST_POOL_INIT_CAP);
    p->count   = 0;
    p->cap     = p->entries ? PRST_POOL_INIT_CAP : 0;
}

static inline void prst_pool_free(PrstPool *p) {
    if (!p->entries) return;
    for (int i = 0; i < p->count; i++) {
        if (p->entries[i].value.type == VAL_STRING && p->entries[i].value.as.string)
            free(p->entries[i].value.as.string);
    }
    free(p->entries);
    p->entries = NULL;
    p->count   = 0;
    p->cap     = 0;
}

static inline int prst_pool_find(PrstPool *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->entries[i].name, name) == 0)
            return i;
    return -1;
}

/* Set a prst variable.
 * First declaration: stores value + declared_type.
 * Re-declaration (reload): type must match — collision → err, returns -1.
 * Returns index on success, -1 on error. */
static inline int prst_pool_set(PrstPool *p, const char *name,
                                  Value value, ErrStack *err) {
    int idx = prst_pool_find(p, name);
    if (idx >= 0) {
        if (p->entries[idx].declared_type != value.type) {
            char buf[280];
            snprintf(buf, sizeof(buf),
                "prst collision: '%s' was type %d, reload attempts type %d — state preserved",
                name, (int)p->entries[idx].declared_type, (int)value.type);
            if (err) errstack_push(err, ERR_RELOAD, buf, "<prst>", 0);
            return -1;
        }
        if (p->entries[idx].value.type == VAL_STRING &&
            p->entries[idx].value.as.string)
            free(p->entries[idx].value.as.string);
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        p->entries[idx].value = value;
        return idx;
    }
    if (p->count >= p->cap) {
        int new_cap = p->cap > 0 ? p->cap * 2 : PRST_POOL_INIT_CAP;
        PrstEntry *ne = (PrstEntry *)realloc(p->entries,
                            sizeof(PrstEntry) * new_cap);
        if (!ne) {
            if (err) errstack_push(err, ERR_FLUXA,
                "out of memory growing prst pool", "<prst>", 0);
            return -1;
        }
        p->entries = ne;
        p->cap     = new_cap;
    }
    strncpy(p->entries[p->count].name, name, 255);
    p->entries[p->count].name[255]     = '\0';
    p->entries[p->count].declared_type = value.type;
    if (value.type == VAL_STRING && value.as.string)
        value.as.string = strdup(value.as.string);
    p->entries[p->count].value        = value;
    p->entries[p->count].stack_offset = -1;   /* set by caller after decl */
    return p->count++;
}

/* Set the stack_offset for a prst entry — called once after NODE_VAR_DECL
 * resolution so the VM sync pass can read rt->stack[offset] directly. */
static inline void prst_pool_set_offset(PrstPool *p, const char *name,
                                         int offset) {
    int idx = prst_pool_find(p, name);
    if (idx >= 0) p->entries[idx].stack_offset = offset;
}

static inline int prst_pool_get(PrstPool *p, const char *name, Value *out) {
    int idx = prst_pool_find(p, name);
    if (idx < 0) return 0;
    *out = p->entries[idx].value;
    return 1;
}

static inline int prst_pool_has(PrstPool *p, const char *name) {
    return prst_pool_find(p, name) >= 0;
}

static inline void prst_pool_invalidate(PrstPool *p, const char *name) {
    int idx = prst_pool_find(p, name);
    if (idx < 0) return;
    if (p->entries[idx].value.type == VAL_STRING &&
        p->entries[idx].value.as.string)
        free(p->entries[idx].value.as.string);
    p->entries[idx] = p->entries[p->count - 1];
    p->count--;
}

#endif /* FLUXA_PRST_POOL_H */
