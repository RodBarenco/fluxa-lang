/* scope.h — Fluxa Variable Scope
 * Sprint 6: VAL_ARR with contiguous heap array (replaces uthash "arr[i]" keys)
 *           VAL_ERR_STACK for err access
 */
#ifndef FLUXA_SCOPE_H
#define FLUXA_SCOPE_H

#include "../vendor/uthash.h"
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

#endif /* FLUXA_SCOPE_H */
