/* scope.c — Fluxa Variable Scope implementation
 * Sprint 6: VAL_ARR frees the contiguous data array on scope_free
 */
#define _POSIX_C_SOURCE 200809L
#include "scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void value_free_data(Value *v) {
    if (!v) return;
    if (v->type == VAL_STRING && v->as.string) {
        free(v->as.string);
        v->as.string = NULL;
    }
    if (v->type == VAL_ARR && v->as.arr.data && v->as.arr.owned) {
        /* only free when this scope owns the data (owned=1) */
        /* owned=0 means it is a reference — caller retains ownership */
        for (int i = 0; i < v->as.arr.size; i++) {
            if (v->as.arr.data[i].type == VAL_STRING && v->as.arr.data[i].as.string)
                free(v->as.arr.data[i].as.string);
        }
        free(v->as.arr.data);
        v->as.arr.data  = NULL;
        v->as.arr.size  = 0;
        v->as.arr.owned = 0;
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
