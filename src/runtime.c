/* runtime.c — Fluxa Runtime implementation */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "scope.h"
#include "resolver.h"
#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Call stack ──────────────────────────────────────────────────────────── */
#define FLUXA_MAX_DEPTH  1000
#define FLUXA_STACK_SIZE 512   /* max local variables per frame */

/* Issue #21: return signal — propagates value up through eval frames */
typedef struct {
    int   active;   /* 1 = a return was executed */
    Value value;
} ReturnSignal;

/* ── Runtime state ───────────────────────────────────────────────────────── */
typedef struct {
    Scope        scope;
    Value        stack[FLUXA_STACK_SIZE]; /* flat variable storage */
    int          stack_size;              /* slots in use          */
    int          had_error;
    int          call_depth;
    ReturnSignal ret;
} Runtime;

static void rt_error(Runtime *rt, const char *msg) {
    fprintf(stderr, "[fluxa] Runtime error: %s\n", msg);
    rt->had_error = 1;
}

/* ── Variable access — stack first, uthash fallback ─────────────────────── */
static inline Value rt_get(Runtime *rt, ASTNode *node, const char *name) {
    if (node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        return rt->stack[node->resolved_offset];
    }
    Value v; v.type = VAL_NIL;
    if (!scope_get(&rt->scope, name, &v)) {
        char buf[280];
        snprintf(buf, sizeof(buf), "undefined variable: %s", name);
        rt_error(rt, buf);
    }
    return v;
}

static inline void rt_set(Runtime *rt, ASTNode *node,
                           const char *name, Value v) {
    if (node->resolved_offset >= 0) {
        if (node->resolved_offset >= rt->stack_size)
            rt->stack_size = node->resolved_offset + 1;
        rt->stack[node->resolved_offset] = v;
        return;
    }
    scope_set(&rt->scope, name, v);
}

static Value eval(Runtime *rt, ASTNode *node);

/* ── Print helper ────────────────────────────────────────────────────────── */
static void print_value(Value v) {
    switch (v.type) {
        case VAL_NIL:    printf("nil");                                break;
        case VAL_INT:    printf("%ld",  v.as.integer);                 break;
        case VAL_FLOAT:  printf("%g",   v.as.real);                    break;
        case VAL_BOOL:   printf("%s",   v.as.boolean ? "true":"false"); break;
        case VAL_STRING: printf("%s",   v.as.string);                  break;
        case VAL_FUNC:   printf("<fn %s>", v.as.func->as.func_decl.name); break;
    }
}

/* ── Built-ins ───────────────────────────────────────────────────────────── */
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

/* ── Arithmetic ──────────────────────────────────────────────────────────── */
static Value eval_binary(Runtime *rt, ASTNode *node) {
    Value      left  = eval(rt, node->as.binary.left);
    Value      right = eval(rt, node->as.binary.right);
    const char *op   = node->as.binary.op;

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

/* ── Issue #21: call user-defined function ───────────────────────────────── */
static Value call_function(Runtime *rt, ASTNode *fn_node, ASTNode *call_node) {
    if (rt->call_depth >= FLUXA_MAX_DEPTH) {
        rt_error(rt, "stack overflow — max call depth reached");
        return val_nil();
    }

    int param_count = fn_node->as.func_decl.param_count;
    int arg_count   = call_node->as.list.count;

    if (arg_count != param_count) {
        char buf[280];
        snprintf(buf, sizeof(buf),
            "function '%s' expects %d argument(s), got %d",
            fn_node->as.func_decl.name, param_count, arg_count);
        rt_error(rt, buf);
        return val_nil();
    }

    /* evaluate arguments in the CALLER's scope */
    Value *args = NULL;
    if (param_count > 0) {
        args = (Value*)malloc(sizeof(Value) * param_count);
        for (int i = 0; i < param_count; i++)
            args[i] = eval(rt, call_node->as.list.children[i]);
    }
    if (rt->had_error) { free(args); return val_nil(); }

    /* save caller state */
    Scope  caller_scope      = rt->scope;
    int    caller_stack_size = rt->stack_size;
    Value  caller_stack[FLUXA_STACK_SIZE];
    memcpy(caller_stack, rt->stack, sizeof(Value) * FLUXA_STACK_SIZE);

    /* create new frame */
    rt->scope      = scope_new();
    rt->stack_size = 0;
    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt->stack[i].type = VAL_NIL;
    rt->call_depth++;

    /* bind parameters — use stack offsets 0..N */
    for (int i = 0; i < param_count; i++) {
        rt->stack[i]       = args[i];
        if (rt->stack_size <= i) rt->stack_size = i + 1;
    }
    free(args);

    /* register function itself for recursion */
    Value self; self.type = VAL_FUNC; self.as.func = fn_node;
    scope_set(&rt->scope, fn_node->as.func_decl.name, self);

    /* clear return signal */
    rt->ret.active = 0;
    rt->ret.value  = val_nil();

    /* execute body */
    ASTNode *body = fn_node->as.func_decl.body;
    for (int i = 0; i < body->as.list.count; i++) {
        eval(rt, body->as.list.children[i]);
        if (rt->had_error || rt->ret.active) break;
    }

    Value result = rt->ret.active ? rt->ret.value : val_nil();
    rt->ret.active = 0;

    /* restore caller state */
    scope_free(&rt->scope);
    rt->scope      = caller_scope;
    rt->stack_size = caller_stack_size;
    memcpy(rt->stack, caller_stack, sizeof(Value) * FLUXA_STACK_SIZE);
    rt->call_depth--;

    return result;
}

/* ── Eval ────────────────────────────────────────────────────────────────── */
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
            return rt_get(rt, node, name);
        }

        case NODE_VAR_DECL: {
            Value v = eval(rt, node->as.var_decl.initializer);
            if (rt->had_error) return val_nil();
            rt_set(rt, node, node->as.var_decl.var_name, v);
            return val_nil();
        }

        case NODE_ASSIGN: {
            Value v = eval(rt, node->as.assign.value);
            if (rt->had_error) return val_nil();
            /* check declared if falling back to scope */
            if (node->resolved_offset < 0 &&
                !scope_has(&rt->scope, node->as.assign.var_name)) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "assignment to undeclared variable: %s",
                    node->as.assign.var_name);
                rt_error(rt, buf);
                return val_nil();
            }
            rt_set(rt, node, node->as.assign.var_name, v);
            return val_nil();
        }

        case NODE_BINARY_EXPR:
            return eval_binary(rt, node);

        /* Issue #21 — function declaration: register in scope */
        case NODE_FUNC_DECL: {
            /* store the ASTNode pointer as a function value */
            Value v;
            v.type      = VAL_FUNC;
            v.as.func   = node;
            scope_set(&rt->scope, node->as.func_decl.name, v);
            return val_nil();
        }

        /* Issue #21 — return statement */
        case NODE_RETURN: {
            Value v = node->as.ret.value
                ? eval(rt, node->as.ret.value)
                : val_nil();
            rt->ret.active = 1;
            rt->ret.value  = v;
            return v;
        }

        case NODE_FUNC_CALL: {
            const char *name = node->as.list.name;
            /* built-ins */
            if (strcmp(name, "print") == 0) return builtin_print(rt, node);
            if (strcmp(name, "len")   == 0) return builtin_len(rt, node);

            /* Issue #21 — user-defined function */
            Value fn_val;
            if (!scope_get(&rt->scope, name, &fn_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined function: %s", name);
                rt_error(rt, buf);
                return val_nil();
            }
            if (fn_val.type != VAL_FUNC) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not a function", name);
                rt_error(rt, buf);
                return val_nil();
            }
            return call_function(rt, fn_val.as.func, node);
        }

        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++) {
                eval(rt, node->as.list.children[i]);
                if (rt->had_error || rt->ret.active) break;
            }
            return val_nil();

        case NODE_IF: {
            Value cond = eval(rt, node->as.if_stmt.condition);
            int truthy = 0;
            if      (cond.type == VAL_BOOL)   truthy = cond.as.boolean;
            else if (cond.type == VAL_INT)    truthy = cond.as.integer != 0;
            else if (cond.type == VAL_FLOAT)  truthy = cond.as.real    != 0.0;
            else if (cond.type == VAL_STRING) truthy = cond.as.string && cond.as.string[0];
            if (truthy)
                eval(rt, node->as.if_stmt.then_body);
            else if (node->as.if_stmt.else_body)
                eval(rt, node->as.if_stmt.else_body);
            return val_nil();
        }

        case NODE_WHILE: {
            Chunk body_chunk, cond_chunk;
            int use_bc = chunk_compile_body(&body_chunk,
                             node->as.while_stmt.body);
            int use_cc = 0;
            if (use_bc) {
                chunk_init(&cond_chunk);
                compile_expr(&cond_chunk, node->as.while_stmt.condition);
                if (cond_chunk.ok) {
                    Instruction r2; r2.op = OP_RETURN;
                    chunk_emit(&cond_chunk, r2);
                    use_cc = 1;
                } else {
                    chunk_free(&cond_chunk);
                    chunk_free(&body_chunk);
                    use_bc = 0;
                }
            }

            if (use_bc) {
                /* Issue #22 — O(1) seeding: use pre-computed list */
                int sc = node->as.while_stmt.seed_count;
                for (int k = 0; k < sc; k++) {
                    const char *name = node->as.while_stmt.seed_vars[k].name;
                    int off          = node->as.while_stmt.seed_vars[k].offset;
                    if (off >= 0 && off < rt->stack_size)
                        scope_set(&rt->scope, name, rt->stack[off]);
                }

                /* ── FAST PATH: bytecode loop with computed gotos ── */
                while (1) {
                    VMStack cvs; cvs.top = 0;
                    for (int ip = 0; ip < cond_chunk.count; ip++) {
                        Instruction *ins = &cond_chunk.code[ip];
                        if (ins->op == OP_RETURN) break;
                        switch (ins->op) {
                            case OP_PUSH_INT: vs_push(&cvs,val_int(ins->arg.ival)); break;
                            case OP_LOAD: {
                                if (!ins->cached_entry)
                                    HASH_FIND_STR(rt->scope.table,ins->arg.sval,ins->cached_entry);
                                vs_push(&cvs,ins->cached_entry?ins->cached_entry->value:val_nil());
                                break;
                            }
                            case OP_LT:  {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_LT));  break;}
                            case OP_GT:  {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_GT));  break;}
                            case OP_LTE: {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_LTE)); break;}
                            case OP_GTE: {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_GTE)); break;}
                            case OP_EQ:  {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_EQ));  break;}
                            case OP_NEQ: {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_compare(l,r2,OP_NEQ)); break;}
                            case OP_ADD: {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_arith(l,r2,OP_ADD)); break;}
                            case OP_SUB: {Value r2=vs_pop(&cvs);Value l=vs_pop(&cvs);vs_push(&cvs,vm_arith(l,r2,OP_SUB)); break;}
                            default: break;
                        }
                    }
                    if (cvs.top==0 || !vm_truthy(vs_pop(&cvs))) break;
                    vm_run(&body_chunk, &rt->scope, rt->stack, rt->stack_size);
                    if (rt->had_error || rt->ret.active) break;
                }

                /* Issue #22 — O(1) sync back: scope → stack */
                for (int k = 0; k < sc; k++) {
                    const char *name = node->as.while_stmt.seed_vars[k].name;
                    int off          = node->as.while_stmt.seed_vars[k].offset;
                    Value v; v.type = VAL_NIL;
                    scope_get(&rt->scope, name, &v);
                    if (off >= 0 && off < FLUXA_STACK_SIZE)
                        rt->stack[off] = v;
                }

                chunk_free(&body_chunk);
                chunk_free(&cond_chunk);
            } else {
                /* ── SLOW PATH: AST walk ── */
                int limit = 100000000;
                while (limit-- > 0) {
                    Value cond = eval(rt, node->as.while_stmt.condition);
                    if (rt->had_error || rt->ret.active) break;
                    int truthy = 0;
                    if      (cond.type==VAL_BOOL)  truthy = cond.as.boolean;
                    else if (cond.type==VAL_INT)   truthy = cond.as.integer != 0;
                    else if (cond.type==VAL_FLOAT) truthy = cond.as.real    != 0.0;
                    if (!truthy) break;
                    eval(rt, node->as.while_stmt.body);
                    if (rt->had_error || rt->ret.active) break;
                }
            }
            return val_nil();
        }

        case NODE_ARR_DECL: {
            int size = node->as.arr_decl.size;
            for (int i = 0; i < size; i++) {
                char key[280];
                snprintf(key, sizeof(key), "%s[%d]",
                         node->as.arr_decl.arr_name, i);
                Value v = eval(rt, node->as.arr_decl.elements[i]);
                if (rt->had_error) return val_nil();
                scope_set(&rt->scope, key, v);
            }
            char size_key[280];
            snprintf(size_key, sizeof(size_key), "%s#size",
                     node->as.arr_decl.arr_name);
            scope_set(&rt->scope, size_key, val_int(size));
            return val_nil();
        }

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
                Value elem; elem.type = VAL_NIL;
                scope_get(&rt->scope, elem_key, &elem);
                /* use rt_set — loop var may be on stack (resolver) */
                rt_set(rt, node, node->as.for_stmt.var_name, elem);
                eval(rt, node->as.for_stmt.body);
                if (rt->had_error || rt->ret.active) break;
            }
            return val_nil();
        }

        case NODE_PROGRAM:
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

    /* Step 3: run name resolver — converts var names to stack offsets */
    int slots = resolver_run(program);
    if (slots < 0) {
        fprintf(stderr, "[fluxa] aborting due to resolver errors.\n");
        return 1;
    }

    Runtime rt;
    rt.scope      = scope_new();
    rt.stack_size = slots;
    rt.had_error  = 0;
    rt.call_depth = 0;
    rt.ret.active = 0;
    rt.ret.value  = val_nil();

    /* zero-initialise the value stack */
    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        if (rt.had_error) break;
    }

    scope_free(&rt.scope);
    return rt.had_error ? 1 : 0;
}
