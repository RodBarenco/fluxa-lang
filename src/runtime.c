/* runtime.c — Fluxa Runtime
 * Sprint 6:   danger/err, contiguous arr, free(), PrstGraph stub, GCTable stub
 * Sprint 8:   rt_error_line (linha nos erros), runtime_exec_with_rt (Handover)
 * Sprint 9:   IPC safe-point hook (ipc_apply_pending_set)
 */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "scope.h"
#include "resolver.h"
#include "bytecode.h"
#include "builtins.h"
#include "block.h"
#include "fluxa_ffi.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Sprint 9: IPC pending-set hook — defined in ipc_server.c. */
extern void ipc_apply_pending_set(Runtime *rt);
struct IpcRtView;
extern void ipc_rtview_update(struct IpcRtView *view, Runtime *rt);

/* Global IPC view pointer — set by run_dev before each exec.
 * NULL in script mode and on RP2040. */
static struct IpcRtView *g_ipc_view = NULL;

void runtime_set_ipc_view(void *view) {
    g_ipc_view = (struct IpcRtView *)view;
}

/* ── Global cancel flag for -dev mode ────────────────────────────────────── */
static volatile int  *g_cancel_flag = NULL;

void runtime_set_cancel_flag(volatile int *flag) {
    g_cancel_flag = flag;
}

/* ── Error helpers ────────────────────────────────────────────────────────── */
/* Sprint 8: rt_error_line inclui número de linha na mensagem e no ErrEntry.
 * rt_error é mantido para compatibilidade — chama rt_error_line com line=0. */
static void rt_error_line(Runtime *rt, const char *msg, int line) {
    /* Se não veio linha explícita, usa a última linha rastreada */
    int eff_line = (line > 0) ? line : rt->current_line;
    if (rt->danger_depth > 0) {
        const char *ctx = rt->current_instance
                        ? rt->current_instance->name
                        : "<global>";
        errstack_push(&rt->err_stack, ERR_FLUXA, msg, ctx, eff_line);
    } else {
        if (eff_line > 0)
            fprintf(stderr, "[fluxa] Runtime error (line %d): %s\n", eff_line, msg);
        else
            fprintf(stderr, "[fluxa] Runtime error: %s\n", msg);
        rt->had_error = 1;
    }
}

static void rt_error(Runtime *rt, const char *msg) {
    rt_error_line(rt, msg, 0);
}

/* ── Variable access ─────────────────────────────────────────────────────── */
static inline Value rt_get(Runtime *rt, ASTNode *node, const char *name) {
    if (node && node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        Value v = rt->stack[node->resolved_offset];
        return v;
    }
    if (rt->current_instance) {
        Value v;
        if (scope_get(&rt->current_instance->scope, name, &v)) return v;
    }
    Value v; v.type = VAL_NIL;
    if (scope_get(&rt->scope, name, &v)) return v;
    /* Fall back to global scope — allows fns to call other top-level fns.
     * global_scope.table holds the top-level uthash table, which is
     * different from rt->scope.table when we are inside a fn call frame. */
    if (rt->call_depth > 0) {
        if (scope_table_get(rt->global_table, name, &v)) return v;
    }
    char buf[280];
    snprintf(buf, sizeof(buf), "undefined variable: %s", name);
    rt_error(rt, buf);
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
        /* nil == nil is true; nil == anything_else is false */
        if (left.type==VAL_NIL   && right.type==VAL_NIL)   return val_bool(1);
        if (left.type==VAL_NIL   || right.type==VAL_NIL)   return val_bool(0);
        if (left.type==VAL_INT   && right.type==VAL_INT)   return val_bool(left.as.integer==right.as.integer);
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) return val_bool(left.as.real==right.as.real);
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  return val_bool(left.as.boolean==right.as.boolean);
        if (left.type==VAL_STRING&& right.type==VAL_STRING) return val_bool(strcmp(left.as.string,right.as.string)==0);
        return val_bool(0);
    }
    if (strcmp(op, "!=") == 0) {
        /* nil != nil is false; nil != anything_else is true */
        if (left.type==VAL_NIL   && right.type==VAL_NIL)   return val_bool(0);
        if (left.type==VAL_NIL   || right.type==VAL_NIL)   return val_bool(1);
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

/* ── Function call with TCO trampoline ───────────────────────────────────── */
/*
 * Design:
 *   - The outer `while(1)` is the trampoline. On a normal call it executes
 *     once and returns. On a tail call (return self(args) or return other(args)
 *     at tail position) it loops, reusing the same C stack frame.
 *   - Tail call is detected in NODE_RETURN: if the return value is a
 *     NODE_FUNC_CALL that resolves to a VAL_FUNC, we set rt->ret.tco_* and
 *     break the body loop instead of recursing into call_function again.
 *   - This gives O(1) C stack depth for tail-recursive Fluxa functions.
 *   - Non-tail calls (e.g. `return n * fatorial(n-1)`) are NOT tail calls
 *     and still recurse normally — their depth is bounded by FLUXA_MAX_DEPTH.
 */
static Value call_function(Runtime *rt, ASTNode *fn_node,
                            ASTNode **arg_nodes, int arg_count,
                            BlockInstance *method_inst) {
    if (rt->call_depth >= FLUXA_MAX_DEPTH) {
        rt_error(rt, "stack overflow — max call depth reached");
        return val_nil();
    }

    /* ── Evaluate arguments in the CALLER's scope before swapping ── */
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

    /* ── Save caller frame ── */
    Scope          caller_scope = rt->scope;
    int            caller_sz    = rt->stack_size;
    BlockInstance *caller_inst  = rt->current_instance;
    int            save_slots   = (caller_sz < FLUXA_STACK_SIZE) ? caller_sz : FLUXA_STACK_SIZE;
    Value         *caller_stack = NULL;
    if (save_slots > 0) {
        caller_stack = (Value*)malloc(sizeof(Value) * save_slots);
        if (!caller_stack) { free(args); rt_error(rt, "out of memory in call frame"); return val_nil(); }
        memcpy(caller_stack, rt->stack, sizeof(Value) * save_slots);
    }

    rt->scope            = scope_new();
    rt->stack_size       = 0;
    rt->current_instance = method_inst;
    rt->call_depth++;

    Value result = val_nil();

    /* ── Trampoline loop — iterates on tail calls, exits on normal return ── */
    while (1) {
        /* Zero enough slots to clean both function's params+locals AND
         * any stale values left by the caller frame (caller_sz slots).
         * Without this, callers with more slots than this function leave
         * stale values that rt_get picks up via the stack path. */
        int zero_slots = param_count + 64;
        if (zero_slots < caller_sz + 1) zero_slots = caller_sz + 1;
        if (zero_slots > FLUXA_STACK_SIZE) zero_slots = FLUXA_STACK_SIZE;
        for (int i = 0; i < zero_slots; i++) rt->stack[i].type = VAL_NIL;

        for (int i = 0; i < param_count; i++) {
            if (args[i].type == VAL_ARR)
                rt->stack[i] = val_arr_ref(args[i].as.arr.data, args[i].as.arr.size);
            else
                rt->stack[i] = args[i];
            if (rt->stack_size <= i) rt->stack_size = i + 1;
        }
        free(args);
        args = NULL;

        /* Register self for recursion lookup */
        Value self; self.type = VAL_FUNC; self.as.func = fn_node;
        scope_set(&rt->scope, fn_node->as.func_decl.name, self);
        rt->ret.active     = 0;
        rt->ret.tco_active = 0;
        rt->ret.tco_fn     = NULL;
        rt->ret.tco_args   = NULL;
        rt->ret.value      = val_nil();

        /* Execute body */
        ASTNode *body = fn_node->as.func_decl.body;
        for (int i = 0; i < body->as.list.count; i++) {
            eval(rt, body->as.list.children[i]);
            if (rt->had_error || rt->ret.active || rt->ret.tco_active) break;
        }

        if (rt->had_error) { result = val_nil(); break; }

        /* ── Tail call detected — loop instead of recurse ── */
        if (rt->ret.tco_active) {
            ASTNode *next_fn   = rt->ret.tco_fn;
            Value   *next_args = rt->ret.tco_args;
            int      next_argc = rt->ret.tco_arg_count;

            /* Validate param count for next iteration */
            if (next_argc != next_fn->as.func_decl.param_count) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "function '%s' expects %d argument(s), got %d (tail call)",
                    next_fn->as.func_decl.name,
                    next_fn->as.func_decl.param_count, next_argc);
                rt_error(rt, buf);
                free(next_args);
                result = val_nil();
                break;
            }

            /* Reset scope for next iteration — reuse same C frame */
            scope_free(&rt->scope);
            rt->scope      = scope_new();
            rt->stack_size = 0;
            rt->ret.tco_active = 0;

            fn_node    = next_fn;
            param_count = next_fn->as.func_decl.param_count;
            args        = next_args;    /* already evaluated */
            method_inst = rt->current_instance; /* keep same instance context */
            continue;  /* trampoline: go back to top of while(1) */
        }

        /* Normal return */
        result = rt->ret.active ? rt->ret.value : val_nil();
        rt->ret.active = 0;
        break;
    }

    /* ── Restore caller frame ── */
    free(args); /* safety — NULL if already freed in trampoline */
    scope_free(&rt->scope);
    rt->scope            = caller_scope;
    rt->stack_size       = caller_sz;
    rt->current_instance = caller_inst;
    if (save_slots > 0)
        memcpy(rt->stack, caller_stack, sizeof(Value) * save_slots);
    free(caller_stack);
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
    } else if (member->type == NODE_ARR_DECL) {
        /* Sprint 6.b: arr field in Block — allocate and init directly */
        int size = member->as.arr_decl.size;
        Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
        if (!data) return;
        if (member->as.arr_decl.default_init) {
            Value def = eval(rt, member->as.arr_decl.default_value);
            for (int i = 0; i < size; i++) {
                if (def.type == VAL_STRING && def.as.string)
                    data[i] = val_string(def.as.string);
                else
                    data[i] = def;
            }
        } else {
            for (int i = 0; i < size; i++)
                data[i] = eval(rt, member->as.arr_decl.elements[i]);
        }
        Value arr = val_arr(data, size);
        scope_set(scope, member->as.arr_decl.arr_name, arr);
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
    /* Sprint 8: atualiza linha atual para mensagens de erro precisas */
    if (node->line > 0) rt->current_line = node->line;

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

            /* Sprint 7: record prst reads for dependency graph */
            Value v = rt_get(rt, node, name);
            if (rt->mode == FLUXA_MODE_PROJECT && rt->call_depth > 0) {
                /* If this var is in global_table it is a prst — record dep */
                Value tmp;
                if (scope_table_get(rt->global_table, name, &tmp)) {
                    const char *ctx = rt->current_instance
                                    ? rt->current_instance->name : "<global>";
                    prst_graph_record(&rt->prst_graph, name, ctx);
                }
            }
            return v;
        }

        case NODE_VAR_DECL: {
            /* Sprint 7: prst semantics
             * SCRIPT mode: prst is a warning + no-op persistence.
             * PROJECT mode:
             *   - If this prst name already exists in the pool (reload):
             *     skip AST initializer, restore value from pool.
             *   - If new: evaluate initializer, store in pool + scope.
             *   - Type collision: pool rejects, error reported via err_stack. */
            int is_prst = node->as.var_decl.persistent;
            const char *vname = node->as.var_decl.var_name;

            if (is_prst) {
                if (rt->mode == FLUXA_MODE_SCRIPT) {
                    fprintf(stderr,
                        "[fluxa] warning: prst '%s' ignored in script mode\n",
                        vname);
                    /* fall through to normal evaluation */
                } else {
                    /* PROJECT mode: check pool first */
                    Value pooled;
                    if (prst_pool_has(&rt->prst_pool, vname)) {
                        /* Reload path: pool has a value from the previous run.
                         *
                         * Two cases:
                         *   A) User edited the initializer in source (e.g. 12->99).
                         *      The source is the authoritative new value.
                         *   B) The runtime mutated the variable (e.g. a counter).
                         *      The pool value survives the reload.
                         *
                         * We distinguish by evaluating the source initializer and
                         * comparing it to the pooled value.  If they differ, the
                         * user changed the source -> use source.  If they match,
                         * keep the runtime value from the pool.
                         */
                        prst_pool_get(&rt->prst_pool, vname, &pooled);

                        Value src_init = eval(rt, node->as.var_decl.initializer);
                        if (rt->had_error) return val_nil();

                        int src_changed = 0;
                        if (src_init.type != pooled.type) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_INT &&
                                   src_init.as.integer != pooled.as.integer) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_FLOAT &&
                                   src_init.as.real != pooled.as.real) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_BOOL &&
                                   src_init.as.boolean != pooled.as.boolean) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_STRING) {
                            const char *a = src_init.as.string ? src_init.as.string : "";
                            const char *b = pooled.as.string   ? pooled.as.string   : "";
                            if (strcmp(a, b) != 0) src_changed = 1;
                        }

                        Value chosen = src_changed ? src_init : pooled;

                        /* Always refresh offset: resolver may assign a different
                         * slot on a new parse of the same file. */
                        prst_pool_set(&rt->prst_pool, vname, chosen, &rt->err_stack);
                        prst_pool_set_offset(&rt->prst_pool, vname,
                                             node->resolved_offset);
                        rt_set(rt, node, vname, chosen);
                        scope_table_set(&rt->global_table, vname, chosen);
                        return val_nil();
                    }
                    /* First run: evaluate and register */
                    Value v = eval(rt, node->as.var_decl.initializer);
                    if (rt->had_error) return val_nil();
                    int ok = prst_pool_set(&rt->prst_pool, vname, v, &rt->err_stack);
                    if (ok >= 0) {
                        rt_set(rt, node, vname, v);
                        /* Record stack offset so post-VM sync can read rt->stack */
                        prst_pool_set_offset(&rt->prst_pool, vname,
                                              node->resolved_offset);
                        scope_table_set(&rt->global_table, vname, v);
                    }
                    return val_nil();
                }
            }

            Value v = eval(rt, node->as.var_decl.initializer);
            if (rt->had_error) return val_nil();
            rt_set(rt, node, vname, v);
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
            /* Sprint 7: keep prst_pool in sync so fluxa explain shows
             * current values and reload restores the latest state. */
            if (rt->mode == FLUXA_MODE_PROJECT &&
                prst_pool_has(&rt->prst_pool, node->as.assign.var_name)) {
                prst_pool_set(&rt->prst_pool, node->as.assign.var_name,
                              v, &rt->err_stack);
                scope_table_set(&rt->global_table, node->as.assign.var_name, v);
            }
            return val_nil();
        }

        case NODE_BINARY_EXPR:
            return eval_binary(rt, node);

        case NODE_FUNC_DECL: {
            Value v; v.type = VAL_FUNC; v.as.func = node;
            scope_set(&rt->scope, node->as.func_decl.name, v);
            /* Sprint 7: top-level fns go to global_table so any function
             * can call any other function regardless of declaration order.
             * global_table is the single lookup table for cross-function
             * visibility — contains both fns and prst vars. */
            if (rt->call_depth == 0)
                scope_table_set(&rt->global_table, node->as.func_decl.name, v);
            return val_nil();
        }

        case NODE_RETURN: {
            /* ── Tail Call Optimization detection ──
             * If the return expression is a bare FUNC_CALL (not embedded in
             * a binary expression like `n * f(n-1)`), we can reuse the
             * current call frame instead of growing the C stack.
             * Condition: we are inside a function (call_depth > 0) AND
             * the return node's value is a NODE_FUNC_CALL. */
            ASTNode *ret_expr = node->as.ret.value;
            if (ret_expr && rt->call_depth > 0 &&
                ret_expr->type == NODE_FUNC_CALL) {
                const char *fn_name = ret_expr->as.list.name;
                /* Resolve the function */
                Value fn_val;
                int found = scope_get(&rt->scope, fn_name, &fn_val);
                if (!found && rt->current_instance)
                    found = scope_get(&rt->current_instance->scope, fn_name, &fn_val);
                if (!found && rt->call_depth > 0)
                    found = scope_table_get(rt->global_table, fn_name, &fn_val);
                if (found && fn_val.type == VAL_FUNC && !builtin_is(fn_name)) {
                    /* Evaluate arguments NOW in current scope */
                    int argc = ret_expr->as.list.count;
                    Value *tco_args = NULL;
                    if (argc > 0) {
                        tco_args = (Value*)malloc(sizeof(Value) * argc);
                        for (int i = 0; i < argc; i++)
                            tco_args[i] = eval(rt, ret_expr->as.list.children[i]);
                    }
                    if (!rt->had_error) {
                        rt->ret.tco_active    = 1;
                        rt->ret.tco_fn        = fn_val.as.func;
                        rt->ret.tco_args      = tco_args;
                        rt->ret.tco_arg_count = argc;
                        rt->ret.active        = 0;
                        return val_nil();
                    }
                    free(tco_args);
                }
            }
            /* Normal (non-tail) return */
            Value v = ret_expr ? eval(rt, ret_expr) : val_nil();
            rt->ret.active = 1;
            rt->ret.value  = v;
            return v;
        }

        case NODE_FUNC_CALL: {
            const char *name = node->as.list.name;
            if (builtin_is(name))
                return builtin_dispatch(rt, node, (EvalFn)eval);
            Value fn_val;
            int found = scope_get(&rt->scope, name, &fn_val);
            /* Inside a Block method: look up sibling methods in instance scope */
            if (!found && rt->current_instance)
                found = scope_get(&rt->current_instance->scope, name, &fn_val);
            /* Fall back to global scope for top-level fns */
            if (!found && rt->call_depth > 0)
                found = scope_table_get(rt->global_table, name, &fn_val);
            if (!found) {
                char buf[280];
                snprintf(buf, sizeof(buf), "undefined function: %s", name);
                rt_error(rt, buf); return val_nil();
            }
            if (fn_val.type != VAL_FUNC) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not a function", name);
                rt_error(rt, buf); return val_nil();
            }
            /* If resolved from instance scope, pass the instance as context */
            BlockInstance *call_inst = NULL;
            if (fn_val.type == VAL_FUNC && rt->current_instance) {
                Value check;
                if (scope_get(&rt->current_instance->scope, name, &check) &&
                    check.type == VAL_FUNC)
                    call_inst = rt->current_instance;
            }
            return call_function(rt, fn_val.as.func,
                                 node->as.list.children, node->as.list.count,
                                 call_inst);
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
                vm_run(&chunk, &rt->scope, rt->stack, rt->stack_size,
                       rt->cancel_flag);
                chunk_free(&chunk);
                /* Sprint 7: after VM, sync prst vars from rt->stack back to
                 * pool and global_table. The VM writes rt->stack[offset]
                 * directly — the PrstEntry stores the offset set at decl time. */
                if (rt->mode == FLUXA_MODE_PROJECT) {
                    for (int pi = 0; pi < rt->prst_pool.count; pi++) {
                        PrstEntry *pe = &rt->prst_pool.entries[pi];
                        int off = pe->stack_offset;
                        if (off >= 0 && off < rt->stack_size) {
                            Value sv = rt->stack[off];
                            /* Only sync if type still matches declared type */
                            if (sv.type == pe->declared_type) {
                                prst_pool_set(&rt->prst_pool, pe->name,
                                              sv, &rt->err_stack);
                                scope_table_set(&rt->global_table, pe->name, sv);
                            }
                        }
                    }
                }
            } else {
                int limit = 100000000;
                while (limit-- > 0) {
                    /* -dev mode: stop if file changed */
                    if (rt->cancel_flag && *rt->cancel_flag) break;
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

        /* ── Sprint 6/6.b: contiguous arr on heap ──────────────────────────── */
        case NODE_ARR_DECL: {
            int size = node->as.arr_decl.size;
            Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
            if (!data) { rt_error(rt, "out of memory allocating array"); return val_nil(); }

            if (node->as.arr_decl.default_init) {
                /* Sprint 6.b: fill all slots with single default value */
                Value def = eval(rt, node->as.arr_decl.default_value);
                if (rt->had_error) { free(data); return val_nil(); }
                /* fast path for numeric types via memset-like loop */
                for (int i = 0; i < size; i++) {
                    if (def.type == VAL_STRING && def.as.string)
                        data[i] = val_string(def.as.string); /* strdup each */
                    else
                        data[i] = def; /* int/float/bool: direct copy */
                }
            } else {
                /* explicit list initializer */
                for (int i = 0; i < size; i++) {
                    data[i] = eval(rt, node->as.arr_decl.elements[i]);
                    if (rt->had_error) { free(data); return val_nil(); }
                }
            }

            Value arr = val_arr(data, size);
            /* Store in instance scope if inside a Block method */
            if (rt->current_instance) {
                scope_set(&rt->current_instance->scope,
                          node->as.arr_decl.arr_name, arr);
            } else {
                scope_set(&rt->scope, node->as.arr_decl.arr_name, arr);
            }
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

            /* normal arr access — check stack first (fn params), then scope */
            arr_val.type = VAL_NIL;
            if (node->resolved_offset >= 0 &&
                node->resolved_offset < rt->stack_size) {
                arr_val = rt->stack[node->resolved_offset];
            }
            if (arr_val.type != VAL_ARR) {
                /* fall back to scope (declared in this scope or instance) */
                if (!scope_get(&rt->scope, arr_name, &arr_val)) {
                    if (rt->current_instance)
                        scope_get(&rt->current_instance->scope, arr_name, &arr_val);
                }
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

            /* find array — check stack first (fn params), then scope */
            Value *stack_arr = NULL;
            ScopeEntry *entry = NULL;
            if (node->resolved_offset >= 0 &&
                node->resolved_offset < rt->stack_size &&
                rt->stack[node->resolved_offset].type == VAL_ARR) {
                stack_arr = &rt->stack[node->resolved_offset];
            }
            if (!stack_arr) {
                if (rt->current_instance)
                    HASH_FIND_STR(rt->current_instance->scope.table, arr_name, entry);
                if (!entry)
                    HASH_FIND_STR(rt->scope.table, arr_name, entry);
            }
            /* get the actual FluxaArr to mutate */
            FluxaArr *target_arr = stack_arr
                                 ? &stack_arr->as.arr
                                 : (entry ? &entry->value.as.arr : NULL);
            if (!target_arr || (stack_arr ? stack_arr->type : entry->value.type) != VAL_ARR) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not an array", arr_name);
                rt_error(rt, buf); return val_nil();
            }
            if (idx < 0 || idx >= target_arr->size) {
                char buf[280];
                snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld] (size %d)",
                         arr_name, idx, target_arr->size);
                rt_error(rt, buf); return val_nil();
            }
            Value v = eval(rt, node->as.arr_assign.value);
            if (rt->had_error) return val_nil();
            /* free old string if needed */
            if (target_arr->data[idx].type == VAL_STRING &&
                target_arr->data[idx].as.string)
                free(target_arr->data[idx].as.string);
            /* copy new value; strdup strings */
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = strdup(v.as.string);
            target_arr->data[idx] = v;
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
                free(entry->value.as.arr.data);
                entry->value.as.arr.data = NULL;
                entry->value.as.arr.size = 0;
            } else if (entry->value.type == VAL_STRING && entry->value.as.string) {
                free(entry->value.as.string);
                entry->value.as.string = NULL;
            }
            entry->value = val_nil();
            return val_nil();
        }

        /* ── Sprint 6.b: import c and FFI calls ────────────────────────── */
        case NODE_IMPORT_C: {
            const char *lib_name = node->as.import_c.lib_name;
            const char *alias    = node->as.import_c.alias;
            char path[256];
            ffi_resolve_path(lib_name, path, sizeof(path));
            /* load inside danger context if available, else load directly */
            ffi_load_lib(&rt->ffi, &rt->err_stack, alias, path);
            /* if load failed outside danger, it's a hard error */
            if (rt->err_stack.count > 0 && rt->danger_depth == 0) {
                const ErrEntry *e = errstack_get(&rt->err_stack, 0);
                if (e) {
                    fprintf(stderr, "[fluxa] Runtime error: %s\n", e->message);
                    rt->had_error = 1;
                }
            }
            return val_nil();
        }

        case NODE_FFI_CALL: {
            /* parser guarantees this is inside danger */
            if (rt->danger_depth == 0) {
                rt_error(rt, "FFI call outside danger block");
                return val_nil();
            }
            const char *lib_alias = node->as.ffi_call.lib_alias;
            const char *sym_name  = node->as.ffi_call.sym_name;
            const char *ret_type_s = node->as.ffi_call.ret_type;

            FFILib *lib = ffi_find_lib(&rt->ffi, lib_alias);
            if (!lib) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "FFI: library '%s' not loaded", lib_alias);
                rt_error(rt, buf);
                return val_nil();
            }

            /* evaluate arguments */
            int arg_count = node->as.ffi_call.arg_count;
            Value *args = NULL;
            if (arg_count > 0) {
                args = (Value*)malloc(sizeof(Value) * arg_count);
                for (int i = 0; i < arg_count; i++)
                    args[i] = eval(rt, node->as.ffi_call.args[i]);
            }
            if (rt->had_error) { free(args); return val_nil(); }

            /* determine return type */
            ValType ret_vt = VAL_NIL;
            if      (strcmp(ret_type_s, "int")   == 0) ret_vt = VAL_INT;
            else if (strcmp(ret_type_s, "float") == 0) ret_vt = VAL_FLOAT;
            else if (strcmp(ret_type_s, "str")   == 0) ret_vt = VAL_STRING;
            else if (strcmp(ret_type_s, "bool")  == 0) ret_vt = VAL_BOOL;

            const char *ctx = rt->current_instance
                           ? rt->current_instance->name : "<global>";
            Value result = fluxa_ffi_call(lib, sym_name, ret_vt,
                                          args, arg_count,
                                          &rt->err_stack, ctx);
            free(args);
            return result;
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
                /* Sprint 6.b: try as FFI call — lib.symbol(args) */
                FFILib *lib = ffi_find_lib(&rt->ffi, owner);
                if (lib) {
                    if (rt->danger_depth == 0) {
                        char buf[280];
                        snprintf(buf, sizeof(buf),
                            "FFI call to '%s.%s' must be inside danger block",
                            owner, method);
                        rt_error(rt, buf); return val_nil();
                    }
                    int argc = node->as.member_call.arg_count;
                    Value *args = NULL;
                    if (argc > 0) {
                        args = (Value*)malloc(sizeof(Value) * argc);
                        for (int i = 0; i < argc; i++)
                            args[i] = eval(rt, node->as.member_call.args[i]);
                    }
                    if (rt->had_error) { free(args); return val_nil(); }
                    /* default return type: float (covers libm math fns) */
                    ValType ret_vt = VAL_FLOAT;
                    const char *ctx = owner;
                    Value result = fluxa_ffi_call(lib, method, ret_vt,
                                                  args, argc,
                                                  &rt->err_stack, ctx);
                    free(args);
                    return result;
                }
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

    /* ── Sprint 7: mode detection ─────────────────────────────────────────
     * resolver_has_prst() scans AST for any prst declaration.
     * prst present → PROJECT mode: PrstPool + PrstGraph active.
     * no prst      → SCRIPT mode:  lightweight, no persistence infra.    */
    FluxaMode mode = resolver_has_prst(program)
                   ? FLUXA_MODE_PROJECT
                   : FLUXA_MODE_SCRIPT;

    /* Load fluxa.toml for runtime config (gc_cap, etc.) */
    FluxaConfig config = fluxa_config_find_and_load();

    Runtime rt;
    rt.scope            = scope_new();
    rt.global_table = NULL;
    rt.stack_size       = 0;   /* grows via rt_set; slots pre-sizes the static array */
    rt.had_error        = 0;
    rt.call_depth       = 0;
    rt.ret.active       = 0;
    rt.ret.tco_active   = 0;
    rt.ret.tco_fn       = NULL;
    rt.ret.tco_args     = NULL;
    rt.ret.value        = val_nil();
    rt.current_instance = NULL;
    rt.danger_depth     = 0;
    rt.cycle_count      = 0;
    rt.dry_run          = 0;
    rt.current_line     = 0;
    rt.cancel_flag      = g_cancel_flag;  /* watcher sets this in -dev mode */
    rt.mode             = mode;
    rt.config           = config;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);

    if (mode == FLUXA_MODE_PROJECT) {
        prst_pool_init(&rt.prst_pool);   /* pool é dinâmico; prst_cap usado abaixo */
        /* prst_pool não tem init_cap: o cap inicial fixo é PRST_POOL_INIT_CAP;
         * aqui pré-alocamos conforme config se diferente do default */
        if (config.prst_cap != PRST_POOL_INIT_CAP && config.prst_cap > 0) {
            PrstEntry *ne = (PrstEntry *)realloc(rt.prst_pool.entries,
                                sizeof(PrstEntry) * (size_t)config.prst_cap);
            if (ne) { rt.prst_pool.entries = ne; rt.prst_pool.cap = config.prst_cap; }
        }
        prst_graph_init_cap(&rt.prst_graph, config.prst_graph_cap);
    } else {
        /* Script mode: zero out pool/graph so free() calls are safe */
        rt.prst_pool.entries = NULL;
        rt.prst_pool.count   = 0;
        rt.prst_pool.cap     = 0;
        rt.prst_graph.deps   = NULL;
        rt.prst_graph.count  = 0;
        rt.prst_graph.cap    = 0;
    }

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        rt.cycle_count++;
        if (runtime_is_safe_point(&rt)) {
            ipc_apply_pending_set(&rt);
            if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);
        }
        if (rt.had_error) break;
    }

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc);
    if (mode == FLUXA_MODE_PROJECT) {
        prst_pool_free(&rt.prst_pool);
        prst_graph_free(&rt.prst_graph);
    }
    ffi_registry_free(&rt.ffi);
    return rt.had_error ? 1 : 0;
}

/* runtime_exec_explain: like runtime_exec but prints explain output
 * before teardown — used by `fluxa explain <file>`. Forces PROJECT mode
 * so PrstPool and PrstGraph are always active for explain. */
int runtime_exec_explain(ASTNode *program) {
    if (!program || program->type != NODE_PROGRAM) {
        fprintf(stderr, "[fluxa] runtime: invalid program node\n");
        return 1;
    }

    int slots = resolver_run(program);
    if (slots < 0) {
        fprintf(stderr, "[fluxa] aborting due to resolver errors.\n");
        return 1;
    }

    FluxaConfig config = fluxa_config_find_and_load();

    Runtime rt;
    rt.scope            = scope_new();
    rt.global_table     = NULL;
    rt.stack_size       = 0;
    rt.had_error        = 0;
    rt.call_depth       = 0;
    rt.ret.active       = 0;
    rt.ret.tco_active   = 0;
    rt.ret.tco_fn       = NULL;
    rt.ret.tco_args     = NULL;
    rt.ret.value        = val_nil();
    rt.current_instance = NULL;
    rt.danger_depth     = 0;
    rt.cycle_count      = 0;
    rt.dry_run          = 0;
    rt.current_line     = 0;
    rt.cancel_flag      = g_cancel_flag;
    rt.mode             = FLUXA_MODE_PROJECT;  /* always project for explain */
    rt.config           = config;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);
    prst_pool_init(&rt.prst_pool);
    if (config.prst_cap != PRST_POOL_INIT_CAP && config.prst_cap > 0) {
        PrstEntry *ne = (PrstEntry *)realloc(rt.prst_pool.entries,
                            sizeof(PrstEntry) * (size_t)config.prst_cap);
        if (ne) { rt.prst_pool.entries = ne; rt.prst_pool.cap = config.prst_cap; }
    }
    prst_graph_init_cap(&rt.prst_graph, config.prst_graph_cap);

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        rt.cycle_count++;
        if (runtime_is_safe_point(&rt)) {
            ipc_apply_pending_set(&rt);
            if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);
        }
        if (rt.had_error) break;
    }
    runtime_explain(&rt);

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc);
    prst_pool_free(&rt.prst_pool);
    prst_graph_free(&rt.prst_graph);
    ffi_registry_free(&rt.ffi);
    return rt.had_error ? 1 : 0;
}

/* ── runtime_explain ─────────────────────────────────────────────────────── */
/* Prints the full runtime state for `fluxa explain`.
 * Called after program execution completes in PROJECT mode.
 * Shows: prst variables, non-prst variables, Blocks, and dep graph. */
void runtime_explain(Runtime *rt) {
    printf("\n── prst (sobrevivem ao reload) ");
    printf("────────────────────────────────────\n");
    if (rt->prst_pool.count == 0) {
        printf("  (nenhuma)\n");
    } else {
        for (int i = 0; i < rt->prst_pool.count; i++) {
            PrstEntry *e = &rt->prst_pool.entries[i];
            /* Prefer value from global_table (reflects latest assignments,
             * including those done via bytecode VM) over pool snapshot */
            Value cur = e->value;
            scope_table_get(rt->global_table, e->name, &cur);
            printf("  %-20s ", e->name);
            switch (cur.type) {
                case VAL_INT:   printf("int   = %ld\n",  cur.as.integer); break;
                case VAL_FLOAT: printf("float = %g\n",   cur.as.real);    break;
                case VAL_BOOL:  printf("bool  = %s\n",   cur.as.boolean ? "true" : "false"); break;
                case VAL_STRING:printf("str   = \"%s\"\n", cur.as.string ? cur.as.string : ""); break;
                default:        printf("(%d)\n", cur.type); break;
            }
        }
    }

    printf("\n── Blocks ─────────────────────────────────────────────────────\n");
    BlockDef *def, *dtmp;
    int block_count = 0;
    HASH_ITER(hh, g_block_defs, def, dtmp) {
        BlockInstance *inst = block_inst_find(def->name);
        int prst_count = 0;
        int fn_count   = 0;
        for (int i = 0; i < def->node->as.block_decl.count; i++) {
            ASTNode *m = def->node->as.block_decl.members[i];
            if (m->type == NODE_VAR_DECL  && m->as.var_decl.persistent)  prst_count++;
            if (m->type == NODE_FUNC_DECL) fn_count++;
        }
        if (inst && inst->is_root) {
            printf("  %-16s (raiz)  — %d prst, %d fn\n",
                   def->name, prst_count, fn_count);
        }
        block_count++;
        (void)inst;
    }
    /* typeof instances */
    BlockInstance *inst, *itmp;
    HASH_ITER(hh, g_block_instances, inst, itmp) {
        if (!inst->is_root) {
            printf("  %-16s typeof %s\n", inst->name, inst->def->name);
        }
    }
    if (block_count == 0) printf("  (nenhum)\n");

    printf("\n── Dependências registradas ───────────────────────────────────\n");
    if (rt->prst_graph.count == 0) {
        printf("  nenhuma — estado atual compatível com o código\n");
    } else {
        for (int i = 0; i < rt->prst_graph.count; i++) {
            printf("  %-20s  <-  %s\n",
                   rt->prst_graph.deps[i].prst_name,
                   rt->prst_graph.deps[i].reader_ctx);
        }
    }
    printf("\n");
}

/* ── runtime_apply — Sprint 7.b ──────────────────────────────────────────── */
/* Re-execute a program preserving prst state from a previous run.
 * pool_in: PrstPool from the previous runtime (values survive the reload).
 *
 * Semantics:
 *   - prst vars that exist in pool_in are restored instead of re-initialized
 *   - prst vars with type collision push ERR_RELOAD and keep old value
 *   - prst_graph is rebuilt from scratch (deps re-registered during eval)
 *   - non-prst state (stack, scope, Blocks) is fresh
 *
 * Sprint 7.c will add: cascade abort via prst_graph_invalidate before eval.
 */
int runtime_apply(ASTNode *program, PrstPool *pool_in) {
    if (!program || program->type != NODE_PROGRAM) {
        fprintf(stderr, "[fluxa] runtime_apply: invalid program node\n");
        return 1;
    }

    int slots = resolver_run(program);
    if (slots < 0) {
        fprintf(stderr, "[fluxa] aborting due to resolver errors.\n");
        return 1;
    }

    FluxaConfig config = fluxa_config_find_and_load();

    Runtime rt;
    rt.scope            = scope_new();
    rt.global_table     = NULL;
    rt.stack_size       = 0;
    rt.had_error        = 0;
    rt.call_depth       = 0;
    rt.ret.active       = 0;
    rt.ret.tco_active   = 0;
    rt.ret.tco_fn       = NULL;
    rt.ret.tco_args     = NULL;
    rt.ret.value        = val_nil();
    rt.current_instance = NULL;
    rt.danger_depth     = 0;
    rt.cycle_count      = 0;
    rt.dry_run          = 0;
    rt.current_line     = 0;
    rt.cancel_flag      = g_cancel_flag;  /* watcher sets this in -dev mode */
    rt.mode             = FLUXA_MODE_PROJECT;
    rt.config           = config;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);
    prst_graph_init_cap(&rt.prst_graph, config.prst_graph_cap);

    /* Transfer pool from previous run — prst values survive the reload */
    if (pool_in && pool_in->count > 0) {
        rt.prst_pool = *pool_in;   /* shallow copy — we own it now */
        for (int i = 0; i < rt.prst_pool.count; i++)
            prst_graph_invalidate(&rt.prst_graph, rt.prst_pool.entries[i].name);
    } else {
        prst_pool_init(&rt.prst_pool);
        if (config.prst_cap != PRST_POOL_INIT_CAP && config.prst_cap > 0) {
            PrstEntry *ne = (PrstEntry *)realloc(rt.prst_pool.entries,
                                sizeof(PrstEntry) * (size_t)config.prst_cap);
            if (ne) { rt.prst_pool.entries = ne; rt.prst_pool.cap = config.prst_cap; }
        }
    }

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt.stack[i].type = VAL_NIL;

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        rt.cycle_count++;
        if (runtime_is_safe_point(&rt)) {
            ipc_apply_pending_set(&rt);
            if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);
        }
        if (rt.had_error) break;
    }

    int result = rt.had_error ? 1 : 0;
    /* If caller passed pool_in, update it with the final pool state.
     * This allows chained applies (next reload gets current values). */
    if (pool_in) *pool_in = rt.prst_pool;
    else prst_pool_free(&rt.prst_pool);

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc);
    prst_graph_free(&rt.prst_graph);
    ffi_registry_free(&rt.ffi);
    return result;
}

/* ── runtime_exec_with_rt — Sprint 8 ────────────────────────────────────── */
/* Executa um programa em um Runtime já alocado e parcialmente inicializado
 * pelo caller (handover_step3_dry_run ou runtime_apply estendido).
 *
 * Contrato de entrada:
 *   - rt->scope, rt->stack, rt->prst_pool, rt->prst_graph foram inicializados
 *   - rt->dry_run = 1 para Ciclo Imaginário, 0 para execução real
 *   - rt->mode = FLUXA_MODE_PROJECT ou FLUXA_MODE_SCRIPT
 *
 * Contrato de saída:
 *   - rt->prst_pool atualizado com valores finais
 *   - rt->err_stack populado com qualquer erro encontrado
 *   - Retorna 0 (sucesso) ou 1 (had_error)
 *
 * NÃO faz cleanup — o caller é responsável por scope_free, gc_collect_all etc.
 */
int runtime_exec_with_rt(Runtime *rt, ASTNode *program) {
    if (!rt || !program || program->type != NODE_PROGRAM) return 1;

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt->stack[i].type = VAL_NIL;
    rt->stack_size = 0;

    /* Invalida deps do grafo — serão re-registrados durante execução */
    for (int i = 0; i < rt->prst_pool.count; i++)
        prst_graph_invalidate(&rt->prst_graph, rt->prst_pool.entries[i].name);

    for (int i = 0; i < program->as.list.count; i++) {
        eval(rt, program->as.list.children[i]);
        rt->cycle_count++;
        if (rt->had_error) break;
        /* Sprint 8: em Ciclo Imaginário, qualquer entrada na err_stack
         * conta como falha mesmo que had_error não esteja setado —
         * pois danger bloqueia had_error mas o handover precisa saber. */
        if (rt->dry_run && rt->err_stack.count > 0) {
            rt->had_error = 1;
            break;
        }
    }

    return rt->had_error ? 1 : 0;
}
