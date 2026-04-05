/* scope.c — Fluxa Variable Scope implementation
 * Sprint 6:   VAL_ARR frees the contiguous data array on scope_free
 * Sprint 9.c: VAL_DYN frees FluxaDyn + all items recursively
 */
#define _POSIX_C_SOURCE 200809L
#include "scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declaration ─────────────────────────────────────────────────── */
void value_free_data(Value *v);

/* Callback for freeing dyn-owned Block clones (set by runtime, avoids circular deps) */
static void (*g_block_inst_free_cb)(void *inst) = NULL;

void scope_set_block_free_cb(void (*cb)(void *inst)) {
    g_block_inst_free_cb = cb;
}

/* Free all heap resources owned by a FluxaDyn. */
void fluxa_dyn_free(FluxaDyn *d) {
    if (!d) return;
    if (d->items) {
        for (int i = 0; i < d->count; i++)
            value_free_data(&d->items[i]);
        free(d->items);
    }
    free(d);
}

void value_free_data(Value *v) {
    /* Ownership rules:
     *   VAL_STRING   — free the strdup'd char*
     *   VAL_ARR      — free items + data when owned=1 (reference: owned=0, skip)
     *   VAL_DYN      — free FluxaDyn + all items recursively
     *   VAL_PTR      — opaque C pointer, NOT freed (FFI lib owns it)
     *   VAL_BLOCK_INST — NOT freed here (block_registry owns all instances)
     *   primitives   — no heap allocation, nothing to do
     */
    if (!v) return;
    switch (v->type) {
        case VAL_STRING:
            if (v->as.string) { free(v->as.string); v->as.string = NULL; }
            break;
        case VAL_ARR:
            if (v->as.arr.data && v->as.arr.owned) {
                for (int i = 0; i < v->as.arr.size; i++)
                    value_free_data(&v->as.arr.data[i]);
                free(v->as.arr.data);
                v->as.arr.data  = NULL;
                v->as.arr.size  = 0;
                v->as.arr.owned = 0;
            }
            break;
        case VAL_DYN:
            if (v->as.dyn) { fluxa_dyn_free(v->as.dyn); v->as.dyn = NULL; }
            break;
        case VAL_BLOCK_INST:
            /* Only free if it is a dyn-owned clone (not in global registry).
             * The callback is set by the runtime to avoid circular includes. */
            if (v->as.block_inst && g_block_inst_free_cb)
                g_block_inst_free_cb(v->as.block_inst);
            v->as.block_inst = NULL;
            break;
        /* VAL_PTR and primitives: not owned by this scope */
        default:
            break;
    }
}

Scope scope_new(void) {
    Scope s; s.table = NULL; return s;
}

void scope_set(Scope *s, const char *name, Value value) {
    ScopeEntry *entry = NULL;
    HASH_FIND_STR(s->table, name, entry);
    if (entry) {
        value_free_data(&entry->value);
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        /* Note: VAL_ARR ownership transfers — caller must not free data */
        entry->value = value;
    } else {
        entry = (ScopeEntry*)calloc(1, sizeof(ScopeEntry));
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->persistent = 0;
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        entry->value = value;
        HASH_ADD_STR(s->table, name, entry);
    }
}

int scope_get(Scope *s, const char *name, Value *out) {
    ScopeEntry *entry = NULL;
    HASH_FIND_STR(s->table, name, entry);
    if (!entry) return 0;
    *out = entry->value;
    return 1;
}

int scope_has(Scope *s, const char *name) {
    ScopeEntry *entry = NULL;
    HASH_FIND_STR(s->table, name, entry);
    return entry != NULL;
}

void scope_free(Scope *s) {
    ScopeEntry *entry, *tmp;
    HASH_ITER(hh, s->table, entry, tmp) {
        HASH_DEL(s->table, entry);
        value_free_data(&entry->value);
        free(entry);
    }
    s->table = NULL;
}
