/* runtime.c — Fluxa Runtime implementation */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Value (what a Fluxa expression evaluates to) ────────────────────────── */
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
        char  *string;   /* owned by the AST node — do not free */
    } as;
} Value;

static Value val_nil(void)          { Value v; v.type = VAL_NIL; return v; }
static Value val_int(long i)        { Value v; v.type = VAL_INT;    v.as.integer = i; return v; }
static Value val_float(double f)    { Value v; v.type = VAL_FLOAT;  v.as.real    = f; return v; }
static Value val_bool(int b)        { Value v; v.type = VAL_BOOL;   v.as.boolean = b; return v; }
static Value val_string(char *s)    { Value v; v.type = VAL_STRING; v.as.string  = s; return v; }

/* ── Evaluate a single AST node ──────────────────────────────────────────── */
static Value eval(ASTNode *node);

static void print_value(Value v) {
    switch (v.type) {
        case VAL_NIL:    printf("nil");                              break;
        case VAL_INT:    printf("%ld", v.as.integer);               break;
        case VAL_FLOAT:  printf("%g",  v.as.real);                  break;
        case VAL_BOOL:   printf("%s",  v.as.boolean ? "true":"false"); break;
        case VAL_STRING: printf("%s",  v.as.string);                break;
    }
}

/* built-in: print(arg, ...) */
static Value builtin_print(ASTNode *call) {
    for (int i = 0; i < call->as.list.count; i++) {
        Value v = eval(call->as.list.children[i]);
        print_value(v);
        if (i < call->as.list.count - 1) printf(" ");
    }
    printf("\n");
    return val_nil();
}

/* built-in: len(arg) */
static Value builtin_len(ASTNode *call) {
    if (call->as.list.count != 1) {
        fprintf(stderr, "[fluxa] len() expects exactly 1 argument\n");
        return val_nil();
    }
    Value v = eval(call->as.list.children[0]);
    if (v.type == VAL_STRING) return val_int((long)strlen(v.as.string));
    fprintf(stderr, "[fluxa] len() called on non-string value\n");
    return val_nil();
}

static Value eval(ASTNode *node) {
    if (!node) return val_nil();

    switch (node->type) {
        case NODE_STRING_LIT:  return val_string(node->as.str.value);
        case NODE_INT_LIT:     return val_int(node->as.integer.value);
        case NODE_FLOAT_LIT:   return val_float(node->as.real.value);
        case NODE_BOOL_LIT:    return val_bool(node->as.boolean.value);
        case NODE_IDENTIFIER:
            if (strcmp(node->as.str.value, "nil") == 0) return val_nil();
            fprintf(stderr, "[fluxa] undefined identifier: %s\n", node->as.str.value);
            return val_nil();

        case NODE_FUNC_CALL: {
            const char *name = node->as.list.name;
            if (strcmp(name, "print") == 0) return builtin_print(node);
            if (strcmp(name, "len")   == 0) return builtin_len(node);
            fprintf(stderr, "[fluxa] undefined function: %s\n", name);
            return val_nil();
        }

        case NODE_PROGRAM:
            /* should not be evaluated directly */
            return val_nil();
    }
    return val_nil();
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int runtime_exec(ASTNode *program) {
    if (!program || program->type != NODE_PROGRAM) {
        fprintf(stderr, "[fluxa] runtime: invalid program node\n");
        return 1;
    }
    for (int i = 0; i < program->as.list.count; i++) {
        eval(program->as.list.children[i]);
    }
    return 0;
}
