/* scope.h — Fluxa Variable Scope */
#ifndef FLUXA_SCOPE_H
#define FLUXA_SCOPE_H

#include "../vendor/uthash.h"

typedef enum {
    VAL_NIL,
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STRING,
    VAL_FUNC,
    VAL_BLOCK_INST,   /* Sprint 5: pointer to BlockInstance */
} ValType;

#ifndef FLUXA_AST_NODE_DECLARED
#define FLUXA_AST_NODE_DECLARED
typedef struct ASTNode ASTNode;
#endif

/* Forward declaration for BlockInstance */
struct BlockInstance;

typedef struct {
    ValType type;
    union {
        long              integer;
        double            real;
        int               boolean;
        char             *string;
        ASTNode          *func;
        struct BlockInstance *block_inst;  /* Sprint 5 */
    } as;
} Value;

static inline Value val_nil(void)        { Value v; v.type = VAL_NIL;                    return v; }
static inline Value val_int(long i)      { Value v; v.type = VAL_INT;    v.as.integer=i;  return v; }
static inline Value val_float(double f)  { Value v; v.type = VAL_FLOAT;  v.as.real=f;     return v; }
static inline Value val_bool(int b)      { Value v; v.type = VAL_BOOL;   v.as.boolean=b;  return v; }

static inline Value val_string(const char *s) {
    Value v;
    v.type = VAL_STRING;
    v.as.string = s ? strdup(s) : strdup("");
    return v;
}

typedef struct ScopeEntry {
    char           name[256];
    Value          value;
    int            persistent;
    UT_hash_handle hh;
} ScopeEntry;

typedef struct {
    ScopeEntry *table;
} Scope;

Scope  scope_new(void);
void   scope_set(Scope *s, const char *name, Value value);
int    scope_get(Scope *s, const char *name, Value *out);
int    scope_has(Scope *s, const char *name);
void   scope_free(Scope *s);

#endif /* FLUXA_SCOPE_H */
