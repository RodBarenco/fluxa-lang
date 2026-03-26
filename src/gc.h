/* gc.h — Fluxa Garbage Collector (Sprint 6 stub)
 *
 * Philosophy:
 *   - Non-aggressive: GC only runs at explicit safe points, never mid-execution
 *   - Manual free() coexists — user has full control over explicit frees
 *   - GC only collects what was NOT manually freed
 *   - Sprint 6: infrastructure only — mark/track allocations, no collection yet
 *   - Future sprint: add sweep pass at safe points (function return, end of danger)
 *
 * Design:
 *   - Every heap allocation that should be GC-managed goes through gc_alloc()
 *   - GC tracks these pointers in a fixed-size table (no dynamic growth)
 *   - free() removes from the table explicitly
 *   - At a safe point, gc_collect() sweeps unmarked entries
 *
 * Sprint 6 delivers: gc_alloc(), gc_free(), gc_mark_safe_point() (no-op),
 *                    GCTable in Runtime.
 * Future sprint delivers: mark phase, sweep phase, reachability from scopes.
 */
#ifndef FLUXA_GC_H
#define FLUXA_GC_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GC_TABLE_CAP 1024

typedef struct {
    void *ptr;
    int   size;       /* bytes allocated — for stats/budgeting */
    int   marked;     /* reachability mark — used in future sweep */
    int   pinned;     /* 1 = manually managed, never collected by GC */
} GCEntry;

typedef struct {
    GCEntry entries[GC_TABLE_CAP];
    int     count;
    size_t  total_bytes;   /* running total — useful for debugging */
} GCTable;

static inline void gc_init(GCTable *g) {
    memset(g, 0, sizeof(GCTable));
}

/* Allocate memory. Sprint 6: wraps malloc — GC tracking deferred to
 * future GC sprint. scope_free() / value_free_data() own the lifetime
 * of arr data; registering here causes double-free at gc_collect_all.
 * The GCTable struct and gc_mark_safe_point() are retained as stubs
 * so Sprint 7+ can add real tracking without changing the API. */
static inline void *gc_alloc(GCTable *g, int size) {
    (void)g;
    return malloc((size_t)size);
}

/* Free memory manually. Sprint 6: wraps free — GC table not used yet. */
static inline void gc_free(GCTable *g, void *ptr) {
    (void)g;
    free(ptr);
}

/* Safe point marker — no-op now, future sprint runs sweep here */
static inline void gc_mark_safe_point(GCTable *g) {
    (void)g;
    /* TODO future sprint: mark reachable from scopes, sweep unmarked */
}

/* Sprint 6: no-op — memory is owned by scope/block and freed there.
 * Future GC sprint adds real sweep here. */
static inline void gc_collect_all(GCTable *g) {
    (void)g;
}

#endif /* FLUXA_GC_H */
