/* fluxa_std_flxthread.h — Fluxa Standard Library: flxthread
 *
 * Threading for Fluxa. Disabled on embedded targets (FLUXA_EMBEDDED).
 * Compiled when FLUXA_STD_FLXTHREAD is defined.
 *
 * Model:
 *   - Threads are identified by a string name — no opaque handles
 *   - Each thread running a Block method has a mailbox (FIFO queue)
 *   - Mailbox is drained at the back-edge of every while loop (fast path)
 *   - Global prst vars accessed by multiple threads are protected by
 *     named mutexes registered via ft.lock("var_name")
 *   - ft.resolve_all() joins all running threads
 *
 * API:
 *   ft.new("name", instance, "method")  — spawn Block method thread
 *   ft.new("name", global_fn)           — spawn global function thread
 *   ft.message("name", "method", arg)   — non-blocking call via mailbox
 *   ft.message("name", "method")        — non-blocking call, no arg
 *   ft.await("name", "method", arg)     — blocking call, returns value
 *   ft.await("name", "method")          — blocking call, no arg
 *   ft.lock("var_name")                 — register prst global with mutex
 *   ft.resolve_all()                    — wait for all threads to finish
 *
 * Thread safety:
 *   - Block instance scope is owned by one thread — no races on prst-in-Block
 *   - Global prst vars need ft.lock() if accessed by multiple threads
 *   - Mailbox has its own mutex — ft.message is always safe to call
 *   - ft.await acquires a per-thread condition variable
 */
#ifndef FLUXA_STD_FLXTHREAD_H
#define FLUXA_STD_FLXTHREAD_H

#ifndef FLUXA_EMBEDDED

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Limits ──────────────────────────────────────────────────────────────── */
#ifndef FLUXA_THREAD_MAX
#define FLUXA_THREAD_MAX     16
#endif
#ifndef FLUXA_MAILBOX_MAX
#define FLUXA_MAILBOX_MAX    64
#endif
#ifndef FLUXA_LOCK_MAX
#define FLUXA_LOCK_MAX       32
#endif
#ifndef FLUXA_AWAIT_TIMEOUT_MS
#define FLUXA_AWAIT_TIMEOUT_MS 5000
#endif

/* ── Mailbox message ─────────────────────────────────────────────────────── */
/* Heap-allocated reply channel shared between ft.await (caller) and
 * the thread that processes the message. Both sides hold the same pointer,
 * so mutex/condvar are never copied. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    Value           value;
    int             ready;
} FlxReply;

typedef struct {
    char      method[128];  /* method name to call on the Block instance */
    Value     arg;          /* single argument (VAL_NIL if none)          */
    FlxReply *reply;        /* non-NULL if caller wants a return value    */
} FlxMessage;

/* ── Per-thread state ────────────────────────────────────────────────────── */
typedef struct FlxThread {
    char        name[64];
    pthread_t   tid;
    int         active;        /* 1 while running                       */
    int         is_block;      /* 1 = Block method, 0 = global function */

    /* Block method path */
    void       *runtime;       /* Runtime* — opaque to avoid circ  */
    void       *instance;      /* BlockInstance* — the typeof instance  */
    char        method[128];   /* initial method to call                */

    /* Global function path */
    void       *fn_node;       /* ASTNode* of the function              */

    /* Mailbox */
    pthread_mutex_t  mb_mu;
    FlxMessage       mb_queue[FLUXA_MAILBOX_MAX];
    int              mb_head;
    int              mb_tail;
    int              mb_count;

    /* Result of the last mailbox drain (for ft.await) */
    pthread_mutex_t  drain_mu;
    pthread_cond_t   drain_cv;
} FlxThread;

/* ── Named mutex for ft.lock ─────────────────────────────────────────────── */
typedef struct {
    char            var_name[128];
    pthread_mutex_t mu;
    int             active;
} FlxNamedLock;

/* ── Thread registry (process-global) ───────────────────────────────────── */
typedef struct {
    FlxThread    threads[FLUXA_THREAD_MAX];
    int          count;
    pthread_mutex_t registry_mu;

    FlxNamedLock locks[FLUXA_LOCK_MAX];
    int          lock_count;
} FlxRegistry;

/* One global registry per process */
static FlxRegistry g_flx_registry = {
    .count      = 0,
    .lock_count = 0,
    .registry_mu = PTHREAD_MUTEX_INITIALIZER,
};

/* ── Registry helpers ────────────────────────────────────────────────────── */
static inline FlxThread *flx_find_thread(const char *name) {
    for (int i = 0; i < g_flx_registry.count; i++) {
        if (g_flx_registry.threads[i].active &&
            strcmp(g_flx_registry.threads[i].name, name) == 0)
            return &g_flx_registry.threads[i];
    }
    return NULL;
}

static inline FlxThread *flx_alloc_thread(const char *name) {
    pthread_mutex_lock(&g_flx_registry.registry_mu);
    /* Reuse inactive slot */
    for (int i = 0; i < g_flx_registry.count; i++) {
        if (!g_flx_registry.threads[i].active) {
            FlxThread *t = &g_flx_registry.threads[i];
            memset(t, 0, sizeof(*t));
            strncpy(t->name, name, sizeof(t->name)-1);
            t->active = 1;
            pthread_mutex_init(&t->mb_mu, NULL);
            pthread_mutex_init(&t->drain_mu, NULL);
            pthread_cond_init(&t->drain_cv, NULL);
            pthread_mutex_unlock(&g_flx_registry.registry_mu);
            return t;
        }
    }
    /* New slot */
    if (g_flx_registry.count >= FLUXA_THREAD_MAX) {
        pthread_mutex_unlock(&g_flx_registry.registry_mu);
        return NULL;
    }
    FlxThread *t = &g_flx_registry.threads[g_flx_registry.count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name)-1);
    t->active = 1;
    pthread_mutex_init(&t->mb_mu, NULL);
    pthread_mutex_init(&t->drain_mu, NULL);
    pthread_cond_init(&t->drain_cv, NULL);
    pthread_mutex_unlock(&g_flx_registry.registry_mu);
    return t;
}

/* ── Mailbox operations ──────────────────────────────────────────────────── */

/* Push a message onto the thread's mailbox. Thread-safe. */
/* Push a message. If reply != NULL, the thread will signal it when done. */
static inline int flx_mailbox_push(FlxThread *t, const char *method,
                                    Value arg, FlxReply *reply) {
    pthread_mutex_lock(&t->mb_mu);
    if (t->mb_count >= FLUXA_MAILBOX_MAX) {
        pthread_mutex_unlock(&t->mb_mu);
        return 0;
    }
    FlxMessage *msg = &t->mb_queue[t->mb_tail];
    memset(msg, 0, sizeof(*msg));
    strncpy(msg->method, method, sizeof(msg->method)-1);
    msg->arg   = arg;
    msg->reply = reply;
    t->mb_tail = (t->mb_tail + 1) % FLUXA_MAILBOX_MAX;
    t->mb_count++;
    pthread_mutex_unlock(&t->mb_mu);
    return 1;
}

/* Drain all pending messages. Called at while back-edge by the thread.
 * Returns 1 if any messages were processed. */


/* ── Named lock operations ───────────────────────────────────────────────── */
static inline FlxNamedLock *flx_find_lock(const char *var_name) {
    for (int i = 0; i < g_flx_registry.lock_count; i++) {
        if (g_flx_registry.locks[i].active &&
            strcmp(g_flx_registry.locks[i].var_name, var_name) == 0)
            return &g_flx_registry.locks[i];
    }
    return NULL;
}

static inline FlxNamedLock *flx_register_lock(const char *var_name) {
    FlxNamedLock *existing = flx_find_lock(var_name);
    if (existing) return existing;
    if (g_flx_registry.lock_count >= FLUXA_LOCK_MAX) return NULL;
    FlxNamedLock *lk = &g_flx_registry.locks[g_flx_registry.lock_count++];
    memset(lk, 0, sizeof(*lk));
    strncpy(lk->var_name, var_name, sizeof(lk->var_name)-1);
    pthread_mutex_init(&lk->mu, NULL);
    lk->active = 1;
    return lk;
}

/* Acquire a named lock — called by runtime before reading/writing a
 * locked prst var. Returns 1 if lock exists and was acquired. */
static inline int flx_lock_acquire(const char *var_name) {
    FlxNamedLock *lk = flx_find_lock(var_name);
    if (!lk) return 0;
    pthread_mutex_lock(&lk->mu);
    return 1;
}

static inline void flx_lock_release(const char *var_name) {
    FlxNamedLock *lk = flx_find_lock(var_name);
    if (lk) pthread_mutex_unlock(&lk->mu);
}

/* ── Value helpers ───────────────────────────────────────────────────────── */
static inline Value flxt_nil(void)  { Value v; v.type = VAL_NIL; return v; }
static inline Value flxt_bool(int b){ Value v; v.type = VAL_BOOL; v.as.boolean=b; return v; }
static inline Value flxt_int(long n){ Value v; v.type = VAL_INT; v.as.integer=n; return v; }

/* Mailbox drain — called by runtime.c at while back-edge.
 * Defined in fluxa_std_flxthread.c, declared here for runtime.c inclusion. */
extern int flx_mailbox_drain(FlxThread *t, void *rt_ptr, void *instance_ptr);

#endif /* FLUXA_EMBEDDED */
#endif /* FLUXA_STD_FLXTHREAD_H */
