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
} PendingSet;

/* One pending slot is enough — SET is serialized via the safe point loop.
 * A second SET before the first is applied overwrites the first (last-write-wins).
 * This is intentional: the operator is providing a new target value. */
static PendingSet g_pending_set;

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
        int found = prst_pool_get(&view->prst_snapshot, req.name, &v);
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

        resp.status = IPC_STATUS_OK;
        snprintf(resp.message, sizeof resp.message,
                 "queued: %.60s (applied at next safe point)", req.name);
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
void ipc_apply_pending_set(Runtime *rt) {
    if (!g_pending_set.pending) return;
    __sync_synchronize();

    Value v;
    switch (g_pending_set.type_tag) {
        case IPC_TYPE_INT:   v = val_int((long)g_pending_set.i_val);     break;
        case IPC_TYPE_FLOAT: v = val_float(g_pending_set.f_val);         break;
        case IPC_TYPE_BOOL:  v = val_bool(g_pending_set.b_val != 0);     break;
        default: g_pending_set.pending = 0; return;
    }

    prst_pool_set(&rt->prst_pool, g_pending_set.name, v, NULL);
    /* Also update live scope so the running program sees the new value immediately */
    Value existing;
    if (scope_get(&rt->scope, g_pending_set.name, &existing) == 1) {
        scope_set(&rt->scope, g_pending_set.name, v);
    }

    __sync_synchronize();
    g_pending_set.pending = 0;

    /* g_ipc_view is refreshed by the runtime loop after this call returns */
}
