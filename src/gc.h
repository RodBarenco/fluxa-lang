/* gc.h — Fluxa Garbage Collector (Sprint 7)
 *
 * Sprint 6: stub — gc_alloc/gc_free wrapped malloc/free, no tracking.
 * Sprint 7: real cap enforcement + configurable gc_cap from fluxa.toml.
 *
 * GCTable tracks every heap allocation made through gc_alloc().
 * The cap is runtime-configurable (default GC_TABLE_CAP = 1024) via
 * fluxa.toml [runtime] gc_cap. Overflow pushes to err_stack without crash.
 *
 * Philosophy: non-aggressive — GC only runs at explicit safe points.
 * Manual free() coexists. Sweep pass deferred to future sprint.
 */
#ifndef FLUXA_GC_H
#define FLUXA_GC_H

#include "err.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GC_TABLE_CAP 1024   /* default — overridable via fluxa.toml */

typedef struct {
    void *ptr;
    int   size;
    int   marked;
    int   pinned;
} GCEntry;

typedef struct {
    GCEntry entries[GC_TABLE_CAP];
    int     count;
    int     cap;          /* Sprint 7: runtime cap — default GC_TABLE_CAP  */
    size_t  total_bytes;
} GCTable;

static inline void gc_init(GCTable *g, int cap) {
    memset(g, 0, sizeof(GCTable));
    g->cap = (cap > 0 && cap <= GC_TABLE_CAP) ? cap : GC_TABLE_CAP;
}

/* Allocate memory. Tracks in the GCTable up to cap entries.
 * On overflow: pushes to err_stack and returns NULL — no crash. */
static inline void *gc_alloc(GCTable *g, int size, ErrStack *err) {
    if (g->count >= g->cap) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "GC cap reached (%d objects) — free memory or increase gc_cap in fluxa.toml",
            g->cap);
        if (err) errstack_push(err, ERR_FLUXA, buf, "<gc>", 0);
        return NULL;
    }
    void *ptr = malloc((size_t)size);
    if (!ptr) {
        if (err) errstack_push(err, ERR_FLUXA, "out of memory in gc_alloc", "<gc>", 0);
        return NULL;
    }
    g->entries[g->count].ptr    = ptr;
    g->entries[g->count].size   = size;
    g->entries[g->count].marked = 0;
    g->entries[g->count].pinned = 0;
    g->count++;
    g->total_bytes += (size_t)size;
    return ptr;
}

/* Free memory manually — removes from tracking table. */
static inline void gc_free(GCTable *g, void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < g->count; i++) {
        if (g->entries[i].ptr == ptr) {
            g->total_bytes -= (size_t)g->entries[i].size;
            free(ptr);
            /* compact: move last entry into this slot */
            g->entries[i] = g->entries[g->count - 1];
            g->count--;
            return;
        }
    }
    /* Not tracked — free anyway (e.g. manually allocated before GC init) */
    free(ptr);
}

/* Safe point marker — future sweep pass runs here */
static inline void gc_mark_safe_point(GCTable *g) {
    (void)g;
    /* TODO future sprint: mark reachable from scopes, sweep unmarked */
}

/* Collect all tracked allocations — called at runtime shutdown. */
static inline void gc_collect_all(GCTable *g) {
    for (int i = 0; i < g->count; i++) {
        if (g->entries[i].ptr && !g->entries[i].pinned) {
            free(g->entries[i].ptr);
            g->entries[i].ptr = NULL;
        }
    }
    g->count       = 0;
    g->total_bytes = 0;
}

#endif /* FLUXA_GC_H */
