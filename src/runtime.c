/* runtime.c — Fluxa Runtime
 * Sprint 6:   danger/err, contiguous arr, free(), PrstGraph stub, GCTable stub
 * Sprint 8:   rt_error_line (line numbers in errors), runtime_exec_with_rt (Handover)
 * Sprint 9:   IPC safe-point hook (ipc_apply_pending_set)
 * Sprint 9.b: fluxa set writes to rt->stack[offset] so bytecode VM sees it immediately
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
/* Sprint 9.b: clear live_rt before Runtime teardown — defined in ipc_server.c. */
extern void ipc_rtview_clear_live(struct IpcRtView *view);

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
/* Sprint 8: rt_error_line includes line number in message and ErrEntry.
 * rt_error kept for compatibility — calls rt_error_line with line=0. */
static void rt_error_line(Runtime *rt, const char *msg, int line) {
    /* If no explicit line given, use the last tracked line */
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

/* ── Type checking ────────────────────────────────────────────────────────── */
/* Map a declared type name (from the AST) to the corresponding ValType.
 * Returns VAL_NIL when the type name is unknown or does not constrain
 * (e.g. "nil" return type on functions — not used in var declarations). */
static ValType type_name_to_val(const char *t) {
    if (!t) return VAL_NIL;
    if (strcmp(t, "int")   == 0) return VAL_INT;
    if (strcmp(t, "float") == 0) return VAL_FLOAT;
    if (strcmp(t, "bool")  == 0) return VAL_BOOL;
    if (strcmp(t, "str")   == 0) return VAL_STRING;
    if (strcmp(t, "dyn")   == 0) return VAL_NIL; /* dyn accepts any — checked separately */
    return VAL_NIL; /* arr, nil, etc. — no runtime constraint here */
}

static const char *val_type_name(ValType t) {
    switch (t) {
        case VAL_INT:        return "int";
        case VAL_FLOAT:      return "float";
        case VAL_BOOL:       return "bool";
        case VAL_STRING:     return "str";
        case VAL_FUNC:       return "fn";
        case VAL_BLOCK_INST: return "Block";
        case VAL_ARR:        return "arr";
        case VAL_ERR_STACK:  return "err";
        case VAL_DYN:        return "dyn";
        case VAL_PTR:        return "ptr";
        case VAL_NIL:        return "nil";
        default:             return "?";
    }
}

/* Check that `v` is compatible with the declared type `decl_type_name`.
 * Reports a runtime error and returns 0 on mismatch; returns 1 on OK.
 * `context` is used in the error message (variable name). */
static int rt_type_check(Runtime *rt, ASTNode *node,
                         const char *decl_type_name, Value v,
                         const char *context) {
    ValType expected = type_name_to_val(decl_type_name);
    if (expected == VAL_NIL) return 1; /* unknown / unconstrained — allow */
    if (v.type == expected)  return 1; /* match */

    char buf[320];
    snprintf(buf, sizeof(buf),
             "type error: '%s' declared as %s but assigned %s",
             context, decl_type_name, val_type_name(v.type));
    rt_error_line(rt, buf, node ? node->line : 0);
    return 0;
}

/* ── Variable access ─────────────────────────────────────────────────────── */
#ifdef FLUXA_STD_MATH
#include "std/math/fluxa_std_math.h"
#endif
#ifdef FLUXA_STD_CSV
#include "std/csv/fluxa_std_csv.h"
#endif
#ifdef FLUXA_STD_JSON
#include "std/json/fluxa_std_json.h"
#endif
#ifdef FLUXA_STD_STRINGS
#include "std/str/fluxa_std_strings.h"
#endif
#ifdef FLUXA_STD_TIME
#include "std/time/fluxa_std_time.h"
#endif
#ifdef FLUXA_STD_FLXTHREAD
#include "std/flxthread/fluxa_std_flxthread.h"
extern Value fluxa_std_flxthread_call(const char *fn_name,
                                       const Value *args, int argc,
                                       ErrStack *err, int *had_error,
                                       int line, void *rt_ptr);
#endif

/* ── Block clone free callback ───────────────────────────────────────────── */
/* Called by value_free_data for VAL_BLOCK_INST inside dyn items.
 * Only frees instances that are dyn-owned clones (not in block_registry). */
static void rt_block_clone_free_cb(void *ptr) __attribute__((unused));
static void rt_block_clone_free_cb(void *ptr) {
    BlockInstance *bi = (BlockInstance*)ptr;
    if (!bi) return;
    /* Check if registered in global registry */
    BlockInstance *found = block_inst_find(bi->name);
    /* If not found, or found but different pointer => it is an unregistered clone */
    if (!found || found != bi)
        block_inst_free_unregistered(bi);
    /* If found == bi it belongs to the registry — do NOT free */
}

/* ── GC helpers — bridge between GCTable and value_free_data ─────────────── */

/* Pin a dyn value in the GC when a scope takes ownership */
static inline void rt_gc_pin(Runtime *rt, Value *v) {
    if (v && v->type == VAL_DYN && v->as.dyn)
        gc_pin(&rt->gc, v->as.dyn);
}

/* Unpin a dyn value in the GC when a scope releases it */
static inline void rt_gc_unpin(Runtime *rt, Value *v) {
    if (v && v->type == VAL_DYN && v->as.dyn)
        gc_unpin(&rt->gc, v->as.dyn);
}


static inline Value rt_get(Runtime *rt, ASTNode *node, const char *name) {
    if (node && node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        Value v = rt->stack[node->resolved_offset];
        /* Only trust the stack slot if it contains a real value.
         * Block instances and other scope-stored vars have a resolved_offset
         * assigned by the resolver but are never written to the stack —
         * their slot stays VAL_NIL. Fall through to scope lookup in that case. */
        if (v.type != VAL_NIL) return v;
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

/* ── Arithmetic & logical operators ─────────────────────────────────────── */
static Value eval_binary(Runtime *rt, ASTNode *node) {
    const char *op = node->as.binary.op;

    /* ── Logical NOT (unary, right is NULL) ────────────────────────────── */
    if (strcmp(op, "!") == 0) {
        Value operand = eval(rt, node->as.binary.left);
        if (rt->had_error) return val_nil();
        int truthy = 0;
        if      (operand.type == VAL_BOOL)  truthy = operand.as.boolean;
        else if (operand.type == VAL_INT)   truthy = operand.as.integer != 0;
        else if (operand.type == VAL_FLOAT) truthy = operand.as.real != 0.0;
        else if (operand.type == VAL_NIL)   truthy = 0;
        else truthy = 1; /* non-nil, non-zero = truthy */
        return val_bool(!truthy);
    }

    /* ── Short-circuit logical AND ─────────────────────────────────────── */
    if (strcmp(op, "&&") == 0) {
        Value left = eval(rt, node->as.binary.left);
        if (rt->had_error) return val_nil();
        int l_truthy = (left.type==VAL_BOOL) ? left.as.boolean
                     : (left.type==VAL_INT)  ? (left.as.integer != 0)
                     : (left.type!=VAL_NIL);
        if (!l_truthy) return val_bool(0); /* short-circuit */
        Value right = eval(rt, node->as.binary.right);
        if (rt->had_error) return val_nil();
        int r_truthy = (right.type==VAL_BOOL) ? right.as.boolean
                     : (right.type==VAL_INT)  ? (right.as.integer != 0)
                     : (right.type!=VAL_NIL);
        return val_bool(r_truthy);
    }

    /* ── Short-circuit logical OR ──────────────────────────────────────── */
    if (strcmp(op, "||") == 0) {
        Value left = eval(rt, node->as.binary.left);
        if (rt->had_error) return val_nil();
        int l_truthy = (left.type==VAL_BOOL) ? left.as.boolean
                     : (left.type==VAL_INT)  ? (left.as.integer != 0)
                     : (left.type!=VAL_NIL);
        if (l_truthy) return val_bool(1); /* short-circuit */
        Value right = eval(rt, node->as.binary.right);
        if (rt->had_error) return val_nil();
        int r_truthy = (right.type==VAL_BOOL) ? right.as.boolean
                     : (right.type==VAL_INT)  ? (right.as.integer != 0)
                     : (right.type!=VAL_NIL);
        return val_bool(r_truthy);
    }

    /* ── All other ops: evaluate both sides first ──────────────────────── */
    Value      left  = eval(rt, node->as.binary.left);
    if (rt->had_error) return val_nil();
    Value      right = eval(rt, node->as.binary.right);
    if (rt->had_error) return val_nil();

    if (strcmp(op, "==") == 0) {
        if (left.type==VAL_NIL   && right.type==VAL_NIL)   return val_bool(1);
        if (left.type==VAL_NIL   || right.type==VAL_NIL)   return val_bool(0);
        if (left.type==VAL_INT   && right.type==VAL_INT)   return val_bool(left.as.integer==right.as.integer);
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) return val_bool(left.as.real==right.as.real);
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  return val_bool(left.as.boolean==right.as.boolean);
        if (left.type==VAL_STRING&& right.type==VAL_STRING) return val_bool(strcmp(left.as.string,right.as.string)==0);
        return val_bool(0);
    }
    if (strcmp(op, "!=") == 0) {
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
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "arithmetic on non-numeric value (got %s for '%s')",
                 val_type_name(left.type), op);
        rt_error_line(rt, buf, node->line);
        return val_nil();
    }
    if      (right.type==VAL_INT)   r = (double)right.as.integer;
    else if (right.type==VAL_FLOAT) r = right.as.real;
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "arithmetic on non-numeric value (got %s for '%s')",
                 val_type_name(right.type), op);
        rt_error_line(rt, buf, node->line);
        return val_nil();
    }

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
        /* arr field in Block — deep copy if default is another arr */
        int size = member->as.arr_decl.size;
        if (member->as.arr_decl.default_init) {
            Value def = eval(rt, member->as.arr_decl.default_value);
            if (def.type == VAL_ARR) {
                /* Deep copy — never share the data pointer */
                int src_size = def.as.arr.size;
                Value *data = (Value*)calloc((size_t)size, sizeof(Value));
                if (!data) return;
                for (int i = 0; i < src_size && i < size; i++) {
                    data[i] = def.as.arr.data[i];
                    if (data[i].type == VAL_STRING && data[i].as.string)
                        data[i].as.string = strdup(data[i].as.string);
                }
                Value arr = val_arr(data, size);
                scope_set(scope, member->as.arr_decl.arr_name, arr);
                return;
            }
            Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
            if (!data) return;
            for (int i = 0; i < size; i++) {
                if (def.type == VAL_STRING && def.as.string)
                    data[i] = val_string(def.as.string);
                else
                    data[i] = def;
            }
            Value arr = val_arr(data, size);
            scope_set(scope, member->as.arr_decl.arr_name, arr);
        } else {
            Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
            if (!data) return;
            for (int i = 0; i < size; i++)
                data[i] = eval(rt, member->as.arr_decl.elements[i]);
            Value arr = val_arr(data, size);
            scope_set(scope, member->as.arr_decl.arr_name, arr);
        }
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

/* ── Runtime-aware scope cleanup ─────────────────────────────────────────── */
/* Unpins all VAL_DYN entries before releasing the scope.
 * Must be used instead of bare scope_free() wherever a Runtime is available. */
static inline void rt_scope_free(Runtime *rt, Scope *s) {
    if (!s || !s->table) { scope_free(s); return; }
    ScopeEntry *entry, *tmp;
    HASH_ITER(hh, s->table, entry, tmp) {
        rt_gc_unpin(rt, &entry->value);
        /* VAL_BLOCK_INST: only free dyn-owned clones (not in global registry) */
        if (entry->value.type == VAL_BLOCK_INST && entry->value.as.block_inst) {
            BlockInstance *bi = entry->value.as.block_inst;
            BlockInstance *found = block_inst_find(bi->name);
            if (!found || found != bi)
                block_inst_free_unregistered(bi);
        }
    }
    scope_free(s);
}

/* ── Eval ────────────────────────────────────────────────────────────────── */
static Value eval(Runtime *rt, ASTNode *node) {
    if (!node || rt->had_error) return val_nil();
    /* Sprint 8: update current line for precise error messages */
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

                        /* Compare new source initializer against init_value
                         * (the declared value at first run or at migration time).
                         * If they differ → user edited the source → use source.
                         * If they match → runtime mutated the value → keep pooled. */
                        int entry_idx = prst_pool_find(&rt->prst_pool, vname);
                        Value ref = (entry_idx >= 0)
                            ? rt->prst_pool.entries[entry_idx].init_value
                            : pooled;

                        int src_changed = 0;
                        if (src_init.type != ref.type) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_INT &&
                                   src_init.as.integer != ref.as.integer) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_FLOAT &&
                                   src_init.as.real != ref.as.real) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_BOOL &&
                                   src_init.as.boolean != ref.as.boolean) {
                            src_changed = 1;
                        } else if (src_init.type == VAL_STRING) {
                            const char *a = src_init.as.string ? src_init.as.string : "";
                            const char *b = ref.as.string      ? ref.as.string      : "";
                            if (strcmp(a, b) != 0) src_changed = 1;
                        }

                        Value chosen = src_changed ? src_init : pooled;

                        /* Always refresh offset: resolver may assign a different
                         * slot on a new parse of the same file. */
                        prst_pool_set(&rt->prst_pool, vname, chosen, &rt->err_stack);
                        prst_pool_set_offset(&rt->prst_pool, vname,
                                             node->resolved_offset);
                        /* If the source declaration changed, update init_value
                         * so future reloads use the new declaration as baseline. */
                        if (src_changed && entry_idx >= 0) {
                            Value *iv = &rt->prst_pool.entries[entry_idx].init_value;
                            if (iv->type == VAL_STRING && iv->as.string)
                                free(iv->as.string);
                            *iv = src_init;
                            if (iv->type == VAL_STRING && iv->as.string)
                                iv->as.string = strdup(iv->as.string);
                        }
                        rt_set(rt, node, vname, chosen);
                        scope_table_set(&rt->global_table, vname, chosen);
                        return val_nil();
                    }
                    /* First run: evaluate and register */
                    Value v = eval(rt, node->as.var_decl.initializer);
                    if (rt->had_error) return val_nil();
                    if (!rt_type_check(rt, node, node->as.var_decl.type_name, v, vname))
                        return val_nil();
                    int ok = prst_pool_set(&rt->prst_pool, vname, v, &rt->err_stack);
                    if (ok >= 0) {
                        rt_set(rt, node, vname, v);
                        rt_gc_pin(rt, &v);
                        /* Record stack offset so post-VM sync can read rt->stack */
                        prst_pool_set_offset(&rt->prst_pool, vname,
                                              node->resolved_offset);
                        scope_table_set(&rt->global_table, vname, v);
                    }
                    return val_nil();
                }
            }

            int err_before = rt->err_stack.count;
            Value v = eval(rt, node->as.var_decl.initializer);
            if (rt->had_error) return val_nil();
            /* Inside a danger block eval errors go to err_stack (not had_error).
             * If the stack grew, the eval failed — skip the type check to avoid
             * a spurious second error message. */
            if (rt->err_stack.count > err_before) {
                rt_set(rt, node, vname, v);
                return val_nil();
            }
            if (!rt_type_check(rt, node, node->as.var_decl.type_name, v, vname))
                return val_nil();
            rt_set(rt, node, vname, v);
            /* GC: pin dyn when scope takes ownership */
            rt_gc_pin(rt, &v);
            return val_nil();
        }


        case NODE_ASSIGN: {
            int err_before = rt->err_stack.count;
            Value v = eval(rt, node->as.assign.value);
            if (rt->had_error) return val_nil();

            /* Type check: new value must match the type of the variable as
             * currently stored. We read the current value to get its type.
             * Skip when: variable not in scope, current is NIL (uninitialized),
             * or eval already pushed an error into err_stack (danger context). */
            if (rt->err_stack.count == err_before) {
                Value cur = rt_get(rt, node, node->as.assign.var_name);
                if (cur.type != VAL_NIL && cur.type != v.type) {
                    char buf[320];
                    snprintf(buf, sizeof(buf),
                             "type error: '%s' is %s, cannot assign %s",
                             node->as.assign.var_name,
                             val_type_name(cur.type),
                             val_type_name(v.type));
                    rt_error_line(rt, buf, node->line);
                    return val_nil();
                }
            }

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
             * current values and reload restores the latest state.
             * Skip during dry_run (Ciclo Imaginário) — mutations must not
             * pollute the pool that will be transferred to the live runtime. */
            if (!rt->dry_run &&
                rt->mode == FLUXA_MODE_PROJECT &&
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
                            if (sv.type == pe->declared_type) {
                                prst_pool_set(&rt->prst_pool, pe->name,
                                              sv, &rt->err_stack);
                                scope_table_set(&rt->global_table, pe->name, sv);
                            }
                        }
                    }
                }
            } else {
                /* cancel_flag is set in -dev mode — it's the exit mechanism.
                 * Script mode (no cancel_flag) keeps the 100M safety limit. */
                long limit = rt->cancel_flag ? -1L : 100000000L;
                while (limit != 0) {
                    if (limit > 0) limit--;
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
                    /* GC safe point at while back-edge — only sweep when dyn objects exist */
                    if (rt->gc.count > 0) gc_sweep(&rt->gc, gc_dyn_free_fn);
#ifdef FLUXA_STD_FLXTHREAD
                    /* flxthread mailbox drain — O(1) fast path when no messages.
                     * Returns -1 when stop_requested — breaks the while loop. */
                    if (rt->current_thread &&
                        flx_mailbox_drain((FlxThread *)rt->current_thread,
                                          rt, rt->current_instance) < 0)
                        break;  /* cooperative stop — exit while loop */
#endif
                    /* IPC safe point (Sprint 9.b) */
                    if (g_ipc_view) ipc_rtview_update(g_ipc_view, rt);
                    ipc_apply_pending_set(rt);
                }
            }
            return val_nil();
        }

        /* ── Sprint 6/6.b: contiguous arr on heap ──────────────────────────── */
        case NODE_ARR_DECL: {
            int size = node->as.arr_decl.size;
            const char *arr_type = node->as.arr_decl.type_name; /* "int","float","str","bool" */

            if (node->as.arr_decl.default_init) {
                /* Default initializer: may be a primitive (fill all slots)
                 * OR another arr (deep copy into this arr).                 */
                Value def = eval(rt, node->as.arr_decl.default_value);
                if (rt->had_error) return val_nil();

                if (def.type == VAL_ARR) {
                    /* Deep copy — source arr must have compatible element type.
                     * Destination size must be >= source size (extra slots zeroed). */
                    if (def.as.arr.size > size) {
                        char buf[320];
                        snprintf(buf, sizeof(buf),
                            "arr copy: source size %d is larger than destination size %d"
                            " — destination must be >= source size",
                            def.as.arr.size, size);
                        rt_error(rt, buf); return val_nil();
                    }
                    Value *data = (Value*)calloc((size_t)size, sizeof(Value));
                    if (!data) { rt_error(rt, "out of memory allocating array"); return val_nil(); }
                    for (int i = 0; i < def.as.arr.size; i++) {
                        data[i] = def.as.arr.data[i];
                        if (data[i].type == VAL_STRING && data[i].as.string)
                            data[i].as.string = strdup(data[i].as.string);
                    }
                    /* Zero-fill remaining slots with type-appropriate zero */
                    for (int i = def.as.arr.size; i < size; i++) {
                        if (arr_type && strcmp(arr_type, "float") == 0)
                            data[i] = val_float(0.0);
                        else if (arr_type && strcmp(arr_type, "str") == 0)
                            data[i] = val_string("");
                        else if (arr_type && strcmp(arr_type, "bool") == 0)
                            data[i] = val_bool(0);
                        else
                            data[i] = val_int(0);
                    }
                    Value arr = val_arr(data, size);
                    if (rt->current_instance)
                        scope_set(&rt->current_instance->scope,
                                  node->as.arr_decl.arr_name, arr);
                    else
                        scope_set(&rt->scope, node->as.arr_decl.arr_name, arr);
                    return val_nil();
                }

                /* Primitive default: fill all slots (type-checked) */
                Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
                if (!data) { rt_error(rt, "out of memory allocating array"); return val_nil(); }
                for (int i = 0; i < size; i++) {
                    if (arr_type && def.type != VAL_NIL) {
                        /* type check each element */
                        int ok = 1;
                        if (strcmp(arr_type,"int")==0   && def.type!=VAL_INT)   ok=0;
                        if (strcmp(arr_type,"float")==0 && def.type!=VAL_FLOAT) ok=0;
                        if (strcmp(arr_type,"str")==0   && def.type!=VAL_STRING)ok=0;
                        if (strcmp(arr_type,"bool")==0  && def.type!=VAL_BOOL)  ok=0;
                        if (!ok) {
                            char buf[280];
                            snprintf(buf, sizeof(buf),
                                "arr type error: declared as %s arr but element is %s",
                                arr_type, val_type_name(def.type));
                            free(data); rt_error(rt, buf); return val_nil();
                        }
                    }
                    if (def.type == VAL_STRING && def.as.string)
                        data[i] = val_string(def.as.string);
                    else
                        data[i] = def;
                }
                Value arr = val_arr(data, size);
                if (rt->current_instance)
                    scope_set(&rt->current_instance->scope,
                              node->as.arr_decl.arr_name, arr);
                else
                    scope_set(&rt->scope, node->as.arr_decl.arr_name, arr);
                return val_nil();
            } else {
                /* explicit list initializer — type-check each element */
                Value *data = (Value*)malloc((size_t)(size * (int)sizeof(Value)));
                if (!data) { rt_error(rt, "out of memory allocating array"); return val_nil(); }
                for (int i = 0; i < size; i++) {
                    data[i] = eval(rt, node->as.arr_decl.elements[i]);
                    if (rt->had_error) { free(data); return val_nil(); }
                    /* type enforcement on each element */
                    if (arr_type && data[i].type != VAL_NIL) {
                        int ok = 1;
                        if (strcmp(arr_type,"int")==0   && data[i].type!=VAL_INT)   ok=0;
                        if (strcmp(arr_type,"float")==0 && data[i].type!=VAL_FLOAT) ok=0;
                        if (strcmp(arr_type,"str")==0   && data[i].type!=VAL_STRING)ok=0;
                        if (strcmp(arr_type,"bool")==0  && data[i].type!=VAL_BOOL)  ok=0;
                        if (!ok) {
                            char buf[280];
                            snprintf(buf, sizeof(buf),
                                "arr type error: declared as %s arr"
                                " but element[%d] is %s",
                                arr_type, i, val_type_name(data[i].type));
                            free(data); rt_error(rt, buf); return val_nil();
                        }
                    }
                }
                Value arr = val_arr(data, size);
                if (rt->current_instance)
                    scope_set(&rt->current_instance->scope,
                              node->as.arr_decl.arr_name, arr);
                else
                    scope_set(&rt->scope, node->as.arr_decl.arr_name, arr);
                /* Register in prst pool if declared persistent */
                if (node->as.arr_decl.persistent && !rt->dry_run &&
                    rt->mode == FLUXA_MODE_PROJECT) {
                    int pi = prst_pool_find(&rt->prst_pool,
                                            node->as.arr_decl.arr_name);
                    if (pi < 0) {
                        /* First run — register with init_value = declared arr */
                        prst_pool_set(&rt->prst_pool,
                                      node->as.arr_decl.arr_name,
                                      arr, &rt->err_stack);
                        pi = prst_pool_find(&rt->prst_pool,
                                            node->as.arr_decl.arr_name);
                        if (pi >= 0) {
                            rt->prst_pool.entries[pi].declared_type = VAL_ARR;
                            rt->prst_pool.entries[pi].init_value    = arr;
                        }
                    } else {
                        /* Reload — deep copy pooled arr into scope so mutations
                         * to scope don't alias the pool's copy */
                        Value pooled = rt->prst_pool.entries[pi].value;
                        if (pooled.type == VAL_ARR && pooled.as.arr.data) {
                            int psz = pooled.as.arr.size;
                            Value *dcopy = (Value*)malloc(
                                sizeof(Value) * (size_t)psz);
                            if (dcopy) {
                                for (int _pi = 0; _pi < psz; _pi++) {
                                    dcopy[_pi] = pooled.as.arr.data[_pi];
                                    if (dcopy[_pi].type == VAL_STRING &&
                                        dcopy[_pi].as.string)
                                        dcopy[_pi].as.string =
                                            strdup(dcopy[_pi].as.string);
                                }
                                Value restored = val_arr(dcopy, psz);
                                scope_set(&rt->scope,
                                          node->as.arr_decl.arr_name, restored);
                            }
                        }
                    }
                }
                return val_nil();
            }

        }

        case NODE_ARR_ACCESS: {
            Value arr_val;
            const char *arr_name = node->as.arr_access.arr_name;

            /* Sprint 6: special case — err[i] */
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

            /* Load variable from stack or scope */
            arr_val.type = VAL_NIL;
            if (node->resolved_offset >= 0 &&
                node->resolved_offset < rt->stack_size) {
                arr_val = rt->stack[node->resolved_offset];
            }
            if (arr_val.type == VAL_NIL) {
                if (!scope_get(&rt->scope, arr_name, &arr_val)) {
                    if (rt->current_instance)
                        scope_get(&rt->current_instance->scope, arr_name, &arr_val);
                }
            }

            /* Sprint 9.c: dispatch to dyn if the variable is VAL_DYN */
            if (arr_val.type == VAL_DYN) {
                FluxaDyn *d = arr_val.as.dyn;
                if (!d) { rt_error(rt, "dyn is nil"); return val_nil(); }
                Value idx_val = eval(rt, node->as.arr_access.index);
                if (idx_val.type != VAL_INT) { rt_error(rt, "dyn index must be int"); return val_nil(); }
                long idx = idx_val.as.integer;
                if (idx < 0 || idx >= d->count) {
                    char buf[200];
                    snprintf(buf, sizeof(buf), "dyn index out of bounds: %s[%ld] (count %d)",
                             arr_name, idx, d->count);
                    rt_error(rt, buf); return val_nil();
                }
                return d->items[idx];
            }

            if (arr_val.type != VAL_ARR) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not an array or dyn", arr_name);
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

            /* Sprint 9.c: dispatch to dyn if variable is VAL_DYN.
             * Use rt_get so stack-resident vars (resolved_offset path) are found. */
            {
                Value maybe_dyn = rt_get(rt, node, arr_name);
                /* rt_get sets had_error if var not found — save/restore cleanly */
                if (rt->had_error) { rt->had_error = 0; maybe_dyn.type = VAL_NIL; }
                if (maybe_dyn.type == VAL_DYN) {
                    FluxaDyn *d = maybe_dyn.as.dyn;
                    Value idx_v = eval(rt, node->as.arr_assign.index);
                    if (rt->had_error) return val_nil();
                    if (idx_v.type != VAL_INT) { rt_error(rt, "dyn index must be int"); return val_nil(); }
                    long idx = idx_v.as.integer;
                    if (idx < 0) { rt_error(rt, "dyn index cannot be negative"); return val_nil(); }
                    /* Reset had_error before evaluating value so eval doesn't short-circuit */
                    Value v = eval(rt, node->as.arr_assign.value);
                    /* Auto-grow */
                    if (idx >= d->count) {
                        while (idx >= d->cap) {
                            d->cap = d->cap * 2 + 1;
                            Value *nb = (Value*)realloc(d->items, sizeof(Value) * (size_t)d->cap);
                            if (!nb) { rt_error(rt, "out of memory growing dyn"); return val_nil(); }
                            d->items = nb;
                        }
                        for (long fi = d->count; fi < idx; fi++) d->items[fi] = val_nil();
                        d->count = (int)idx + 1;
                    }
                    /* Type validation */
                    if (v.type == VAL_DYN) {
                        rt_error(rt, "dyn cannot contain dyn"
                                 " — use Block to compose dynamic structures");
                        return val_nil();
                    }
                    if (v.type == VAL_ARR) {
                        if (v.as.arr.data && v.as.arr.size > 0) {
                            Value *nd = (Value*)malloc(sizeof(Value)*(size_t)v.as.arr.size);
                            if (!nd) { rt_error(rt, "out of memory copying arr into dyn"); return val_nil(); }
                            for (int j = 0; j < v.as.arr.size; j++) {
                                nd[j] = v.as.arr.data[j];
                                if (nd[j].type == VAL_STRING && nd[j].as.string)
                                    nd[j].as.string = strdup(nd[j].as.string);
                            }
                            v = val_arr(nd, v.as.arr.size);
                        }
                    } else if (v.type == VAL_BLOCK_INST && v.as.block_inst) {
                        BlockInstance *clone = block_inst_clone(v.as.block_inst);
                        if (!clone) { rt_error(rt, "out of memory cloning Block into dyn"); return val_nil(); }
                        v.as.block_inst = clone;
                    } else if (v.type == VAL_STRING && v.as.string) {
                        v.as.string = strdup(v.as.string);
                    }
                    /* Free old value's heap resources before overwriting */
                    value_free_data(&d->items[idx]);
                    d->items[idx] = v;
                    return val_nil();
                }
            }

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
            int err_before2 = rt->err_stack.count;
            Value v = eval(rt, node->as.arr_assign.value);
            if (rt->had_error) return val_nil();
            /* Type check: arr is homogeneous — new element must match the
             * type of the existing element at that index.
             * Skip if eval already pushed a danger error, or if the slot
             * is NIL (e.g. uninitialized default). */
            if (rt->err_stack.count == err_before2) {
                ValType elem_type = target_arr->data[idx].type;
                if (elem_type != VAL_NIL && elem_type != v.type) {
                    char buf[320];
                    snprintf(buf, sizeof(buf),
                             "type error: %s[%ld] is %s, cannot assign %s",
                             arr_name, idx,
                             val_type_name(elem_type), val_type_name(v.type));
                    rt_error_line(rt, buf, node->line);
                    return val_nil();
                }
            }
            /* free old string if needed */
            if (target_arr->data[idx].type == VAL_STRING &&
                target_arr->data[idx].as.string)
                free(target_arr->data[idx].as.string);
            /* copy new value; strdup strings */
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = strdup(v.as.string);
            target_arr->data[idx] = v;

            /* Sync mutated arr back to prst pool if it's a prst variable */
            if (!rt->dry_run && rt->mode == FLUXA_MODE_PROJECT) {
                int pi = prst_pool_find(&rt->prst_pool, arr_name);
                if (pi >= 0) {
                    /* The pool entry holds its own copy — update element in place */
                    Value *pool_val = &rt->prst_pool.entries[pi].value;
                    if (pool_val->type == VAL_ARR && pool_val->as.arr.data &&
                        idx < pool_val->as.arr.size) {
                        if (pool_val->as.arr.data[idx].type == VAL_STRING &&
                            pool_val->as.arr.data[idx].as.string)
                            free(pool_val->as.arr.data[idx].as.string);
                        Value pv = v;
                        if (pv.type == VAL_STRING && pv.as.string)
                            pv.as.string = strdup(pv.as.string);
                        pool_val->as.arr.data[idx] = pv;
                    }
                }
            }
            return val_nil();
        }

        /* ── Sprint 9.c: dyn literal ─────────────────────────────────────── */
        case NODE_DYN_LIT: {
            int count = node->as.dyn_lit.count;
            FluxaDyn *d = (FluxaDyn*)malloc(sizeof(FluxaDyn));
            if (!d) { rt_error(rt, "out of memory allocating dyn"); return val_nil(); }
            d->cap   = count > 4 ? count : 4;
            d->count = 0;
            d->items = (Value*)malloc(sizeof(Value) * (size_t)d->cap);
            if (!d->items) { free(d); rt_error(rt, "out of memory"); return val_nil(); }

            for (int i = 0; i < count; i++) {
                Value elem = eval(rt, node->as.dyn_lit.elements[i]);
                if (rt->had_error) { fluxa_dyn_free(d); return val_nil(); }

                /* ── Type validation and ownership rules ── */
                if (elem.type == VAL_DYN) {
                    fluxa_dyn_free(d);
                    rt_error(rt, "dyn cannot contain dyn"
                             " — use Block to compose dynamic structures");
                    return val_nil();
                }
                if (elem.type == VAL_ARR) {
                    /* Deep copy arr — dyn owns the new copy */
                    if (elem.as.arr.data && elem.as.arr.size > 0) {
                        Value *new_data = (Value*)malloc(
                            sizeof(Value) * (size_t)elem.as.arr.size);
                        if (!new_data) {
                            fluxa_dyn_free(d);
                            rt_error(rt, "out of memory copying arr into dyn");
                            return val_nil();
                        }
                        for (int j = 0; j < elem.as.arr.size; j++) {
                            new_data[j] = elem.as.arr.data[j];
                            if (new_data[j].type == VAL_STRING &&
                                new_data[j].as.string)
                                new_data[j].as.string =
                                    strdup(new_data[j].as.string);
                        }
                        elem = val_arr(new_data, elem.as.arr.size); /* owned=1 */
                    }
                } else if (elem.type == VAL_BLOCK_INST && elem.as.block_inst) {
                    /* typeof-implicit: clone current state, dyn owns the clone */
                    BlockInstance *clone = block_inst_clone(elem.as.block_inst);
                    if (!clone) {
                        fluxa_dyn_free(d);
                        rt_error(rt, "out of memory cloning Block into dyn");
                        return val_nil();
                    }
                    elem.as.block_inst = clone;
                } else if (elem.type == VAL_STRING && elem.as.string) {
                    elem.as.string = strdup(elem.as.string);
                }

                d->items[d->count++] = elem;
            }

            /* Register with GC — pin_count starts at 0, caller pins via VAR_DECL */
            gc_register(&rt->gc, d,
                sizeof(FluxaDyn) + sizeof(Value) * (size_t)d->cap,
                &rt->err_stack);
            return val_dyn(d);
        }

        case NODE_DYN_ACCESS: {
            const char *dname = node->as.dyn_access.dyn_name;
            Value dv; dv.type = VAL_NIL;
            if (node->resolved_offset >= 0 && node->resolved_offset < rt->stack_size)
                dv = rt->stack[node->resolved_offset];
            if (dv.type != VAL_DYN) scope_get(&rt->scope, dname, &dv);
            if (dv.type != VAL_DYN) {
                char buf[200]; snprintf(buf, sizeof(buf), "'%s' is not a dyn", dname);
                rt_error(rt, buf); return val_nil();
            }
            FluxaDyn *d = dv.as.dyn;
            Value idx_v = eval(rt, node->as.dyn_access.index);
            if (idx_v.type != VAL_INT) { rt_error(rt, "dyn index must be int"); return val_nil(); }
            long idx = idx_v.as.integer;
            if (idx < 0 || idx >= d->count) {
                char buf[200];
                snprintf(buf, sizeof(buf), "dyn index out of bounds: %s[%ld] (count %d)",
                         dname, idx, d->count);
                rt_error(rt, buf); return val_nil();
            }
            return d->items[idx];
        }

        case NODE_DYN_ASSIGN: {
            const char *dname = node->as.dyn_assign.dyn_name;
            Value dv; dv.type = VAL_NIL;
            Value *stack_slot = NULL;
            ScopeEntry *dyn_entry = NULL;
            if (node->resolved_offset >= 0 && node->resolved_offset < rt->stack_size) {
                stack_slot = &rt->stack[node->resolved_offset];
                dv = *stack_slot;
            }
            if (dv.type != VAL_DYN) {
                HASH_FIND_STR(rt->scope.table, dname, dyn_entry);
                if (dyn_entry) dv = dyn_entry->value;
            }
            if (dv.type != VAL_DYN) {
                char buf[200]; snprintf(buf, sizeof(buf), "'%s' is not a dyn", dname);
                rt_error(rt, buf); return val_nil();
            }
            FluxaDyn *d = dv.as.dyn;

            Value idx_v = eval(rt, node->as.dyn_assign.index);
            if (rt->had_error) return val_nil();
            if (idx_v.type != VAL_INT) { rt_error(rt, "dyn index must be int"); return val_nil(); }
            long idx = idx_v.as.integer;
            if (idx < 0) { rt_error(rt, "dyn index cannot be negative"); return val_nil(); }

            Value v = eval(rt, node->as.dyn_assign.value);
            if (rt->had_error) return val_nil();

            /* Auto-grow: extend dyn to cover index */
            if (idx >= d->count) {
                /* Grow cap if needed */
                while (idx >= d->cap) {
                    d->cap = d->cap * 2 + 1;
                    Value *newbuf = (Value*)realloc(d->items, sizeof(Value) * (size_t)d->cap);
                    if (!newbuf) { rt_error(rt, "out of memory growing dyn"); return val_nil(); }
                    d->items = newbuf;
                }
                /* Fill gap with nil */
                for (long fi = d->count; fi < idx; fi++)
                    d->items[fi] = val_nil();
                d->count = (int)idx + 1;
            }

            /* Type validation */
            if (v.type == VAL_DYN) {
                rt_error(rt, "dyn cannot contain dyn"
                         " — use Block to compose dynamic structures");
                return val_nil();
            }
            if (v.type == VAL_ARR) {
                /* Deep copy arr — dyn owns the new copy */
                if (v.as.arr.data && v.as.arr.size > 0) {
                    Value *nd = (Value*)malloc(sizeof(Value) * (size_t)v.as.arr.size);
                    if (!nd) { rt_error(rt, "out of memory copying arr into dyn"); return val_nil(); }
                    for (int j = 0; j < v.as.arr.size; j++) {
                        nd[j] = v.as.arr.data[j];
                        if (nd[j].type == VAL_STRING && nd[j].as.string)
                            nd[j].as.string = strdup(nd[j].as.string);
                    }
                    v = val_arr(nd, v.as.arr.size);
                }
            } else if (v.type == VAL_BLOCK_INST && v.as.block_inst) {
                /* typeof-implicit clone */
                BlockInstance *clone = block_inst_clone(v.as.block_inst);
                if (!clone) { rt_error(rt, "out of memory cloning Block into dyn"); return val_nil(); }
                v.as.block_inst = clone;
            } else if (v.type == VAL_STRING && v.as.string) {
                v.as.string = strdup(v.as.string);
            }

            /* Free old value's heap resources before overwriting */
            value_free_data(&d->items[idx]);
            d->items[idx] = v;
            return val_nil();
        }

        /* ── Sprint 9.c bugfix: dyn[i].campo / dyn[i].metodo() ─────────── */
        case NODE_INDEXED_MEMBER_ACCESS: {
            const char *dyn_name = node->as.indexed_member_access.dyn_name;
            const char *field    = node->as.indexed_member_access.field;

            /* Resolve the dyn value — use rt_get so stack-stored vars work */
            Value dv = rt_get(rt, node, dyn_name);
            if (rt->had_error) return val_nil();
            if (dv.type != VAL_DYN) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not a dyn", dyn_name);
                rt_error(rt, buf); return val_nil();
            }

            /* Evaluate index */
            Value idx_v = eval(rt, node->as.indexed_member_access.index);
            if (rt->had_error) return val_nil();
            if (idx_v.type != VAL_INT) {
                rt_error(rt, "dyn index must be int"); return val_nil();
            }
            long idx = idx_v.as.integer;
            FluxaDyn *d = dv.as.dyn;
            if (idx < 0 || idx >= d->count) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "dyn index out of bounds: %s[%ld] (count %d)",
                    dyn_name, idx, d->count);
                rt_error(rt, buf); return val_nil();
            }

            /* Element must be a Block instance */
            Value elem = d->items[idx];
            if (elem.type != VAL_BLOCK_INST || !elem.as.block_inst) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "%s[%ld] is not a Block instance", dyn_name, idx);
                rt_error(rt, buf); return val_nil();
            }
            BlockInstance *inst = elem.as.block_inst;

            /* Read the field */
            Value field_val;
            if (!scope_get(&inst->scope, field, &field_val)) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "%s[%ld] has no member '%s'", dyn_name, idx, field);
                rt_error(rt, buf); return val_nil();
            }
            return field_val;
        }

        case NODE_INDEXED_MEMBER_CALL: {
            const char *dyn_name = node->as.indexed_member_call.dyn_name;
            const char *method   = node->as.indexed_member_call.method;

            /* Resolve the dyn value — use rt_get so stack-stored vars work */
            Value dv = rt_get(rt, node, dyn_name);
            if (rt->had_error) return val_nil();
            if (dv.type != VAL_DYN) {
                char buf[280];
                snprintf(buf, sizeof(buf), "'%s' is not a dyn", dyn_name);
                rt_error(rt, buf); return val_nil();
            }

            /* Evaluate index */
            Value idx_v = eval(rt, node->as.indexed_member_call.index);
            if (rt->had_error) return val_nil();
            if (idx_v.type != VAL_INT) {
                rt_error(rt, "dyn index must be int"); return val_nil();
            }
            long idx = idx_v.as.integer;
            FluxaDyn *d = dv.as.dyn;
            if (idx < 0 || idx >= d->count) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "dyn index out of bounds: %s[%ld] (count %d)",
                    dyn_name, idx, d->count);
                rt_error(rt, buf); return val_nil();
            }

            /* Element must be a Block instance */
            Value elem = d->items[idx];
            if (elem.type != VAL_BLOCK_INST || !elem.as.block_inst) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "%s[%ld] is not a Block instance", dyn_name, idx);
                rt_error(rt, buf); return val_nil();
            }
            BlockInstance *inst = elem.as.block_inst;

            /* Look up the method in the instance scope */
            Value fn_val;
            if (!scope_get(&inst->scope, method, &fn_val) ||
                fn_val.type != VAL_FUNC) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "%s[%ld] has no method '%s'", dyn_name, idx, method);
                rt_error(rt, buf); return val_nil();
            }

            /* Call the method — call_function evaluates args internally */
            int argc = node->as.indexed_member_call.arg_count;
            BlockInstance *prev = rt->current_instance;
            rt->current_instance = inst;
            Value result = call_function(rt, fn_val.as.func,
                                         node->as.indexed_member_call.args,
                                         argc, inst);
            rt->current_instance = prev;
            return result;
        }

        case NODE_FOR: {
            const char *arr_name = node->as.for_stmt.arr_name;
            /* Look up the iterable — multi-scope search.
             * We cannot use rt_get(rt, node, ...) here because
             * node->resolved_offset belongs to the loop var, not the iterable.
             * Instead search: stack (by name scan), instance scope, frame, global. */
            Value arr_val; arr_val.type = VAL_NIL;
            int found = 0;
            /* 1. Stack slot via resolver-resolved offset */
            int arr_off = node->as.for_stmt.arr_resolved_offset;
            if (!found && arr_off >= 0 && arr_off < rt->stack_size) {
                Value sv = rt->stack[arr_off];
                if (sv.type != VAL_NIL) { arr_val = sv; found = 1; }
            }
            /* 2. Block instance scope */
            if (!found && rt->current_instance)
                found = scope_get(&rt->current_instance->scope, arr_name, &arr_val);
            /* 3. Current frame scope */
            if (!found)
                found = scope_get(&rt->scope, arr_name, &arr_val);
            /* 4. Global table (inside fn calls) */
            if (!found && rt->call_depth > 0)
                found = scope_table_get(rt->global_table, arr_name, &arr_val);
            /* 5. prst pool */
            if (!found && rt->mode == FLUXA_MODE_PROJECT) {
                int pi = prst_pool_find(&rt->prst_pool, arr_name);
                if (pi >= 0) { arr_val = rt->prst_pool.entries[pi].value; found = 1; }
            }
            if (!found) {
                char buf[280];
                snprintf(buf, sizeof(buf), "for: undefined variable '%s'", arr_name);
                rt_error(rt, buf); return val_nil();
            }

            if (arr_val.type == VAL_ARR) {
                /* for x in arr */
                for (int i = 0; i < arr_val.as.arr.size; i++) {
                    rt_set(rt, node, node->as.for_stmt.var_name,
                           arr_val.as.arr.data[i]);
                    eval(rt, node->as.for_stmt.body);
                    if (rt->had_error || rt->ret.active) break;
                }
            } else if (arr_val.type == VAL_DYN && arr_val.as.dyn) {
                /* for x in dyn — iterate over all elements */
                FluxaDyn *d = arr_val.as.dyn;
                for (int i = 0; i < d->count; i++) {
                    rt_set(rt, node, node->as.for_stmt.var_name, d->items[i]);
                    eval(rt, node->as.for_stmt.body);
                    if (rt->had_error || rt->ret.active) break;
                }
            } else {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "'%s' is not iterable — for..in requires arr or dyn",
                    arr_name);
                rt_error(rt, buf);
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

            /* GC safe point — sweep dyn objects with pin_count == 0 */
            gc_sweep(&rt->gc, gc_dyn_free_fn);
            return val_nil();
        }

        /* ── Sprint 6: free() ────────────────────────────────────────────── */
        case NODE_FREE: {
            const char *var_name = node->as.free_stmt.var_name;

            /* prst check — hard error in all modes, bypasses danger capture */
            if (rt->mode == FLUXA_MODE_PROJECT &&
                prst_pool_find(&rt->prst_pool, var_name) >= 0) {
                char buf[320];
                snprintf(buf, sizeof(buf),
                    "[fluxa] Runtime error (line %d): cannot free prst variable '%s'"
                    " — prst vars are managed by the runtime."
                    " Remove the prst declaration to allow manual free.",
                    rt->current_line, var_name);
                fprintf(stderr, "%s\n", buf);
                rt->had_error = 1;
                return val_nil();
            }

            /* Locate the value — check stack first (covers script-mode locals,
             * fn params, top-level vars resolved to a stack slot), then scope
             * hash table, then Block instance scope. */
            Value *target = NULL;

            /* 1. Stack slot */
            if (node->resolved_offset >= 0 &&
                node->resolved_offset < rt->stack_size &&
                rt->stack[node->resolved_offset].type != VAL_NIL) {
                target = &rt->stack[node->resolved_offset];
            }

            /* 2. Current Block instance scope */
            if (!target && rt->current_instance) {
                ScopeEntry *e = NULL;
                HASH_FIND_STR(rt->current_instance->scope.table, var_name, e);
                if (e) target = &e->value;
            }

            /* 3. Current frame scope */
            if (!target) {
                ScopeEntry *e = NULL;
                HASH_FIND_STR(rt->scope.table, var_name, e);
                if (e) target = &e->value;
            }

            /* 4. Global table (cross-fn visibility) */
            if (!target && rt->call_depth > 0) {
                ScopeEntry *e = NULL;
                HASH_FIND_STR(rt->global_table, var_name, e);
                if (e) target = &e->value;
            }

            if (!target) {
                char buf[280];
                snprintf(buf, sizeof(buf), "free(): undefined variable '%s'", var_name);
                rt_error(rt, buf);
                return val_nil();
            }

            /* For dyn: unregister from GC before freeing */
            if (target->type == VAL_DYN && target->as.dyn)
                gc_unregister(&rt->gc, target->as.dyn);

            /* Free all heap resources and zero the slot */
            value_free_data(target);
            *target = val_nil();
            return val_nil();
        }

        /* ── Sprint 6.b: import c and FFI calls ────────────────────────── */
        /* ── import std <lib> ────────────────────────────────────────────── */
        case NODE_IMPORT_STD: {
            const char *lib = node->as.import_std.lib_name;
            /* Check that the lib was declared in [libs] of fluxa.toml.
             * If not declared, reject with a clear error — do not silently
             * allow access to libs that add binary weight without consent. */
            int declared = 0;
            if (strcmp(lib, "math") == 0 && rt->config.std_libs.has_math) declared = 1;
            if (strcmp(lib, "csv")  == 0 && rt->config.std_libs.has_csv)  declared = 1;
            if (strcmp(lib, "json") == 0 && rt->config.std_libs.has_json) declared = 1;
            if (strcmp(lib, "strings") == 0 && rt->config.std_libs.has_strings) declared = 1;
            if (strcmp(lib, "time")      == 0 && rt->config.std_libs.has_time)      declared = 1;
            if (strcmp(lib, "flxthread") == 0 && rt->config.std_libs.has_flxthread) declared = 1;
            if (strcmp(lib, "ft")         == 0 && rt->config.std_libs.has_flxthread) declared = 1;
            if (!declared) {
                char buf[280];
                snprintf(buf, sizeof(buf),
                    "import std %s: library not declared in [libs] of fluxa.toml."
                    " Add std.%s = 1.0 to [libs] in fluxa.toml to enable it.",
                    lib, lib);
                rt_error(rt, buf);
            }
            return val_nil();  /* registration is implicit via config flags */
        }

        case NODE_IMPORT_C: {
            const char *lib_name = node->as.import_c.lib_name;
            const char *alias    = node->as.import_c.alias;
            /* Skip if already loaded via [ffi] toml section */
            if (ffi_find_lib(&rt->ffi, alias)) return val_nil();
            char path[256];
            ffi_resolve_path("auto", lib_name, path, sizeof(path));
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
            /* Sprint 9.c-3: look up signature for pointer marshalling */
            const FfiSig *sig = ffi_find_sig(lib, sym_name);
            Value result = fluxa_ffi_call(lib, sym_name, ret_vt,
                                          sig,
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

            /* ── std library dispatch — math.fn(), csv.fn(), etc. ─────────── */
            /* std libs are usable outside danger (unlike import c).           */
#ifdef FLUXA_STD_MATH
            if (rt->config.std_libs.has_math && strcmp(owner, "math") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16]; /* max 16 args — more than enough for math */
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_math_call(method, args, argc,
                                           &rt->err_stack, &rt->had_error,
                                           rt->current_line);
            }
#endif /* FLUXA_STD_MATH */

#ifdef FLUXA_STD_CSV
            if (rt->config.std_libs.has_csv && strcmp(owner, "csv") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_csv_call(method, args, argc,
                                          &rt->err_stack, &rt->had_error,
                                          rt->current_line);
            }
#endif /* FLUXA_STD_CSV */

#ifdef FLUXA_STD_JSON
            if (rt->config.std_libs.has_json && strcmp(owner, "json") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_json_call(method, args, argc,
                                           &rt->err_stack, &rt->had_error,
                                           rt->current_line);
            }
#endif /* FLUXA_STD_JSON */

#ifdef FLUXA_STD_STRINGS
            if (rt->config.std_libs.has_strings && strcmp(owner, "strings") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_strings_call(method, args, argc,
                                          &rt->err_stack, &rt->had_error,
                                          rt->current_line);
            }
#endif /* FLUXA_STD_STRINGS */

#ifdef FLUXA_STD_TIME
            if (rt->config.std_libs.has_time && strcmp(owner, "time") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_time_call(method, args, argc,
                                           &rt->err_stack, &rt->had_error,
                                           rt->current_line);
            }
#endif /* FLUXA_STD_TIME */

#ifdef FLUXA_STD_FLXTHREAD
            if (rt->config.std_libs.has_flxthread && strcmp(owner, "ft") == 0) {
                int argc = node->as.member_call.arg_count;
                Value args[16];
                if (argc > 16) argc = 16;
                for (int i = 0; i < argc; i++) {
                    args[i] = eval(rt, node->as.member_call.args[i]);
                    if (rt->had_error) return val_nil();
                }
                return fluxa_std_flxthread_call(method, args, argc,
                                                &rt->err_stack, &rt->had_error,
                                                rt->current_line, rt);
            }
#endif /* FLUXA_STD_FLXTHREAD */

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
                    /* Sprint 9.c-3: look up signature for pointer marshalling */
                    const FfiSig *msig = ffi_find_sig(lib, method);
                    Value result = fluxa_ffi_call(lib, method, ret_vt,
                                                  msig,
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

/* ── Per-thread Runtime clone ────────────────────────────────────────────── */
Runtime *runtime_clone_for_thread(Runtime *parent) {
    Runtime *rt = (Runtime *)calloc(1, sizeof(Runtime));
    if (!rt) return NULL;
    /* Own fields — each thread gets a fresh copy */
    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt->stack[i].type = VAL_NIL;
    rt->stack_size       = 0;
    rt->had_error        = 0;
    rt->danger_depth     = 0;
    rt->call_depth       = 0;
    rt->current_instance = NULL;
    rt->current_thread   = NULL;
    rt->dry_run          = 0;
    rt->current_line     = 0;
    rt->ret.active       = 0;
    rt->ret.tco_active   = 0;
    errstack_clear(&rt->err_stack);
    /* Shared fields — read from parent */
    rt->global_table = parent->global_table;  /* fn lookup — read-only */
    rt->config       = parent->config;         /* lib flags — read-only */
    rt->mode         = parent->mode;
    rt->cancel_flag  = parent->cancel_flag;
    /* Shared mutable — prst pool needs external mutex for thread safety */
    rt->prst_pool    = parent->prst_pool;
    rt->prst_graph   = parent->prst_graph;
    /* GC: each thread has its own GC table to avoid races on dyn alloc */
    gc_init(&rt->gc, parent->config.gc_cap > 0 ? parent->config.gc_cap : 256);
    /* FFI: shared (read-only after init) */
    rt->ffi          = parent->ffi;
    /* IPC: not used in threads */
    return rt;
}

void runtime_free_thread_clone(Runtime *clone) {
    if (!clone) return;
    gc_sweep(&clone->gc, gc_dyn_free_fn);
    free(clone);
}

/* ── Public eval wrapper for std libs ────────────────────────────────────── */
Value runtime_eval(Runtime *rt, ASTNode *node) {
    return eval(rt, node);
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
    rt.current_thread    = NULL;
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
    /* Sprint 9.c-2: pre-load [ffi] libs from fluxa.toml */
    ffi_load_from_config(&rt.ffi, &rt.err_stack, &config);
    /* std libs: propagate config flags to runtime (has_math, has_csv, etc.) */
    rt.config = config;

    if (mode == FLUXA_MODE_PROJECT) {
        prst_pool_init(&rt.prst_pool);   /* pool is dynamic; prst_cap used below */
        /* prst_pool has no init_cap: the fixed initial cap is PRST_POOL_INIT_CAP;
         * here we pre-allocate per config if different from default */
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

    /* Sprint 9.b: expose live rt so IPC SET can write to rt->stack directly */
    if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        rt.cycle_count++;
        if (runtime_is_safe_point(&rt)) {
            ipc_apply_pending_set(&rt);
            if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);
        }
        if (rt.had_error) break;
    }

    if (g_ipc_view) ipc_rtview_clear_live(g_ipc_view);

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc, gc_dyn_free_fn);
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
    /* Sprint 9.c-2: pre-load [ffi] libs from fluxa.toml */
    ffi_load_from_config(&rt.ffi, &rt.err_stack, &config);
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
    gc_collect_all(&rt.gc, gc_dyn_free_fn);
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
    printf("\n── prst (persist across reloads) ");
    printf("────────────────────────────────────\n");
    if (rt->prst_pool.count == 0) {
        printf("  (none)\n");
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
            printf("  %-16s (root)  — %d prst, %d fn\n",
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
    if (block_count == 0) printf("  (none)\n");

    printf("\n── Registered dependencies ────────────────────────────────────\n");
    if (rt->prst_graph.count == 0) {
        printf("  none — state is consistent with the code\n");
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
    /* Sprint 9.c-2: pre-load [ffi] libs from fluxa.toml */
    ffi_load_from_config(&rt.ffi, &rt.err_stack, &config);
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

    if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);

    for (int i = 0; i < program->as.list.count; i++) {
        eval(&rt, program->as.list.children[i]);
        rt.cycle_count++;
        if (runtime_is_safe_point(&rt)) {
            ipc_apply_pending_set(&rt);
            if (g_ipc_view) ipc_rtview_update(g_ipc_view, &rt);
        }
        if (rt.had_error) break;
    }

    if (g_ipc_view) ipc_rtview_clear_live(g_ipc_view);

    int result = rt.had_error ? 1 : 0;
    /* If caller passed pool_in, update it with the final pool state.
     * This allows chained applies (next reload gets current values). */
    if (pool_in) *pool_in = rt.prst_pool;
    else prst_pool_free(&rt.prst_pool);

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc, gc_dyn_free_fn);
    prst_graph_free(&rt.prst_graph);
    ffi_registry_free(&rt.ffi);
    return result;
}

/* ── runtime_exec_with_rt — Sprint 8 ────────────────────────────────────── */
/* Execute a program in an already-allocated and partially-initialized Runtime
 * by the caller (handover_step3_dry_run or extended runtime_apply).
 *
 * Entry contract:
 *   - rt->scope, rt->stack, rt->prst_pool, rt->prst_graph were initialized
 *   - rt->dry_run = 1 for Dry Run, 0 for real execution
 *   - rt->mode = FLUXA_MODE_PROJECT or FLUXA_MODE_SCRIPT
 *
 * Exit contract:
 *   - rt->prst_pool updated with final values
 *   - rt->err_stack populated with any errors encountered
 *   - Returns 0 (success) or 1 (had_error)
 *
 * Does NOT cleanup — caller is responsible for scope_free, gc_collect_all etc.
 */
int runtime_exec_with_rt(Runtime *rt, ASTNode *program) {
    if (!rt || !program || program->type != NODE_PROGRAM) return 1;

    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt->stack[i].type = VAL_NIL;
    rt->stack_size = 0;

    /* Invalidate graph deps — will be re-registered during execution */
    for (int i = 0; i < rt->prst_pool.count; i++)
        prst_graph_invalidate(&rt->prst_graph, rt->prst_pool.entries[i].name);

    for (int i = 0; i < program->as.list.count; i++) {
        eval(rt, program->as.list.children[i]);
        rt->cycle_count++;
        if (rt->had_error) break;
        /* Sprint 8: in Dry Run, any entry in err_stack
         * counts as failure even if had_error is not set —
         * because danger blocks had_error but handover needs to know. */
        if (rt->dry_run && rt->err_stack.count > 0) {
            rt->had_error = 1;
            break;
        }
    }

    return rt->had_error ? 1 : 0;
}
