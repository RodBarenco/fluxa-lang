/* scope.h — Fluxa Variable Scope
 * Hash table of variables for the current execution context.
 * Uses uthash for O(1) lookup.
 */
#ifndef FLUXA_SCOPE_H
#define FLUXA_SCOPE_H

#include "../vendor/uthash.h"

/* ── Value types ─────────────────────────────────────────────────────────── */
typedef enum {
    VAL_NIL,
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STRING,
} ValType;

typedef struct {
    ValType type;
    union {
        long   integer;
        double real;
        int    boolean;
        char  *string;   /* heap-allocated — owned by the scope entry */
    } as;
} Value;

/* ── Constructors ────────────────────────────────────────────────────────── */
static inline Value val_nil(void)        { Value v; v.type = VAL_NIL;                   return v; }
static inline Value val_int(long i)      { Value v; v.type = VAL_INT;    v.as.integer=i; return v; }
static inline Value val_float(double f)  { Value v; v.type = VAL_FLOAT;  v.as.real=f;    return v; }
static inline Value val_bool(int b)      { Value v; v.type = VAL_BOOL;   v.as.boolean=b; return v; }

/* string value — scope takes ownership of a strdup'd copy */
static inline Value val_string(const char *s) {
    Value v;
    v.type = VAL_STRING;
    v.as.string = s ? strdup(s) : strdup("");
    return v;
}

/* ── Scope entry (uthash node) ───────────────────────────────────────────── */
typedef struct ScopeEntry {
    char           name[256];   /* hash key */
    Value          value;
    int            persistent;  /* prst flag — Sprint 6 */
    UT_hash_handle hh;
} ScopeEntry;

/* ── Scope ───────────────────────────────────────────────────────────────── */
typedef struct {
    ScopeEntry *table;          /* uthash head */
} Scope;

/* ── Public API ──────────────────────────────────────────────────────────── */
Scope  scope_new(void);
void   scope_set(Scope *s, const char *name, Value value);
int    scope_get(Scope *s, const char *name, Value *out);  /* 1=found, 0=not found */
int    scope_has(Scope *s, const char *name);
void   scope_free(Scope *s);

#endif /* FLUXA_SCOPE_H */
