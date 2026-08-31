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
static Value flx_invoke_method_n(Runtime *rt, BlockInstance *inst,
                                  const char *method_name,
                                  const Value *args, int argc);

static Value flx_invoke_method(Runtime *rt, BlockInstance *inst,
                                const char *method_name,
                                Value arg, int has_arg) {
    if (has_arg)
        return flx_invoke_method_n(rt, inst, method_name, &arg, 1);
    return flx_invoke_method_n(rt, inst, method_name, NULL, 0);
}

static Value flx_invoke_method_n(Runtime *rt, BlockInstance *inst,
                                  const char *method_name,
                                  const Value *args, int argc) {
    if (!inst || !method_name) return flxt_nil();

    Value fn_val; fn_val.type = VAL_NIL;
    char method_key[256];
    if (!block_method_key(method_name, method_key, sizeof(method_key)) ||
        !scope_get(&inst->scope, method_key, &fn_val) ||
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

    /* Bind arguments to parameters via scope (not stack). */
    int bind_n = argc < fn_node->as.func_decl.param_count
                 ? argc : fn_node->as.func_decl.param_count;
    for (int _ai = 0; _ai < bind_n; _ai++) {
        const char *pname = fn_node->as.func_decl.param_names[_ai];
        scope_set(&rt->scope, pname, args[_ai]);
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

        Value result = flx_invoke_method_n(rt, inst, msg.method,
                                           msg.args, msg.argc);

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

    /* Restore prst vars into clone scope */
    if (rt->mode == FLUXA_MODE_PROJECT) {
        for (int _pi = 0; _pi < rt->prst_pool.count; _pi++) {
            PrstEntry *_pe = &rt->prst_pool.entries[_pi];
            scope_set(&rt->scope, _pe->name, _pe->value);
        }
    }

    /* Bind initial args from ft.new(name, fn, arg1, arg2, ...) to stack slots.
     * Params are positional — stack[0] = first param, stack[1] = second, etc.
     * Mirror exactly what call_function does. */
    int bind_n = t->fn_argc < fn->as.func_decl.param_count
                 ? t->fn_argc : fn->as.func_decl.param_count;
    if (bind_n > 0) {
        /* Zero enough slots first */
        int zero_n = fn->as.func_decl.param_count + 4;
        if (zero_n > FLUXA_STACK_SIZE) zero_n = FLUXA_STACK_SIZE;
        for (int _zi = 0; _zi < zero_n; _zi++) rt->stack[_zi].type = VAL_NIL;
        for (int _ai = 0; _ai < bind_n; _ai++) {
            Value av = t->fn_args[_ai];
            if (av.type == VAL_ARR)
                rt->stack[_ai] = val_arr_ref(av.as.arr.data, av.as.arr.size,
                                             av.as.arr.owned);
            else
                rt->stack[_ai] = av;
            if (rt->stack_size <= _ai) rt->stack_size = _ai + 1;
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

        /* ── batch form: ft.new(prefix, count, "fn" [, args...]) ─────────────
         * Spawns <count> global-function threads named prefix1 .. prefixN
         * (1-indexed, matching hand-written w1..wN). Disambiguated purely by
         * TYPE: args[1] is an int here, a string in the unary global-fn form
         * ft.new("w1","worker",srv), and a Block in ft.new("t",inst,"m"). So a
         * numeric NAME like ft.new("w10","worker") stays in args[0] and takes
         * the unary path unchanged. This branch returns before the single
         * thread is allocated, so the existing paths are untouched. */
        if (args[1].type == VAL_INT) {
            long bcount = args[1].as.integer;
            if (bcount < 1 || bcount > FLUXA_THREAD_MAX) {
                snprintf(errbuf, sizeof(errbuf),
                    "ft.new: batch count %ld out of range (1..%d)",
                    bcount, FLUXA_THREAD_MAX);
                FT_ERR(errbuf);
            }
            if (argc < 3 || args[2].type != VAL_STRING || !args[2].as.string)
                FT_ERR("ft.new: batch form is ft.new(prefix, count, \"fn\" [, args...])");
            const char *bfn = args[2].as.string;
            Value bfv; bfv.type = VAL_NIL;
            scope_table_get(rt->global_table, bfn, &bfv);
            if (bfv.type != VAL_FUNC) {
                snprintf(errbuf, sizeof(errbuf),
                    "ft.new: function '%s' not found", bfn);
                FT_ERR(errbuf);
            }
            ASTNode *bnode = bfv.as.func;
            int bextra   = argc - 3;   /* args beyond (prefix, count, fn) */
            int bcfg_max = rt->config.ft_max_msg_args > 0
                           ? rt->config.ft_max_msg_args : 2;
            if (bextra > bcfg_max) {
                snprintf(errbuf, sizeof(errbuf),
                    "ft.new: too many arguments (%d), max_msg_args=%d",
                    bextra, bcfg_max);
                FT_ERR(errbuf);
            }
            if (bextra > bnode->as.func_decl.param_count) {
                snprintf(errbuf, sizeof(errbuf),
                    "ft.new: function '%s' has %d param(s), got %d arg(s)",
                    bfn, bnode->as.func_decl.param_count, bextra);
                FT_ERR(errbuf);
            }
            char bname[64];   /* matches FlxThread.name[64] — canonical strncpy size */
            for (long bi = 1; bi <= bcount; bi++) {
                snprintf(bname, sizeof(bname), "%s%ld", tname, bi);
                if (flx_find_thread(bname)) {
                    snprintf(errbuf, sizeof(errbuf),
                        "ft.new: thread name '%s' already active", bname);
                    FT_ERR(errbuf);
                }
                FlxThread *bt = flx_alloc_thread(bname);
                if (!bt) FT_ERR("ft.new: max thread count reached");
                FlxRunnerArg *bra = (FlxRunnerArg *)malloc(sizeof(FlxRunnerArg));
                if (!bra) { bt->active = 0; FT_ERR("ft.new: out of memory"); }
                bt->is_block = 0;
                bt->fn_node  = bnode;
                bt->fn_argc  = bextra;
                for (int _ai = 0; _ai < bextra; _ai++)
                    bt->fn_args[_ai] = args[3 + _ai];
                bra->thread = bt; bra->rt = rt; bra->inst = NULL;
                pthread_create(&bt->tid, NULL, flx_fn_runner, bra);
                pthread_detach(bt->tid);
            }
            return flxt_nil();
        }

        if (flx_find_thread(tname))
            FT_ERR("thread name already active — ft.resolve_all() first");

        FlxThread *t = flx_alloc_thread(tname);
        if (!t) FT_ERR("max thread count reached");

        FlxRunnerArg *ra = (FlxRunnerArg *)malloc(sizeof(FlxRunnerArg));
        if (!ra) { t->active = 0; FT_ERR("out of memory"); }

        /* ft.new("name", fn_name_str [, arg1, arg2, ...]) — global function */
        if (args[1].type == VAL_STRING && args[1].as.string &&
            !(argc >= 3 && args[2].type == VAL_STRING &&
              args[1].type == VAL_STRING)) {
            /* Disambiguate: if argc==3 and args[2] is VAL_BLOCK_INST
             * or args[1] is instance — fall through to Block path.
             * Global fn path: args[1] is fn name str, rest are fn args. */
        }
        if (args[1].type == VAL_STRING && args[1].as.string &&
            !(argc >= 3 && args[2].type != VAL_STRING &&
              (args[2].type == VAL_BLOCK_INST || args[2].type == VAL_NIL))) {
            /* Check it's not ft.new(name, instance, method) */
        }
        /* Simpler detection: global fn if args[1] is VAL_STRING and
         * (argc==2, or args[2] is not a method-name following a Block). */
        if (args[1].type == VAL_STRING && args[1].as.string &&
            (argc == 2 ||
             (argc >= 3 && args[2].type != VAL_BLOCK_INST))) {
            /* Could still be ft.new(name, fn, arg1, arg2...) */
            const char *fn_str = args[1].as.string;
            Value fn_val; fn_val.type = VAL_NIL;
            scope_table_get(rt->global_table, fn_str, &fn_val);
            if (fn_val.type == VAL_FUNC) {
                /* Global function — valid. Collect extra args. */
                int cfg_max = rt->config.ft_max_msg_args > 0
                              ? rt->config.ft_max_msg_args : 2;
                int fn_extra = argc - 2; /* args beyond fn name */
                if (fn_extra > cfg_max) {
                    free(ra); t->active = 0;
                    snprintf(errbuf, sizeof(errbuf),
                        "ft.new: too many arguments (%d), max_msg_args=%d",
                        fn_extra, cfg_max);
                    FT_ERR(errbuf);
                }
                /* Verify arity */
                ASTNode *fn_node = fn_val.as.func;
                if (fn_extra > fn_node->as.func_decl.param_count) {
                    free(ra); t->active = 0;
                    snprintf(errbuf, sizeof(errbuf),
                        "ft.new: function '%s' has %d param(s), got %d arg(s)",
                        fn_str, fn_node->as.func_decl.param_count, fn_extra);
                    FT_ERR(errbuf);
                }
                t->is_block = 0;
                t->fn_node  = fn_node;
                t->fn_argc  = fn_extra;
                for (int _ai = 0; _ai < fn_extra; _ai++)
                    t->fn_args[_ai] = args[2 + _ai];
                ra->thread  = t; ra->rt = rt; ra->inst = NULL;
                pthread_create(&t->tid, NULL, flx_fn_runner, ra);
                pthread_detach(t->tid);
                return flxt_nil();
            }
            /* fn_str not found as function — fall through to error */
            free(ra); t->active = 0;
            snprintf(errbuf, sizeof(errbuf),
                "ft.new: function '%s' not found", fn_str);
            FT_ERR(errbuf);
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
        char method_key[256];
        if (!block_method_key(method_name, method_key, sizeof(method_key)) ||
            !scope_get(&inst->scope, method_key, &fn_chk) ||
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
        int cfg_max = rt->config.ft_max_msg_args > 0
                      ? rt->config.ft_max_msg_args : 2;
        int msg_argc = argc - 2;
        if (msg_argc > cfg_max) {
            snprintf(errbuf, sizeof(errbuf),
                "ft.message: too many arguments (%d), max_msg_args=%d",
                msg_argc, cfg_max);
            FT_ERR(errbuf);
        }
        if (!flx_mailbox_push(t, method, args + 2, msg_argc, NULL)) {
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
        int cfg_max = rt->config.ft_max_msg_args > 0
                      ? rt->config.ft_max_msg_args : 2;
        int aw_argc = argc - 2;
        if (aw_argc > cfg_max) {
            snprintf(errbuf, sizeof(errbuf),
                "ft.await: too many arguments (%d), max_msg_args=%d",
                aw_argc, cfg_max);
            FT_ERR(errbuf);
        }
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

        if (!flx_mailbox_push(t, method, args + 2, aw_argc, rep)) {
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
         * any prst writes made by child threads (global fn threads).
         * Use the same pool resolution as RT_POOL: shared if clone, local if main. */
        if (rt->mode == FLUXA_MODE_PROJECT) {
            PrstPool *pool = rt->shared_prst_pool ? rt->shared_prst_pool : &rt->prst_pool;
            for (int _pi = 0; _pi < pool->count; _pi++) {
                PrstEntry *_pe = &pool->entries[_pi];
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
