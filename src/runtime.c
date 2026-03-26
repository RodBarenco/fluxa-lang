/* runtime.c — Fluxa Runtime
 * Sprint 6: danger/err, arr contíguo, free(), PrstGraph stub, GCTable stub
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
/* Sprint 6: if inside danger, accumulate in err_stack instead of aborting.
 * context_name is the current function/Block name for the ErrEntry. */
static void rt_error(Runtime *rt, const char *msg) {
    if (rt->danger_depth > 0) {
        /* inside danger — push to stack, do NOT set had_error */
        const char *ctx = rt->current_instance
                        ? rt->current_instance->name
                        : "<global>";
        errstack_push(&rt->err_stack, ERR_FLUXA, msg, ctx, 0);
    } else {
        fprintf(stderr, "[fluxa] Runtime error: %s\n", msg);
        rt->had_error = 1;
    }
}

/* ── Variable access ─────────────────────────────────────────────────────── */
static inline Value rt_get(Runtime *rt, ASTNode *node, const char *name) {
    if (node && node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        Value v = rt->stack[node->resolved_offset];
        /* Sprint 6: record prst read for hot-reload graph */
        return v;
    }
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
        if (left.type==VAL_INT   && right.type==VAL_INT)   return val_bool(left.as.integer==right.as.integer);
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) return val_bool(left.as.real==right.as.real);
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  return val_bool(left.as.boolean==right.as.boolean);
        if (left.type==VAL_STRING&& right.type==VAL_STRING) return val_bool(strcmp(left.as.string,right.as.string)==0);
        return val_bool(0);
    }
    if (strcmp(op, "!=") == 0) {
        if (left.type==VAL_INT   && right.type==VAL_INT)   return val_bool(left.as.integer!=right.as.integer);
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) return val_bool(left.as.real!=right.as.real);
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  return val_bool(left.as.boolean!=right.as.boolean);
        if (left.type==VAL_STRING&& right.type==VAL_STRING) return val_bool(strcmp(left.as.string,right.as.string)!=0);
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
        if (r==0) { rt_error(rt, "division by zero"); return val_nil(); }
        double res=l/r; return both_int?val_int((long)res):val_float(res);
    }
    if (strcmp(op,"%")==0) {
        if (!both_int) { rt_error(rt, "modulo requires integer operands"); return val_nil(); }
        if ((long)r==0) { rt_error(rt, "modulo by zero"); return val_nil(); }
        return val_int((long)l % (long)r);
    }
    if (strcmp(op,"<")==0)  return val_bool(l< r);
    if (strcmp(op,">")==0)  return val_bool(l> r);
    if (strcmp(op,"<=")==0) return val_bool(l<=r);
    if (strcmp(op,">=")==0) return val_bool(l>=r);
    rt_error(rt, "unknown operator"); return val_nil();
}

/* ── Function call ───────────────────────────────────────────────────────── */
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
        snprintf(buf, sizeof(buf), "function '%s' expects %d argument(s), got %d",
            fn_node->as.func_decl.name, param_count, arg_count);
        rt_error(rt, buf); return val_nil();
    }

    Value *args = NULL;
    if (param_count > 0) {
        args = (Value*)malloc(sizeof(Value) * param_count);
        for (int i = 0; i < param_count; i++)
            args[i] = eval(rt, arg_nodes[i]);
    }
    if (rt->had_error) { free(args); return val_nil(); }

    Scope          caller_scope = rt->scope;
    int            caller_sz    = rt->stack_size;
    BlockInstance *caller_inst  = rt->current_instance;
    int            save_slots   = (caller_sz < FLUXA_STACK_SIZE) ? caller_sz : FLUXA_STACK_SIZE;
    Value          caller_stack[FLUXA_STACK_SIZE];
    if (save_slots > 0)
        memcpy(caller_stack, rt->stack, sizeof(Value) * save_slots);

    rt->scope            = scope_new();
    rt->stack_size       = 0;
    rt->current_instance = method_inst;
    rt->call_depth++;
    int zero_slots = param_count + 64;
    if (zero_slots > FLUXA_STACK_SIZE) zero_slots = FLUXA_STACK_SIZE;
    for (int i = 0; i < zero_slots; i++) rt->stack[i].type = VAL_NIL;

    for (int i = 0; i < param_count; i++) {
        rt->stack[i] = args[i];
        if (rt->stack_size <= i) rt->stack_size = i + 1;
    }
    free(args);

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

    scope_free(&rt->scope);
    rt->scope            = caller_scope;
    rt->stack_size       = caller_sz;
    rt->current_instance = caller_inst;
    if (save_slots > 0)
        memcpy(rt->stack, caller_stack, sizeof(Value) * save_slots);
    rt->call_depth--;
    return result;
}

/* ── Block init callback ─────────────────────────────────────────────────── */
typedef struct { Runtime *rt; } InitCtx;

static void block_member_init(ASTNode *member, Scope *scope, void *userdata) {
    InitCtx *ctx = (InitCtx*)userdata;
    Runtime *rt  = ctx->rt;
    if (member->type == NODE_VAR_DECL) {
        Value v = eval(rt, member->as.var_decl.initializer);
        if (!rt->had_error)
            scope_set(scope, member->as.var_decl.var_name, v);
    } else if (member->type == NODE_FUNC_DECL) {
        Value v; v.type = VAL_FUNC; v.as.func = member;
        scope_set(scope, member->as.func_decl.name, v);
    }
}

/* ── Resolve instance ────────────────────────────────────────────────────── */
static BlockInstance *resolve_instance(Runtime *rt, const char *owner_name) {
    BlockInstance *inst = block_inst_find(owner_name);
    if (inst) return inst;
    Value v;
    if (scope_get(&rt->scope, owner_name, &v) && v.type == VAL_BLOCK_INST)
        return v.as.block_inst;
    return NULL;
}

/* ── Eval ────────────────────────────────────────────────────────────────── */
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

            /* Sprint 6: err as a value
             * Readable both inside and immediately after danger.
             * Returns nil if stack is empty.
             * errstack_clear() at the START of the next danger block
             * is what resets it — not exiting the block. */
            if (strcmp(name, "err") == 0) {
                if (rt->err_stack.count == 0) return val_nil();
                Value v;
                v.type         = VAL_ERR_STACK;
                v.as.err_stack = &rt->err_stack;
                return v;
            }

            /* Sprint 6: record prst reads for hot-reload graph */
            Value v = rt_get(rt, node, name);
            /* TODO Sprint 7: if persistent, prst_graph_record */
            return v;
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
            if (rt->current_instance &&
                scope_has(&rt->current_instance->scope, node->as.assign.var_name)) {
                scope_set(&rt->current_instance->scope, node->as.assign.var_name, v);
                return val_nil();
            }
            if (node->resolved_offset < 0 &&
                !scope_has(&rt->scope, node->as.assign.var_name)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "assignment to undeclared variable: %s",
                         node->as.assign.var_name);
                rt_error(rt, buf); return val_nil();
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
                                 node->as.list.children, node->as.list.count, NULL);
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
            if (truthy)        eval(rt, node->as.if_stmt.then_body);
            else if (node->as.if_stmt.else_body) eval(rt, node->as.if_stmt.else_body);
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

        /* ── Sprint 6: arr contíguo na heap ─────────────────────────────── */
        case NODE_ARR_DECL: {
            int size = node->as.arr_decl.size;
            /* allocate contiguous Value array via GC table */
            Value *data = (Value*)gc_alloc(&rt->gc, size * (int)sizeof(Value));
            if (!data) { rt_error(rt, "out of memory allocating array"); return val_nil(); }
            for (int i = 0; i < size; i++) {
                data[i] = eval(rt, node->as.arr_decl.elements[i]);
                if (rt->had_error) {
                    gc_free(&rt->gc, data);
                    return val_nil();
                }
            }
            Value arr = val_arr(data, size);
            scope_set(&rt->scope, node->as.arr_decl.arr_name, arr);
            return val_nil();
        }

        case NODE_ARR_ACCESS: {
            Value arr_val;
            const char *arr_name = node->as.arr_access.arr_name;

            /* Sprint 6: special case — err[i]
             * Readable after danger block until next danger clears it */
            if (strcmp(arr_name, "err") == 0) {
                if (rt->err_stack.count == 0)
                    return val_nil();
                Value idx_val = eval(rt, node->as.arr_access.index);
                if (idx_val.type != VAL_INT) {
                    rt_error(rt, "err index must be an integer"); return val_nil();
                }
                const ErrEntry *e = errstack_get(&rt->err_stack, (int)idx_val.as.integer);
                if (!e) return val_nil();
                return val_string(e->message);
            }

            /* normal arr access */
            if (!scope_get(&rt->scope, arr_name, &arr_val)) {
                /* also check instance scope */
                if (rt->current_instance)
                    scope_get(&rt->current_instance->scope, arr_name, &arr_val);
            }
            if (arr_val.type != VAL_ARR) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not an array", arr_name);
                rt_error(rt, buf); return val_nil();
            }
            Value idx_val = eval(rt, node->as.arr_access.index);
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer"); return val_nil();
            }
            long idx = idx_val.as.integer;
            if (idx < 0 || idx >= arr_val.as.arr.size) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld] (size %d)",
                         arr_name, idx, arr_val.as.arr.size);
                rt_error(rt, buf); return val_nil();
            }
            return arr_val.as.arr.data[idx];
        }

        case NODE_ARR_ASSIGN: {
            const char *arr_name = node->as.arr_assign.arr_name;
            Value idx_val = eval(rt, node->as.arr_assign.index);
            if (rt->had_error) return val_nil();
            if (idx_val.type != VAL_INT) {
                rt_error(rt, "array index must be an integer"); return val_nil();
            }
            long idx = idx_val.as.integer;

            /* find the ScopeEntry to mutate in place */
            ScopeEntry *entry = NULL;
            if (rt->current_instance)
                HASH_FIND_STR(rt->current_instance->scope.table, arr_name, entry);
            if (!entry)
                HASH_FIND_STR(rt->scope.table, arr_name, entry);
            if (!entry || entry->value.type != VAL_ARR) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not an array", arr_name);
                rt_error(rt, buf); return val_nil();
            }
            if (idx < 0 || idx >= entry->value.as.arr.size) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld] (size %d)",
                         arr_name, idx, entry->value.as.arr.size);
                rt_error(rt, buf); return val_nil();
            }
            Value v = eval(rt, node->as.arr_assign.value);
            if (rt->had_error) return val_nil();
            /* free old string if needed */
            if (entry->value.as.arr.data[idx].type == VAL_STRING &&
                entry->value.as.arr.data[idx].as.string)
                free(entry->value.as.arr.data[idx].as.string);
            /* copy new value; strdup strings */
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = strdup(v.as.string);
            entry->value.as.arr.data[idx] = v;
            return val_nil();
        }

        case NODE_FOR: {
            Value arr_val;
            const char *arr_name = node->as.for_stmt.arr_name;
            if (!scope_get(&rt->scope, arr_name, &arr_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined array: %s", arr_name);
                rt_error(rt, buf); return val_nil();
            }
            if (arr_val.type != VAL_ARR) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not an array", arr_name);
                rt_error(rt, buf); return val_nil();
            }
            for (int i = 0; i < arr_val.as.arr.size; i++) {
                rt_set(rt, node, node->as.for_stmt.var_name, arr_val.as.arr.data[i]);
                eval(rt, node->as.for_stmt.body);
                if (rt->had_error || rt->ret.active) break;
            }
            return val_nil();
        }

        /* ── Sprint 6: danger ────────────────────────────────────────────── */
        case NODE_DANGER: {
            /* danger is a boundary — clear err before entering */
            errstack_clear(&rt->err_stack);
            rt->danger_depth = 1;

            eval(rt, node->as.danger_stmt.body);

            rt->danger_depth = 0;
            /* errors stay in err_stack but do NOT propagate as had_error */
            rt->had_error = 0;

            /* safe point for GC stub */
            gc_mark_safe_point(&rt->gc);
            return val_nil();
        }

        /* ── Sprint 6: free() ────────────────────────────────────────────── */
        case NODE_FREE: {
            const char *var_name = node->as.free_stmt.var_name;
            /* find and free the value — VAL_ARR frees its data via gc_free */
            ScopeEntry *entry = NULL;
            HASH_FIND_STR(rt->scope.table, var_name, entry);
            if (!entry) {
                char buf[280];
                snprintf(buf, sizeof(buf), "free(): undefined variable '%s'", var_name);
                rt_error(rt, buf); return val_nil();
            }
            if (entry->value.type == VAL_ARR && entry->value.as.arr.data) {
                /* free string elements, then the data array */
                for (int i = 0; i < entry->value.as.arr.size; i++) {
                    if (entry->value.as.arr.data[i].type == VAL_STRING &&
                        entry->value.as.arr.data[i].as.string)
                        free(entry->value.as.arr.data[i].as.string);
                }
                gc_free(&rt->gc, entry->value.as.arr.data);
                entry->value.as.arr.data = NULL;
                entry->value.as.arr.size = 0;
            } else if (entry->value.type == VAL_STRING && entry->value.as.string) {
                free(entry->value.as.string);
                entry->value.as.string = NULL;
            }
            entry->value = val_nil();
            return val_nil();
        }

        /* ── Sprint 5: Block & typeof ────────────────────────────────────── */
        case NODE_BLOCK_DECL: {
            BlockDef *def = block_def_register(node->as.block_decl.name, node);
            InitCtx ctx; ctx.rt = rt;
            BlockInstance *root = block_inst_create(
                node->as.block_decl.name, def, block_member_init, &ctx, 1);
            Value bv; bv.type = VAL_BLOCK_INST; bv.as.block_inst = root;
            scope_set(&rt->scope, node->as.block_decl.name, bv);
            return val_nil();
        }

        case NODE_TYPEOF_INST: {
            const char *origin_name = node->as.typeof_inst.origin_name;
            const char *inst_name   = node->as.typeof_inst.inst_name;
            BlockDef *def = block_def_find(origin_name);
            if (!def) {
                char buf[280];
                BlockInstance *bad = block_inst_find(origin_name);
                if (bad && !bad->is_root)
                    snprintf(buf, sizeof(buf),
                        "typeof: '%s' is a Block instance -- only Block definitions "
                        "can be used as typeof origin, not instances", origin_name);
                else
                    snprintf(buf, sizeof(buf), "typeof: undefined Block '%s'", origin_name);
                rt_error(rt, buf); return val_nil();
            }
            {
                BlockInstance *chk = block_inst_find(origin_name);
                if (chk && !chk->is_root) {
                    char buf[280];
                    snprintf(buf, sizeof(buf),
                        "typeof: '%s' is a Block instance -- only Block definitions "
                        "can be used as typeof origin, not instances", origin_name);
                    rt_error(rt, buf); return val_nil();
                }
            }
            InitCtx ctx; ctx.rt = rt;
            BlockInstance *inst = block_inst_create(inst_name, def, block_member_init, &ctx, 0);
            Value bv; bv.type = VAL_BLOCK_INST; bv.as.block_inst = inst;
            scope_set(&rt->scope, inst_name, bv);
            return val_nil();
        }

        case NODE_MEMBER_ACCESS: {
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
            const char *owner  = node->as.member_call.owner;
            const char *method = node->as.member_call.method;
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
                                 node->as.member_call.arg_count, inst);
        }

        case NODE_PROGRAM:
            return val_nil();

        default:
            return val_nil();
    }
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
    rt.danger_depth     = 0;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc);
    prst_graph_init(&rt.prst_graph);

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        if (rt.had_error) break;
    }

    scope_free(&rt.scope);
    block_registry_free();
    gc_collect_all(&rt.gc);
    prst_graph_free(&rt.prst_graph);
    return rt.had_error ? 1 : 0;
}
