/* gc.h — Fluxa Garbage Collector
 *
 * Sprint 10: pin/unpin model. Only VAL_DYN enters the GCTable.
 *
 * Storage: open-addressing hash table keyed by pointer, fixed capacity.
 * O(1) average for register/pin/unpin/unregister.
 * O(n) sweep — n = number of tracked dyn objects (bounded by gc_cap).
 *
 * Philosophy: non-aggressive. GC sweeps only at explicit safe points.
 * Manual free() always works and is preferred for large allocations.
 */
#ifndef FLUXA_GC_H
#define FLUXA_GC_H

#include "err.h"
#include "scope.h"  /* for FluxaDyn, fluxa_dyn_free */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define GC_TABLE_CAP 1024   /* default — overridable via fluxa.toml */
#define GC_SLOT_EMPTY  0
#define GC_SLOT_USED   1
#define GC_SLOT_DEAD   2    /* tombstone for open addressing */

typedef struct {
    void  *ptr;        /* key — FluxaDyn* pointer              */
    int    pin_count;  /* >0 = referenced by at least one scope */
    size_t size_bytes; /* telemetry                             */
    int    state;      /* GC_SLOT_EMPTY / USED / DEAD           */
} GCEntry;

typedef struct {
    GCEntry *slots;    /* heap-allocated, cap entries           */
    int      count;    /* number of USED slots                  */
    int      cap;      /* total slot count (always power-of-2)  */
    size_t   total_bytes;
} GCTable;

/* Round up to next power of 2 */
static inline int gc_next_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

static inline void gc_init(GCTable *g, int cap) {
    int c = gc_next_pow2(cap > 0 ? cap : GC_TABLE_CAP);
    g->slots = (GCEntry*)calloc((size_t)c, sizeof(GCEntry));
    g->count = 0;
    g->cap   = g->slots ? c : 0;
    g->total_bytes = 0;
}

/* FNV-32 hash of a pointer */
static inline unsigned gc_hash(void *ptr, int cap) {
    uintptr_t v = (uintptr_t)ptr;
    unsigned h = 2166136261u;
    unsigned char *b = (unsigned char*)&v;
    for (int i = 0; i < (int)sizeof(uintptr_t); i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h & (unsigned)(cap - 1);
}

/* Find slot for ptr. Returns NULL if table is NULL/empty. */
static inline GCEntry *gc_find_slot(GCTable *g, void *ptr) {
    if (!g->slots || g->cap == 0) return NULL;
    unsigned h = gc_hash(ptr, g->cap);
    for (int i = 0; i < g->cap; i++) {
        unsigned idx = (h + (unsigned)i) & (unsigned)(g->cap - 1);
        GCEntry *e = &g->slots[idx];
        if (e->state == GC_SLOT_EMPTY) return NULL;
        if (e->state == GC_SLOT_USED && e->ptr == ptr) return e;
    }
    return NULL;
}

/* Register a new FluxaDyn. pin_count starts at 0. */
static inline void gc_register(GCTable *g, void *ptr, size_t size_bytes,
                                ErrStack *err) {
    if (!ptr || !g->slots) return;
    if (gc_find_slot(g, ptr)) return; /* already registered */

    /* grow if > 75% full */
    if (g->count * 4 >= g->cap * 3) {
        int new_cap = g->cap * 2;
        GCEntry *new_slots = (GCEntry*)calloc((size_t)new_cap, sizeof(GCEntry));
        if (!new_slots) {
            if (err) errstack_push(err, ERR_FLUXA,
                "GC: out of memory growing table", "<gc>", 0);
            return;
        }
        /* rehash */
        for (int i = 0; i < g->cap; i++) {
            if (g->slots[i].state != GC_SLOT_USED) continue;
            unsigned h = gc_hash(g->slots[i].ptr, new_cap);
            for (int j = 0; j < new_cap; j++) {
                unsigned idx = (h + (unsigned)j) & (unsigned)(new_cap - 1);
                if (new_slots[idx].state == GC_SLOT_EMPTY) {
                    new_slots[idx] = g->slots[i];
                    break;
                }
            }
        }
        free(g->slots);
        g->slots = new_slots;
        g->cap   = new_cap;
    }

    unsigned h = gc_hash(ptr, g->cap);
    for (int i = 0; i < g->cap; i++) {
        unsigned idx = (h + (unsigned)i) & (unsigned)(g->cap - 1);
        GCEntry *e = &g->slots[idx];
        if (e->state != GC_SLOT_USED) {
            e->ptr        = ptr;
            e->pin_count  = 0;
            e->size_bytes = size_bytes;
            e->state      = GC_SLOT_USED;
            g->count++;
            g->total_bytes += size_bytes;
            return;
        }
    }
}

static inline void gc_pin(GCTable *g, void *ptr) {
    GCEntry *e = gc_find_slot(g, ptr);
    if (e) e->pin_count++;
}

static inline int gc_unpin(GCTable *g, void *ptr) {
    GCEntry *e = gc_find_slot(g, ptr);
    if (!e) return 0;
    if (e->pin_count > 0) e->pin_count--;
    return (e->pin_count == 0) ? 1 : 0;
}

static inline void gc_unregister(GCTable *g, void *ptr) {
    if (!g->slots) return;
    unsigned h = gc_hash(ptr, g->cap);
    for (int i = 0; i < g->cap; i++) {
        unsigned idx = (h + (unsigned)i) & (unsigned)(g->cap - 1);
        GCEntry *e = &g->slots[idx];
        if (e->state == GC_SLOT_EMPTY) return;
        if (e->state == GC_SLOT_USED && e->ptr == ptr) {
            g->total_bytes -= e->size_bytes;
            g->count--;
            e->state = GC_SLOT_DEAD; /* tombstone */
            return;
        }
    }
}

typedef void (*GCFreeFn)(void *ptr);

static inline int gc_sweep(GCTable *g, GCFreeFn free_fn) {
    int freed = 0;
    if (!g->slots) return 0;
    for (int i = 0; i < g->cap; i++) {
        GCEntry *e = &g->slots[i];
        if (e->state == GC_SLOT_USED && e->pin_count == 0 && e->ptr) {
            if (free_fn) free_fn(e->ptr);
            g->total_bytes -= e->size_bytes;
            g->count--;
            e->state = GC_SLOT_DEAD;
            freed++;
        }
    }
    return freed;
}

static inline void gc_collect_all(GCTable *g, GCFreeFn free_fn) {
    if (!g->slots) return;
    for (int i = 0; i < g->cap; i++) {
        if (g->slots[i].state == GC_SLOT_USED && g->slots[i].ptr && free_fn)
            free_fn(g->slots[i].ptr);
    }
    free(g->slots);
    g->slots       = NULL;
    g->count       = 0;
    g->cap         = 0;
    g->total_bytes = 0;
}

/* Convenience free fn for sweep/collect */
static inline void gc_dyn_free_fn(void *ptr) {
    fluxa_dyn_free((FluxaDyn*)ptr);
}

#endif /* FLUXA_GC_H */
