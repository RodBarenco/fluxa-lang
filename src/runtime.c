/* runtime.c — Fluxa Runtime
 * Sprint 5: Block/typeof eval, member access/call/assign, current_instance
 * Issues #34 (enxugado), #40 (current_instance), #41 (Block eval)
 */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "scope.h"
#include "resolver.h"
#include "bytecode.h"
#include "builtins.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Error helper ────────────────────────────────────────────────────────── */
static void rt_error(Runtime *rt, const char *msg) {
    fprintf(stderr, "[fluxa] Runtime error: %s\n", msg);
    rt->had_error = 1;
}

/* ── Variable access — stack first, scope fallback ───────────────────────── */
static inline Value rt_get(Runtime *rt, ASTNode *node, const char *name) {
    if (node && node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        return rt->stack[node->resolved_offset];
    }
    /* If inside a method, check instance scope first */
    if (rt->current_instance) {
        Value v;
        if (scope_get(&rt->current_instance->scope, name, &v)) return v;
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
    if (node && node->resolved_offset >= 0) {
        if (node->resolved_offset >= rt->stack_size)
            rt->stack_size = node->resolved_offset + 1;
        rt->stack[node->resolved_offset] = v;
        return;
    }
    /* If inside a method, write to instance scope */
    if (rt->current_instance) {
        if (scope_has(&rt->current_instance->scope, name)) {
            scope_set(&rt->current_instance->scope, name, v);
            return;
        }
    }
    scope_set(&rt->scope, name, v);
}

/* ── Forward declaration ─────────────────────────────────────────────────── */
static Value eval(Runtime *rt, ASTNode *node);

/* ── Arithmetic ──────────────────────────────────────────────────────────── */
static Value eval_binary(Runtime *rt, ASTNode *node) {
    Value      left  = eval(rt, node->as.binary.left);
    Value      right = eval(rt, node->as.binary.right);
    const char *op   = node->as.binary.op;

    if (strcmp(op, "==") == 0) {
        if (left.type==VAL_INT    && right.type==VAL_INT)
            return val_bool(left.as.integer == right.as.integer);
        if (left.type==VAL_FLOAT  && right.type==VAL_FLOAT)
            return val_bool(left.as.real == right.as.real);
        if (left.type==VAL_BOOL   && right.type==VAL_BOOL)
            return val_bool(left.as.boolean == right.as.boolean);
        if (left.type==VAL_STRING && right.type==VAL_STRING)
            return val_bool(strcmp(left.as.string, right.as.string)==0);
        return val_bool(0);
    }
    if (strcmp(op, "!=") == 0) {
        if (left.type==VAL_INT    && right.type==VAL_INT)
            return val_bool(left.as.integer != right.as.integer);
        if (left.type==VAL_FLOAT  && right.type==VAL_FLOAT)
            return val_bool(left.as.real != right.as.real);
        if (left.type==VAL_BOOL   && right.type==VAL_BOOL)
            return val_bool(left.as.boolean != right.as.boolean);
        if (left.type==VAL_STRING && right.type==VAL_STRING)
            return val_bool(strcmp(left.as.string, right.as.string)!=0);
        return val_bool(1);
    }

    double l, r;
    int both_int = (left.type==VAL_INT && right.type==VAL_INT);
    if      (left.type==VAL_INT)   l = (double)left.as.integer;
    else if (left.type==VAL_FLOAT) l = left.as.real;
    else { rt_error(rt, "arithmetic on non-numeric value"); return val_nil(); }
    if      (right.type==VAL_INT)   r = (double)right.as.integer;
    else if (right.type==VAL_FLOAT) r = right.as.real;
    else { rt_error(rt, "arithmetic on non-numeric value"); return val_nil(); }

    if (strcmp(op,"+")==0) { double res=l+r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"-")==0) { double res=l-r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"*")==0) { double res=l*r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"/")==0) {
        if (r==0) { rt_error(rt,"division by zero"); return val_nil(); }
        double res=l/r; return both_int?val_int((long)res):val_float(res);
    }
    if (strcmp(op,"%")==0) {
        if (!both_int) { rt_error(rt,"modulo requires integer operands"); return val_nil(); }
        if ((long)r==0) { rt_error(rt,"modulo by zero"); return val_nil(); }
        return val_int((long)l % (long)r);
    }
    if (strcmp(op,"<")==0)  return val_bool(l <  r);
    if (strcmp(op,">")==0)  return val_bool(l >  r);
    if (strcmp(op,"<=")==0) return val_bool(l <= r);
    if (strcmp(op,">=")==0) return val_bool(l >= r);

    rt_error(rt, "unknown operator");
    return val_nil();
}

/* ── User-defined function call ──────────────────────────────────────────── */
static Value call_function(Runtime *rt, ASTNode *fn_node,
                            ASTNode **arg_nodes, int arg_count,
                            BlockInstance *method_inst) {
    if (rt->call_depth >= FLUXA_MAX_DEPTH) {
        rt_error(rt, "stack overflow — max call depth reached");
        return val_nil();
    }

    int param_count = fn_node->as.func_decl.param_count;
    if (arg_count != param_count) {
        char buf[280];
        snprintf(buf, sizeof(buf),
            "function '%s' expects %d argument(s), got %d",
            fn_node->as.func_decl.name, param_count, arg_count);
        rt_error(rt, buf);
        return val_nil();
    }

    /* evaluate arguments in caller's scope */
    Value *args = NULL;
    if (param_count > 0) {
        args = (Value*)malloc(sizeof(Value) * param_count);
        for (int i = 0; i < param_count; i++)
            args[i] = eval(rt, arg_nodes[i]);
    }
    if (rt->had_error) { free(args); return val_nil(); }

    /* save caller state — only copy live slots to minimise memcpy overhead */
    Scope          caller_scope    = rt->scope;
    int            caller_sz       = rt->stack_size;
    BlockInstance *caller_inst     = rt->current_instance;
    /* Clamp save to actual live slots (never more than FLUXA_STACK_SIZE) */
    int            save_slots      = (caller_sz < FLUXA_STACK_SIZE)
                                     ? caller_sz : FLUXA_STACK_SIZE;
    Value          caller_stack[FLUXA_STACK_SIZE];
    if (save_slots > 0)
        memcpy(caller_stack, rt->stack, sizeof(Value) * save_slots);

    /* new frame — zero only the slots we will actually use */
    rt->scope            = scope_new();
    rt->stack_size       = 0;
    rt->current_instance = method_inst;  /* NULL for global fns */
    rt->call_depth++;
    /* zero only param slots + a small buffer for locals (max 64) */
    int zero_slots = param_count + 64;
    if (zero_slots > FLUXA_STACK_SIZE) zero_slots = FLUXA_STACK_SIZE;
    for (int i = 0; i < zero_slots; i++) rt->stack[i].type = VAL_NIL;

    /* bind parameters */
    for (int i = 0; i < param_count; i++) {
        rt->stack[i] = args[i];
        if (rt->stack_size <= i) rt->stack_size = i + 1;
    }
    free(args);

    /* register function itself for recursion */
    Value self; self.type = VAL_FUNC; self.as.func = fn_node;
    scope_set(&rt->scope, fn_node->as.func_decl.name, self);

    rt->ret.active = 0;
    rt->ret.value  = val_nil();

    ASTNode *body = fn_node->as.func_decl.body;
    for (int i = 0; i < body->as.list.count; i++) {
        eval(rt, body->as.list.children[i]);
        if (rt->had_error || rt->ret.active) break;
    }

    Value result = rt->ret.active ? rt->ret.value : val_nil();
    rt->ret.active = 0;

    /* restore caller — only the slots we saved */
    scope_free(&rt->scope);
    rt->scope            = caller_scope;
    rt->stack_size       = caller_sz;
    rt->current_instance = caller_inst;
    if (save_slots > 0)
        memcpy(rt->stack, caller_stack, sizeof(Value) * save_slots);
    rt->call_depth--;

    return result;
}

/* ── Block init callback (used by block_inst_create) ────────────────────── */
typedef struct { Runtime *rt; } InitCtx;

static void block_member_init(ASTNode *member, Scope *scope, void *userdata) {
    InitCtx *ctx = (InitCtx*)userdata;
    Runtime *rt  = ctx->rt;

    if (member->type == NODE_VAR_DECL) {
        Value v = eval(rt, member->as.var_decl.initializer);
        if (!rt->had_error)
            scope_set(scope, member->as.var_decl.var_name, v);
    } else if (member->type == NODE_FUNC_DECL) {
        /* store function pointer in instance scope */
        Value v; v.type = VAL_FUNC; v.as.func = member;
        scope_set(scope, member->as.func_decl.name, v);
    }
}

/* ── Resolve instance: by name from scope or block registry ─────────────── */
static BlockInstance *resolve_instance(Runtime *rt, const char *owner_name) {
    /* check block registry first */
    BlockInstance *inst = block_inst_find(owner_name);
    if (inst) return inst;

    /* check scope (variable holding a block inst value) */
    Value v;
    if (scope_get(&rt->scope, owner_name, &v) && v.type == VAL_BLOCK_INST)
        return v.as.block_inst;

    return NULL;
}

/* ── eval function (exported for builtins.c via EvalFn) ─────────────────── */
static Value eval(Runtime *rt, ASTNode *node) {
    if (!node || rt->had_error) return val_nil();

    switch (node->type) {
        case NODE_STRING_LIT: return val_string(node->as.str.value);
        case NODE_INT_LIT:    return val_int(node->as.integer.value);
        case NODE_FLOAT_LIT:  return val_float(node->as.real.value);
        case NODE_BOOL_LIT:   return val_bool(node->as.boolean.value);

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
            /* If inside a method, try instance scope first */
            if (rt->current_instance &&
                scope_has(&rt->current_instance->scope, node->as.assign.var_name)) {
                scope_set(&rt->current_instance->scope, node->as.assign.var_name, v);
                return val_nil();
            }
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

        case NODE_FUNC_DECL: {
            Value v; v.type = VAL_FUNC; v.as.func = node;
            scope_set(&rt->scope, node->as.func_decl.name, v);
            return val_nil();
        }

        case NODE_RETURN: {
            Value v = node->as.ret.value ? eval(rt, node->as.ret.value) : val_nil();
            rt->ret.active = 1;
            rt->ret.value  = v;
            return v;
        }

        case NODE_FUNC_CALL: {
            const char *name = node->as.list.name;
            if (builtin_is(name))
                return builtin_dispatch(rt, node, (EvalFn)eval);

            Value fn_val;
            if (!scope_get(&rt->scope, name, &fn_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined function: %s", name);
                rt_error(rt, buf); return val_nil();
            }
            if (fn_val.type != VAL_FUNC) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not a function", name);
                rt_error(rt, buf); return val_nil();
            }
            return call_function(rt, fn_val.as.func,
                                 node->as.list.children, node->as.list.count,
                                 NULL);
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
            if      (cond.type==VAL_BOOL)   truthy = cond.as.boolean;
            else if (cond.type==VAL_INT)    truthy = cond.as.integer != 0;
            else if (cond.type==VAL_FLOAT)  truthy = cond.as.real    != 0.0;
            else if (cond.type==VAL_STRING) truthy = cond.as.string && cond.as.string[0];
            if (truthy)
                eval(rt, node->as.if_stmt.then_body);
            else if (node->as.if_stmt.else_body)
                eval(rt, node->as.if_stmt.else_body);
            return val_nil();
        }

        case NODE_WHILE: {
            Chunk chunk;
            if (chunk_compile_loop(&chunk, node)) {
                vm_run(&chunk, &rt->scope, rt->stack, rt->stack_size);
                chunk_free(&chunk);
            } else {
                int limit = 100000000;
                while (limit-- > 0) {
                    Value cond = eval(rt, node->as.while_stmt.condition);
                    if (rt->had_error || rt->ret.active) break;
                    int truthy = 0;
                    if      (cond.type==VAL_BOOL)   truthy = cond.as.boolean;
                    else if (cond.type==VAL_INT)    truthy = cond.as.integer != 0;
                    else if (cond.type==VAL_FLOAT)  truthy = cond.as.real    != 0.0;
                    else if (cond.type==VAL_STRING) truthy = cond.as.string && cond.as.string[0];
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
                snprintf(key, sizeof(key), "%s[%d]", node->as.arr_decl.arr_name, i);
                Value v = eval(rt, node->as.arr_decl.elements[i]);
                if (rt->had_error) return val_nil();
                scope_set(&rt->scope, key, v);
            }
            char size_key[280];
            snprintf(size_key, sizeof(size_key), "%s#size", node->as.arr_decl.arr_name);
            scope_set(&rt->scope, size_key, val_int(size));
            return val_nil();
        }

        case NODE_ARR_ACCESS: {
            Value idx_val = eval(rt, node->as.arr_access.index);
            if (rt->had_error) return val_nil();
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer"); return val_nil();
            }
            char key[280];
            snprintf(key, sizeof(key), "%s[%ld]",
                     node->as.arr_access.arr_name, idx_val.as.integer);
            Value v;
            if (!scope_get(&rt->scope, key, &v)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld]",
                         node->as.arr_access.arr_name, idx_val.as.integer);
                rt_error(rt, buf); return val_nil();
            }
            return v;
        }

        case NODE_ARR_ASSIGN: {
            Value idx_val = eval(rt, node->as.arr_assign.index);
            if (rt->had_error) return val_nil();
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer"); return val_nil();
            }
            char key[280];
            snprintf(key, sizeof(key), "%s[%ld]",
                     node->as.arr_assign.arr_name, idx_val.as.integer);
            if (!scope_has(&rt->scope, key)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld]",
                         node->as.arr_assign.arr_name, idx_val.as.integer);
                rt_error(rt, buf); return val_nil();
            }
            Value v = eval(rt, node->as.arr_assign.value);
            if (rt->had_error) return val_nil();
            scope_set(&rt->scope, key, v);
            return val_nil();
        }

        case NODE_FOR: {
            char size_key[280];
            snprintf(size_key, sizeof(size_key), "%s#size", node->as.for_stmt.arr_name);
            Value size_val;
            if (!scope_get(&rt->scope, size_key, &size_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined array: %s", node->as.for_stmt.arr_name);
                rt_error(rt, buf); return val_nil();
            }
            int size = (int)size_val.as.integer;
            for (int i = 0; i < size; i++) {
                char elem_key[280];
                snprintf(elem_key, sizeof(elem_key), "%s[%d]",
                         node->as.for_stmt.arr_name, i);
                Value elem; elem.type = VAL_NIL;
                scope_get(&rt->scope, elem_key, &elem);
                rt_set(rt, node, node->as.for_stmt.var_name, elem);
                eval(rt, node->as.for_stmt.body);
                if (rt->had_error || rt->ret.active) break;
            }
            return val_nil();
        }

        /* ── Sprint 5 — Block & typeof ─────────────────────────────────── */

        case NODE_BLOCK_DECL: {
            /* #41: register BlockDef, create root instance */
            BlockDef *def = block_def_register(node->as.block_decl.name, node);

            InitCtx ctx; ctx.rt = rt;
            BlockInstance *root = block_inst_create(
                node->as.block_decl.name, def, block_member_init, &ctx, 1);

            /* expose root in global scope as VAL_BLOCK_INST */
            Value bv; bv.type = VAL_BLOCK_INST; bv.as.block_inst = root;
            scope_set(&rt->scope, node->as.block_decl.name, bv);
            return val_nil();
        }

        case NODE_TYPEOF_INST: {
            /* #41: validate origin is a BlockDef, not a user-created instance */
            const char *origin_name = node->as.typeof_inst.origin_name;
            const char *inst_name   = node->as.typeof_inst.inst_name;

            BlockDef *def = block_def_find(origin_name);

            if (!def) {
                /* No BlockDef found. Could be a typeof-created instance or
                 * just an undefined name. */
                char buf[280];
                BlockInstance *bad = block_inst_find(origin_name);
                if (bad && !bad->is_root) {
                    snprintf(buf, sizeof(buf),
                        "typeof: '%s' is a Block instance -- only Block definitions "
                        "can be used as typeof origin, not instances",
                        origin_name);
                } else {
                    snprintf(buf, sizeof(buf),
                        "typeof: undefined Block '%s'", origin_name);
                }
                rt_error(rt, buf);
                return val_nil();
            }

            /* def found -- but verify origin is not a typeof-created instance
             * that happens to share a name with a BlockDef (shouldn't normally
             * happen, but guard it). Root instances (is_root=1) are allowed. */
            {
                BlockInstance *chk = block_inst_find(origin_name);
                if (chk && !chk->is_root) {
                    char buf[280];
                    snprintf(buf, sizeof(buf),
                        "typeof: '%s' is a Block instance -- only Block definitions "
                        "can be used as typeof origin, not instances",
                        origin_name);
                    rt_error(rt, buf);
                    return val_nil();
                }
            }

            InitCtx ctx; ctx.rt = rt;
            BlockInstance *inst = block_inst_create(
                inst_name, def, block_member_init, &ctx, 0);

            Value bv; bv.type = VAL_BLOCK_INST; bv.as.block_inst = inst;
            scope_set(&rt->scope, inst_name, bv);
            return val_nil();
        }


        case NODE_MEMBER_ACCESS: {
            /* inst.field — read from instance scope */
            const char *owner = node->as.member_access.owner;
            const char *field = node->as.member_access.field;

            BlockInstance *inst = resolve_instance(rt, owner);
            if (!inst) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined Block instance: %s", owner);
                rt_error(rt, buf); return val_nil();
            }
            Value v;
            if (!scope_get(&inst->scope, field, &v)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' has no member '%s'", owner, field);
                rt_error(rt, buf); return val_nil();
            }
            return v;
        }

        case NODE_MEMBER_ASSIGN: {
            /* inst.field = val */
            const char *owner = node->as.member_assign.owner;
            const char *field = node->as.member_assign.field;

            BlockInstance *inst = resolve_instance(rt, owner);
            if (!inst) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined Block instance: %s", owner);
                rt_error(rt, buf); return val_nil();
            }
            Value v = eval(rt, node->as.member_assign.value);
            if (rt->had_error) return val_nil();
            scope_set(&inst->scope, field, v);
            return val_nil();
        }

        case NODE_MEMBER_CALL: {
            /* inst.method(args) */
            const char *owner  = node->as.member_call.owner;
            const char *method = node->as.member_call.method;

            /* Special case: print(inst.field) calls on builtins don't reach here,
             * but inst.print() would. Check builtins first just in case. */

            BlockInstance *inst = resolve_instance(rt, owner);
            if (!inst) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined Block instance: %s", owner);
                rt_error(rt, buf); return val_nil();
            }

            Value fn_val;
            if (!scope_get(&inst->scope, method, &fn_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' has no method '%s'", owner, method);
                rt_error(rt, buf); return val_nil();
            }
            if (fn_val.type != VAL_FUNC) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s.%s' is not a function", owner, method);
                rt_error(rt, buf); return val_nil();
            }

            return call_function(rt, fn_val.as.func,
                                 node->as.member_call.args,
                                 node->as.member_call.arg_count,
                                 inst);
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

    int slots = resolver_run(program);
    if (slots < 0) {
        fprintf(stderr, "[fluxa] aborting due to resolver errors.\n");
        return 1;
    }

    Runtime rt;
    rt.scope            = scope_new();
    rt.stack_size       = slots;
    rt.had_error        = 0;
    rt.call_depth       = 0;
    rt.ret.active       = 0;
    rt.ret.value        = val_nil();
    rt.current_instance = NULL;

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        if (rt.had_error) break;
    }

    scope_free(&rt.scope);
    block_registry_free();
    return rt.had_error ? 1 : 0;
}
