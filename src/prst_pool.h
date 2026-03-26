/* prst_pool.h — Persistent Variable Pool (Sprint 6.b)
 *
 * Replaces the PrstGraph stub from Sprint 6.
 *
 * Design:
 *   - Dynamic array via realloc, factor ×2, initial cap 64
 *   - Offsets are integer indices — safe after realloc (no pointer aliases)
 *   - prst is ONLY valid at module/Block/main scope — parser enforces this
 *   - Growth happens at declaration time only, never inside a hot path
 *
 * Sprint 7 hook:
 *   - prst_pool_invalidate() stub — Sprint 7 adds cascade invalidation logic
 */
#ifndef FLUXA_PRST_POOL_H
#define FLUXA_PRST_POOL_H

#include "scope.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PRST_POOL_INIT_CAP 64

/* ── Entry ───────────────────────────────────────────────────────────────── */
typedef struct {
    char  name[256];
    Value value;
} PrstEntry;

/* ── Pool ────────────────────────────────────────────────────────────────── */
typedef struct {
    PrstEntry *entries;   /* heap — realloc'd, factor ×2 */
    int        count;
    int        cap;
} PrstPool;

/* ── API ─────────────────────────────────────────────────────────────────── */

static inline void prst_pool_init(PrstPool *p) {
    p->entries = (PrstEntry *)malloc(sizeof(PrstEntry) * PRST_POOL_INIT_CAP);
    p->count   = 0;
    p->cap     = p->entries ? PRST_POOL_INIT_CAP : 0;
}

static inline void prst_pool_free(PrstPool *p) {
    if (!p->entries) return;
    /* free string values */
    for (int i = 0; i < p->count; i++) {
        if (p->entries[i].value.type == VAL_STRING && p->entries[i].value.as.string)
            free(p->entries[i].value.as.string);
    }
    free(p->entries);
    p->entries = NULL;
    p->count   = 0;
    p->cap     = 0;
}

/* Returns index of entry, or -1 on failure */
static inline int prst_pool_find(PrstPool *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->entries[i].name, name) == 0)
            return i;
    return -1;
}

/* Set a prst variable — creates if not exists, updates if exists.
 * Returns the index, or -1 on allocation failure. */
static inline int prst_pool_set(PrstPool *p, const char *name, Value value) {
    int idx = prst_pool_find(p, name);
    if (idx >= 0) {
        /* update existing — free old string */
        if (p->entries[idx].value.type == VAL_STRING &&
            p->entries[idx].value.as.string)
            free(p->entries[idx].value.as.string);
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        p->entries[idx].value = value;
        return idx;
    }

    /* grow if needed */
    if (p->count >= p->cap) {
        int new_cap = p->cap > 0 ? p->cap * 2 : PRST_POOL_INIT_CAP;
        PrstEntry *new_entries = (PrstEntry *)realloc(
            p->entries, sizeof(PrstEntry) * new_cap);
        if (!new_entries) {
            fprintf(stderr, "[fluxa] prst pool: out of memory growing pool\n");
            return -1;
        }
        p->entries = new_entries;
        p->cap     = new_cap;
    }

    strncpy(p->entries[p->count].name, name, 255);
    p->entries[p->count].name[255] = '\0';
    if (value.type == VAL_STRING && value.as.string)
        value.as.string = strdup(value.as.string);
    p->entries[p->count].value = value;
    return p->count++;
}

/* Get a prst variable. Returns 1 if found, 0 if not. */
static inline int prst_pool_get(PrstPool *p, const char *name, Value *out) {
    int idx = prst_pool_find(p, name);
    if (idx < 0) return 0;
    *out = p->entries[idx].value;
    return 1;
}

/* Sprint 7 hook — stub, no-op until hot reload is implemented */
static inline void prst_pool_invalidate(PrstPool *p, const char *name) {
    (void)p;
    (void)name;
    /* TODO Sprint 7: cascade invalidate all executions that read this prst */
}

#endif /* FLUXA_PRST_POOL_H */
