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
#include "fluxa_alloc.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Thread-shared prst pool accessor.
 * In thread clones, shared_prst_pool points to the parent's PrstPool so all
 * threads read/write the same live state. In the main runtime, it is NULL and
 * we fall back to the local prst_pool. ft.lock() provides per-variable
 * serialization of the read-modify-write cycle for locked prst vars. */
#define RT_POOL(rt) ((rt)->shared_prst_pool ? (rt)->shared_prst_pool : &(rt)->prst_pool)

#ifdef FLUXA_STD_FLXTHREAD
#include "std/flxthread/fluxa_std_flxthread.h"
#endif

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

/* ── Sprint 13: Runtime Update Protocol — restart snapshot ───────────────── */
/* Path to a serialized PrstPool left by the previous binary via execve.
 * Set once by main() before runtime_exec(). Consumed on first cycle. */
static char g_restart_snapshot_path[4096] = {0};

void runtime_set_restart_snapshot(const char *path) {
    if (path) strncpy(g_restart_snapshot_path, path, sizeof(g_restart_snapshot_path)-1);
}

/* ── Hot reload: replaying a prst initializer ─────────────────────────────── */
/* On reload the prst restore path compares the declared initializer against the
 * baseline in the pool, so that editing `prst int n = 12` to `= 99` wins over
 * the runtime value. Detecting that edit means evaluating the initializer — and
 * that is only safe for expressions which have no side effects AND produce a
 * value the comparison can actually read (int/float/bool/str).
 *
 * Everything else — a std lib call, an FFI call, a Block method, a dyn literal
 * — must NOT be replayed. Re-running the constructor a second time is what made
 * `prst dyn win = graph.init(...)` open a fresh window on every save: the call
 * ran, a new window appeared, and the resulting value was then discarded anyway
 * because the comparison has no branch for VAL_DYN. The same shape reopens a
 * database with sqlite.open and reallocates a cursor with csv.open.
 *
 * So: replay only the literal-shaped initializers. For the rest the pooled
 * value simply wins, which is precisely what `prst` promises. */
static int prst_init_is_replayable(const ASTNode *n) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STRING_LIT:
            return 1;
        case NODE_BINARY_EXPR:
            return prst_init_is_replayable(n->as.binary.left) &&
                   prst_init_is_replayable(n->as.binary.right);
        default:
            return 0;
    }
}

/* The prst pool outlives the runtime that produced it, so anything it still
 * points at must survive the teardown collect. Detaching those dyn wrappers
 * from the GC table hands them to the pool for the length of the reload; the
 * next run re-registers them when the declaration is restored.
 *
 * Only the wrapper is at stake: the VAL_PTR a lib parks inside it (a window, a
 * cursor, a connection) is opaque to the GC either way. That asymmetry is what
 * made the old behavior so confusing — the window object stayed alive while the
 * handle that reached it was freed underneath the pool. */
static void prst_detach_dyns_from_gc(GCTable *gc, PrstPool *pool) {
    if (!pool || !pool->entries) return;
    for (int i = 0; i < pool->count; i++) {
        Value *v  = &pool->entries[i].value;
        Value *iv = &pool->entries[i].init_value;
        if (v->type  == VAL_DYN && v->as.dyn)  gc_unregister(gc, v->as.dyn);
        if (iv->type == VAL_DYN && iv->as.dyn) gc_unregister(gc, iv->as.dyn);
    }
}

/* A Dry Run is a rehearsal, and a rehearsal must not hand a live external
 * resource to production. dry_run suppresses print but not lib calls, so
 * graph.init() during step 3 really does open a window; step 3 then collects
 * B's GC while deliberately letting the pool survive into step 4. Anything the
 * rehearsal built is therefore already freed by the time the pool is handed
 * over — a live handle in that pool is a dangling one.
 *
 * Turning those entries back into headstones (VAL_NIL under the original
 * declared_type) says exactly the right thing to the runtime that takes over:
 * this resource was rehearsed, not created; build it for real. Serializable
 * values are untouched — they are what the snapshot exists to carry. */
void runtime_prst_headstone_resources(PrstPool *pool) {
    if (!pool || !pool->entries) return;
    for (int i = 0; i < pool->count; i++) {
        Value *v  = &pool->entries[i].value;
        Value *iv = &pool->entries[i].init_value;
        if (v->type  == VAL_DYN) { v->type  = VAL_NIL; v->as.dyn  = NULL; }
        if (iv->type == VAL_DYN) { iv->type = VAL_NIL; iv->as.dyn = NULL; }
    }
}

/* ── Error helpers ────────────────────────────────────────────────────────── */
/* Sprint 8: rt_error_line includes line number in message and ErrEntry.
 * rt_error kept for compatibility — calls rt_error_line with line=0. */
/* Where an error happened, as "file:line" when the file is known.  Modules are
 * parsed one at a time and each numbers its own lines from 1, so a bare line
 * number cannot be traced back to a file once several are imported. */
static void rt_error_where(Runtime *rt, int line, char *out, size_t outsz) {
    const char *src = fluxa_src_name(rt->current_src);
    if (line > 0 && src) snprintf(out, outsz, " (%s:%d)", src, line);
    else if (line > 0)   snprintf(out, outsz, " (line %d)", line);
    else if (src)        snprintf(out, outsz, " (%s)", src);
    else                 out[0] = '\0';
}

static void rt_error_line(Runtime *rt, const char *msg, int line) {
    /* If no explicit line given, use the last tracked line */
    int eff_line = (line > 0) ? line : rt->current_line;
    if (rt->danger_depth > 0) {
        const char *ctx = rt->current_instance
                        ? rt->current_instance->name
                        : "<global>";
        errstack_push(&rt->err_stack, ERR_FLUXA, msg, ctx, eff_line);
    } else {
        char where[192];
        rt_error_where(rt, eff_line, where, sizeof(where));
        fprintf(stderr, "[fluxa] Runtime error%s: %s\n", where, msg);
        rt->had_error = 1;
    }
}

/* A stdlib or FFI call reports by pushing onto the error stack and setting
 * had_error, which is what a `danger` block reads.  Outside one, nobody reads
 * it: the process used to exit non-zero having printed nothing at all, so a
 * misspelled library function looked like a silent nil.  Surface whatever the
 * call pushed, in the same shape rt_error_line prints. */
static void rt_surface_lib_errors(Runtime *rt, int before) {
    if (rt->danger_depth > 0) return;
    if (rt->err_stack.count <= before) return;
    char where[192];
    rt_error_where(rt, rt->current_line, where, sizeof(where));
    /* errstack_get indexes from the most recent, so walk the new entries from
     * the oldest of them to report them in the order they were raised. */
    for (int i = rt->err_stack.count - before - 1; i >= 0; i--) {
        const ErrEntry *e = errstack_get(&rt->err_stack, i);
        if (e) fprintf(stderr, "[fluxa] Runtime error%s: %s\n", where, e->message);
    }
    rt->err_stack.count = before;
    rt->had_error = 1;
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
/* All stdlib includes are centralized in lib_registry_gen.h.
 * To add a new lib: create the header + FLUXA_LIB_EXPORT + lib.mk.
 * No changes needed here. */
#include "lib_registry_gen.h"

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
    /* ── Warm path (Sprint 11) ───────────────────────────────────────────────
     * warm_local: resolver confirmed this is a function-local variable, never
     * prst. We read the WarmSlot (1 byte) rather than the full ASTNode union.
     * If the slot is stable (QJL guard = 1, stable_runs >= 2) and the type
     * in the stack slot matches the observed type, return directly —
     * zero prst_pool_has, zero scope_get, zero strcmp.
     *
     * If the slot is not yet warm, we still skip prst_pool_has (safe because
     * warm_local guarantees the variable is never prst) and fall through to
     * the stack slot read + scope fallback. warm_record() observes the result
     * for future promotion. */
    if (node && node->warm_local && node->resolved_offset >= 0) {
        int off = node->resolved_offset;

        /* ── Promoted warm read ──────────────────────────────────────────────
         * rt->current_wf is set once at call_function entry (O(1) hash).
         * Inside rt_get it is just a pointer load — zero hash cost.
         * Only active when the function has been promoted (stable_runs >= 2). */
        WarmFunc *wf = rt->current_wf;
        if (wf) {
            if (warm_func_is_promoted(wf) && off < WARM_SLOTS_MAX &&
                off < rt->stack_size) {
                WarmSlot *ws = &wf->slots[off];
                if (ws->qjl_guard) {
                    Value v = rt->stack[off];
                    /* QJL residual: exact type match — 1-byte WarmSlot read */
                    if (v.type != 0 &&
                        warm_type_from_val_type((int)v.type) == ws->observed_type) {
                        return v; /* warm read: 1B slot + 8B stack = 9B touched */
                    }
                    /* Type diverged: QJL guard fires, demote to cold */
                    ws->qjl_guard   = 0;
                    wf->stable_runs = 0;
                }
            } else if (warm_func_observing(wf)) {
                /* Observation phase — build profile for future promotion.
                 * Stops automatically after WARM_OBS_LIMIT function calls.
                 * After that: either promoted (fast) or cold-locked (just
                 * falls through to direct stack read below — zero overhead). */
                if (off < rt->stack_size) {
                    Value v = rt->stack[off];
                    if (v.type != 0) {
                        warm_record(wf, off, (int)v.type);
                        return v;
                    }
                }
            }
        }

        /* ── Direct stack read (warm_local: never-prst, skip prst_pool_has) ─ */
        if (off < rt->stack_size) {
            Value v = rt->stack[off];
            if (v.type != 0) return v;
        }
        Value v; v.type = 0;
        if (scope_get(&rt->scope, name, &v)) return v;
        { char buf[280];
          snprintf(buf, sizeof(buf), "undefined variable: %s", name);
          rt_error(rt, buf); }
        return v;
    }

    /* ── Cold path — full resolution ─────────────────────────────────────────
     * When inside a function call, prst globals must be read from scope,
     * not from the stack. Local fn vars (params, locals) share stack slot
     * numbers with global prst vars in the resolver's flat offset space. */
    if (rt->call_depth > 0 && node && node->resolved_offset >= 0 &&
        prst_pool_has(RT_POOL(rt), name)) {
        Value v; v.type = VAL_NIL;
        /* In thread clones, always read prst from the shared pool so we see
         * the latest value written by any thread. Local scope is a stale copy.
         * Note: lock is acquired in NODE_ASSIGN (write side) — the read here
         * is intentionally outside the lock to avoid recursive mutex acquire. */
        if (rt->shared_prst_pool) {
            prst_pool_get(RT_POOL(rt), name, &v);
            return v;
        }
        if (scope_get(&rt->scope, name, &v) && v.type != VAL_NIL) return v;
        /* prst var not yet in scope (first access before fn writes it) —
         * try the global scope table which holds the top-level frame. */
        if (scope_table_get(rt->global_table, name, &v) && v.type != VAL_NIL) return v;
        return v;
    }
    if (node && node->resolved_offset >= 0 &&
        node->resolved_offset < rt->stack_size) {
        Value v = rt->stack[node->resolved_offset];
        /* Only trust the stack slot if it contains a real value.
         * Block instances and other scope-stored vars have a resolved_offset
         * assigned by the resolver but are never written to the stack —
         * their slot stays VAL_NIL. Fall through to scope lookup in that case.
         * In thread clones, prst globals live in the shared pool — if the
         * stack slot is NIL (not yet written by this thread), read the pool. */
        if (v.type != VAL_NIL) {
            /* If we're in a thread clone and this var is a prst global,
             * always read from shared pool to see other threads' writes. */
            if (rt->shared_prst_pool && prst_pool_has(RT_POOL(rt), name)) {
                prst_pool_get(RT_POOL(rt), name, &v);
            }
            return v;
        }
    }
    /* Thread clone fallback: if prst var not yet on local stack, read pool. */
    if (rt->shared_prst_pool && prst_pool_has(RT_POOL(rt), name)) {
        Value v; v.type = VAL_NIL;
        prst_pool_get(RT_POOL(rt), name, &v);
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
        /* Drop the previous slot's GC reference before overwriting. Only
         * checked when at least one dyn is alive — keeps the rt_set hot
         * path free for int/str/bench workloads. Without this, reassigning
         * a dyn variable in a loop keeps every prior dyn pinned and the
         * sweep skips them, producing a slow accumulating leak. */
        /* Slot overwrite: detach from old GC reference, attach new. Symmetric
         * pin/unpin keeps every slot=>dyn invariant maintained even across
         * NODE_ASSIGN (which doesn't carry an explicit pin step like
         * NODE_VAR_DECL does). Then release any owned heap data on the slot
         * — strings and array data — so initialization patterns like
         *   `str x = ""` followed by `x = call()`
         *   `str arr p[N] = [...]` in a worker loop
         * don't leak the previous strdup'd buffer or arr storage. */
        {
            Value *_old = &rt->stack[node->resolved_offset];
            if (_old->type == VAL_DYN && _old->as.dyn &&
                !(v.type == VAL_DYN && v.as.dyn == _old->as.dyn))
                gc_unpin(&rt->gc, _old->as.dyn);
            if (_old->type == VAL_STRING && _old->as.string &&
                !(v.type == VAL_STRING && v.as.string == _old->as.string))
                fxstr_release(_old->as.string);
            else if (_old->type == VAL_ARR && _old->as.arr.data &&
                     fluxa_arr_is_owned(&_old->as.arr) &&
                     !(v.type == VAL_ARR && v.as.arr.data == _old->as.arr.data))
                value_release_data(_old);
        }
        rt->stack[node->resolved_offset] = v;
        /* Pin the new VAL_DYN — symmetric with the unpin above so the slot
         * holds exactly one strong reference, allowing gc_sweep to collect
         * orphaned dyns produced by intermediate calls without touching the
         * one we still hold. */
        if (v.type == VAL_DYN && v.as.dyn)
            gc_pin(&rt->gc, v.as.dyn);
        return;
    }
    if (rt->current_instance) {
        if (scope_has(&rt->current_instance->scope, name)) {
            /* Bug K fix: adopt the value (owned store). scope_set would
             * strdup a copy and abandon the evaluated original — one leaked
             * string per field assignment in module-Block methods. */
            scope_set_owned(&rt->current_instance->scope, name, v);
            return;
        }
    }
    /* Bug K fix: same adoption for unresolved frame locals — scope_set's
     * copy-in leaked the evaluated original once per declaration/assign. */
    scope_set_owned(&rt->scope, name, v);
}

/* ── Forward declaration ─────────────────────────────────────────────────── */
static Value eval(Runtime *rt, ASTNode *node);
/* v0.14: callback bridging OP_CALL_METHOD / OP_CALL_FUNC — Value* owner for cache */
static Value vm_call_callback(void *rt_opaque, Value *owner_kv,
                               const char *method_or_func,
                               Value *args, int argc);
/* v0.14: field access callbacks — Value* owner_kv for inline cache */
static Value vm_get_field_callback(void *rt_opaque, Value *owner_kv, const char *field);
static void  vm_set_field_callback(void *rt_opaque, Value *owner_kv, const char *field, Value val);
static int   vm_indexable_callback(void *rt_opaque, const ASTNode *fn_node,
                                  const char *name, int resolved_offset);
static const char *vm_probe_callback(void *rt_opaque, int kind,
                                     const char *owner, const char *name,
                                     int *flag);
static Value vm_index_callback(void *rt_opaque, int action,
                               VMIndexCache *cache, const char *name,
                               Value *registers, int register_count,
                               int resolved_offset, Value index,
                               Value incoming);
/* v0.14: tick callback — called at every OP_JUMP back-edge. */
static void vm_tick_callback(void *rt_opaque);

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

    /* ── All other ops: evaluate both sides first ──────────────────────── *
     * Track ownership: VAL_STRING produced by a literal or by a call is
     * heap-owned by this eval and must be released before we return; the
     * binary ops below (==, !=, arithmetic) never store the operands into
     * the result so freeing them is safe. Without this, every
     * `s == "literal"` comparison inside a worker loop leaks the strdup of
     * the literal. Aliased operands (variable refs, member/array access) are
     * NOT freed — they share storage with their owner. */
    ASTNode *_ln = node->as.binary.left;
    ASTNode *_rn = node->as.binary.right;
    int _lown = (_ln->type == NODE_STRING_LIT)  ||
                (_ln->type == NODE_MEMBER_CALL) ||
                (_ln->type == NODE_FUNC_CALL)   ||
                (_ln->type == NODE_FFI_CALL)    ||
                (_ln->type == NODE_IDENTIFIER) ||
                (_ln->type == NODE_ARR_ACCESS)  ||
                (_ln->type == NODE_MEMBER_ACCESS);
    int _rown = (_rn->type == NODE_STRING_LIT)  ||
                (_rn->type == NODE_MEMBER_CALL) ||
                (_rn->type == NODE_FUNC_CALL)   ||
                (_rn->type == NODE_FFI_CALL)    ||
                (_rn->type == NODE_IDENTIFIER) ||
                (_rn->type == NODE_ARR_ACCESS)  ||
                (_rn->type == NODE_MEMBER_ACCESS);
    Value      left  = eval(rt, _ln);
    if (rt->had_error) return val_nil();
    Value      right = eval(rt, _rn);
    if (rt->had_error) {
        if (_lown && left.type == VAL_STRING && left.as.string) fxstr_release(left.as.string);
        return val_nil();
    }
    #define _BIN_RET(v) do { \
        Value _vv = (v); \
        if (_lown && left.type  == VAL_STRING && left.as.string)  fxstr_release(left.as.string); \
        if (_rown && right.type == VAL_STRING && right.as.string) fxstr_release(right.as.string); \
        return _vv; \
    } while (0)

    if (strcmp(op, "==") == 0) {
        if (left.type==VAL_NIL   && right.type==VAL_NIL)   _BIN_RET(val_bool(1));
        if (left.type==VAL_NIL   || right.type==VAL_NIL)   _BIN_RET(val_bool(0));
        if (left.type==VAL_INT   && right.type==VAL_INT)   _BIN_RET(val_bool(left.as.integer==right.as.integer));
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) _BIN_RET(val_bool(left.as.real==right.as.real));
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  _BIN_RET(val_bool(left.as.boolean==right.as.boolean));
        if (left.type==VAL_STRING&& right.type==VAL_STRING) _BIN_RET(val_bool(strcmp(left.as.string,right.as.string)==0));
        _BIN_RET(val_bool(0));
    }
    if (strcmp(op, "!=") == 0) {
        if (left.type==VAL_NIL   && right.type==VAL_NIL)   _BIN_RET(val_bool(0));
        if (left.type==VAL_NIL   || right.type==VAL_NIL)   _BIN_RET(val_bool(1));
        if (left.type==VAL_INT   && right.type==VAL_INT)   _BIN_RET(val_bool(left.as.integer!=right.as.integer));
        if (left.type==VAL_FLOAT && right.type==VAL_FLOAT) _BIN_RET(val_bool(left.as.real!=right.as.real));
        if (left.type==VAL_BOOL  && right.type==VAL_BOOL)  _BIN_RET(val_bool(left.as.boolean!=right.as.boolean));
        if (left.type==VAL_STRING&& right.type==VAL_STRING) _BIN_RET(val_bool(strcmp(left.as.string,right.as.string)!=0));
        _BIN_RET(val_bool(1));
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
        _BIN_RET(val_nil());
    }
    if      (right.type==VAL_INT)   r = (double)right.as.integer;
    else if (right.type==VAL_FLOAT) r = right.as.real;
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "arithmetic on non-numeric value (got %s for '%s')",
                 val_type_name(right.type), op);
        rt_error_line(rt, buf, node->line);
        _BIN_RET(val_nil());
    }

    if (strcmp(op,"+")==0) { double res=l+r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"-")==0) { double res=l-r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"*")==0) { double res=l*r; return both_int?val_int((long)res):val_float(res); }
    if (strcmp(op,"/")==0) {
        if (r==0) { rt_error(rt, "division by zero"); _BIN_RET(val_nil()); }
        double res=l/r; return both_int?val_int((long)res):val_float(res);
    }
    if (strcmp(op,"%")==0) {
        if (!both_int) { rt_error(rt, "modulo requires integer operands"); _BIN_RET(val_nil()); }
        if ((long)r==0) { rt_error(rt, "modulo by zero"); _BIN_RET(val_nil()); }
        _BIN_RET(val_int((long)l % (long)r));
    }
    if (strcmp(op,"<")==0)  _BIN_RET(val_bool(l< r));
    if (strcmp(op,">")==0)  _BIN_RET(val_bool(l> r));
    if (strcmp(op,"<=")==0) _BIN_RET(val_bool(l<=r));
    if (strcmp(op,">=")==0) _BIN_RET(val_bool(l>=r));
    rt_error(rt, "unknown operator"); _BIN_RET(val_nil());
    #undef _BIN_RET
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
/* ── Frame slot teardown ─────────────────────────────────────────────────
 * Release heap values owned by the callee's stack slots before the frame is
 * discarded (normal return) or reused (tail call). Ownership unification:
 * every producer (literal, call, identifier, arr/member access) hands stores
 * an owned VAL_STRING copy, so each slot is the sole owner of its string.
 * VAL_ARR slots own their data iff .owned (param refs are borrowed views).
 * VAL_DYN slots are left UNTOUCHED: parameter binding adopts the caller's
 * dyn pointer WITHOUT acquiring a pin (only rt_set pins), so unpinning here
 * would steal the caller's pin and let the GC sweep a live dyn (verified:
 * heap-use-after-free on the game's `win` bind). Dyn lifecycle stays exactly
 * as before this patch — pins are dropped by reassignment and lib release
 * functions (sqlite.close / json2.discard / ...), per the language guide.
 * `keep` guards against releasing heap data aliased by the return value.
 * `from`: first slot to release. Interpreter frames own their params (args
 * are handed over by the caller) and release from 0. The VM->interpreter
 * bridges adopt VM REGISTER pointers into param slots — those stay owned by
 * the VM register lifecycle, so the bridges release locals only (from=argc). */
static void frame_release_slots_from(Runtime *rt, int from, const Value *keep) {
    for (int _i = from; _i < rt->stack_size; _i++) {
        Value *sv = &rt->stack[_i];
        switch (sv->type) {
        case VAL_STRING:
            if (sv->as.string &&
                !(keep && keep->type == VAL_STRING &&
                  keep->as.string == sv->as.string))
                fxstr_release(sv->as.string);
            sv->type = VAL_NIL; sv->as.string = NULL;
            break;
        case VAL_ARR:
            if (sv->as.arr.data && fluxa_arr_is_owned(&sv->as.arr) &&
                !(keep && keep->type == VAL_ARR &&
                  keep->as.arr.data == sv->as.arr.data))
                value_release_data(sv);
            sv->type = VAL_NIL;
            break;
        default: break;   /* VAL_DYN and scalars: untouched (see above) */
        }
    }
}

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
    ASTNode       *caller_fn    = rt->current_fn;   /* warm path: save caller fn */
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
    rt->current_fn       = fn_node;   /* warm path: this fn is the stable key */
    rt->current_wf       = (rt->warm.enabled)
                           ? warm_profile_get_func(&rt->warm, (uintptr_t)fn_node)
                           : NULL;    /* O(1) hash — done once per call entry */
    rt->call_depth++;

    /* Populate new scope with scalar prst vars so rt_get can find them
     * by name without aliasing stack slots with local fn vars.
     * Skip pointer-based types (Block, dyn, arr) — they must not be
     * shallow-copied into each frame as that corrupts pointer semantics.
     * For VAL_STRING: scope_free() will free the char* — so we must
     * strdup() to give each frame its own copy independent of the pool. */
    if (rt->mode == FLUXA_MODE_PROJECT) {
        for (int _pi = 0; _pi < RT_POOL(rt)->count; _pi++) {
            PrstEntry *_pe = &RT_POOL(rt)->entries[_pi];
            ValType _vt = _pe->value.type;
            if (_vt == VAL_INT || _vt == VAL_FLOAT || _vt == VAL_BOOL) {
                scope_set(&rt->scope, _pe->name, _pe->value);
            } else if (_vt == VAL_STRING && _pe->value.as.string) {
                /* scope_set copies in (fxstr_new) — passing the pool's raw
                 * pointer directly is safe and kills the old pre-copy leak. */
                scope_set(&rt->scope, _pe->name, _pe->value);
            }
        }
    }

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
                rt->stack[i] = val_arr_ref(args[i].as.arr.data, args[i].as.arr.size,
                                           args[i].as.arr.owned);
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

        /* Update WHT path signature after body completes — warm profile
         * uses this to detect stable execution paths (stable_runs counter).
         * current_wf is already set for this function — no hash lookup. */
        if (rt->warm.enabled && rt->current_wf != NULL) {
            warm_update_sig(rt->current_wf);
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
            /* Tail call: the frame is reused — release this iteration's
             * heap-owning slots first (next_args are owned copies, safe). */
            frame_release_slots_from(rt, 0, NULL);
            rt->stack_size = 0;
            rt->ret.tco_active = 0;

            fn_node     = next_fn;
            param_count = next_fn->as.func_decl.param_count;
            args        = next_args;    /* already evaluated */
            method_inst = rt->current_instance; /* keep same instance context */
            /* Warm path: update cached fn identity for the new TCO target */
            rt->current_fn = next_fn;
            rt->current_wf = (rt->warm.enabled)
                             ? warm_profile_get_func(&rt->warm, (uintptr_t)next_fn)
                             : NULL;
            continue;  /* trampoline: go back to top of while(1) */
        }

        /* Normal return */
        result = rt->ret.active ? rt->ret.value : val_nil();
        rt->ret.active = 0;
        break;
    }

    /* ── Restore caller frame ── */
    free(args); /* safety — NULL if already freed in trampoline */

    /* ── arr return ownership fix ────────────────────────────────────────
     * If the function returns a VAL_ARR, its data pointer is shared with
     * a scope entry in rt->scope. scope_free() below will call
     * value_release_data() on that entry and free the data — leaving
     * result.as.arr.data as a dangling pointer.
     *
     * Fix: deep-copy the arr data before freeing the scope.
     * The copy is owned by the caller; the scope entry is freed normally.
     * This is O(n) on arr size — negligible for typical arr returns.     */
    if (result.type == VAL_ARR && result.as.arr.data &&
        fluxa_arr_is_owned(&result.as.arr) && result.as.arr.size > 0) {
        int n = result.as.arr.size;
        Value *copy = (Value *)malloc(sizeof(Value) * (size_t)n);
        if (copy) {
            for (int _i = 0; _i < n; _i++) {
                copy[_i] = result.as.arr.data[_i];
                if (copy[_i].type == VAL_STRING && copy[_i].as.string)
                    copy[_i].as.string = fxstr_retain(copy[_i].as.string);
            }
            result.as.arr.data  = copy;
            fluxa_arr_set_owned(&result.as.arr, 1);
        }
        /* If malloc fails: result.as.arr.data will be freed by scope_free
         * and become dangling. This is an OOM edge case — better than a
         * silent use-after-free on every arr return. */
    }

    /* Str returns need no per-method strdup here: ownership unification
     * makes every producer (identifier, member/arr access, calls) hand the
     * return path an owned copy, so `result` never aliases instance or
     * frame storage. A dup at this point would leak the producer's copy. */

    /* Ownership teardown: free the callee's heap-owning slots before the
     * caller frame is restored over them. Without this, the last value in
     * every str local leaks once per call (bug K, leak B). */
    frame_release_slots_from(rt, 0, &result);
    scope_free(&rt->scope);
    rt->scope            = caller_scope;
    rt->stack_size       = caller_sz;
    rt->current_instance = caller_inst;
    rt->current_fn       = caller_fn;   /* warm path: restore caller fn */
    rt->current_wf       = (rt->warm.enabled && caller_fn)
                           ? warm_profile_get_func(&rt->warm, (uintptr_t)caller_fn)
                           : NULL;
    if (caller_stack) {
        memcpy(rt->stack, caller_stack, sizeof(Value) * save_slots);
        free(caller_stack);
    }
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
            /* Bug K fix: adopt the initializer value — scope_set's copy-in
             * leaked the evaluated original once per field per instance. */
            scope_set_owned(scope, member->as.var_decl.var_name, v);
    } else if (member->type == NODE_FUNC_DECL) {
        Value v; v.type = VAL_FUNC; v.as.func = member;
        char key[256];
        if (!block_method_key(member->as.func_decl.name, key, sizeof(key))) {
            rt_error(rt, "invalid Block method identifier");
            return;
        }
        scope_set(scope, key, v);
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
                        data[i].as.string = fxstr_retain(data[i].as.string);
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

/* ── v0.14: vm_tick_callback — called at OP_JUMP back-edge ──────────────── */
/* Runs GC sweep and (in thread clones) processes the mailbox.
 * The lib (flxthread) knows nothing about this — it registers a mailbox
 * and the thread runtime calls its own processing naturally here.           */
/* ── vm_store_callback — called by OP_MOVE on variable-register stores ──── */
/* Mirrors rt_set's GC protocol for stores performed by the VM: unpin the
 * dyn being overwritten, pin the dyn being stored (slot=>pin invariant,
 * "pin_count > 0 = referenced by at least one scope"). Without this, a dyn
 * assigned inside a compiled while loop stays at pin_count 0 and the
 * back-edge gc_sweep frees it while the variable still holds it (UAF).
 * Self-move (same dyn on both sides) is a no-op to keep counts balanced. */
static void vm_store_callback(void *rt_opaque, Value *slot, Value *incoming) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return;
    FluxaDyn *old_d = (slot->type     == VAL_DYN) ? slot->as.dyn     : NULL;
    FluxaDyn *new_d = (incoming->type == VAL_DYN) ? incoming->as.dyn : NULL;
    if (old_d == new_d) return;
    if (old_d) gc_unpin(&rt->gc, old_d);
    if (new_d) gc_pin(&rt->gc, new_d);
}

/* Cache only the owning Value slot.  The heap payload may be replaced, so a
 * cached FluxaArr.data pointer would be a stale-pointer/UAF risk. */
static Value *vm_index_slot(Runtime *rt, VMIndexCache *cache, const char *name,
                            Value *registers, int register_count,
                            int resolved_offset) {
    if (cache->slot) return cache->slot;
    if (resolved_offset >= 0 && resolved_offset < register_count &&
        registers[resolved_offset].type == VAL_ARR) {
        cache->slot = &registers[resolved_offset];
        return cache->slot;
    }
    ScopeEntry *entry = NULL;
    if (rt->current_instance)
        HASH_FIND_STR(rt->current_instance->scope.table, name, entry);
    if (!entry) HASH_FIND_STR(rt->scope.table, name, entry);
    if (!entry && rt->call_depth > 0)
        HASH_FIND_STR(rt->global_table, name, entry);
    if (entry && entry->value.type == VAL_ARR) {
        cache->slot = &entry->value;
        return cache->slot;
    }
    return NULL;
}

/* String reads require owned retains and dyn owns growth/GC behavior.  Until
 * VM temporaries have an explicit release protocol, only fixed primitive
 * arrays are eligible for this bytecode path. */
static int vm_array_is_primitive(Value *slot) {
    if (!slot || slot->type != VAL_ARR) return 0;
    if (!fluxa_arr_has_cached_type(&slot->as.arr))
        fluxa_arr_cache_type(&slot->as.arr,
                             fluxa_arr_detect_type(slot->as.arr.data,
                                                   slot->as.arr.size));
    ValType t = fluxa_arr_cached_type(&slot->as.arr);
    return t == VAL_NIL || t == VAL_INT || t == VAL_FLOAT || t == VAL_BOOL;
}

/* "int arr", "float arr", "bool arr" — a declared element type the compiled
 * path can carry.  A bare "arr" parameter is untyped and "str arr" owns its
 * elements, so neither qualifies. */
static int arr_decl_type_is_value(const char *t) {
    if (!t) return 0;
    return strncmp(t, "int ",   4) == 0 ||
           strncmp(t, "float ", 6) == 0 ||
           strncmp(t, "bool ",  5) == 0 ||
           strcmp(t, "int") == 0 || strcmp(t, "float") == 0 ||
           strcmp(t, "bool") == 0;
}

/* Does this Block declaration own `fn_node` as one of its methods?  Only then
 * do its field declarations describe what that function's body will see. */
static int block_owns_fn(BlockDef *def, const ASTNode *fn_node) {
    if (!def || !def->node || def->node->type != NODE_BLOCK_DECL) return 0;
    for (int i = 0; i < def->node->as.block_decl.count; i++)
        if (def->node->as.block_decl.members[i] == fn_node) return 1;
    return 0;
}

/* Declared element type of an array named `name` in a Block declaration. */
static const char *block_arr_decl_type(BlockDef *def, const char *name) {
    if (!def || !def->node || def->node->type != NODE_BLOCK_DECL) return NULL;
    for (int i = 0; i < def->node->as.block_decl.count; i++) {
        ASTNode *m = def->node->as.block_decl.members[i];
        if (m && m->type == NODE_ARR_DECL && m->as.arr_decl.arr_name &&
            strcmp(m->as.arr_decl.arr_name, name) == 0) {
            if (m->as.arr_decl.persistent) return NULL;  /* prst: pool rules */
            return m->as.arr_decl.type_name;
        }
    }
    return NULL;
}

/* Declared element type of an array local to a function body. */
static const char *fn_local_arr_decl_type(const ASTNode *body, const char *name) {
    if (!body || body->type != NODE_BLOCK_STMT) return NULL;
    for (int i = 0; i < body->as.list.count; i++) {
        ASTNode *st = body->as.list.children[i];
        if (st && st->type == NODE_ARR_DECL && st->as.arr_decl.arr_name &&
            strcmp(st->as.arr_decl.arr_name, name) == 0) {
            if (st->as.arr_decl.persistent) return NULL;
            return st->as.arr_decl.type_name;
        }
    }
    return NULL;
}

/* A function-body chunk is cached on its AST node and reused for every later
 * call, from any instance, so this answers only from declarations: the
 * function's own parameters and array locals, and — when the function really
 * is a method of the current instance's Block — that Block's array fields.
 * The runtime revalidates the element type on every read and write, so this
 * decides whether to emit the opcode, not whether it is safe. */
static int vm_fn_indexable(Runtime *rt, const ASTNode *fn_node,
                           const char *name) {
    if (!fn_node || fn_node->type != NODE_FUNC_DECL) return 0;
    for (int i = 0; i < fn_node->as.func_decl.param_count; i++) {
        const char *pn = fn_node->as.func_decl.param_names[i];
        if (pn && strcmp(pn, name) == 0)
            return arr_decl_type_is_value(fn_node->as.func_decl.param_types[i]);
    }
    const char *t = fn_local_arr_decl_type(fn_node->as.func_decl.body, name);
    if (t) return arr_decl_type_is_value(t);
    if (rt->current_instance && block_owns_fn(rt->current_instance->def, fn_node)) {
        t = block_arr_decl_type(rt->current_instance->def, name);
        if (t) return arr_decl_type_is_value(t);
    }
    return 0;
}

static int vm_indexable_callback(void *rt_opaque, const ASTNode *fn_node,
                                 const char *name, int resolved_offset) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt || !name || strcmp(name, "err") == 0) return 0;
    if (prst_pool_has(RT_POOL(rt), name)) {
        /* prst arrays carry pool synchronisation; a loop chunk mirrors writes
         * to the pool, but only after resolving the entry for this run. */
        if (fn_node) return 0;
    }
    if (fn_node) return vm_fn_indexable(rt, fn_node, name);
    VMIndexCache cache = { NULL, -2, 0 };
    Value *slot = vm_index_slot(rt, &cache, name, rt->stack,
                                rt->stack_size, resolved_offset);
    return vm_array_is_primitive(slot);
}

/* Declared-type lookups over a Block's AST.  Used only while a chunk is being
 * built, never while it runs. */
static const char *block_decl_type(BlockDef *def, const char *name, int *is_fn) {
    if (!def || !def->node || def->node->type != NODE_BLOCK_DECL) return NULL;
    ASTNode *decl = def->node;
    for (int i = 0; i < decl->as.block_decl.count; i++) {
        ASTNode *m = decl->as.block_decl.members[i];
        if (!m) continue;
        if (m->type == NODE_VAR_DECL && m->as.var_decl.var_name &&
            strcmp(m->as.var_decl.var_name, name) == 0) {
            /* prst fields carry pool synchronisation and, in thread clones,
             * locking that the field opcodes do not perform.  Report them as
             * unknown so the loop keeps the evaluator's path. */
            if (m->as.var_decl.persistent) return NULL;
            if (is_fn) *is_fn = 0;
            return m->as.var_decl.type_name;
        }
        if (m->type == NODE_FUNC_DECL && m->as.func_decl.name &&
            strcmp(m->as.func_decl.name, name) == 0) {
            if (is_fn) *is_fn = 1;
            return m->as.func_decl.return_type;
        }
    }
    return NULL;
}

/* int, float and bool never own heap data, so a register holding one needs no
 * retain, no release and no drop. */
static int type_name_is_value(const char *t) {
    return t && (strcmp(t, "int") == 0 || strcmp(t, "float") == 0 ||
                 strcmp(t, "bool") == 0);
}

/* Declared types the bytecode field path can carry.  int, float and bool own
 * no heap data; str is carried as an owned reference — the field callback
 * retains on the way out and the register's drop balances it.  Everything else
 * (arr, dyn, Block instances, unknown) stays on the evaluator. */
static int type_name_is_field_ok(const char *t) {
    return type_name_is_value(t) || (t && strcmp(t, "str") == 0);
}

/* A declared return type that can never produce a string.  "nil" included:
 * those calls hand back VAL_NIL. */
static int ret_type_never_string(const char *t) {
    return t && (type_name_is_value(t) || strcmp(t, "nil") == 0);
}

/* Compile-time probe — see vm_probe_cb_t in bytecode.h.  Everything here runs
 * while a chunk is being built; none of it is on the per-iteration path.
 *
 * Bare identifiers inside a Block method are the case that mattered: the
 * resolver leaves them without a stack slot, which used to demote the whole
 * enclosing loop to the tree walker even though rt_get/rt_set reach them
 * through rt->current_instance.  prst names are refused so the pool keeps the
 * precedence rt_get gives it, and the opcode carries the instance name rather
 * than a pointer, so that name must resolve back to this same instance. */
static const char *vm_probe_callback(void *rt_opaque, int kind,
                                     const char *owner, const char *name,
                                     int *flag) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (flag) *flag = 0;
    if (!rt || !name) return NULL;

    if (kind == VM_PROBE_BARE_FIELD) {
        BlockInstance *inst = rt->current_instance;
        if (!inst || !inst->name[0]) return NULL;
        if (prst_pool_has(RT_POOL(rt), name)) return NULL;
        int is_fn = 0;
        const char *t = block_decl_type(inst->def, name, &is_fn);
        if (!t || is_fn || !type_name_is_field_ok(t)) return NULL;
        if (!scope_has(&inst->scope, name)) return NULL;
        if (resolve_instance(rt, inst->name) != inst) return NULL;
        if (flag) *flag = type_name_is_value(t);
        return inst->name;
    }

    if (kind == VM_PROBE_FIELD) {
        if (!owner) return NULL;
        BlockInstance *inst = block_inst_find(owner);
        if (!inst) return NULL;
        int is_fn = 0;
        const char *t = block_decl_type(inst->def, name, &is_fn);
        if (!t || is_fn) return NULL;
        if (flag) *flag = type_name_is_value(t);
        return owner;
    }

    /* VM_PROBE_RET — owner NULL means a plain function. */
    if (owner) {
        BlockInstance *inst = block_inst_find(owner);
        BlockDef *def = inst ? inst->def : block_def_find(owner);
        int is_fn = 0;
        const char *t = block_decl_type(def, name, &is_fn);
        if (!t || !is_fn) return NULL;   /* stdlib, FFI, unknown: stay owned */
        if (flag) *flag = ret_type_never_string(t);
        return owner;
    }
    Value fn_val; fn_val.type = VAL_NIL;
    if ((!scope_get(&rt->scope, name, &fn_val) || fn_val.type != VAL_FUNC) &&
        (!scope_table_get(rt->global_table, name, &fn_val) ||
         fn_val.type != VAL_FUNC))
        return NULL;
    ASTNode *fn = fn_val.as.func;
    if (!fn || fn->type != NODE_FUNC_DECL) return NULL;
    if (flag) *flag = ret_type_never_string(fn->as.func_decl.return_type);
    return name;
}

static Value vm_index_callback(void *rt_opaque, int action,
                               VMIndexCache *cache, const char *name,
                               Value *registers, int register_count,
                               int resolved_offset, Value index,
                               Value incoming) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return action == VM_INDEX_PREP ? val_bool(0) : val_nil();
    Value *slot = vm_index_slot(rt, cache, name, registers,
                                register_count, resolved_offset);
    if (!slot || slot->type != VAL_ARR) {
        char buf[280];
        snprintf(buf, sizeof(buf), "'%s' is not an array", name);
        rt_error(rt, buf); cache->failed = 1;
        return action == VM_INDEX_PREP ? val_bool(0) : val_nil();
    }
    if (index.type != VAL_INT) {
        rt_error(rt, "array index must be an integer"); cache->failed = 1;
        return action == VM_INDEX_PREP ? val_bool(0) : val_nil();
    }
    long idx = index.as.integer;
    if (idx < 0 || idx >= slot->as.arr.size) {
        char buf[280];
        snprintf(buf, sizeof(buf), "array index out of bounds: %s[%ld] (size %d)",
                 name, idx, slot->as.arr.size);
        rt_error(rt, buf); cache->failed = 1;
        return action == VM_INDEX_PREP ? val_bool(0) : val_nil();
    }
    if (action == VM_INDEX_PREP) return val_bool(1);
    if (action == VM_INDEX_GET) {
        /* int, float and bool lead the ValType enum and own nothing, so the
         * register can hold one with no retain and no drop.  Compilation is
         * supposed to admit only those, but a chunk cached on a function's AST
         * node is compiled once and reused for every later call, so that gate
         * cannot be the thing safety rests on.  Refuse here rather than let a
         * borrowed string or array reach a register that will not release it. */
        const Value *elem = &slot->as.arr.data[idx];
        if (elem->type > VAL_BOOL) {
            char buf[300];
            snprintf(buf, sizeof(buf),
                     "%s[%ld] holds %s, which the compiled path does not carry",
                     name, idx, val_type_name(elem->type));
            rt_error(rt, buf); cache->failed = 1;
            return val_nil();
        }
        return *elem;
    }

    ValType old_type = slot->as.arr.data[idx].type;
    if (old_type != VAL_NIL && old_type != incoming.type) {
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "type error: %s[%ld] is %s, cannot assign %s", name, idx,
                 val_type_name(old_type), val_type_name(incoming.type));
        rt_error(rt, buf); cache->failed = 1;
        return val_nil();
    }
    /* Same reasoning as the read side, for what the homogeneity check above
     * cannot catch: an element that is still nil accepts any type, and two
     * strings match each other.  Only value types may be stored through this
     * path, so a chunk cached on an AST node can never adopt owned data into
     * an element without the ownership the evaluator would have applied. */
    if (old_type > VAL_BOOL || incoming.type > VAL_BOOL) {
        char buf[300];
        snprintf(buf, sizeof(buf),
                 "%s[%ld]: the compiled path stores only int, float and bool",
                 name, idx);
        rt_error(rt, buf); cache->failed = 1;
        return val_nil();
    }
    slot->as.arr.data[idx] = incoming;
    if (!rt->dry_run && rt->mode == FLUXA_MODE_PROJECT) {
        if (cache->prst_index == -2)
            cache->prst_index = prst_pool_find(RT_POOL(rt), name);
        if (cache->prst_index >= 0) {
            Value *pool = &RT_POOL(rt)->entries[cache->prst_index].value;
            if (pool->type == VAL_ARR && idx < pool->as.arr.size)
                pool->as.arr.data[idx] = incoming;
        }
    }
    return val_nil();
}

static void vm_tick_callback(void *rt_opaque) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return;
    /* GC sweep — only if there's something to collect */
    if (rt->gc.count > 0)
        gc_sweep(&rt->gc, gc_dyn_free_fn);
    /* Mailbox — only in thread clones (current_thread is NULL in main runtime) */
#ifdef FLUXA_STD_FLXTHREAD
    if (rt->current_thread)
        flx_mailbox_drain((FlxThread*)rt->current_thread, rt, rt->current_instance);
#endif
}

/* ── v0.14 Fase 3: method_try_inline ────────────────────────────────────── */
/* Attempt to execute a Block method inline — zero frame save/restore.
 *
 * Inlinable: methods whose body consists ONLY of:
 *   - NODE_MEMBER_ASSIGN where value is a simple expression (binary/literal/ident/member_access)
 *   - NODE_RETURN with a simple expression
 *   - NODE_ASSIGN / NODE_VAR_DECL with simple value
 *
 * "Simple expression": no FUNC_CALL, no MEMBER_CALL, no indexed access, no danger.
 * Params are accessed from args[], fields via scope_get/set on inst->scope directly.
 *
 * Returns 1 + writes *result if inline succeeded.
 * Returns 0 if method is not inlinable — caller falls back to full call_function path.
 *
 * Constraints:
 *   - No scope_new / scope_free — we never build a new scope.
 *   - Params live in a local array indexed by their resolved_offset (0..param_count-1).
 *   - Only MEMBER_ASSIGN and RETURN are acted upon; others cause fallback.
 *   - safe for Block fields: scope_get/set on inst->scope.
 *   - Not recursive — if body calls another fn, fallback.
 */

static int expr_is_inlinable(const ASTNode *e, int param_count) {
    if (!e) return 0;
    switch (e->type) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STRING_LIT:
            return 1;
        case NODE_IDENTIFIER:
            /* Only inline if it's a confirmed param slot.
             * Block fields have resolved_offset that may alias param slots —
             * reject any identifier that isn't in 0..param_count-1. */
            return (e->resolved_offset >= 0 &&
                    e->resolved_offset < param_count &&
                    e->warm_local);
        case NODE_MEMBER_ACCESS:
            return 1;  /* inst.field — direct scope read, safe */
        case NODE_BINARY_EXPR:
            return expr_is_inlinable(e->as.binary.left,  param_count) &&
                   expr_is_inlinable(e->as.binary.right, param_count);
        default:
            return 0;
    }
}

static int stmt_is_inlinable(const ASTNode *s, int param_count) {
    if (!s) return 0;
    switch (s->type) {
        case NODE_MEMBER_ASSIGN:
            return expr_is_inlinable(s->as.member_assign.value, param_count);
        case NODE_RETURN:
            return !s->as.ret.value || expr_is_inlinable(s->as.ret.value, param_count);
        case NODE_ASSIGN:
        case NODE_VAR_DECL: {
            const ASTNode *val = (s->type == NODE_VAR_DECL)
                               ? s->as.var_decl.initializer
                               : s->as.assign.value;
            return s->resolved_offset >= 0 && expr_is_inlinable(val, param_count);
        }
        default:
            return 0;
    }
}

/* Evaluate a simple expression inline — no eval() call, no scope lookup overhead.
 * params: args array indexed by resolved_offset (param 0 = args[0], etc.)
 * inst:   Block instance for MEMBER_ACCESS field reads                       */
/* Ownership contract: the returned Value is OWNED by the caller.  A string
 * literal allocates, and the param and field branches retain, so every branch
 * hands back one reference the caller must release or hand on.  The branches
 * used to disagree — literals allocated while params and fields were borrowed
 * aliases — and every consumer had to know which branch it had reached. */
static Value eval_simple_expr(const ASTNode *e, const Value *params, int param_count,
                               BlockInstance *inst, Runtime *rt) {
    if (!e) return val_nil();
    switch (e->type) {
        case NODE_INT_LIT:    return val_int(e->as.integer.value);
        case NODE_FLOAT_LIT:  return val_float(e->as.real.value);
        case NODE_BOOL_LIT:   return val_bool(e->as.boolean.value);
        case NODE_STRING_LIT: return val_string(e->as.str.value);
        case NODE_IDENTIFIER: {
            int off = e->resolved_offset;
            /* Only params are safe — fields are rejected by expr_is_inlinable */
            if (off >= 0 && off < param_count) {
                Value v = params[off];
                if (v.type == VAL_STRING && v.as.string)
                    v.as.string = fxstr_retain(v.as.string);
                return v;
            }
            return val_nil();
        }
        case NODE_MEMBER_ACCESS: {
            Value v; v.type = VAL_NIL;
            BlockInstance *fi = resolve_instance(rt, e->as.member_access.owner);
            if (fi) scope_get(&fi->scope, e->as.member_access.field, &v);
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = fxstr_retain(v.as.string);
            return v;
        }
        case NODE_BINARY_EXPR: {
            Value l = eval_simple_expr(e->as.binary.left,  params, param_count, inst, rt);
            Value r = eval_simple_expr(e->as.binary.right, params, param_count, inst, rt);
            const char *op = e->as.binary.op;
            /* One exit: the operands are owned by this frame and no result
             * here carries a string, so both are released before returning. */
            Value out = val_nil();
            if (l.type == VAL_INT && r.type == VAL_INT) {
                long lv = l.as.integer, rv = r.as.integer;
                if      (!strcmp(op,"+"))  out = val_int(lv + rv);
                else if (!strcmp(op,"-"))  out = val_int(lv - rv);
                else if (!strcmp(op,"*"))  out = val_int(lv * rv);
                else if (!strcmp(op,"/"))  out = rv ? val_int(lv / rv) : val_int(0);
                else if (!strcmp(op,"%"))  out = rv ? val_int(lv % rv) : val_int(0);
                else if (!strcmp(op,"==")) out = val_bool(lv == rv);
                else if (!strcmp(op,"!=")) out = val_bool(lv != rv);
                else if (!strcmp(op,"<"))  out = val_bool(lv <  rv);
                else if (!strcmp(op,">"))  out = val_bool(lv >  rv);
                else if (!strcmp(op,"<=")) out = val_bool(lv <= rv);
                else if (!strcmp(op,">=")) out = val_bool(lv >= rv);
            }
            if (l.type == VAL_FLOAT || r.type == VAL_FLOAT) {
                double lv = (l.type==VAL_INT) ? (double)l.as.integer : l.as.real;
                double rv = (r.type==VAL_INT) ? (double)r.as.integer : r.as.real;
                if      (!strcmp(op,"+"))  out = val_float(lv + rv);
                else if (!strcmp(op,"-"))  out = val_float(lv - rv);
                else if (!strcmp(op,"*"))  out = val_float(lv * rv);
                else if (!strcmp(op,"/"))  out = rv ? val_float(lv/rv) : val_float(0);
                else if (!strcmp(op,"==")) out = val_bool(lv == rv);
                else if (!strcmp(op,"<"))  out = val_bool(lv <  rv);
                else if (!strcmp(op,">"))  out = val_bool(lv >  rv);
            }
            if (l.type == VAL_STRING && l.as.string) fxstr_release(l.as.string);
            if (r.type == VAL_STRING && r.as.string) fxstr_release(r.as.string);
            return out;
        }
        default:
            return val_nil();
    }
}

static int method_try_inline(Runtime *rt, ASTNode *fn_node,
                              BlockInstance *inst,
                              const Value *args, int argc,
                              Value *result) {
    (void)inst; (void)argc;
    ASTNode *body = fn_node->as.func_decl.body;
    int param_count = fn_node->as.func_decl.param_count;

    /* Verify all statements are inlinable */
    for (int i = 0; i < body->as.list.count; i++) {
        if (!stmt_is_inlinable(body->as.list.children[i], param_count))
            return 0;
    }

    /* All inlinable — execute without frame save/restore */
    *result = val_nil();
    for (int i = 0; i < body->as.list.count; i++) {
        ASTNode *s = body->as.list.children[i];
        switch (s->type) {
            case NODE_MEMBER_ASSIGN: {
                BlockInstance *fi = resolve_instance(rt, s->as.member_assign.owner);
                if (!fi) { return 0; }
                Value v = eval_simple_expr(s->as.member_assign.value,
                                           args, param_count, inst, rt);
                /* Spec §13.6: every producer hands over its own reference and
                 * every store adopts it.  Sharing an immutable refcounted
                 * buffer between two Blocks does not weaken the independence
                 * §7.4 requires — a string cannot be changed in place, so
                 * neither Block can observe the other through it. */
                scope_set_owned(&fi->scope, s->as.member_assign.field, v);
                break;
            }
            case NODE_ASSIGN:
            case NODE_VAR_DECL: {
                /* Local assignment — we have no stack here; only allowed if
                 * the local is a param slot (resolved_offset < param_count).
                 * This handles patterns like: int tmp = a + b  followed by use. */
                /* For safety: fall back if local assignment */
                return 0;
            }
            case NODE_RETURN: {
                if (s->as.ret.value)
                    *result = eval_simple_expr(s->as.ret.value,
                                               args, param_count, inst, rt);
                return 1;
            }
            default:
                return 0;
        }
        if (rt->had_error) return 0;
    }
    return 1;
}

/* ── v0.14: vm_get_field_callback / vm_set_field_callback ───────────────── */
/* OP_GET_FIELD: read a Block instance field into a VM register.
 * Mirrors eval NODE_MEMBER_ACCESS — resolve_instance + scope_get.         */
/* Resolve instance from a mutable constant Value.
 * Cold path (VAL_STRING): call resolve_instance, then patch kv to VAL_PTR.
 * Hot path  (VAL_PTR):    deref directly — zero hash lookup.              */
static inline BlockInstance *resolve_inst_cached(Runtime *rt, Value *owner_kv) {
    if (__builtin_expect(owner_kv->type == VAL_PTR, 1)) {
        /* Hot path: already cached */
        return (BlockInstance *)owner_kv->as.ptr;
    }
    /* Cold path: resolve by name and cache */
    BlockInstance *inst = resolve_instance(rt, owner_kv->as.string);
    if (inst) {
        /* The Chunk owns owner_kv's cached name.  Release that reference
         * before replacing the constant with the resolved instance pointer. */
        fxstr_release(owner_kv->as.string);
        owner_kv->type   = VAL_PTR;
        owner_kv->as.ptr = inst;
    }
    return inst;
}

static Value vm_get_field_callback(void *rt_opaque, Value *owner_kv, const char *field) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return val_nil();
    BlockInstance *inst = resolve_inst_cached(rt, owner_kv);
    if (!inst) {
        char buf[280];
        snprintf(buf, sizeof(buf), "undefined Block instance (OP_GET_FIELD)");
        rt_error(rt, buf); return val_nil();
    }
    Value v; v.type = VAL_NIL;
    scope_get(&inst->scope, field, &v);
    /* scope_get hands back a borrowed alias of the field's own storage, but
     * every VM string register owns one reference and drops it at the end of
     * the statement.  Without a retain here that drop frees the field itself:
     * reading a str field inside a compiled loop was a use-after-free.  Same
     * rule method_try_inline already applies to an inlined `return field`. */
    if (v.type == VAL_STRING && v.as.string)
        v.as.string = fxstr_retain(v.as.string);
    return v;
}

static void vm_set_field_callback(void *rt_opaque, Value *owner_kv, const char *field, Value val) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return;
    BlockInstance *inst = resolve_inst_cached(rt, owner_kv);
    if (!inst) return;
    scope_set(&inst->scope, field, val);
}

/* Interpreter fallback for calls made by bytecode loops.
 *
 * NODE_RETURN represents `return fn(args)` as ret.tco_active so the normal
 * call_function() trampoline can reuse its frame.  The old VM bridges ran a
 * single body and inspected only ret.active; a fallback callee ending in a
 * tail call consequently returned VAL_NIL.  That nil could then be bound to
 * the next method parameter and reported as an apparent scope violation.
 *
 * Preserve the language's TCO and ownership contracts here: save the VM frame
 * once for the whole tail-call chain, skip teardown of the initial borrowed
 * VM parameters, and own/release arguments produced by subsequent tail calls. */
static Value vm_eval_fallback(Runtime *rt, ASTNode *fn_node,
                              BlockInstance *instance,
                              const Value *vm_args, int argc) {
    if (rt->call_depth >= FLUXA_MAX_DEPTH) {
        rt_error(rt, "stack overflow — max call depth reached");
        return val_nil();
    }
    if (argc < 0 || argc > FLUXA_STACK_SIZE) {
        rt_error(rt, "VM call argument count exceeds the variable stack");
        return val_nil();
    }

    Scope          caller_scope = rt->scope;
    int            caller_sz    = rt->stack_size;
    BlockInstance *caller_inst  = rt->current_instance;
    ASTNode       *caller_fn    = rt->current_fn;
    int save_slots = caller_sz < FLUXA_STACK_SIZE
                   ? caller_sz : FLUXA_STACK_SIZE;
    Value caller_stack[FLUXA_STACK_SIZE];
    if (save_slots > 0)
        memcpy(caller_stack, rt->stack,
               sizeof(Value) * (size_t)save_slots);

    Value *pending_args = NULL;
    if (argc > 0) {
        pending_args = (Value *)malloc(sizeof(Value) * (size_t)argc);
        if (!pending_args) {
            rt_error(rt, "out of memory copying VM call arguments");
            return val_nil();
        }
        memcpy(pending_args, vm_args, sizeof(Value) * (size_t)argc);
    }

    rt->scope            = scope_new();
    rt->stack_size       = 0;
    rt->current_instance = instance;
    rt->call_depth++;

    int params_borrowed = 1;
    int param_count = argc;
    Value result = val_nil();

    while (1) {
        int declared = fn_node->as.func_decl.param_count;
        if (declared < 0 || declared > FLUXA_STACK_SIZE) {
            rt_error(rt, "tail-call parameter count exceeds the variable stack");
            free(pending_args);
            pending_args = NULL;
            break;
        }
        if (param_count != declared) {
            char buf[280];
            snprintf(buf, sizeof(buf),
                     "function '%s' expects %d argument(s), got %d (tail call)",
                     fn_node->as.func_decl.name, declared, param_count);
            rt_error(rt, buf);
            free(pending_args);
            pending_args = NULL;
            break;
        }

        int zero_slots = param_count + 64;
        if (zero_slots < caller_sz + 1) zero_slots = caller_sz + 1;
        if (zero_slots > FLUXA_STACK_SIZE) zero_slots = FLUXA_STACK_SIZE;
        for (int i = 0; i < zero_slots; i++) rt->stack[i].type = VAL_NIL;
        rt->stack_size = 0;
        for (int i = 0; i < param_count; i++) {
            rt->stack[i] = pending_args[i];
            rt->stack_size = i + 1;
        }
        free(pending_args);
        pending_args = NULL;

        rt->current_fn = fn_node;
        rt->current_wf = rt->warm.enabled
                       ? warm_profile_get_func(&rt->warm, (uintptr_t)fn_node)
                       : NULL;
        Value self; self.type = VAL_FUNC; self.as.func = fn_node;
        scope_set(&rt->scope, fn_node->as.func_decl.name, self);
        rt->ret.active        = 0;
        rt->ret.tco_active    = 0;
        rt->ret.tco_fn        = NULL;
        rt->ret.tco_args      = NULL;
        rt->ret.tco_arg_count = 0;
        rt->ret.value         = val_nil();

        ASTNode *body = fn_node->as.func_decl.body;
        for (int i = 0; i < body->as.list.count; i++) {
            eval(rt, body->as.list.children[i]);
            if (rt->had_error || rt->ret.active || rt->ret.tco_active) break;
        }
        if (rt->warm.enabled && rt->current_wf)
            warm_update_sig(rt->current_wf);
        if (rt->had_error) break;

        if (rt->ret.tco_active) {
            ASTNode *next_fn = rt->ret.tco_fn;
            Value *next_args = rt->ret.tco_args;
            int next_argc = rt->ret.tco_arg_count;

            frame_release_slots_from(rt,
                                     params_borrowed ? param_count : 0,
                                     NULL);
            scope_free(&rt->scope);
            rt->scope = scope_new();
            rt->stack_size = 0;
            rt->ret.tco_active = 0;
            rt->ret.tco_fn = NULL;
            rt->ret.tco_args = NULL;
            rt->ret.tco_arg_count = 0;

            fn_node         = next_fn;
            pending_args    = next_args;
            param_count     = next_argc;
            params_borrowed = 0;
            continue;
        }

        result = rt->ret.active ? rt->ret.value : val_nil();
        rt->ret.active = 0;
        break;
    }

    free(pending_args);
    frame_release_slots_from(rt, params_borrowed ? param_count : 0, &result);
    scope_free(&rt->scope);
    rt->scope            = caller_scope;
    rt->stack_size       = caller_sz;
    rt->current_instance = caller_inst;
    rt->current_fn       = caller_fn;
    rt->current_wf       = (rt->warm.enabled && caller_fn)
                           ? warm_profile_get_func(&rt->warm,
                                                   (uintptr_t)caller_fn)
                           : NULL;
    if (save_slots > 0)
        memcpy(rt->stack, caller_stack,
               sizeof(Value) * (size_t)save_slots);
    rt->call_depth--;
    return result;
}

/* ── v0.14: vm_call_callback — bridges OP_CALL_METHOD / OP_CALL_FUNC ────── */
/* Called from vm_run when the bytecode VM encounters a call opcode.
 * owner: NULL → plain function call; non-NULL → Block method call.
 * args: pointer into R[] of the VM — valid only during this call.
 *
 * For Block method calls:
 *   1. Check stdlib registry (same logic as NODE_MEMBER_CALL in eval)
 *   2. resolve_instance(owner) → inst
 *   3. scope_get(inst->scope, method) → fn_val
 *   4. call_function(fn_val, pre-evaluated args[], inst)
 *
 * For plain function calls:
 *   1. Lookup fn in rt->scope or global_table
 *   2. call_function with no instance
 *
 * Ownership: args[] are read-only values from R[]. We copy them into a
 * local Value array for call_function (which takes ASTNode** args normally).
 * We use a pre-evaluated variant path to avoid re-evaluating. */
static Value vm_call_callback(void *rt_opaque, Value *owner_kv,
                               const char *method_or_func,
                               Value *args, int argc) {
    Runtime *rt = (Runtime *)rt_opaque;
    if (!rt) return val_nil();

    if (owner_kv) {
        /* ── Block method call or stdlib/FFI ──────────────────────────── */
        /* For stdlib: always resolve by name (VAL_STRING) since stdlib
         * owners are not BlockInstances and must not be cached as VAL_PTR */
        const char *owner_str = (owner_kv->type == VAL_STRING)
                                ? owner_kv->as.string : NULL;

        /* 1. Stdlib dispatch — only when we have a string owner name.
         * Capture the result so we can register VAL_DYN returns with the GC
         * (same as NODE_MEMBER_CALL in eval). Without this, a dyn returned
         * by a lib call from bytecode-VM context would live outside the GC
         * and leak on slot reassignment. */
        if (owner_str) for (int _ri = 0; _ri < FLUXA_LIB_COUNT; _ri++) {
            const FluxaLibEntry *_e = &fluxa_lib_registry[_ri];
            if (!_e->enabled) continue;
            if (!fluxa_std_lib_enabled(&rt->config.std_libs, _e->name)) continue;
            if (strcmp(owner_str, _e->owner) != 0) continue;
            Value _ret;
            int _err_before = rt->err_stack.count;
            if (_e->cfg_aware && _e->call_cfg)
                _ret = _e->call_cfg(method_or_func, args, argc,
                                    &rt->err_stack, &rt->had_error,
                                    rt->current_line, &rt->config);
            else if (_e->rt_aware && _e->call_rt)
                _ret = _e->call_rt(method_or_func, args, argc,
                                   &rt->err_stack, &rt->had_error,
                                   rt->current_line, rt);
            else if (_e->call)
                _ret = _e->call(method_or_func, args, argc,
                                &rt->err_stack, &rt->had_error,
                                rt->current_line);
            else continue;
            rt_surface_lib_errors(rt, _err_before);
            if (_ret.type == VAL_DYN && _ret.as.dyn) {
                GCEntry *_gce = gc_find_slot(&rt->gc, _ret.as.dyn);
                if (!_gce) {
                    gc_register(&rt->gc, _ret.as.dyn,
                        sizeof(FluxaDyn) + sizeof(Value) * (size_t)_ret.as.dyn->cap,
                        &rt->err_stack);
                }
            }
            return _ret;
        }

        /* 2. Block instance method — use inline cache via resolve_inst_cached */
        BlockInstance *inst = resolve_inst_cached(rt, owner_kv);
        if (!inst) {
            const char *oname = (owner_kv->type==VAL_STRING) ? owner_kv->as.string : "(cached)";
            char buf[280];
            snprintf(buf, sizeof(buf), "undefined Block instance: %s", oname);
            rt_error(rt, buf); return val_nil();
        }
        Value fn_val; fn_val.type = VAL_NIL;
        if (!scope_get(&inst->scope, method_or_func, &fn_val) ||
            fn_val.type != VAL_FUNC) {
            char buf[280];
            char public_method[256];
            if (!block_method_public(method_or_func, public_method,
                                     sizeof(public_method)))
                snprintf(public_method, sizeof(public_method), "%s", method_or_func);
            snprintf(buf, sizeof(buf), "block has no method '%.220s'",
                     public_method);
            rt_error(rt, buf); return val_nil();
        }
        ASTNode *fn_node = fn_val.as.func;
        int param_count  = fn_node->as.func_decl.param_count;
        if (argc != param_count) {
            char buf[280];
            char public_method[256];
            if (!block_method_public(method_or_func, public_method,
                                     sizeof(public_method)))
                snprintf(public_method, sizeof(public_method), "%s", method_or_func);
            snprintf(buf, sizeof(buf),
                "Block method '%.220s' expects %d arg(s), got %d",
                public_method, param_count, argc);
            rt_error(rt, buf); return val_nil();
        }
        /* ── Fase 3: try inline execution first (zero frame overhead) ─── */
        {
            Value inline_result;
            if (method_try_inline(rt, fn_node, inst, args, argc, &inline_result)) {
                /* eval_simple_expr already hands back an owned reference —
                 * retaining again added one nobody dropped, leaking a string
                 * per call for every method that inlines a `return`. */
                return inline_result;
            }
        }
        return vm_eval_fallback(rt, fn_node, inst, args, argc);
    }

    /* ── Plain function call ────────────────────────────────────────────── */
    /* Builtins first */
    if (builtin_is(method_or_func))
        return builtin_dispatch_values(rt, method_or_func, args, argc);

    Value fn_val; fn_val.type = VAL_NIL;
    /* 1. Current scope (local fns registered in this frame) */
    if (!scope_get(&rt->scope, method_or_func, &fn_val) || fn_val.type != VAL_FUNC) {
        /* 2. Instance scope — handles intra-Block calls like iterate() from run() */
        if (rt->current_instance) {
            char key[256];
            if (block_method_key(method_or_func, key, sizeof(key)))
                scope_get(&rt->current_instance->scope, key, &fn_val);
        }
    }
    /* 3. Global table — top-level functions */
    if (fn_val.type != VAL_FUNC)
        scope_table_get(rt->global_table, method_or_func, &fn_val);
    if (fn_val.type != VAL_FUNC) {
        char buf[280];
        snprintf(buf, sizeof(buf), "undefined function '%s' (OP_CALL_FUNC)",
                 method_or_func);
        rt_error(rt, buf); return val_nil();
    }
    ASTNode *fn_node    = fn_val.as.func;
    int      param_count = fn_node->as.func_decl.param_count;
    if (argc != param_count) {
        char buf[280];
        snprintf(buf, sizeof(buf), "function '%s' expects %d arg(s), got %d",
                 method_or_func, param_count, argc);
        rt_error(rt, buf); return val_nil();
    }

    /* ── Fase 2 fast path: use compiled fn chunk if available ─────────── */
    /* Only compile pure functions (not Block methods — they access inst->scope
     * via field names which can't be resolved to stack offsets in fn body). */
    if (!fn_node->fn_chunk) {
        Chunk *ch = (Chunk *)malloc(sizeof(Chunk));
        /* Parameters are not bound until fn_regs exists. Avoid inferring an
         * array parameter from an unrelated caller/global slot of same name;
         * its nested while can compile after the real binding exists. */
        if (ch && chunk_compile_fn(ch, fn_node, vm_indexable_callback,
                                  vm_probe_callback, rt)) {
            fn_node->fn_chunk = ch;  /* cache on AST node — never freed */
        } else {
            free(ch);
            fn_node->fn_chunk = (void *)(uintptr_t)1;  /* sentinel: failed, don't retry */
        }
    }
    if (fn_node->fn_chunk && fn_node->fn_chunk != (void *)(uintptr_t)1) {
        Chunk *ch = (Chunk *)fn_node->fn_chunk;
        /* fn_regs must cover all registers the chunk uses: 0..chunk.next_reg-1.
         * Add 32 slots safety margin in case of edge cases in next_reg tracking. */
        int reg_count = (int)ch->next_reg + 32;
        if (reg_count < param_count + 32) reg_count = param_count + 32;
        Value *fn_regs = (Value *)calloc((size_t)reg_count, sizeof(Value));
        if (!fn_regs) goto fn_fallback;
        for (int _i = 0; _i < param_count; _i++) fn_regs[_i] = args[_i];
        Value result = vm_run_fn(ch, fn_regs, reg_count,
                                 vm_call_callback, vm_get_field_callback,
                                 vm_set_field_callback,
                                 vm_index_callback, rt);
        /* VM string registers own one reference each.  Preserve one owned
         * reference for the returned Value, then release locals/temporaries;
         * parameter registers remain borrowed from the caller. */
        if (result.type == VAL_STRING && result.as.string)
            result.as.string = fxstr_retain(result.as.string);
        for (int _i = param_count; _i < reg_count; _i++) {
            if (fn_regs[_i].type == VAL_STRING && fn_regs[_i].as.string)
                fxstr_release(fn_regs[_i].as.string);
        }
        free(fn_regs);
        return result;
    }
    fn_fallback:
    return vm_eval_fallback(rt, fn_node, rt->current_instance, args, argc);
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
    if (node->line > 0) { rt->current_line = node->line;
                          rt->current_src  = node->src_id; }

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
            /* Owned read: retain the buffer (O(1)). The caller holds its own
             * reference — `str y = x` shares the immutable buffer, and a
             * later `x = ...` releases x's ref without touching y's. */
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = fxstr_retain(v.as.string);
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
                    if (prst_pool_has(RT_POOL(rt), vname)) {
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
                        prst_pool_get(RT_POOL(rt), vname, &pooled);

                        /* A resource handle that came back from a restart
                         * snapshot is a headstone, not a handle: the wire format
                         * carries no VAL_DYN (a pointer cannot survive execve),
                         * so prst_deser_value leaves VAL_NIL behind while
                         * declared_type still says VAL_DYN. Restoring that nil
                         * would leave the program with a window that never
                         * opened. Two different situations, two answers:
                         *
                         *   in-process reload  → the pointer is still valid,
                         *                        the pool wins, same window
                         *   runtime swap       → the pointer is gone, the
                         *                        resource must be born again
                         *
                         * Measurements, counters and any serializable value
                         * survive both. Only the external resource is rebuilt. */
                        int entry_idx0 = prst_pool_find(RT_POOL(rt), vname);
                        int pooled_is_headstone =
                            (entry_idx0 >= 0 && pooled.type == VAL_NIL &&
                             RT_POOL(rt)->entries[entry_idx0].declared_type != VAL_NIL);

                        /* Replay the initializer only when doing so is free of
                         * side effects — see prst_init_is_replayable — or when
                         * the pool has nothing usable left to restore. */
                        int replayable =
                            prst_init_is_replayable(node->as.var_decl.initializer);
                        Value src_init = val_nil();
                        if (replayable || pooled_is_headstone) {
                            src_init = eval(rt, node->as.var_decl.initializer);
                            if (rt->had_error) return val_nil();
                        }
                        if (pooled_is_headstone) {
                            /* Rebuilt from the declaration. Re-register the new
                             * handle so the pool owns a live value again. */
                            if (!rt_type_check(rt, node,
                                    node->as.var_decl.type_name, src_init, vname))
                                return val_nil();
                            RT_POOL(rt)->entries[entry_idx0].declared_type = src_init.type;
                            prst_pool_set(RT_POOL(rt), vname, src_init, &rt->err_stack);
                            prst_pool_set_offset(RT_POOL(rt), vname,
                                                 node->resolved_offset);
                            rt_set(rt, node, vname, src_init);
                            scope_table_set(&rt->global_table, vname, src_init);
                            return val_nil();
                        }

                        /* Compare new source initializer against init_value
                         * (the declared value at first run or at migration time).
                         * If they differ → user edited the source → use source.
                         * If they match → runtime mutated the value → keep pooled. */
                        int entry_idx = prst_pool_find(RT_POOL(rt), vname);
                        Value ref = (entry_idx >= 0)
                            ? RT_POOL(rt)->entries[entry_idx].init_value
                            : pooled;

                        int src_changed = 0;
                        if (!replayable) {
                            src_changed = 0;   /* pooled value wins, always */
                        } else if (src_init.type != ref.type) {
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
                        /* prst_pool_get returns the pool's RAW pointer. The
                         * stack slot must own independent heap data — frame
                         * teardown releases slot strings on every return.
                         * Strings: copy. Arrays: mark the slot's view as
                         * borrowed so teardown/rt_set never free pool data. */
                        if (!src_changed) {
                            if (chosen.type == VAL_STRING && chosen.as.string)
                                chosen.as.string = fxstr_new(chosen.as.string); /* pool is plain malloc */
                            else if (chosen.type == VAL_ARR)
                                fluxa_arr_set_owned(&chosen.as.arr, 0);
                            else if (chosen.type == VAL_DYN && chosen.as.dyn &&
                                     !gc_find_slot(&rt->gc, chosen.as.dyn)) {
                                /* The wrapper crossed the reload boundary owned
                                 * by the pool. Hand it back to this runtime's GC
                                 * so telemetry sees it and rt_set can pin it;
                                 * teardown detaches it again for the next run. */
                                gc_register(&rt->gc, chosen.as.dyn,
                                    sizeof(FluxaDyn) +
                                    sizeof(Value) * (size_t)chosen.as.dyn->cap,
                                    &rt->err_stack);
                            }
                        }

                        /* Always refresh offset: resolver may assign a different
                         * slot on a new parse of the same file. */
                        prst_pool_set(RT_POOL(rt), vname, chosen, &rt->err_stack);
                        prst_pool_set_offset(RT_POOL(rt), vname,
                                             node->resolved_offset);
                        /* If the source declaration changed, update init_value
                         * so future reloads use the new declaration as baseline. */
                        if (src_changed && entry_idx >= 0) {
                            Value *iv = &RT_POOL(rt)->entries[entry_idx].init_value;
                            if (iv->type == VAL_STRING && iv->as.string)
                                free(iv->as.string); /* POOL-INTERNAL storage: plain malloc — never fxstr_release */
                            *iv = src_init;
                            if (iv->type == VAL_STRING && iv->as.string)
                                iv->as.string = strdup(iv->as.string); /* pool-internal storage: plain malloc, freed by the pool */
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
                    int ok = prst_pool_set(RT_POOL(rt), vname, v, &rt->err_stack);
                    if (ok >= 0) {
                        rt_set(rt, node, vname, v);
                        /* GC pin is handled by rt_set: a VAL_DYN landing in
                         * a stack slot is pinned automatically. Adding a
                         * second rt_gc_pin here would create a leak (pin
                         * count stays >0 forever on this slot). */
                        /* Record stack offset so post-VM sync can read rt->stack */
                        prst_pool_set_offset(RT_POOL(rt), vname,
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
            /* GC pin is handled by rt_set: a VAL_DYN landing in a stack slot
             * is pinned automatically (symmetric with the unpin of the old
             * slot value). Adding a second rt_gc_pin here would leak. */
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
                /* Bug K fix: adopt the evaluated value. scope_set here made a
                 * strdup copy and abandoned the original — one leaked string
                 * per str-field assignment in Block methods reached via this
                 * short-circuit (module singletons among them). */
                scope_set_owned(&rt->current_instance->scope,
                                node->as.assign.var_name, v);
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
             * pollute the pool that will be transferred to the live runtime.
             * flxthread: if ft.lock("var") was called, acquire the named
             * mutex before writing to the shared pool and release after. */
            if (!rt->dry_run &&
                rt->mode == FLUXA_MODE_PROJECT &&
                prst_pool_has(RT_POOL(rt), node->as.assign.var_name)) {
#ifdef FLUXA_STD_FLXTHREAD
                int _locked = rt->shared_prst_pool
                              ? flx_lock_acquire(node->as.assign.var_name)
                              : 0;
#endif
                prst_pool_set(RT_POOL(rt), node->as.assign.var_name,
                              v, &rt->err_stack);
                scope_table_set(&rt->global_table, node->as.assign.var_name, v);
#ifdef FLUXA_STD_FLXTHREAD
                if (_locked) flx_lock_release(node->as.assign.var_name);
#endif
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
                if (!found && rt->current_instance) {
                    char key[256];
                    if (block_method_key(fn_name, key, sizeof(key)))
                        found = scope_get(&rt->current_instance->scope, key, &fn_val);
                }
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
            if (!found && rt->current_instance) {
                char key[256];
                if (block_method_key(name, key, sizeof(key)))
                    found = scope_get(&rt->current_instance->scope, key, &fn_val);
            }
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
                char key[256];
                if (block_method_key(name, key, sizeof(key)) &&
                    scope_get(&rt->current_instance->scope, key, &check) &&
                    check.type == VAL_FUNC)
                    call_inst = rt->current_instance;
            }
            return call_function(rt, fn_val.as.func,
                                 node->as.list.children, node->as.list.count,
                                 call_inst);
        }

        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++) {
                ASTNode *_ch = node->as.list.children[i];
                Value _sv = eval(rt, _ch);
                /* Statement-expression: the Value is discarded. If it holds
                 * heap data that this eval newly produced (lib call result
                 * or function call result returning a VAL_STRING/VAL_DYN),
                 * release it now — otherwise it leaks once per iteration
                 * (e.g. `json2.get(doc, "name")` called for its side-effect). */
                int _own = (_ch->type == NODE_MEMBER_CALL) ||
                           (_ch->type == NODE_FUNC_CALL)   ||
                           (_ch->type == NODE_FFI_CALL)    ||
                           (_ch->type == NODE_IDENTIFIER)  ||  /* owned reads */
                           (_ch->type == NODE_ARR_ACCESS)  ||
                           (_ch->type == NODE_MEMBER_ACCESS);
                if (_own) {
                    if (_sv.type == VAL_STRING && _sv.as.string) {
                        fxstr_release(_sv.as.string);
                    } else if (_sv.type == VAL_DYN && _sv.as.dyn) {
                        /* For dyn returns, GC owns it now (registered by the
                         * lib dispatch). Unregister and free immediately
                         * since no slot references it. */
                        GCEntry *_e = gc_find_slot(&rt->gc, _sv.as.dyn);
                        if (_e && _e->pin_count == 0) {
                            gc_unregister(&rt->gc, _sv.as.dyn);
                            fluxa_dyn_free(_sv.as.dyn);
                        }
                    }
                }
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
            if (chunk_compile_loop(&chunk, node,
                                   vm_indexable_callback,
                                   vm_probe_callback, rt)) {
                /* Thread clone: refresh stack from shared pool before VM so
                 * the loop starts with the latest prst values from any thread. */
                if (rt->shared_prst_pool && rt->mode == FLUXA_MODE_PROJECT) {
                    PrstPool *pool = rt->shared_prst_pool;
                    for (int _pi = 0; _pi < pool->count; _pi++) {
                        PrstEntry *_pe = &pool->entries[_pi];
                        int _off = _pe->stack_offset;
                        if (_off >= 0 && _off < FLUXA_STACK_SIZE) {
                            rt->stack[_off] = _pe->value;
                            if (_off >= rt->stack_size)
                                rt->stack_size = _off + 1;
                        }
                    }
                }
                /* Tell nested calls (via vm_call_callback) how many stack
                 * slots the VM uses — so call_function saves/restores correctly.
                 * chunk.next_reg is the highest register index + 1 used. */
                /* The frame a nested call saves and restores is bounded by
                 * rt->stack_size.  Temporaries are covered by next_reg, but
                 * locals the VM writes straight into stack[resolved_offset]
                 * never raise it — only the evaluator's rt_set does — so a
                 * loop body with more locals than the temporary watermark had
                 * its caller slots zeroed by the callee and never restored. */
                if (rt->stack_size < (int)chunk.next_reg)
                    rt->stack_size = (int)chunk.next_reg;
                if (rt->stack_size < (int)chunk.max_slot)
                    rt->stack_size = (int)chunk.max_slot;
                /* Temporary registers are reused across independently
                 * compiled chunks.  Drop stale string ownership once here;
                 * each instruction then sees a stable destination type on
                 * loop back-edges, keeping arithmetic opcodes branch-free. */
                for (int _ri = 128; _ri < (int)chunk.next_reg; _ri++) {
                    if (rt->stack[_ri].type == VAL_STRING &&
                        rt->stack[_ri].as.string)
                        fxstr_release(rt->stack[_ri].as.string);
                    rt->stack[_ri] = val_nil();
                }
                /* Pass tick_cb only when needed — avoids function call overhead
                 * at every back-edge for pure loops with no GC/mailbox work. */
                vm_tick_cb_t tick = (rt->gc.count > 0 || rt->current_thread)
                                    ? vm_tick_callback : NULL;
                vm_run(&chunk, &rt->scope, rt->stack, rt->stack_size,
                       rt->cancel_flag, vm_call_callback, rt, tick,
                       vm_get_field_callback, vm_set_field_callback,
                       vm_store_callback, vm_index_callback);
                if (chunk.did_return) {
                    rt->ret.active = 1;
                    rt->ret.value = chunk.return_value;
                }
                chunk_free(&chunk);
                /* Sprint 7: after VM, sync prst vars from rt->stack back to
                 * pool and global_table. The VM writes rt->stack[offset]
                 * directly — the PrstEntry stores the offset set at decl time. */
                if (rt->mode == FLUXA_MODE_PROJECT) {
                    for (int pi = 0; pi < RT_POOL(rt)->count; pi++) {
                        PrstEntry *pe = &RT_POOL(rt)->entries[pi];
                        int off = pe->stack_offset;
                        if (off >= 0 && off < rt->stack_size) {
                            Value sv = rt->stack[off];
                            if (sv.type == pe->declared_type) {
#ifdef FLUXA_STD_FLXTHREAD
                                int _lk = rt->shared_prst_pool
                                          ? flx_lock_acquire(pe->name) : 0;
#endif
                                prst_pool_set(RT_POOL(rt), pe->name,
                                              sv, &rt->err_stack);
                                scope_table_set(&rt->global_table, pe->name, sv);
#ifdef FLUXA_STD_FLXTHREAD
                                if (_lk) flx_lock_release(pe->name);
#endif
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
                            data[i].as.string = fxstr_retain(data[i].as.string);
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
                    ASTNode *_en = node->as.arr_decl.elements[i];
                    data[i] = eval(rt, _en);
                    if (rt->had_error) { free(data); return val_nil(); }
                    /* Ownership: VAL_STRING elements need an independent
                     * copy so freeing the array doesn't double-free a
                     * shared pointer. Skip the strdup when the source node
                     * already produced an owned copy (literals, calls,
                     * identifier reads — NODE_IDENTIFIER now strdups its
                     * VAL_STRING result). Aliased reads (member/array
                     * access) still need the defensive strdup. */
                    if (data[i].type == VAL_STRING && data[i].as.string) {
                        int _src_owned = (_en->type == NODE_STRING_LIT)  ||
                                         (_en->type == NODE_MEMBER_CALL) ||
                                         (_en->type == NODE_FUNC_CALL)   ||
                                         (_en->type == NODE_FFI_CALL)    ||
                                         (_en->type == NODE_IDENTIFIER) ||
                                         (_en->type == NODE_ARR_ACCESS) ||
                                         (_en->type == NODE_MEMBER_ACCESS);
                        if (!_src_owned)
                            data[i].as.string = fxstr_retain(data[i].as.string);
                    }
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
                    int pi = prst_pool_find(RT_POOL(rt),
                                            node->as.arr_decl.arr_name);
                    if (pi < 0) {
                        /* First run — register with init_value = declared arr */
                        prst_pool_set(RT_POOL(rt),
                                      node->as.arr_decl.arr_name,
                                      arr, &rt->err_stack);
                        pi = prst_pool_find(RT_POOL(rt),
                                            node->as.arr_decl.arr_name);
                        if (pi >= 0) {
                            RT_POOL(rt)->entries[pi].declared_type = VAL_ARR;
                            /* prst_pool_set deep-cloned arr into both .value and .init_value.
                             * Assigning arr directly here would alias the scope buffer
                             * causing a double free. The clone already in place is correct. */
                        }
                    } else {
                        /* Reload — deep copy pooled arr into scope so mutations
                         * to scope don't alias the pool's copy */
                        Value pooled = RT_POOL(rt)->entries[pi].value;
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
                                            fxstr_new(dcopy[_pi].as.string); /* pool is plain malloc */
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
                {
                    /* Ownership unification: element reads hand the caller
                     * an owned copy — a second name never aliases the
                     * element's buffer. */
                    Value _ev = d->items[idx];
                    if (_ev.type == VAL_STRING && _ev.as.string)
                        _ev.as.string = fxstr_retain(_ev.as.string);
                    return _ev;
                }
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
            {
                Value _ev = arr_val.as.arr.data[idx];
                if (_ev.type == VAL_STRING && _ev.as.string)
                    _ev.as.string = fxstr_retain(_ev.as.string);   /* owned copy */
                return _ev;
            }
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
                        /* Clear through idx, not up to it. The slot at idx is
                         * about to be released before being overwritten, and
                         * realloc hands back memory holding whatever was there
                         * before — so leaving it uninitialised means releasing
                         * a Value read out of stale bytes. When those bytes
                         * happen to look like VAL_STRING or VAL_ARR, that is a
                         * free() of an arbitrary pointer: a crash inside
                         * value_release_data with no error, at a point that
                         * moves with whatever the allocator did last. */
                        for (long fi = d->count; fi <= idx; fi++) d->items[fi] = val_nil();
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
                                    nd[j].as.string = fxstr_retain(nd[j].as.string);
                            }
                            v = val_arr(nd, v.as.arr.size);
                        }
                    } else if (v.type == VAL_BLOCK_INST && v.as.block_inst) {
                        BlockInstance *clone = block_inst_clone(v.as.block_inst);
                        if (!clone) { rt_error(rt, "out of memory cloning Block into dyn"); return val_nil(); }
                        v.as.block_inst = clone;
                    }
                    /* VAL_STRING: adopt — producers hand an owned copy. */
                    /* Free old value's heap resources before overwriting */
                    value_release_data(&d->items[idx]);
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
                    if (v.type == VAL_STRING && v.as.string)
                        fxstr_release(v.as.string);   /* owned — release on error */
                    return val_nil();
                }
            }
            /* free old string if needed */
            if (target_arr->data[idx].type == VAL_STRING &&
                target_arr->data[idx].as.string)
                fxstr_release(target_arr->data[idx].as.string);
            /* Adopt: every producer hands stores an owned VAL_STRING copy,
             * so the slot becomes the sole owner — a strdup here would leak
             * the producer's copy on every element write. */
            target_arr->data[idx] = v;

            /* Sync mutated arr back to prst pool if it's a prst variable */
            if (!rt->dry_run && rt->mode == FLUXA_MODE_PROJECT) {
                int pi = prst_pool_find(RT_POOL(rt), arr_name);
                if (pi >= 0) {
                    /* The pool entry holds its own copy — update element in place */
                    Value *pool_val = &RT_POOL(rt)->entries[pi].value;
                    if (pool_val->type == VAL_ARR && pool_val->as.arr.data &&
                        idx < pool_val->as.arr.size) {
                        if (pool_val->as.arr.data[idx].type == VAL_STRING &&
                            pool_val->as.arr.data[idx].as.string)
                            free(pool_val->as.arr.data[idx].as.string); /* POOL-INTERNAL: plain malloc */
                        Value pv = v;
                        if (pv.type == VAL_STRING && pv.as.string)
                            pv.as.string = fxstr_new(pv.as.string);
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
                                    fxstr_retain(new_data[j].as.string);
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
                    elem.as.string = fxstr_new(elem.as.string);
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
                /* Fill through idx inclusive — see the note on the other
                 * auto-grow path above. The slot being assigned is released
                 * before it is overwritten, so it must hold a real Value and
                 * not whatever realloc left in the block. */
                for (long fi = d->count; fi <= idx; fi++)
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
                            nd[j].as.string = fxstr_retain(nd[j].as.string);
                    }
                    v = val_arr(nd, v.as.arr.size);
                }
            } else if (v.type == VAL_BLOCK_INST && v.as.block_inst) {
                /* typeof-implicit clone */
                BlockInstance *clone = block_inst_clone(v.as.block_inst);
                if (!clone) { rt_error(rt, "out of memory cloning Block into dyn"); return val_nil(); }
                v.as.block_inst = clone;
            } else if (v.type == VAL_STRING && v.as.string) {
                v.as.string = fxstr_retain(v.as.string);
            }

            /* Free old value's heap resources before overwriting */
            value_release_data(&d->items[idx]);
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
            char method_key[256];
            if (!block_method_key(method, method_key, sizeof(method_key)) ||
                !scope_get(&inst->scope, method_key, &fn_val) ||
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
                int pi = prst_pool_find(RT_POOL(rt), arr_name);
                if (pi >= 0) { arr_val = RT_POOL(rt)->entries[pi].value; found = 1; }
            }
            if (!found) {
                char buf[280];
                snprintf(buf, sizeof(buf), "for: undefined variable '%s'", arr_name);
                rt_error(rt, buf); return val_nil();
            }

            if (arr_val.type == VAL_ARR) {
                /* for x in arr — copy strings so the loop var owns its
                 * own buffer (slot reassignment in rt_set frees the old
                 * string; if x aliased arr.data[i].as.string we'd corrupt
                 * the source array). */
                for (int i = 0; i < arr_val.as.arr.size; i++) {
                    Value _el = arr_val.as.arr.data[i];
                    if (_el.type == VAL_STRING && _el.as.string)
                        _el.as.string = fxstr_new(_el.as.string);
                    rt_set(rt, node, node->as.for_stmt.var_name, _el);
                    eval(rt, node->as.for_stmt.body);
                    if (rt->had_error || rt->ret.active) break;
                }
            } else if (arr_val.type == VAL_DYN && arr_val.as.dyn) {
                /* for x in dyn — same ownership rule as VAL_ARR above. */
                FluxaDyn *d = arr_val.as.dyn;
                for (int i = 0; i < d->count; i++) {
                    Value _el = d->items[i];
                    if (_el.type == VAL_STRING && _el.as.string)
                        _el.as.string = fxstr_new(_el.as.string);
                    rt_set(rt, node, node->as.for_stmt.var_name, _el);
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
                prst_pool_find(RT_POOL(rt), var_name) >= 0) {
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
            value_release_data(target);
            *target = val_nil();
            return val_nil();
        }

        /* ── Sprint 6.b: import c and FFI calls ────────────────────────── */
        /* ── import std <lib> ────────────────────────────────────────────── */
        case NODE_IMPORT_STD: {
            const char *lib = node->as.import_std.lib_name;
            /* Check that the lib was declared in [libs] of fluxa.toml.
             * Uses the registry (lib_registry_gen.h) to validate names.
             * No hardcoded lib names here — new libs register automatically. */
            int declared = fluxa_std_lib_enabled(&rt->config.std_libs, lib);
            /* "ft" is the flxthread alias */
            if (!declared && strcmp(lib, "ft") == 0)
                declared = fluxa_std_lib_enabled(&rt->config.std_libs, "flxthread");
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
                                          &rt->err_stack, ctx,
                                          rt->config.ffi_str_buf_size);
            /* Sprint 9.c-3: write back pointer args into Fluxa vars */
            /* Sprint 9.c-3: write back pointer args into Fluxa vars.
             * FPARAM_ARR excluded: bytes are scattered in-place into
             * arr->data[j] inside ffi.c — no rt_set needed. */
            if (sig && !rt->had_error) {
                for (int i = 0; i < arg_count && i < sig->param_count; i++) {
                    FParamKind k = sig->param_kinds[i];
                    if (k != FPARAM_PTR_INT  &&
                        k != FPARAM_PTR_FLT  &&
                        k != FPARAM_PTR_BOOL &&
                        k != FPARAM_STR) continue;
                    ASTNode *anode = node->as.ffi_call.args[i];
                    if (!anode || anode->type != NODE_IDENTIFIER) continue;
                    rt_set(rt, anode, anode->as.str.value, args[i]);
                }
            }
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
            if (v.type == VAL_STRING && v.as.string)
                v.as.string = fxstr_retain(v.as.string);   /* owned copy */
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
            scope_set_owned(&inst->scope, field, v);   /* adopt owned value */
            return val_nil();
        }

        case NODE_MEMBER_CALL: {
            const char *owner  = node->as.member_call.owner;
            const char *method = node->as.member_call.method;

            /* ── std library dispatch — driven by lib_registry_gen.h ────────── *
             * No lib names hardcoded here. New libs register via              *
             * FLUXA_LIB_EXPORT in their header + lib.mk. Zero changes here.  */
            {
                /* Evaluate arguments once (shared by all libs).
                 *
                 * Argument ownership rules — needed to release transient
                 * heap allocations after the lib call returns. Without this
                 * release, every literal/expression VAL_STRING passed to a
                 * std lib leaks until the program ends (a high-volume HTTP
                 * server with 5-15 literals per request leaks tens of MB
                 * per minute). Conservative classification:
                 *   - NODE_STRING_LIT      — we strdup'd it via val_string()
                 *   - NODE_MEMBER_CALL     — callee returned an owned VAL_STRING
                 *   - NODE_FUNC_CALL       — callee returned an owned VAL_STRING
                 *   - NODE_FFI_CALL        — FFI-returned strings are owned
                 *   - everything else      — assume aliased, do NOT free
                 *     (NODE_IDENTIFIER, NODE_ARR_ACCESS, NODE_MEMBER_ACCESS,
                 *      etc. read shared slot pointers). */
                int _argc = node->as.member_call.arg_count;
                if (_argc > 16) _argc = 16;
                Value _args[16];
                int   _args_owned[16];
                for (int _i = 0; _i < _argc; _i++) {
                    ASTNode *_an = node->as.member_call.args[_i];
                    _args[_i] = eval(rt, _an);
                    if (rt->had_error) {
                        /* Free any already-owned args before bailing. */
                        for (int _j = 0; _j < _i; _j++)
                            if (_args_owned[_j] && _args[_j].type == VAL_STRING && _args[_j].as.string)
                                fxstr_release(_args[_j].as.string);
                        return val_nil();
                    }
                    _args_owned[_i] =
                        (_an->type == NODE_STRING_LIT)  ||
                        (_an->type == NODE_MEMBER_CALL) ||
                        (_an->type == NODE_FUNC_CALL)   ||
                        (_an->type == NODE_FFI_CALL)    ||
                        (_an->type == NODE_IDENTIFIER)  ||  /* strdups */
                        (_an->type == NODE_ARR_ACCESS)  ||  /* strdups */
                        (_an->type == NODE_MEMBER_ACCESS);  /* strdups */
                }

                /* Walk registry — find the lib whose owner matches.
                 * Capture the result in _ret then free owned VAL_STRING args. */
                Value _ret  = val_nil();
                int   _hit  = 0;
                for (int _ri = 0; _ri < FLUXA_LIB_COUNT; _ri++) {
                    const FluxaLibEntry *_e = &fluxa_lib_registry[_ri];
                    if (!_e->enabled) continue;
                    if (!fluxa_std_lib_enabled(&rt->config.std_libs, _e->name))
                        continue;
                    if (strcmp(owner, _e->owner) != 0) continue;

                    int _err_before = rt->err_stack.count;
                    if (_e->cfg_aware && _e->call_cfg) {
                        _ret = _e->call_cfg(method, _args, _argc,
                                            &rt->err_stack, &rt->had_error,
                                            rt->current_line, &rt->config);
                    } else if (_e->rt_aware && _e->call_rt) {
                        _ret = _e->call_rt(method, _args, _argc,
                                           &rt->err_stack, &rt->had_error,
                                           rt->current_line, rt);
                    } else if (_e->call) {
                        _ret = _e->call(method, _args, _argc,
                                        &rt->err_stack, &rt->had_error,
                                        rt->current_line);
                    }
                    rt_surface_lib_errors(rt, _err_before);
                    _hit = 1;
                    break;
                }
                if (_hit) {
                    /* Release transient heap allocations from owned args. */
                    for (int _i = 0; _i < _argc; _i++)
                        if (_args_owned[_i] && _args[_i].type == VAL_STRING && _args[_i].as.string)
                            fxstr_release(_args[_i].as.string);
                    /* Register VAL_DYN returns with the GC so a later
                     * reassignment of the holding slot drops the dyn to
                     * pin_count 0 and a sweep can free it. Without this,
                     * lib-returned dyn wrappers (e.g. json2.parse) live
                     * outside the GC and accumulate across iterations. */
                    if (_ret.type == VAL_DYN && _ret.as.dyn) {
                        GCEntry *_gce = gc_find_slot(&rt->gc, _ret.as.dyn);
                        if (!_gce) {
                            gc_register(&rt->gc, _ret.as.dyn,
                                sizeof(FluxaDyn) + sizeof(Value) * (size_t)_ret.as.dyn->cap,
                                &rt->err_stack);
                        }
                    }
                    return _ret;
                }
                /* No lib matched — owned args still need to be freed if any
                 * other code path is about to consume the result. The fall-
                 * through (FFI, Block instance method, etc.) below evaluates
                 * args again, so release ours here to avoid leaking. */
                for (int _i = 0; _i < _argc; _i++)
                    if (_args_owned[_i] && _args[_i].type == VAL_STRING && _args[_i].as.string)
                        fxstr_release(_args[_i].as.string);
            }

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
                                                  &rt->err_stack, ctx,
                                                  rt->config.ffi_str_buf_size);
                    /* Sprint 9.c-3: write back pointer args into Fluxa vars.
                     * fluxa_ffi_call already updated args[i] for PTR_* kinds,
                     * writable char* (FPARAM_STR), and arr byte buffers (FPARAM_ARR).
                     * Propagate updated values to the original Fluxa variables.
                     * NOTE: FPARAM_ARR is excluded here — the scatter already
                     * wrote bytes directly into arr->data[j] in ffi.c, so
                     * calling rt_set with the whole VAL_ARR would corrupt memory. */
                    if (msig && !rt->had_error) {
                        for (int i = 0; i < argc && i < msig->param_count; i++) {
                            FParamKind k = msig->param_kinds[i];
                            if (k != FPARAM_PTR_INT  &&
                                k != FPARAM_PTR_FLT  &&
                                k != FPARAM_PTR_BOOL &&
                                k != FPARAM_STR) continue;
                            ASTNode *anode = node->as.member_call.args[i];
                            if (!anode || anode->type != NODE_IDENTIFIER) continue;
                            rt_set(rt, anode, anode->as.str.value, args[i]);
                        }
                    }
                    free(args);
                    return result;
                }
                char buf[280];
                /* Hint at stdlib or FFI when the name looks like a known lib */
                if (strcmp(owner, "math") == 0 || strcmp(owner, "csv")  == 0 ||
                    strcmp(owner, "json") == 0 || strcmp(owner, "strings") == 0 ||
                    strcmp(owner, "time") == 0 || strcmp(owner, "ft") == 0 ||
                    strcmp(owner, "flxthread") == 0) {
                    snprintf(buf, sizeof(buf),
                        "'%s' is not enabled — add 'std.%s = \"1.0\"' "
                        "under [libs] in fluxa.toml and 'import std %s' "
                        "at the top of your file.",
                        owner, owner, owner);
                } else {
                    snprintf(buf, sizeof(buf),
                        "undefined identifier '%s' — if this is a C library "
                        "add it under [ffi] in fluxa.toml: %s = \"auto\"",
                        owner, owner);
                }
                rt_error(rt, buf); return val_nil();
            }
            Value fn_val;
            char method_key[256];
            if (!block_method_key(method, method_key, sizeof(method_key)) ||
                !scope_get(&inst->scope, method_key, &fn_val)) {
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
    /* Shared mutable — thread clone points to parent's prst_pool so all
     * threads read/write the same live state. ft.lock() serializes per-var
     * read-modify-write for any variable registered with ft.lock("name"). */
    rt->shared_prst_pool = parent->shared_prst_pool
                           ? parent->shared_prst_pool
                           : &parent->prst_pool;
    rt->prst_graph   = parent->prst_graph;
    /* Pre-populate the clone's stack from the shared pool so the VM and
     * tree-walker both start with the correct prst values.
     * This is a snapshot at thread-start; subsequent reads go back to the
     * pool (for rt_get in shared_prst_pool mode) or via the stack (for the
     * VM, which syncs back to the pool after each while loop). */
    if (rt->mode == FLUXA_MODE_PROJECT) {
        PrstPool *pool = rt->shared_prst_pool;
        for (int _i = 0; _i < pool->count; _i++) {
            PrstEntry *_pe = &pool->entries[_i];
            int _off = _pe->stack_offset;
            if (_off >= 0 && _off < FLUXA_STACK_SIZE) {
                rt->stack[_off] = _pe->value;
                if (_off >= rt->stack_size)
                    rt->stack_size = _off + 1;
            }
        }
    }
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
    return runtime_exec_persist(program, NULL);
}

int runtime_exec_persist(ASTNode *program, PrstPool *pool_out) {
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
    rt.shared_prst_pool = NULL;   /* main runtime owns its own pool */
    rt.config           = config;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);
    warm_profile_init(&rt.warm, config.warm_func_cap);
    rt.warm.enabled = 1;  /* warm path active from first execution */
    rt.current_fn   = NULL;
    rt.current_wf   = NULL;
    /* Sprint 9.c-2: pre-load [ffi] libs from fluxa.toml */
    ffi_load_from_config(&rt.ffi, &rt.err_stack, &config);
    /* std libs: config (including enabled_libs array) propagated to runtime */
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

        /* ── Sprint 13: Runtime Update Protocol — load restart snapshot ──── */
        if (g_restart_snapshot_path[0]) {
            FILE *snap_f = fopen(g_restart_snapshot_path, "rb");
            if (snap_f) {
                fseek(snap_f, 0, SEEK_END);
                long snap_sz = ftell(snap_f); rewind(snap_f);
                if (snap_sz > 0) {
                    void *snap_buf = malloc((size_t)snap_sz);
                    size_t snap_nr = fread(snap_buf, 1, (size_t)snap_sz, snap_f);
                    fclose(snap_f);
                    if (snap_nr == (size_t)snap_sz) {
                        if (prst_pool_deserialize(&rt.prst_pool,
                                                  snap_buf, (size_t)snap_sz)) {
                            fprintf(stderr,
                                "[fluxa] restart: loaded %d prst vars from snapshot\n",
                                rt.prst_pool.count);
                        } else {
                            fprintf(stderr,
                                "[fluxa] restart: WARNING — snapshot load failed, "
                                "starting with empty prst pool\n");
                        }
                    }
                    free(snap_buf);
                } else {
                    fclose(snap_f);
                }
                /* Remove snapshot file — consumed */
                remove(g_restart_snapshot_path);
                g_restart_snapshot_path[0] = '\0';
            } else {
                fprintf(stderr,
                    "[fluxa] restart: WARNING — snapshot file not found: %s\n",
                    g_restart_snapshot_path);
                g_restart_snapshot_path[0] = '\0';
            }
        }
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

    /* Hand the pool over before the collect, and detach whatever it still
     * points at so the sweep below cannot free a wrapper the next run will
     * restore. Must precede gc_collect_all. */
    if (mode == FLUXA_MODE_PROJECT && pool_out) {
        *pool_out = rt.prst_pool;
        prst_detach_dyns_from_gc(&rt.gc, pool_out);
    }

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc, gc_dyn_free_fn);
    if (mode == FLUXA_MODE_PROJECT) {
        if (!pool_out) prst_pool_free(&rt.prst_pool);
        prst_graph_free(&rt.prst_graph);
    }
    ffi_registry_free(&rt.ffi);
    warm_profile_free(&rt.warm);
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
    rt.shared_prst_pool = NULL;
    rt.config           = config;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);
    warm_profile_init(&rt.warm, config.warm_func_cap);
    rt.warm.enabled = 1;
    rt.current_fn   = NULL;
    rt.current_wf   = NULL;
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
    warm_profile_free(&rt.warm);
    return rt.had_error ? 1 : 0;
}

/* ── runtime_explain ─────────────────────────────────────────────────────── */
/* Prints the full runtime state for `fluxa explain`.
 * Called after program execution completes in PROJECT mode.
 * Shows: prst variables, non-prst variables, Blocks, and dep graph. */
void runtime_explain(Runtime *rt) {
    printf("\n── prst (persist across reloads) ");
    printf("────────────────────────────────────\n");
    if (RT_POOL(rt)->count == 0) {
        printf("  (none)\n");
    } else {
        for (int i = 0; i < RT_POOL(rt)->count; i++) {
            PrstEntry *e = &RT_POOL(rt)->entries[i];
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
    rt.shared_prst_pool = NULL;
    errstack_clear(&rt.err_stack);
    gc_init(&rt.gc, config.gc_cap);
    ffi_registry_init(&rt.ffi);
    warm_profile_init(&rt.warm, config.warm_func_cap);
    rt.warm.enabled = 1;
    rt.current_fn   = NULL;
    rt.current_wf   = NULL;
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

    /* The pool survives this runtime, so its dyn wrappers must not be swept
     * below. Must run before gc_collect_all. */
    if (pool_in) prst_detach_dyns_from_gc(&rt.gc, pool_in);

    scope_free(&rt.scope);
    scope_table_free(&rt.global_table);
    block_registry_free();
    gc_collect_all(&rt.gc, gc_dyn_free_fn);
    prst_graph_free(&rt.prst_graph);
    ffi_registry_free(&rt.ffi);
    warm_profile_free(&rt.warm);
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
    for (int i = 0; i < RT_POOL(rt)->count; i++)
        prst_graph_invalidate(&rt->prst_graph, RT_POOL(rt)->entries[i].name);

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
