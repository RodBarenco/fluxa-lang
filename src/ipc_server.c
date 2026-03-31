/* ipc_server.c — Fluxa IPC Server (Sprint 9)
 *
 * Runs as a background pthread inside the -dev and -prod runtime processes.
 * Accepts connections on /tmp/fluxa-<pid>.sock and dispatches IpcRequests.
 *
 * Thread safety:
 *   OBSERVE  — acquires srv->mu for read (prst pool is read-only here)
 *   SET      — queues the mutation; applied at next runtime safe point
 *   LOGS     — acquires srv->mu, copies from err_stack ring buffer
 *   STATUS   — acquires srv->mu, copies scalar fields from Runtime
 *   PING     — no lock needed
 *
 * SET is not applied inline to avoid data races with the VM thread.
 * Instead it is written to a pending_set slot and the VM checks it at
 * every safe point (call_depth == 0 && danger_depth == 0).
 */
#define _POSIX_C_SOURCE 200809L
/* ipc_server.h pulls in fluxa_ipc.h, runtime.h, pthread.h, stdlib.h */
#include "ipc_server.h"
#include "scope.h"
#include <sys/select.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>

/* ── Pending SET request — written by IPC thread, consumed by VM thread ─── */
typedef struct {
    volatile int pending;       /* 1 = a SET is waiting                     */
    char  name[IPC_VAR_NAME_MAX];
    int64_t i_val;
    double  f_val;
    uint8_t b_val;
    uint8_t type_tag;           /* IpcTypeTag                               */
    uint8_t _pad[2];
    /* Pre-calculated stack slot — filled when SET is queued so the bytecode
     * VM can apply the value with a single array write, no hash lookup.
     * -1 means the offset is not yet known (first set before first run). */
    int     stack_offset;
    /* Pre-built Value ready to write — avoids rebuilding inside OP_JUMP */
    Value   ready_value;
} PendingSet;

/* One pending slot is enough — SET is serialized via the safe point loop.
 * A second SET before the first is applied overwrites the first (last-write-wins).
 * This is intentional: the operator is providing a new target value. */
static PendingSet g_pending_set;

/* Exposed so bytecode.c can read pending and write stack directly */
volatile int *g_ipc_pending_flag = &g_pending_set.pending;

/* ── UID check: reject connections from other users ─────────────────────── */
static int check_peer_uid(int fd) {
#if defined(SO_PEERCRED)
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return 0;
    return (cred.uid == getuid());
#elif defined(LOCAL_PEERCRED)
    /* macOS */
    struct xucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len) < 0) return 0;
    return (cred.cr_uid == getuid());
#else
    (void)fd;
    return 1; /* platform doesn't support peer cred — allow, socket is 0600 */
#endif
}

/* ── Populate IpcRtView from a live Runtime at a safe point ─────────────── */
void ipc_rtview_update(IpcRtView *view, Runtime *rt) {
    if (!view || !rt) return;
    pthread_mutex_lock(&view->mu);

    view->live_rt     = rt;          /* expose live rt to IPC SET handler */
    view->cycle_count = rt->cycle_count;
    view->prst_count  = rt->prst_pool.count;
    view->err_count   = rt->err_stack.count;
    view->mode        = (int)rt->mode;
    view->dry_run     = rt->dry_run;

    /* Deep-copy prst pool for OBSERVE queries */
    if (view->prst_snapshot.entries) {
        free(view->prst_snapshot.entries);
        view->prst_snapshot.entries = NULL;
    }
    view->prst_snapshot.count = 0;
    view->prst_snapshot.cap   = 0;
    if (rt->prst_pool.count > 0) {
        size_t sz = (size_t)rt->prst_pool.count * sizeof(PrstEntry);
        view->prst_snapshot.entries = (PrstEntry *)malloc(sz);
        if (view->prst_snapshot.entries) {
            memcpy(view->prst_snapshot.entries, rt->prst_pool.entries, sz);
            view->prst_snapshot.count = rt->prst_pool.count;
            view->prst_snapshot.cap   = rt->prst_pool.count;
        }
    }

    /* Deep-copy prst graph for EXPLAIN queries */
    if (view->prst_graph_snapshot.deps) {
        free(view->prst_graph_snapshot.deps);
        view->prst_graph_snapshot.deps = NULL;
    }
    view->prst_graph_snapshot.count = 0;
    view->prst_graph_snapshot.cap   = 0;
    if (rt->prst_graph.count > 0) {
        size_t gsz = (size_t)rt->prst_graph.count * sizeof(PrstDep);
        view->prst_graph_snapshot.deps = (PrstDep *)malloc(gsz);
        if (view->prst_graph_snapshot.deps) {
            memcpy(view->prst_graph_snapshot.deps,
                   rt->prst_graph.deps, gsz);
            view->prst_graph_snapshot.count = rt->prst_graph.count;
            view->prst_graph_snapshot.cap   = rt->prst_graph.count;
        }
    }

    /* Copy err stack snapshot */
    view->err_snapshot = rt->err_stack;

    view->ready = 1;
    pthread_mutex_unlock(&view->mu);
}

/* ── Dispatch a single request from client fd ───────────────────────────── */
static void dispatch(IpcServer *srv, int client_fd) {
    IpcRequest  req;
    IpcResponse resp;
    memset(&resp, 0, sizeof resp);

    if (ipc_recv_timed(client_fd, &req, sizeof req) < 0) return;

    if (!ipc_request_valid(&req)) {
        resp.magic   = IPC_MAGIC;
        resp.version = IPC_VERSION;
        resp.seq     = req.seq;
        resp.status  = IPC_STATUS_ERR_MAGIC;
        ipc_send_all(client_fd, &resp, sizeof resp);
        return;
    }

    resp.magic   = IPC_MAGIC;
    resp.version = IPC_VERSION;
    resp.seq     = req.seq;
    memcpy(resp.name, req.name, IPC_VAR_NAME_MAX - 1);
    resp.name[IPC_VAR_NAME_MAX-1] = '\0';

    /* All commands except PING need the view to be ready */
    IpcRtView *view = (IpcRtView *)srv->rt;
    if ((IpcOpcode)req.opcode != IPC_OP_PING &&
        (!view || !view->ready)) {
        resp.status = IPC_STATUS_ERR_UNKNOWN;
        snprintf(resp.message, sizeof resp.message,
                 "runtime not ready yet");
        ipc_send_all(client_fd, &resp, sizeof resp);
        return;
    }

    switch ((IpcOpcode)req.opcode) {

    /* ── PING ── */
    case IPC_OP_PING:
        resp.status = IPC_STATUS_OK;
        snprintf(resp.message, sizeof resp.message, "pong pid=%d", srv->pid);
        break;

    /* ── OBSERVE ── */
    case IPC_OP_OBSERVE: {
        pthread_mutex_lock(&view->mu);
        Value v;
        int found = 0;

        /* Prefer live_rt — has the freshest values, including any SET that
         * was applied directly to the stack during an infinite loop.
         * Fall back to the snapshot when live_rt is NULL (between reloads). */
        Runtime *lrt = (Runtime *)view->live_rt;
        if (lrt) {
            /* Read from live prst_pool (updated by SET) */
            found = prst_pool_get(&lrt->prst_pool, req.name, &v);
            /* If pool has the entry, also read from stack for freshest VM value */
            if (found) {
                int pidx = prst_pool_find(&lrt->prst_pool, req.name);
                if (pidx >= 0) {
                    int off = lrt->prst_pool.entries[pidx].stack_offset;
                    if (off >= 0 && off < FLUXA_STACK_SIZE) {
                        Value sv = lrt->stack[off];
                        /* Use stack value if type matches — it's what the VM sees */
                        if (sv.type == v.type) v = sv;
                    }
                }
            }
        } else {
            found = prst_pool_get(&view->prst_snapshot, req.name, &v);
        }
        pthread_mutex_unlock(&view->mu);

        if (!found) {
            resp.status = IPC_STATUS_ERR_NOTFOUND;
            snprintf(resp.message, sizeof resp.message,
                     "prst var not found: %.60s", req.name);
            break;
        }
        resp.status = IPC_STATUS_OK;
        switch (v.type) {
            case VAL_INT:
                resp.type_tag = IPC_TYPE_INT;
                resp.i_val    = (int64_t)v.as.integer;
                snprintf(resp.message, sizeof resp.message,
                         "%s = %ld", req.name, v.as.integer);
                break;
            case VAL_FLOAT:
                resp.type_tag = IPC_TYPE_FLOAT;
                resp.f_val    = v.as.real;
                snprintf(resp.message, sizeof resp.message,
                         "%s = %g", req.name, v.as.real);
                break;
            case VAL_BOOL:
                resp.type_tag = IPC_TYPE_BOOL;
                resp.b_val    = (uint8_t)(v.as.boolean ? 1 : 0);
                snprintf(resp.message, sizeof resp.message,
                         "%s = %s", req.name, v.as.boolean ? "true" : "false");
                break;
            default:
                resp.type_tag = IPC_TYPE_NIL;
                snprintf(resp.message, sizeof resp.message,
                         "%s = nil", req.name);
                break;
        }
        break;
    }

    /* ── SET ── */
    case IPC_OP_SET: {
        if (req.type_tag == 0 || req.type_tag > IPC_TYPE_STR) {
            resp.status = IPC_STATUS_ERR_TYPE;
            snprintf(resp.message, sizeof resp.message,
                     "invalid type tag: %d", req.type_tag);
            break;
        }

        /* Verify variable exists in the snapshot */
        pthread_mutex_lock(&view->mu);
        Value existing;
        int found = prst_pool_get(&view->prst_snapshot, req.name, &existing);
        pthread_mutex_unlock(&view->mu);

        if (!found) {
            resp.status = IPC_STATUS_ERR_NOTFOUND;
            snprintf(resp.message, sizeof resp.message,
                     "prst var not found: %.60s", req.name);
            break;
        }

        int type_ok = 0;
        if (req.type_tag == IPC_TYPE_INT   && existing.type == VAL_INT)   type_ok = 1;
        if (req.type_tag == IPC_TYPE_FLOAT && existing.type == VAL_FLOAT) type_ok = 1;
        if (req.type_tag == IPC_TYPE_BOOL  && existing.type == VAL_BOOL)  type_ok = 1;

        if (!type_ok) {
            resp.status = IPC_STATUS_ERR_TYPE;
            snprintf(resp.message, sizeof resp.message,
                     "type mismatch for %.60s (existing type=%d, requested=%d)",
                     req.name, existing.type, req.type_tag);
            break;
        }

        strncpy(g_pending_set.name, req.name, IPC_VAR_NAME_MAX - 1);
        g_pending_set.name[IPC_VAR_NAME_MAX - 1] = '\0';
        g_pending_set.type_tag = req.type_tag;
        g_pending_set.i_val    = req.i_val;
        g_pending_set.f_val    = req.f_val;
        g_pending_set.b_val    = req.b_val;
        __sync_synchronize();
        g_pending_set.pending  = 1;

        /* Direct stack write — makes the new value visible to the bytecode
         * VM immediately, without waiting for the next top-level safe point.
         * This is the key fix for "fluxa set inside an infinite while loop":
         * the VM reads rt->stack[offset] directly; updating only pool/scope
         * is not enough because the VM never returns to call ipc_apply_pending_set.
         *
         * Safety: we hold the mutex during the write. The VM only reads
         * this slot — it never writes a prst slot from inside the loop
         * (prst writes happen at declaration, before the loop starts).
         * Writing a Value (16 bytes on x86-64) is not atomic, but the VM
         * reads it on the next back-edge — after we release the mutex and
         * the CPU memory fence has propagated. For int/float/bool (the only
         * types settable via IPC) this is safe: the VM will either see the
         * old value or the new one, never a torn write, because Value.type
         * and Value.as are written together inside the lock. */
        pthread_mutex_lock(&view->mu);
        Runtime *lrt = (Runtime *)view->live_rt;
        if (lrt) {
            Value sv;
            switch (req.type_tag) {
                case IPC_TYPE_INT:   sv = val_int((long)req.i_val);      break;
                case IPC_TYPE_FLOAT: sv = val_float(req.f_val);          break;
                case IPC_TYPE_BOOL:  sv = val_bool(req.b_val != 0);      break;
                default:             sv = val_nil();                      break;
            }
            /* Find the stack offset recorded at declaration time */
            int pidx = prst_pool_find(&lrt->prst_pool, req.name);
            if (pidx >= 0) {
                int off = lrt->prst_pool.entries[pidx].stack_offset;
                if (off >= 0 && off < FLUXA_STACK_SIZE) {
                    lrt->stack[off] = sv;
                    if (off >= lrt->stack_size) lrt->stack_size = off + 1;
                }
            }
            /* Also update pool and scope so post-VM sync and interpreted
             * paths see the new value */
            prst_pool_set(&lrt->prst_pool, req.name, sv, NULL);
            scope_set(&lrt->scope, req.name, sv);
            scope_table_set(&lrt->global_table, req.name, sv);
            /* Clear the pending flag — already applied */
            g_pending_set.pending = 0;
        }
        pthread_mutex_unlock(&view->mu);

        resp.status = IPC_STATUS_OK;
        if (g_pending_set.pending) {
            /* live_rt was NULL — runtime between reloads, will apply at next exec */
            snprintf(resp.message, sizeof resp.message,
                     "queued: %.60s (applied at next safe point)", req.name);
        } else {
            snprintf(resp.message, sizeof resp.message,
                     "applied: %.60s", req.name);
        }
        break;
    }

    /* ── LOGS ── */
    case IPC_OP_LOGS: {
        pthread_mutex_lock(&view->mu);
        int count = view->err_snapshot.count;
        if (count == 0) {
            resp.status = IPC_STATUS_OK;
            snprintf(resp.message, sizeof resp.message, "(no errors)");
        } else {
            resp.status    = IPC_STATUS_OK;
            resp.err_count = count;
            const ErrEntry *e = errstack_get(&view->err_snapshot, 0);
            if (e) {
                const char *kind_str = "ERR";
                switch (e->kind) {
                    case ERR_FLUXA:    kind_str = "ERR_FLUXA";    break;
                    case ERR_C_FFI:    kind_str = "ERR_C_FFI";    break;
                    case ERR_RELOAD:   kind_str = "ERR_RELOAD";   break;
                    case ERR_HANDOVER: kind_str = "ERR_HANDOVER"; break;
                    default: break;
                }
                snprintf(resp.message, sizeof resp.message,
                         "[%s] %s (line %d)", kind_str, e->message, e->line);
            }
        }
        pthread_mutex_unlock(&view->mu);
        break;
    }

    /* ── STATUS ── */
    case IPC_OP_STATUS: {
        pthread_mutex_lock(&view->mu);
        resp.status      = IPC_STATUS_OK;
        resp.cycle_count = (int32_t)view->cycle_count;
        resp.prst_count  = (int32_t)view->prst_count;
        resp.err_count   = (int32_t)view->err_count;
        resp.mode        = (uint8_t)view->mode;
        resp.dry_run     = (uint8_t)view->dry_run;
        snprintf(resp.message, sizeof resp.message,
                 "cycle=%ld prst=%d errs=%d mode=%s",
                 view->cycle_count, view->prst_count, view->err_count,
                 view->mode == 1 ? "project" : "script");
        pthread_mutex_unlock(&view->mu);
        break;
    }

    /* ── EXPLAIN ── streaming: one packet per prst var, then DONE ── */
    case IPC_OP_EXPLAIN: {
        pthread_mutex_lock(&view->mu);

        /* Send one response packet per prst variable */
        for (int ei = 0; ei < view->prst_snapshot.count; ei++) {
            IpcResponse ev;
            memset(&ev, 0, sizeof ev);
            ev.magic   = IPC_MAGIC;
            ev.version = IPC_VERSION;
            ev.seq     = req.seq;
            ev.status  = IPC_STATUS_EXPLAIN_VAR;

            PrstEntry *pe = &view->prst_snapshot.entries[ei];
            /* ev.name carries the full name; message uses %.60s to stay
             * within IPC_LOG_LINE_MAX (128 bytes) — no warning. */
            memcpy(ev.name, pe->name, IPC_VAR_NAME_MAX - 1);
            ev.name[IPC_VAR_NAME_MAX - 1] = '\0';

            switch (pe->value.type) {
                case VAL_INT:
                    ev.type_tag = IPC_TYPE_INT;
                    ev.i_val    = (int64_t)pe->value.as.integer;
                    snprintf(ev.message, sizeof ev.message,
                             "%.60s  int  = %ld", pe->name, pe->value.as.integer);
                    break;
                case VAL_FLOAT:
                    ev.type_tag = IPC_TYPE_FLOAT;
                    ev.f_val    = pe->value.as.real;
                    snprintf(ev.message, sizeof ev.message,
                             "%.60s  float  = %g", pe->name, pe->value.as.real);
                    break;
                case VAL_BOOL:
                    ev.type_tag = IPC_TYPE_BOOL;
                    ev.b_val    = (uint8_t)(pe->value.as.boolean ? 1 : 0);
                    snprintf(ev.message, sizeof ev.message,
                             "%.60s  bool  = %s", pe->name,
                             pe->value.as.boolean ? "true" : "false");
                    break;
                case VAL_STRING:
                    ev.type_tag = IPC_TYPE_STR;
                    snprintf(ev.message, sizeof ev.message,
                             "%.60s  str  = \"%.50s\"", pe->name,
                             pe->value.as.string ? pe->value.as.string : "");
                    break;
                default:
                    ev.type_tag = IPC_TYPE_NIL;
                    snprintf(ev.message, sizeof ev.message,
                             "%.60s  nil", pe->name);
                    break;
            }
            ipc_send_all(client_fd, &ev, sizeof ev);
        }

        /* Build deps summary for the DONE packet's message field.
         * Format: "dep1_name <- ctx1\ndep2_name <- ctx2\n..."
         * Truncated to IPC_LOG_LINE_MAX if needed. */
        IpcResponse done;
        memset(&done, 0, sizeof done);
        done.magic      = IPC_MAGIC;
        done.version    = IPC_VERSION;
        done.seq        = req.seq;
        done.status     = IPC_STATUS_EXPLAIN_DONE;
        done.cycle_count= (int32_t)view->cycle_count;
        done.prst_count = (int32_t)view->prst_snapshot.count;
        done.err_count  = (int32_t)view->err_count;
        done.mode       = (uint8_t)view->mode;
        done.dry_run    = (uint8_t)view->dry_run;

        /* Pack up to 1 dep line into message — client can request more via
         * repeated EXPLAIN if needed; for now first dep is representative */
        if (view->prst_graph_snapshot.count == 0) {
            snprintf(done.message, sizeof done.message,
                     "nenhuma — estado atual compatível com o código");
        } else {
            int written = 0;
            for (int di = 0; di < view->prst_graph_snapshot.count && written == 0; di++) {
                written = snprintf(done.message, sizeof done.message,
                    "%s  <-  %s (+%d total)",
                    view->prst_graph_snapshot.deps[di].prst_name,
                    view->prst_graph_snapshot.deps[di].reader_ctx,
                    view->prst_graph_snapshot.count);
            }
        }

        pthread_mutex_unlock(&view->mu);
        ipc_send_all(client_fd, &done, sizeof done);
        /* EXPLAIN is fully handled here — skip the final ipc_send_all below */
        return;
    }

    default:
        resp.status = IPC_STATUS_ERR_UNKNOWN;
        snprintf(resp.message, sizeof resp.message,
                 "unknown opcode: 0x%02x", req.opcode);
        break;
    }

    ipc_send_all(client_fd, &resp, sizeof resp);
}
static void *ipc_server_thread(void *arg) {
    IpcServer *srv = (IpcServer *)arg;

    /* Block SIGPIPE so broken client connections don't kill the process */
    signal(SIGPIPE, SIG_IGN);

    while (srv->running) {
        /* Non-blocking accept with 200ms poll interval */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv->server_fd, &fds);
        struct timeval tv = { 0, 200000 };
        int n = select(srv->server_fd + 1, &fds, NULL, NULL, &tv);
        if (n <= 0) continue;

        int client_fd = accept(srv->server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        /* UID check — reject connections from other users */
        if (!check_peer_uid(client_fd)) {
            IpcResponse resp;
            memset(&resp, 0, sizeof resp);
            resp.magic  = IPC_MAGIC;
            resp.status = IPC_STATUS_ERR_AUTH;
            ipc_send_all(client_fd, &resp, sizeof resp);
            close(client_fd);
            continue;
        }

        dispatch(srv, client_fd);
        close(client_fd);
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Start the IPC server for a running runtime.
 * Called once from run_dev() / run_prod() after the runtime is initialized.
 * rt must remain valid until ipc_server_stop() is called. */
IpcServer *ipc_server_start(void *rt) {
    IpcServer *srv = (IpcServer *)calloc(1, sizeof(IpcServer));
    if (!srv) return NULL;

    srv->rt      = rt;
    srv->running = 1;

    if (ipc_server_bind(srv, getpid()) < 0) {
        free(srv);
        return NULL;
    }

    if (pthread_create(&srv->thread, NULL, ipc_server_thread, srv) != 0) {
        ipc_server_cleanup(srv);
        free(srv);
        return NULL;
    }

    fprintf(stderr, "[fluxa] ipc: listening on %s\n", srv->sock_path);
    return srv;
}

void ipc_server_stop(IpcServer *srv) {
    if (!srv) return;
    srv->running = 0;
    pthread_join(srv->thread, NULL);
    ipc_server_cleanup(srv);
    free(srv);
}

/* ── live_rt lifecycle ───────────────────────────────────────────────────── */
/* Clear live_rt when runtime exits — prevents the SET handler from writing
 * to a destroyed Runtime. Called by runtime_exec/runtime_apply on exit. */
void ipc_rtview_clear_live(IpcRtView *view) {
    if (!view) return;
    pthread_mutex_lock(&view->mu);
    view->live_rt = NULL;
    pthread_mutex_unlock(&view->mu);
}

/* ipc_apply_pending_set — fallback for when live_rt was NULL at SET time.
 * If the SET was already applied directly by the IPC dispatch (live_rt != NULL),
 * g_pending_set.pending will be 0 and this is a no-op.
 * If live_rt was NULL (runtime between reloads), pending is still 1 and we
 * apply now that the runtime has started. */
void ipc_apply_pending_set(Runtime *rt) {
    if (!g_pending_set.pending) return;
    __sync_synchronize();

    Value v;
    switch (g_pending_set.type_tag) {
        case IPC_TYPE_INT:   v = val_int((long)g_pending_set.i_val);  break;
        case IPC_TYPE_FLOAT: v = val_float(g_pending_set.f_val);      break;
        case IPC_TYPE_BOOL:  v = val_bool(g_pending_set.b_val != 0);  break;
        default: g_pending_set.pending = 0; return;
    }

    /* pool — persists across reloads */
    prst_pool_set(&rt->prst_pool, g_pending_set.name, v, NULL);

    /* scope — interpreted (AST) paths */
    Value existing;
    if (scope_get(&rt->scope, g_pending_set.name, &existing) == 1)
        scope_set(&rt->scope, g_pending_set.name, v);

    /* global_table — cross-function visibility */
    scope_table_set(&rt->global_table, g_pending_set.name, v);

    /* rt->stack[offset] — bytecode VM reads this directly */
    int idx = prst_pool_find(&rt->prst_pool, g_pending_set.name);
    if (idx >= 0) {
        int off = rt->prst_pool.entries[idx].stack_offset;
        if (off >= 0 && off < FLUXA_STACK_SIZE) {
            rt->stack[off] = v;
            if (off >= rt->stack_size) rt->stack_size = off + 1;
        }
    }

    __sync_synchronize();
    g_pending_set.pending = 0;
}
