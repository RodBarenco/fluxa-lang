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

        /* Issue #17 — block body */
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++) {
                eval(rt, node->as.list.children[i]);
                if (rt->had_error) break;
            }
            return val_nil();

        /* Issue #17 — if / else */
        case NODE_IF: {
            Value cond = eval(rt, node->as.if_stmt.condition);
            int truthy = 0;
            if      (cond.type == VAL_BOOL)    truthy = cond.as.boolean;
            else if (cond.type == VAL_INT)     truthy = cond.as.integer != 0;
            else if (cond.type == VAL_FLOAT)   truthy = cond.as.real    != 0.0;
            else if (cond.type == VAL_STRING)  truthy = cond.as.string && cond.as.string[0];
            if (truthy)
                eval(rt, node->as.if_stmt.then_body);
            else if (node->as.if_stmt.else_body)
                eval(rt, node->as.if_stmt.else_body);
            return val_nil();
        }

        /* Issue #17 — while */
        case NODE_WHILE: {
            int limit = 10000000; /* safety — removed when prst lands */
            while (limit-- > 0) {
                Value cond = eval(rt, node->as.while_stmt.condition);
                if (rt->had_error) break;
                int truthy = 0;
                if      (cond.type == VAL_BOOL)  truthy = cond.as.boolean;
                else if (cond.type == VAL_INT)   truthy = cond.as.integer != 0;
                else if (cond.type == VAL_FLOAT) truthy = cond.as.real    != 0.0;
                if (!truthy) break;
                eval(rt, node->as.while_stmt.body);
                if (rt->had_error) break;
            }
            return val_nil();
        }

        /* Issue #17 — arr declaration */
        case NODE_ARR_DECL: {
            int size = node->as.arr_decl.size;
            /* store each element as arr_name[0], arr_name[1], ... */
            for (int i = 0; i < size; i++) {
                char key[280];
                snprintf(key, sizeof(key), "%s[%d]",
                         node->as.arr_decl.arr_name, i);
                Value v = eval(rt, node->as.arr_decl.elements[i]);
                if (rt->had_error) return val_nil();
                scope_set(&rt->scope, key, v);
            }
            /* store size as arr_name#size */
            char size_key[280];
            snprintf(size_key, sizeof(size_key), "%s#size",
                     node->as.arr_decl.arr_name);
            scope_set(&rt->scope, size_key, val_int(size));
            return val_nil();
        }

        /* Issue #17 — arr[i] access */
        case NODE_ARR_ACCESS: {
            Value idx_val = eval(rt, node->as.arr_access.index);
            if (rt->had_error) return val_nil();
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer");
                return val_nil();
            }
            char key[280];
            snprintf(key, sizeof(key), "%s[%ld]",
                     node->as.arr_access.arr_name, idx_val.as.integer);
            Value v;
            if (!scope_get(&rt->scope, key, &v)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld]",
                         node->as.arr_access.arr_name, idx_val.as.integer);
                rt_error(rt, buf);
                return val_nil();
            }
            return v;
        }

        /* Issue #17 — arr[i] = val */
        case NODE_ARR_ASSIGN: {
            Value idx_val = eval(rt, node->as.arr_assign.index);
            if (rt->had_error) return val_nil();
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer");
                return val_nil();
            }
            char key[280];
            snprintf(key, sizeof(key), "%s[%ld]",
                     node->as.arr_assign.arr_name, idx_val.as.integer);
            if (!scope_has(&rt->scope, key)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld]",
                         node->as.arr_assign.arr_name, idx_val.as.integer);
                rt_error(rt, buf);
                return val_nil();
            }
            Value v = eval(rt, node->as.arr_assign.value);
            if (rt->had_error) return val_nil();
            scope_set(&rt->scope, key, v);
            return val_nil();
        }

        /* Issue #17 — for x in arr */
        case NODE_FOR: {
            char size_key[280];
            snprintf(size_key, sizeof(size_key), "%s#size",
                     node->as.for_stmt.arr_name);
            Value size_val;
            if (!scope_get(&rt->scope, size_key, &size_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined array: %s",
                         node->as.for_stmt.arr_name);
                rt_error(rt, buf);
                return val_nil();
            }
            int size = (int)size_val.as.integer;
            for (int i = 0; i < size; i++) {
                char elem_key[280];
                snprintf(elem_key, sizeof(elem_key), "%s[%d]",
                         node->as.for_stmt.arr_name, i);
                Value elem;
                scope_get(&rt->scope, elem_key, &elem);
                scope_set(&rt->scope, node->as.for_stmt.var_name, elem);
                eval(rt, node->as.for_stmt.body);
                if (rt->had_error) break;
            }
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

/* ── Sprint 3 additions (Issue #17) ─────────────────────────────────────── */
