/* scope.h — Fluxa Variable Scope
 * Sprint 6: VAL_ARR with contiguous heap array (replaces uthash "arr[i]" keys)
 *           VAL_ERR_STACK for err access
 */
#ifndef FLUXA_SCOPE_H
#define FLUXA_SCOPE_H

#include "uthash.h"
#include <stdlib.h>
#include <string.h>

/* ── Value types ─────────────────────────────────────────────────────────── */
typedef enum {
    VAL_NIL,
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STRING,
    VAL_FUNC,
    VAL_BLOCK_INST,
    VAL_ARR,          /* Sprint 6: contiguous heap array                   */
    VAL_ERR_STACK,    /* Sprint 6: reference to Runtime ErrStack           */
    VAL_DYN,          /* Sprint 9.c: heterogeneous dynamic array           */
    VAL_PTR,          /* Sprint 9.c: opaque C pointer — GC never touches   */
} ValType;

/* Forward declarations */
#ifndef FLUXA_AST_NODE_DECLARED
#define FLUXA_AST_NODE_DECLARED
typedef struct ASTNode ASTNode;
#endif
struct BlockInstance;
/* Include err.h for ErrStack — scope.h needs it for VAL_ERR_STACK */
#include "err.h"

/* ── FluxaArr — contiguous heap array ───────────────────────────────────── */
/* Replaces the "arr[0]", "arr[1]"... uthash entries.
 * data is a malloc'd Value array of fixed size.                            */
struct Value;  /* forward for FluxaArr */
typedef struct {
    struct Value *data;
    int           size;
    int           owned;  /* 1 = this scope owns data (free on scope_free) */
                          /* 0 = reference, caller owns data               */
} FluxaArr;

/* ── FluxaDyn — heterogeneous dynamic array ──────────────────────────────── */
/* Grows via realloc. Each element carries its own ValType tag.
 * VAL_PTR elements are stored but never freed by Fluxa GC. */
typedef struct {
    struct Value *items;   /* heap-allocated, grows via realloc             */
    int           count;
    int           cap;
} FluxaDyn;

/* ── Value ───────────────────────────────────────────────────────────────── */
typedef struct Value {
    ValType type;
    union {
        long              integer;
        double            real;
        int               boolean;
        char             *string;
        ASTNode          *func;
        struct BlockInstance *block_inst;
        FluxaArr          arr;
        ErrStack         *err_stack;
        FluxaDyn         *dyn;   /* heap pointer — owned by scope/stack     */
        void             *ptr;   /* opaque C pointer — VAL_PTR              */
    } as;
} Value;

/* ── Constructors ────────────────────────────────────────────────────────── */
static inline Value val_nil(void)        { Value v; v.type=VAL_NIL;                   return v; }
static inline Value val_int(long i)      { Value v; v.type=VAL_INT;   v.as.integer=i; return v; }
static inline Value val_float(double f)  { Value v; v.type=VAL_FLOAT; v.as.real=f;    return v; }
static inline Value val_bool(int b)      { Value v; v.type=VAL_BOOL;  v.as.boolean=b; return v; }

static inline Value val_string(const char *s) {
    Value v;
    v.type = VAL_STRING;
    v.as.string = s ? strdup(s) : strdup("");
    return v;
}

static inline Value val_arr(Value *data, int size) {
    Value v;
    v.type         = VAL_ARR;
    v.as.arr.data  = data;
    v.as.arr.size  = size;
    v.as.arr.owned = 1;   /* owner by default */
    return v;
}

/* val_arr_ref: pass array by reference — caller retains ownership */
static inline Value val_arr_ref(Value *data, int size) {
    Value v;
    v.type         = VAL_ARR;
    v.as.arr.data  = data;
    v.as.arr.size  = size;
    v.as.arr.owned = 0;   /* reference — do NOT free data */
    return v;
}

static inline Value val_dyn(FluxaDyn *d) {
    Value v; v.type = VAL_DYN; v.as.dyn = d; return v;
}

static inline Value val_ptr(void *p) {
    Value v; v.type = VAL_PTR; v.as.ptr = p; return v;
}

/* ── Scope entry ─────────────────────────────────────────────────────────── */
typedef struct ScopeEntry {
    char           name[256];
    Value          value;
    int            persistent;
    UT_hash_handle hh;
} ScopeEntry;

/* ── Scope ───────────────────────────────────────────────────────────────── */
typedef struct {
    ScopeEntry *table;
} Scope;

/* ── Public API ──────────────────────────────────────────────────────────── */
Scope  scope_new(void);
void   scope_set(Scope *s, const char *name, Value value);
int    scope_get(Scope *s, const char *name, Value *out);
int    scope_has(Scope *s, const char *name);
void   scope_free(Scope *s);

/* ── Block clone free callback ──────────────────────────────────────────── */
/* Set by the runtime at init to allow value_free_data to free dyn-owned
 * Block clones without creating a circular dependency (scope.h ← block.h). */
void scope_set_block_free_cb(void (*cb)(void *inst));

/* ── Heap resource helpers ───────────────────────────────────────────────── */
/* Free all heap resources owned by a Value (not the struct itself).
 * No-op for primitives, VAL_PTR, VAL_BLOCK_INST. Recursive for VAL_DYN. */
void   value_free_data(Value *v);

/* Free a FluxaDyn struct and all its items recursively. */
void   fluxa_dyn_free(FluxaDyn *d);

/* ── Global table helpers ────────────────────────────────────────────────── */
/* Direct uthash table operations — used by runtime's global_table field.
 * These bypass the Scope struct and operate on the raw ScopeEntry* table. */

static inline int scope_table_get(ScopeEntry *table, const char *name, Value *out) {
    ScopeEntry *entry = NULL;
    HASH_FIND_STR(table, name, entry);
    if (!entry) return 0;
    *out = entry->value;
    return 1;
}

static inline void scope_table_set(ScopeEntry **table, const char *name, Value value) {
    ScopeEntry *entry = NULL;
    HASH_FIND_STR(*table, name, entry);
    if (entry) {
        if (entry->value.type == VAL_STRING && entry->value.as.string)
            free(entry->value.as.string);
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        entry->value = value;
    } else {
        entry = (ScopeEntry*)calloc(1, sizeof(ScopeEntry));
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->persistent = 0;
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        entry->value = value;
        HASH_ADD_STR(*table, name, entry);
    }
}

static inline void scope_table_free(ScopeEntry **table) {
    if (!table || !*table) return;
    ScopeEntry *e, *tmp;
    HASH_ITER(hh, *table, e, tmp) {
        HASH_DEL(*table, e);
        if (e->value.type == VAL_STRING && e->value.as.string)
            free(e->value.as.string);
        free(e);
    }
    *table = NULL;
}

/* ── Lib linker macro — no-op when compiled, read by gen_lib_registry.py ── */
#ifndef FLUXA_LIB_EXPORT
#define FLUXA_LIB_EXPORT(...) /* scanner-only: ignored by the C compiler */
#endif

#endif /* FLUXA_SCOPE_H */
