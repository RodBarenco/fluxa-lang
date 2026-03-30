/* builtins.c — Fluxa Built-in Functions implementation (Sprint 5, Issue #35) */
#define _POSIX_C_SOURCE 200809L
#include "builtins.h"
#include "runtime.h"
#include "err.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *BUILTINS[] = { "print", "len", NULL };

int builtin_is(const char *name) {
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(name, BUILTINS[i]) == 0) return 1;
    return 0;
}

static void print_value(Value v) {
    switch (v.type) {
        case VAL_NIL:        printf("nil"); break;
        case VAL_INT:        printf("%ld",  v.as.integer); break;
        case VAL_FLOAT:      printf("%g",   v.as.real); break;
        case VAL_BOOL:       printf("%s",   v.as.boolean ? "true" : "false"); break;
        case VAL_STRING:     printf("%s",   v.as.string); break;
        case VAL_FUNC:       printf("<fn %s>", v.as.func->as.func_decl.name); break;
        case VAL_BLOCK_INST: printf("<Block %s>", v.as.block_inst->name); break;
        case VAL_ARR: {
            printf("[");
            for (int i = 0; i < v.as.arr.size; i++) {
                if (i > 0) printf(", ");
                print_value(v.as.arr.data[i]);
            }
            printf("]");
            break;
        }
        case VAL_ERR_STACK:
            errstack_print((const ErrStack *)v.as.err_stack);
            return;   /* errstack_print adds its own newline per entry */
    }
}

static Value builtin_print(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    /* Sprint 7.b: dry_run suppresses all output (used during handover validation) */
    if (rt->dry_run) {
        for (int i = 0; i < call->as.list.count; i++)
            eval_fn(rt, call->as.list.children[i]); /* evaluate for side-effects only */
        return val_nil();
    }
    for (int i = 0; i < call->as.list.count; i++) {
        Value v = eval_fn(rt, call->as.list.children[i]);
        print_value(v);
        if (i < call->as.list.count - 1) printf(" ");
    }
    printf("\n");
    return val_nil();
}

static Value builtin_len(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    if (call->as.list.count != 1) {
        fprintf(stderr, "[fluxa] Runtime error: len() expects exactly 1 argument\n");
        return val_nil();
    }
    Value v = eval_fn(rt, call->as.list.children[0]);
    if (v.type == VAL_STRING) return val_int((long)strlen(v.as.string));
    fprintf(stderr, "[fluxa] Runtime error: len() called on non-string value\n");
    return val_nil();
}

Value builtin_dispatch(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    const char *name = call->as.list.name;
    if (strcmp(name, "print") == 0) return builtin_print(rt, call, eval_fn);
    if (strcmp(name, "len")   == 0) return builtin_len(rt, call, eval_fn);
    return val_nil();
}
