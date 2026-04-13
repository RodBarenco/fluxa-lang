/* builtins.c — Fluxa Built-in Functions implementation (Sprint 5, Issue #35) */
#define _POSIX_C_SOURCE 200809L
#include "builtins.h"
#include "runtime.h"
#include "err.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *BUILTINS[] = { "print", "len", "input", "input_int", "str_alloc", NULL };

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
        case VAL_DYN: {
            FluxaDyn *d = v.as.dyn;
            printf("[");
            if (d) {
                for (int i = 0; i < d->count; i++) {
                    if (i > 0) printf(", ");
                    print_value(d->items[i]);
                }
            }
            printf("]");
            break;
        }
        case VAL_PTR:
            printf("<ptr %p>", v.as.ptr);
            break;
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
        rt->had_error = 1;
        fprintf(stderr, "[fluxa] Runtime error: len() expects exactly 1 argument\n");
        return val_nil();
    }
    Value v = eval_fn(rt, call->as.list.children[0]);
    if (rt->had_error) return val_nil();
    if (v.type == VAL_STRING) {
        return val_int((long)strlen(v.as.string ? v.as.string : ""));
    }
    if (v.type == VAL_ARR) {
        return val_int((long)v.as.arr.size);
    }
    if (v.type == VAL_DYN) {
        return val_int((long)(v.as.dyn ? v.as.dyn->count : 0));
    }
    rt->had_error = 1;
    fprintf(stderr, "[fluxa] Runtime error: len() called on non-iterable value (got %s)\n",
            v.type == VAL_INT   ? "int"   :
            v.type == VAL_FLOAT ? "float" :
            v.type == VAL_BOOL  ? "bool"  : "unknown");
    return val_nil();
}

/* str_alloc(n) — allocate a mutable char buffer of n bytes for FFI out-params.
 * Returns a str backed by calloc(n+1) — all zeroes, ready for C to write into.
 * The caller owns the buffer; it lives as long as the variable is in scope. */
static Value builtin_str_alloc(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    if (call->as.list.count != 1) {
        rt->had_error = 1;
        fprintf(stderr, "[fluxa] Runtime error: str_alloc() expects exactly 1 argument\n");
        return val_nil();
    }
    Value n = eval_fn(rt, call->as.list.children[0]);
    if (rt->had_error) return val_nil();
    if (n.type != VAL_INT || n.as.integer <= 0) {
        rt->had_error = 1;
        fprintf(stderr, "[fluxa] Runtime error: str_alloc() argument must be a positive int\n");
        return val_nil();
    }
    size_t sz = (size_t)n.as.integer;
    char *buf = calloc(sz + 1, 1);   /* zero-initialised, +1 for null terminator */
    if (!buf) { rt->had_error = 1; return val_nil(); }
    Value v; v.type = VAL_STRING; v.as.string = buf;
    return v;
}

/* input() — reads a line from stdin, returns str (newline stripped) */
static Value builtin_input(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    /* optional prompt argument */
    if (call->as.list.count > 0) {
        Value prompt = eval_fn(rt, call->as.list.children[0]);
        if (prompt.type == VAL_STRING && prompt.as.string)
            printf("%s", prompt.as.string);
        fflush(stdout);
    }
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        /* EOF or error — return empty string */
        Value v; v.type = VAL_STRING; v.as.string = "";
        return v;
    }
    /* strip trailing newline */
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    if (len > 0 && buf[len-1] == '\r') buf[--len] = '\0';
    /* copy to heap so the caller owns the string */
    char *s = malloc((size_t)len + 1);
    if (!s) { rt->had_error = 1; return val_nil(); }
    memcpy(s, buf, (size_t)len + 1);
    Value v; v.type = VAL_STRING; v.as.string = s;
    return v;
}

/* input_int() — reads a line from stdin, parses as int */
static Value builtin_input_int(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    if (call->as.list.count > 0) {
        Value prompt = eval_fn(rt, call->as.list.children[0]);
        if (prompt.type == VAL_STRING && prompt.as.string)
            printf("%s", prompt.as.string);
        fflush(stdout);
    }
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return val_int(0);
    return val_int((long)atol(buf));
}

Value builtin_dispatch(struct Runtime *rt, ASTNode *call, EvalFn eval_fn) {
    const char *name = call->as.list.name;
    if (strcmp(name, "print") == 0) return builtin_print(rt, call, eval_fn);
    if (strcmp(name, "len")       == 0) return builtin_len(rt, call, eval_fn);
    if (strcmp(name, "input")     == 0) return builtin_input(rt, call, eval_fn);
    if (strcmp(name, "input_int") == 0) return builtin_input_int(rt, call, eval_fn);
    if (strcmp(name, "str_alloc") == 0) return builtin_str_alloc(rt, call, eval_fn);
    return val_nil();
}
