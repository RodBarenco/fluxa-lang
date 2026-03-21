/* runtime.c — Fluxa Runtime implementation */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Scope scope;
    int   had_error;
} Runtime;

static void rt_error(Runtime *rt, const char *msg) {
    fprintf(stderr, "[fluxa] Runtime error: %s\n", msg);
    rt->had_error = 1;
}

static Value eval(Runtime *rt, ASTNode *node);

static void print_value(Value v) {
    switch (v.type) {
        case VAL_NIL:    printf("nil");                                break;
        case VAL_INT:    printf("%ld",  v.as.integer);                 break;
        case VAL_FLOAT:  printf("%g",   v.as.real);                    break;
        case VAL_BOOL:   printf("%s",   v.as.boolean ? "true":"false"); break;
        case VAL_STRING: printf("%s",   v.as.string);                  break;
    }
}

static Value builtin_print(Runtime *rt, ASTNode *call) {
    for (int i = 0; i < call->as.list.count; i++) {
        Value v = eval(rt, call->as.list.children[i]);
        print_value(v);
        if (i < call->as.list.count - 1) printf(" ");
    }
    printf("\n");
    return val_nil();
}

static Value builtin_len(Runtime *rt, ASTNode *call) {
    if (call->as.list.count != 1) {
        rt_error(rt, "len() expects exactly 1 argument");
        return val_nil();
    }
    Value v = eval(rt, call->as.list.children[0]);
    if (v.type == VAL_STRING) return val_int((long)strlen(v.as.string));
    rt_error(rt, "len() called on non-string value");
    return val_nil();
}

static Value eval_binary(Runtime *rt, ASTNode *node) {
    Value      left  = eval(rt, node->as.binary.left);
    Value      right = eval(rt, node->as.binary.right);
    const char *op   = node->as.binary.op;

    /* equality */
    if (strcmp(op, "==") == 0) {
        if (left.type == VAL_INT    && right.type == VAL_INT)
            return val_bool(left.as.integer == right.as.integer);
        if (left.type == VAL_FLOAT  && right.type == VAL_FLOAT)
            return val_bool(left.as.real == right.as.real);
        if (left.type == VAL_BOOL   && right.type == VAL_BOOL)
            return val_bool(left.as.boolean == right.as.boolean);
        if (left.type == VAL_STRING && right.type == VAL_STRING)
            return val_bool(strcmp(left.as.string, right.as.string) == 0);
        return val_bool(0);
    }
    if (strcmp(op, "!=") == 0) {
        if (left.type == VAL_INT    && right.type == VAL_INT)
            return val_bool(left.as.integer != right.as.integer);
        if (left.type == VAL_FLOAT  && right.type == VAL_FLOAT)
            return val_bool(left.as.real != right.as.real);
        if (left.type == VAL_BOOL   && right.type == VAL_BOOL)
            return val_bool(left.as.boolean != right.as.boolean);
        if (left.type == VAL_STRING && right.type == VAL_STRING)
            return val_bool(strcmp(left.as.string, right.as.string) != 0);
        return val_bool(1);
    }

    /* numeric */
    double l, r;
    int both_int = (left.type == VAL_INT && right.type == VAL_INT);

    if      (left.type == VAL_INT)   l = (double)left.as.integer;
    else if (left.type == VAL_FLOAT) l = left.as.real;
    else { rt_error(rt, "arithmetic on non-numeric value"); return val_nil(); }

    if      (right.type == VAL_INT)   r = (double)right.as.integer;
    else if (right.type == VAL_FLOAT) r = right.as.real;
    else { rt_error(rt, "arithmetic on non-numeric value"); return val_nil(); }

    if (strcmp(op, "+")  == 0) { double res=l+r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op, "-")  == 0) { double res=l-r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op, "*")  == 0) { double res=l*r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op, "/")  == 0) {
        if (r == 0) { rt_error(rt, "division by zero"); return val_nil(); }
        double res=l/r; return both_int?val_int((long)res):val_float(res);
    }
    if (strcmp(op, "%")  == 0) {
        if (!both_int) { rt_error(rt, "modulo requires integer operands"); return val_nil(); }
        if ((long)r == 0) { rt_error(rt, "modulo by zero"); return val_nil(); }
        return val_int((long)l % (long)r);
    }
    if (strcmp(op, "<")  == 0) return val_bool(l <  r);
    if (strcmp(op, ">")  == 0) return val_bool(l >  r);
    if (strcmp(op, "<=") == 0) return val_bool(l <= r);
    if (strcmp(op, ">=") == 0) return val_bool(l >= r);

    rt_error(rt, "unknown operator");
    return val_nil();
}

static Value eval(Runtime *rt, ASTNode *node) {
    if (!node || rt->had_error) return val_nil();

    switch (node->type) {
        case NODE_STRING_LIT:  return val_string(node->as.str.value);
        case NODE_INT_LIT:     return val_int(node->as.integer.value);
        case NODE_FLOAT_LIT:   return val_float(node->as.real.value);
        case NODE_BOOL_LIT:    return val_bool(node->as.boolean.value);

        case NODE_IDENTIFIER: {
            const char *name = node->as.str.value;
            if (strcmp(name, "nil") == 0) return val_nil();
            Value v;
            if (!scope_get(&rt->scope, name, &v)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined variable: %s", name);
                rt_error(rt, buf);
                return val_nil();
            }
            return v;
        }

        case NODE_VAR_DECL: {
            Value v = eval(rt, node->as.var_decl.initializer);
            if (rt->had_error) return val_nil();
            scope_set(&rt->scope, node->as.var_decl.var_name, v);
            return val_nil();
        }

        case NODE_ASSIGN: {
            const char *name = node->as.assign.var_name;
            if (!scope_has(&rt->scope, name)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "assignment to undeclared variable: %s", name);
                rt_error(rt, buf);
                return val_nil();
            }
            Value v = eval(rt, node->as.assign.value);
            if (rt->had_error) return val_nil();
            scope_set(&rt->scope, name, v);
            return val_nil();
        }

        case NODE_BINARY_EXPR:
            return eval_binary(rt, node);

        case NODE_FUNC_CALL: {
            const char *name = node->as.list.name;
            if (strcmp(name, "print") == 0) return builtin_print(rt, node);
            if (strcmp(name, "len")   == 0) return builtin_len(rt, node);
            char buf[280];
            snprintf(buf, sizeof(buf), "undefined function: %s", name);
            rt_error(rt, buf);
            return val_nil();
        }

        case NODE_PROGRAM:
            return val_nil();
    }
    return val_nil();
}

int runtime_exec(ASTNode *program) {
    if (!program || program->type != NODE_PROGRAM) {
        fprintf(stderr, "[fluxa] runtime: invalid program node\n");
        return 1;
    }
    Runtime rt;
    rt.scope     = scope_new();
    rt.had_error = 0;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        if (rt.had_error) break;
    }

    scope_free(&rt.scope);
    return rt.had_error ? 1 : 0;
}
