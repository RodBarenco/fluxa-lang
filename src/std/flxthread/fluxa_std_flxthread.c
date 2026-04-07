/* fluxa_std_flxthread.c — Fluxa Standard Library: flxthread
 *
 * Implementation uses the Fluxa runtime's eval() directly.
 * Compiled when FLUXA_STD_FLXTHREAD is defined.
 * Linked with -lpthread (already in base Makefile).
 */
#define _POSIX_C_SOURCE 200809L
#ifndef FLUXA_EMBEDDED
#ifdef FLUXA_STD_FLXTHREAD

#include <time.h>
#include "fluxa_std_flxthread.h"

/* Thread-local pointer to the current FlxThread */
__thread FlxThread *g_current_flx_thread = NULL;
#include "../../runtime.h"
#include "../../block.h"
#include "../../scope.h"

/* ── Internal: invoke a method by name on a Block instance ──────────────── */
/* Finds the method's VAL_FUNC in the instance scope, binds the argument
 * into the instance scope under the first param name, then evals the body. */
static Value flx_invoke_method(Runtime *rt, BlockInstance *inst,
                                const char *method_name,
                                Value arg, int has_arg) {
    if (!inst || !method_name) return flxt_nil();

    Value fn_val; fn_val.type = VAL_NIL;
    if (!scope_get(&inst->scope, method_name, &fn_val) ||
        fn_val.type != VAL_FUNC) {
        char buf[280];
        snprintf(buf, sizeof(buf),
            "flxthread: method '%s' not found on Block instance",
            method_name);
        errstack_push(&rt->err_stack, ERR_FLUXA, buf, "flxthread", 0);
        rt->had_error = 1;
        return flxt_nil();
    }

    ASTNode *fn_node = fn_val.as.func;

    /* Set current_instance so prst-in-Block works correctly */
    BlockInstance *prev_inst = rt->current_instance;
    rt->current_instance = inst;

    /* Push a fresh scope frame so params don't alias run()'s locals.
     * Each mailbox-invoked method gets its own clean scope. */
    Scope saved_scope = rt->scope;
    rt->scope.table = NULL;  /* fresh empty scope for this call */

    /* Save and zero the clone's stack so rt_set falls through to
     * inst->scope for Block member writes (total, health, etc.)
     * instead of writing to a potentially aliased stack slot. */
    Value saved_stack[FLUXA_STACK_SIZE];
    int   saved_stack_size = rt->stack_size;
    memcpy(saved_stack, rt->stack, sizeof(rt->stack));
    for (int _si = 0; _si < FLUXA_STACK_SIZE; _si++)
        rt->stack[_si].type = VAL_NIL;
    rt->stack_size = 0;

    /* Bind argument to the first parameter via scope (not stack) */
    if (has_arg && fn_node->as.func_decl.param_count > 0) {
        const char *pname = fn_node->as.func_decl.param_names[0];
        scope_set(&rt->scope, pname, arg);
    }

    /* Eval the function body */
    Value result = runtime_eval(rt, fn_node->as.func_decl.body);
    if (rt->ret.active) {
        result = rt->ret.value;
        rt->ret.active     = 0;
        rt->ret.tco_active = 0;
    }

    /* Restore stack and scope */
    memcpy(rt->stack, saved_stack, sizeof(rt->stack));
    rt->stack_size = saved_stack_size;
    scope_free(&rt->scope);
    rt->scope = saved_scope;

    rt->current_instance = prev_inst;
    return result;
}

/* ── Mailbox drain — called at while back-edge ───────────────────────────── */
/* Fast path: if count == 0, returns immediately (no lock, no cache miss).
 * Three documented cases:
 *   1. Loop with sleep   — drain runs at sleep frequency (~16ms)
 *   2. Hot loop no sleep — drain runs every iteration, O(1) fast path
 *   3. Polling loop      — drain runs every iteration for max responsiveness */
int flx_mailbox_drain(FlxThread *t, void *rt_ptr, void *instance_ptr) {
    if (t->stop_requested) return -1; /* stop requested — O(1) check */
    if (t->mb_count == 0) return 0;   /* fast path — no lock needed */

    Runtime  *rt   = (Runtime *)rt_ptr;
    BlockInstance *inst = (BlockInstance *)instance_ptr;
    int processed = 0;

    while (t->mb_count > 0) {
        pthread_mutex_lock(&t->mb_mu);
        if (t->mb_count == 0) { pthread_mutex_unlock(&t->mb_mu); break; }

        FlxMessage msg = t->mb_queue[t->mb_head];
        t->mb_head = (t->mb_head + 1) % FLUXA_MAILBOX_MAX;
        t->mb_count--;
        pthread_mutex_unlock(&t->mb_mu);

        Value result = flx_invoke_method(rt, inst, msg.method,
                                          msg.arg,
                                          msg.arg.type != VAL_NIL);

        if (msg.reply) {
            pthread_mutex_lock(&msg.reply->mu);
            msg.reply->value = result;
            msg.reply->ready = 1;
            pthread_cond_signal(&msg.reply->cv);
            pthread_mutex_unlock(&msg.reply->mu);
        }
        processed++;
    }
    return processed;
}

/* ── Thread argument struct ──────────────────────────────────────────────── */
typedef struct {
    FlxThread     *thread;
    Runtime  *rt;
    BlockInstance *inst;
} FlxRunnerArg;

/* ── Block method thread runner ──────────────────────────────────────────── */
static void *flx_block_runner(void *arg) {
    FlxRunnerArg  *ra   = (FlxRunnerArg *)arg;
    FlxThread     *t    = ra->thread;
    Runtime       *parent = ra->rt;
    BlockInstance *inst = ra->inst;
    free(ra);

    /* Each thread gets its own Runtime clone so stack/scope/error
     * state are isolated. Global table and config are shared read-only. */
    Runtime *rt = runtime_clone_for_thread(parent);
    if (!rt) { t->active = 0; return NULL; }

    rt->current_thread   = t;
    rt->current_instance = inst;
    g_current_flx_thread = t;

    flx_invoke_method(rt, inst, t->method, flxt_nil(), 0);
    runtime_free_thread_clone(rt);

    t->active = 0;
    pthread_mutex_lock(&t->drain_mu);
    pthread_cond_broadcast(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_mu);
    return NULL;
}

/* ── Global function thread runner ───────────────────────────────────────── */
static void *flx_fn_runner(void *arg) {
    FlxRunnerArg  *ra     = (FlxRunnerArg *)arg;
    FlxThread     *t      = ra->thread;
    Runtime       *parent = ra->rt;
    ASTNode       *fn     = (ASTNode *)t->fn_node;
    free(ra);

    Runtime *rt = runtime_clone_for_thread(parent);
    if (!rt) { t->active = 0; return NULL; }
    rt->current_thread = t;
    g_current_flx_thread = t;

    /* Restore prst vars into clone scope so NODE_ASSIGN and rt_get
     * can find them by name (scope fallback when stack slot is NIL).
     * Note: while-loops compile to the bytecode VM which reads prst
     * via scope_get directly; tree-walk paths use rt_get fallback. */
    if (rt->mode == FLUXA_MODE_PROJECT) {
        for (int _pi = 0; _pi < rt->prst_pool.count; _pi++) {
            PrstEntry *_pe = &rt->prst_pool.entries[_pi];
            scope_set(&rt->scope, _pe->name, _pe->value);
        }
    }

    runtime_eval(rt, fn->as.func_decl.body);
    /* No explicit writeback needed: NODE_ASSIGN syncs pool on every write. */

    runtime_free_thread_clone(rt);

    t->active = 0;
    pthread_mutex_lock(&t->drain_mu);
    pthread_cond_broadcast(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_mu);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
Value fluxa_std_flxthread_call(const char *fn_name,
                                const Value *args, int argc,
                                ErrStack *err, int *had_error,
                                int line, void *rt_ptr) {
    Runtime *rt = (Runtime *)rt_ptr;
    char errbuf[1024];

#define FT_ERR(msg) do { \
    char _fm[1024]; \
    strncpy(_fm, msg, sizeof(_fm)-1); _fm[sizeof(_fm)-1] = '\0'; \
    snprintf(errbuf, sizeof(errbuf), "ft.%s (line %d): %.900s", \
             fn_name, line, _fm); \
    errstack_push(err, ERR_FLUXA, errbuf, "flxthread", line); \
    *had_error = 1; return flxt_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "ft.%s: expected at least %d argument(s), got %d", \
            fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "flxthread", line); \
        *had_error = 1; return flxt_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        FT_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* ── ft.new ─────────────────────────────────────────────────────────── */
    if (strcmp(fn_name, "new") == 0) {
        NEED(2);
        GET_STR(0, tname);

        if (flx_find_thread(tname))
            FT_ERR("thread name already active — ft.resolve_all() first");

        FlxThread *t = flx_alloc_thread(tname);
        if (!t) FT_ERR("max thread count reached");

        FlxRunnerArg *ra = (FlxRunnerArg *)malloc(sizeof(FlxRunnerArg));
        if (!ra) { t->active = 0; FT_ERR("out of memory"); }

        /* ft.new("name", fn_name_str) — global function */
        if (argc == 2 && args[1].type == VAL_STRING && args[1].as.string) {
            const char *fn_str = args[1].as.string;
            Value fn_val; fn_val.type = VAL_NIL;
            scope_table_get(rt->global_table, fn_str, &fn_val);
            if (fn_val.type != VAL_FUNC) {
                free(ra); t->active = 0;
                snprintf(errbuf, sizeof(errbuf),
                    "ft.new: function '%s' not found", fn_str);
                FT_ERR(errbuf);
            }
            t->is_block = 0;
            t->fn_node  = fn_val.as.func;
            ra->thread  = t; ra->rt = rt; ra->inst = NULL;
            pthread_create(&t->tid, NULL, flx_fn_runner, ra);
            pthread_detach(t->tid);
            return flxt_nil();
        }

        /* ft.new("name", instance, "method") — Block method */
        if (argc < 3) { free(ra); t->active = 0;
            FT_ERR("Block thread needs 3 args: ft.new(name, instance, method)"); }
        if (args[1].type != VAL_BLOCK_INST || !args[1].as.block_inst) {
            free(ra); t->active = 0;
            FT_ERR("second argument must be a Block instance"); }
        GET_STR(2, method_name);

        BlockInstance *inst = args[1].as.block_inst;
        /* Verify method exists */
        Value fn_chk; fn_chk.type = VAL_NIL;
        if (!scope_get(&inst->scope, method_name, &fn_chk) ||
            fn_chk.type != VAL_FUNC) {
            free(ra); t->active = 0;
            snprintf(errbuf, sizeof(errbuf),
                "ft.new: method '%s' not found on Block instance", method_name);
            FT_ERR(errbuf);
        }
        t->is_block = 1;
        strncpy(t->method, method_name, sizeof(t->method)-1);
        ra->thread = t; ra->rt = rt; ra->inst = inst;
        pthread_create(&t->tid, NULL, flx_block_runner, ra);
        pthread_detach(t->tid);
        return flxt_nil();
    }

    /* ── ft.message — non-blocking ───────────────────────────────────────── */
    if (strcmp(fn_name, "message") == 0) {
        NEED(2); GET_STR(0, tname); GET_STR(1, method);
        FlxThread *t = flx_find_thread(tname);
        if (!t) { snprintf(errbuf, sizeof(errbuf),
            "ft.message: thread '%s' not found", tname); FT_ERR(errbuf); }
        if (!t->is_block) FT_ERR("ft.message only works on Block threads");
        Value arg = (argc >= 3) ? args[2] : flxt_nil();
        if (!flx_mailbox_push(t, method, arg, NULL)) {
            snprintf(errbuf, sizeof(errbuf),
                "ft.message: mailbox full for '%s'", tname);
            FT_ERR(errbuf);
        }
        return flxt_nil();
    }

    /* ── ft.await — blocking ─────────────────────────────────────────────── */
    if (strcmp(fn_name, "await") == 0) {
        NEED(2); GET_STR(0, tname); GET_STR(1, method);
        FlxThread *t = flx_find_thread(tname);
        if (!t) { snprintf(errbuf, sizeof(errbuf),
            "ft.await: thread '%s' not found", tname); FT_ERR(errbuf); }
        if (!t->is_block) FT_ERR("ft.await only works on Block threads");
        Value arg = (argc >= 3) ? args[2] : flxt_nil();
        /* If thread already finished, call the method directly
         * on the Block instance from the calling thread */
        if (!t->active && t->is_block) {
            /* Retrieve the instance from the runner arg — it was stored
             * in the thread state. For now: error if thread not active. */
            snprintf(errbuf, sizeof(errbuf),
                "ft.await: thread '%s' has already finished. "
                "Call the method directly on the Block instance.", tname);
            FT_ERR(errbuf);
        }

        FlxReply *rep = (FlxReply *)calloc(1, sizeof(FlxReply));
        if (!rep) FT_ERR("ft.await: out of memory");
        pthread_mutex_init(&rep->mu, NULL);
        pthread_cond_init(&rep->cv, NULL);
        rep->ready = 0;

        if (!flx_mailbox_push(t, method, arg, rep)) {
            pthread_mutex_destroy(&rep->mu);
            pthread_cond_destroy(&rep->cv);
            free(rep);
            snprintf(errbuf, sizeof(errbuf),
                "ft.await: mailbox full for '%s'", tname);
            FT_ERR(errbuf);
        }

        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += FLUXA_AWAIT_TIMEOUT_MS / 1000;
        dl.tv_nsec += (long)(FLUXA_AWAIT_TIMEOUT_MS % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&rep->mu);
        while (!rep->ready) {
            if (pthread_cond_timedwait(&rep->cv, &rep->mu, &dl)) {
                pthread_mutex_unlock(&rep->mu);
                pthread_mutex_destroy(&rep->mu);
                pthread_cond_destroy(&rep->cv);
                free(rep);
                snprintf(errbuf, sizeof(errbuf),
                    "ft.await: timeout waiting for '%s'.'%s' (%dms)",
                    tname, method, FLUXA_AWAIT_TIMEOUT_MS);
                FT_ERR(errbuf);
            }
        }
        Value reply = rep->value;
        pthread_mutex_unlock(&rep->mu);
        pthread_mutex_destroy(&rep->mu);
        pthread_cond_destroy(&rep->cv);
        free(rep);
        return reply;
    }

    /* ── ft.lock — register prst global with mutex ───────────────────────── */
    if (strcmp(fn_name, "lock") == 0) {
        NEED(1); GET_STR(0, var_name);
        if (!flx_register_lock(var_name))
            FT_ERR("max lock count reached");
        return flxt_nil();
    }

    /* ── ft.resolve_all — wait for all threads ───────────────────────────── */
    if (strcmp(fn_name, "resolve_all") == 0) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += FLUXA_AWAIT_TIMEOUT_MS / 1000;
        for (int i = 0; i < g_flx_registry.count; i++) {
            FlxThread *t = &g_flx_registry.threads[i];
            if (!t->active) continue;
            pthread_mutex_lock(&t->drain_mu);
            while (t->active)
                if (pthread_cond_timedwait(&t->drain_cv, &t->drain_mu, &dl)) {
                    pthread_mutex_unlock(&t->drain_mu);
                    snprintf(errbuf, sizeof(errbuf),
                        "ft.resolve_all: timeout for thread '%s'", t->name);
                    FT_ERR(errbuf);
                }
            pthread_mutex_unlock(&t->drain_mu);
        }
        /* Memory barrier: ensure all child thread writes to prst_pool
         * are visible to the main thread before we read the pool. */
        pthread_mutex_lock(&g_flx_registry.registry_mu);
        pthread_mutex_unlock(&g_flx_registry.registry_mu);

        /* Sync main runtime stack from prst_pool so main thread sees
         * any prst writes made by child threads (global fn threads). */
        if (rt->mode == FLUXA_MODE_PROJECT) {
            for (int _pi = 0; _pi < rt->prst_pool.count; _pi++) {
                PrstEntry *_pe = &rt->prst_pool.entries[_pi];
                if (_pe->stack_offset >= 0 &&
                    _pe->stack_offset < FLUXA_STACK_SIZE) {
                    rt->stack[_pe->stack_offset] = _pe->value;
                    if (_pe->stack_offset >= rt->stack_size)
                        rt->stack_size = _pe->stack_offset + 1;
                }
                scope_set(&rt->scope, _pe->name, _pe->value);
            }
        }
        return flxt_nil();
    }

    /* ── ft.stop("name") — cooperative stop ────────────────────────────── */
    if (strcmp(fn_name, "stop") == 0) {
        NEED(1); GET_STR(0, tname);
        FlxThread *t = flx_find_thread(tname);
        if (!t) { snprintf(errbuf, sizeof(errbuf),
            "ft.stop: thread '%s' not found", tname); FT_ERR(errbuf); }
        t->stop_requested = 1;
        return flxt_nil();
    }

    /* ── ft.kill("name") — forced stop ──────────────────────────────────── */
    /* Sets stop_requested and marks thread dead. Does NOT call pthread_cancel.
     * WARNING: ft.lock() mutexes held by this thread are NOT released.
     * Pending ft.await() calls are unblocked with nil return value. */
    if (strcmp(fn_name, "kill") == 0) {
        NEED(1); GET_STR(0, tname);
        FlxThread *t = flx_find_thread(tname);
        if (!t) { snprintf(errbuf, sizeof(errbuf),
            "ft.kill: thread '%s' not found", tname); FT_ERR(errbuf); }
        t->stop_requested = 1;
        /* Unblock any ft.await waiting on this thread */
        pthread_mutex_lock(&t->mb_mu);
        while (t->mb_count > 0) {
            FlxMessage *msg = &t->mb_queue[t->mb_head];
            if (msg->reply) {
                pthread_mutex_lock(&msg->reply->mu);
                msg->reply->value = flxt_nil();
                msg->reply->ready = 1;
                pthread_cond_signal(&msg->reply->cv);
                pthread_mutex_unlock(&msg->reply->mu);
            }
            t->mb_head = (t->mb_head + 1) % FLUXA_MAILBOX_MAX;
            t->mb_count--;
        }
        pthread_mutex_unlock(&t->mb_mu);
        t->active = 0;
        pthread_mutex_lock(&t->drain_mu);
        pthread_cond_broadcast(&t->drain_cv);
        pthread_mutex_unlock(&t->drain_mu);
        return flxt_nil();
    }

    /* ── ft.should_stop() → bool ─────────────────────────────────────────── */
    /* Called INSIDE a thread to check if stop was requested.
     * O(1) via thread-local pointer. Usage: while !ft.should_stop() { ... } */
    if (strcmp(fn_name, "should_stop") == 0) {
        FlxThread *t = g_current_flx_thread;
        return flxt_bool(t && t->stop_requested ? 1 : 0);
    }

    /* ── ft.active("name") → bool ───────────────────────────────────────── */
    if (strcmp(fn_name, "active") == 0) {
        NEED(1); GET_STR(0, tname);
        FlxThread *t = flx_find_thread(tname);
        return flxt_bool(t && t->active ? 1 : 0);
    }

    /* ── ft.thread_count() → int ─────────────────────────────────────────── */
    if (strcmp(fn_name, "thread_count") == 0) {
        int n = 0;
        for (int i = 0; i < g_flx_registry.count; i++)
            if (g_flx_registry.threads[i].active) n++;
        return flxt_int(n);
    }

#undef FT_ERR
#undef NEED
#undef GET_STR

    snprintf(errbuf, sizeof(errbuf), "ft.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "flxthread", line);
    *had_error = 1;
    return flxt_nil();
}

#endif /* FLUXA_STD_FLXTHREAD */
#endif /* FLUXA_EMBEDDED */
