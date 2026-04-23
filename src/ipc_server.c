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
#include <sys/stat.h>
#include <fcntl.h>
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

    if (ipc_recv_timed(client_fd, &req, sizeof req) < 0) {
#ifdef FLUXA_SECURE
        /* Short packet — primary flood vector. Don't increment during HARD
         * drain (immune period) to prevent attacker extending the timer. */
        if (srv->rescue_mode != RESCUE_HARD) {
            srv->invalid_burst++;
            if (srv->invalid_burst >= IPC_BURST_THRESHOLD &&
                srv->rescue_mode == RESCUE_NONE) {
                srv->rescue_mode = RESCUE_SOFT;
                srv->window_start = time(NULL);
                fprintf(stderr,
                    "[fluxa] ipc: RESCUE_SOFT activated — %d malformed "
                    "packets (flood detected)\n",
                    srv->invalid_burst);
            }
        }
#endif
        return;
    }

    if (!ipc_request_valid(&req)) {
#ifdef FLUXA_SECURE
        if (srv->rescue_mode != RESCUE_HARD) {
            srv->invalid_burst++;
            if (srv->invalid_burst >= IPC_BURST_THRESHOLD &&
                srv->rescue_mode == RESCUE_NONE) {
                srv->rescue_mode = RESCUE_SOFT;
                srv->window_start = time(NULL);
                fprintf(stderr,
                    "[fluxa] ipc: RESCUE_SOFT activated — %d invalid magic "
                    "packets (insider flood detected)\n",
                    srv->invalid_burst);
            }
        }
        if (srv->rescue_mode != RESCUE_NONE) {
            /* Silent drop */
        } else {
            resp.magic  = IPC_MAGIC;
            resp.version = IPC_VERSION;
            resp.seq    = req.seq;
            resp.status = IPC_STATUS_ERR_MAGIC;
            ipc_send_all(client_fd, &resp, sizeof resp);
        }
#else
        resp.magic   = IPC_MAGIC;
        resp.version = IPC_VERSION;
        resp.seq     = req.seq;
        resp.status  = IPC_STATUS_ERR_MAGIC;
        ipc_send_all(client_fd, &resp, sizeof resp);
#endif
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

        /* Use live_rt for fresh values when runtime is inside a loop.
         * Falls back to snapshot when live_rt is NULL (between reloads). */
        Runtime *lrt = (Runtime *)view->live_rt;

        int var_count = lrt ? lrt->prst_pool.count
                             : view->prst_snapshot.count;

        for (int ei = 0; ei < var_count; ei++) {
            IpcResponse ev;
            memset(&ev, 0, sizeof ev);
            ev.magic   = IPC_MAGIC;
            ev.version = IPC_VERSION;
            ev.seq     = req.seq;
            ev.status  = IPC_STATUS_EXPLAIN_VAR;

            /* Pick entry from live pool or snapshot */
            const char *name;
            Value v;
            if (lrt) {
                PrstEntry *pe = &lrt->prst_pool.entries[ei];
                name = pe->name;
                /* Read from stack if available — freshest value */
                int off = pe->stack_offset;
                if (off >= 0 && off < lrt->stack_size &&
                    lrt->stack[off].type != VAL_NIL)
                    v = lrt->stack[off];
                else
                    v = pe->value;
            } else {
                PrstEntry *pe = &view->prst_snapshot.entries[ei];
                name = pe->name;
                v    = pe->value;
            }

            memcpy(ev.name, name, IPC_VAR_NAME_MAX - 1);
            ev.name[IPC_VAR_NAME_MAX - 1] = '\0';

            switch (v.type) {
                case VAL_INT:
                    ev.type_tag = IPC_TYPE_INT;
                    ev.i_val    = (int64_t)v.as.integer;
                    snprintf(ev.message, sizeof ev.message,
                             "%.50s  int  = %ld", name, v.as.integer);
                    break;
                case VAL_FLOAT:
                    ev.type_tag = IPC_TYPE_FLOAT;
                    ev.f_val    = v.as.real;
                    snprintf(ev.message, sizeof ev.message,
                             "%.50s  float  = %g", name, v.as.real);
                    break;
                case VAL_BOOL:
                    ev.type_tag = IPC_TYPE_BOOL;
                    ev.b_val    = (uint8_t)(v.as.boolean ? 1 : 0);
                    snprintf(ev.message, sizeof ev.message,
                             "%.50s  bool  = %s", name,
                             v.as.boolean ? "true" : "false");
                    break;
                case VAL_STRING:
                    ev.type_tag = IPC_TYPE_STR;
                    snprintf(ev.message, sizeof ev.message,
                             "%.50s  str  = \"%.50s\"", name,
                             v.as.string ? v.as.string : "");
                    break;
                default:
                    ev.type_tag = IPC_TYPE_NIL;
                    snprintf(ev.message, sizeof ev.message,
                             "%.50s  nil", name);
                    break;
            }
            ipc_send_all(client_fd, &ev, sizeof ev);
        }

        /* DONE packet — deps from live_rt graph or snapshot */
        IpcResponse done;
        memset(&done, 0, sizeof done);
        done.magic   = IPC_MAGIC;
        done.version = IPC_VERSION;
        done.seq     = req.seq;
        done.status  = IPC_STATUS_EXPLAIN_DONE;

        if (lrt) {
            done.cycle_count = (int32_t)lrt->cycle_count;
            done.prst_count  = (int32_t)lrt->prst_pool.count;
            done.err_count   = (int32_t)lrt->err_stack.count;
            done.mode        = (uint8_t)lrt->mode;
            done.dry_run     = (uint8_t)lrt->dry_run;
        } else {
            done.cycle_count = (int32_t)view->cycle_count;
            done.prst_count  = (int32_t)view->prst_snapshot.count;
            done.err_count   = (int32_t)view->err_count;
            done.mode        = (uint8_t)view->mode;
            done.dry_run     = (uint8_t)view->dry_run;
        }

        /* Pack dep graph into message — all deps, one per line, truncated */
        int dep_count = lrt ? lrt->prst_graph.count
                             : view->prst_graph_snapshot.count;
        if (dep_count == 0) {
            snprintf(done.message, sizeof done.message,
                     "none — state is consistent with the code");
        } else {
            int pos = 0;
            for (int di = 0; di < dep_count; di++) {
                const char *pn = lrt
                    ? lrt->prst_graph.deps[di].prst_name
                    : view->prst_graph_snapshot.deps[di].prst_name;
                const char *ctx = lrt
                    ? lrt->prst_graph.deps[di].reader_ctx
                    : view->prst_graph_snapshot.deps[di].reader_ctx;
                int rem = (int)sizeof(done.message) - pos - 1;
                if (rem <= 0) break;
                int n = snprintf(done.message + pos, (size_t)rem,
                                 "%s%s <- %s",
                                 di > 0 ? ", " : "", pn, ctx);
                if (n > 0) pos += (n < rem ? n : rem - 1);
            }
        }

        pthread_mutex_unlock(&view->mu);
        ipc_send_all(client_fd, &done, sizeof done);
        return;
    }

    /* ── UPDATE — Runtime Update Protocol (Sprint 13) ── */
    case IPC_OP_UPDATE: {
        /* SECURITY: IPC_OP_UPDATE triggers execve — highest-privilege operation.
         * Enforces UID check unconditionally (even in non-SECURE builds).
         * Error messages to client are intentionally generic to prevent
         * filesystem oracle attacks and state probing.
         * Details are logged to stderr (local, operator-visible only). */

        /* 0. UID check — ALWAYS, regardless of FLUXA_SECURE mode.
         * IPC socket is 0600 so only the owner can connect, but we
         * double-check at the opcode level for defense in depth. */
        if (!check_peer_uid(client_fd)) {
            fprintf(stderr,
                "[fluxa] update: REJECTED — UID mismatch (privilege escalation attempt)\n");
            resp.status = IPC_STATUS_ERR_AUTH;
            snprintf(resp.message, sizeof resp.message, "update: permission denied");
            break;
        }

        /* Extract new binary path */
        char new_bin[IPC_VAR_NAME_MAX];
        strncpy(new_bin, req.name, IPC_VAR_NAME_MAX - 1);
        new_bin[IPC_VAR_NAME_MAX - 1] = '\0';

        if (new_bin[0] == '\0') {
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message, "update: invalid request");
            break;
        }

        /* Path traversal guard — must be absolute and no ".." components */
        if (new_bin[0] != '/' || strstr(new_bin, "..")) {
            fprintf(stderr,
                "[fluxa] update: REJECTED — path traversal or relative path: %s\n",
                new_bin);
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message, "update: invalid binary path");
            break;
        }

        /* 1. Verify binary — details to stderr only, generic error to client */
        struct stat st;
        if (stat(new_bin, &st) != 0 || !(st.st_mode & S_IXUSR)) {
            fprintf(stderr,
                "[fluxa] update: binary check failed: %s\n", new_bin);
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message, "update: binary validation failed");
            break;
        }

        /* 2. Get live runtime */
        pthread_mutex_lock(&view->mu);
        Runtime *lrt = (Runtime *)view->live_rt;
        if (!lrt) {
            pthread_mutex_unlock(&view->mu);
            fprintf(stderr, "[fluxa] update: no active runtime\n");
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message, "update: runtime not ready");
            break;
        }

        /* 3. Safe point check — NOT at safe point means caller should retry.
         * Use a distinct message the CLI can detect without leaking depth values. */
        if (lrt->call_depth != 0 || lrt->danger_depth != 0) {
            pthread_mutex_unlock(&view->mu);
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            /* "retry" keyword is what run_update() in main.c detects */
            snprintf(resp.message, sizeof resp.message,
                     "update: not at safe point — retry");
            break;
        }

#ifdef FLUXA_SECURE
        /* 4. FLUXA_SECURE: require detached .sig file alongside new binary.
         * Generic error to client — no path or filename leaked. */
        if (lrt->config.security.mode != FLUXA_SEC_MODE_OFF) {
            char sig_file[600];
            snprintf(sig_file, sizeof sig_file, "%s.sig", new_bin);
            struct stat sig_st;
            if (stat(sig_file, &sig_st) != 0) {
                pthread_mutex_unlock(&view->mu);
                fprintf(stderr,
                    "[fluxa] update: FLUXA_SECURE — signature file missing: %s.sig\n",
                    new_bin);
                resp.status = IPC_STATUS_ERR_UNKNOWN;
                snprintf(resp.message, sizeof resp.message,
                         "update: signature verification failed");
                break;
            }
            fprintf(stderr,
                "[fluxa] update: FLUXA_SECURE — signature file present\n");
        }
#endif

        /* 5. Serialize prst pool to temp snapshot file */
        char snap_path[256];
        snprintf(snap_path, sizeof snap_path,
                 "/tmp/fluxa-update-%d.snap", (int)getpid());

        void  *pool_buf = NULL;
        size_t pool_sz  = 0;
        if (!prst_pool_serialize(&lrt->prst_pool, &pool_buf, &pool_sz)) {
            pthread_mutex_unlock(&view->mu);
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message,
                     "update: prst pool serialization failed");
            break;
        }

        FILE *snap_f = fopen(snap_path, "wb");
        if (!snap_f) {
            free(pool_buf);
            pthread_mutex_unlock(&view->mu);
            resp.status = IPC_STATUS_ERR_UNKNOWN;
            snprintf(resp.message, sizeof resp.message,
                     "update: snapshot write failed");  /* path logged to stderr */
            break;
        }
        fwrite(pool_buf, 1, pool_sz, snap_f);
        fclose(snap_f);
        free(pool_buf);

        fprintf(stderr,
                "[fluxa] update: snapshot written (%zu bytes) → %s\n",
                pool_sz, snap_path);

        /* 6. Reply to client with snapshot path BEFORE execve */
        resp.status = IPC_STATUS_OK;
        snprintf(resp.message, sizeof resp.message,
                 "update: executing new binary, snapshot written");
        ipc_send_all(client_fd, &resp, sizeof resp);

        /* 7. Preserve original argv so new binary gets same arguments.
         * We pass the runtime script path via environment instead. */

        /* 8. Build env for new process: inherit everything + snapshot path */
        /* Count existing env vars */
        extern char **environ;
        int envc = 0;
        if (environ) while (environ[envc]) envc++;

        char **new_env = (char **)malloc((size_t)(envc + 3) * sizeof(char *));
        for (int ei = 0; ei < envc; ei++) new_env[ei] = environ[ei];

        /* FLUXA_RESTART_SNAPSHOT: prst state for the new binary */
        char snap_env[300];
        snprintf(snap_env, sizeof snap_env,
                 "FLUXA_RESTART_SNAPSHOT=%s", snap_path);
        new_env[envc]   = snap_env;
        new_env[envc+1] = NULL;

        pthread_mutex_unlock(&view->mu);

        /* 9. Reconstruct argv — new binary replaces argv[0], rest is same.
         * We derive the current args from /proc/self/cmdline on Linux. */
        char  cmdline_buf[4096];
        char *argv_ptrs[64];
        int   argc_new = 0;
        int   cmdline_fd = open("/proc/self/cmdline", O_RDONLY);
        if (cmdline_fd >= 0) {
            ssize_t nr = read(cmdline_fd, cmdline_buf, sizeof cmdline_buf - 1);
            close(cmdline_fd);
            if (nr > 0) {
                cmdline_buf[nr] = '\0';
                /* Split on null bytes */
                char *p = cmdline_buf;
                argv_ptrs[argc_new++] = new_bin; /* argv[0] = new binary */
                p += strlen(p) + 1;              /* skip old argv[0] */
                while (p < cmdline_buf + nr && argc_new < 63) {
                    if (*p) argv_ptrs[argc_new++] = p;
                    p += strlen(p) + 1;
                }
            }
        }
        if (argc_new == 0) {
            /* Fallback: minimal argv */
            argv_ptrs[argc_new++] = new_bin;
        }
        argv_ptrs[argc_new] = NULL;

        fprintf(stderr,
                "[fluxa] update: execve(%s) with FLUXA_RESTART_SNAPSHOT=%s\n",
                new_bin, snap_path);

        /* 10. execve — this process is replaced by the new binary */
        execve(new_bin, argv_ptrs, new_env);

        /* execve only returns on error */
        free(new_env);
        fprintf(stderr, "[fluxa] update: execve failed: %s\n", strerror(errno));
        return; /* client already got the OK reply — nothing more to send */
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

#ifdef FLUXA_SECURE
    /* ── Hardened server loop (FLUXA_SECURE=1) ───────────────────────────
     * Two-level RESCUE system prevents attacker from keeping system in
     * permanent degraded state:
     *   RESCUE_SOFT: first burst → silent drop on bad pkts, decays naturally
     *   RESCUE_HARD: second burst while in SOFT → immune drain timer,
     *                attacker cannot extend or reset it                    */
    srv->window_start        = time(NULL);
    srv->drain_start         = 0;
    srv->invalid_burst       = 0;
    srv->rescue_mode         = RESCUE_NONE;
    srv->active_conns        = 0;
    int max_conns  = (srv->ipc_max_conns > 0)
                     ? srv->ipc_max_conns : IPC_MAX_CONNS_DEFAULT;
    int timeout_ms = (srv->handshake_timeout_ms > 0)
                     ? srv->handshake_timeout_ms : IPC_TIMEOUT_MS;

    while (srv->running) {
        time_t now = time(NULL);

        /* RESCUE_HARD: immune drain — attacker cannot extend this timer */
        if (srv->rescue_mode == RESCUE_HARD &&
            (now - srv->drain_start) >= IPC_RESCUE_DRAIN_SEC) {
            srv->rescue_mode   = RESCUE_NONE;
            srv->invalid_burst = 0;
            srv->window_start  = now;
            fprintf(stderr,
                "[fluxa] ipc: RESCUE_HARD cleared after %ds immune drain\n",
                IPC_RESCUE_DRAIN_SEC);
        }

        /* RESCUE_SOFT → RESCUE_HARD: escalate on sustained attack */
        if (srv->rescue_mode == RESCUE_SOFT &&
            srv->invalid_burst >= IPC_BURST_THRESHOLD * 2) {
            srv->rescue_mode = RESCUE_HARD;
            srv->drain_start = now;
            fprintf(stderr,
                "[fluxa] ipc: RESCUE_HARD activated — sustained flood, "
                "immune drain for %ds\n", IPC_RESCUE_DRAIN_SEC);
        }

        /* Leaky bucket: decay by half each window.
         * RESCUE_SOFT self-clears when burst drains to zero. */
        if ((now - srv->window_start) >= IPC_RATE_WINDOW_SEC) {
            srv->invalid_burst = srv->invalid_burst / 2;
            srv->window_start  = now;
            if (srv->rescue_mode == RESCUE_SOFT && srv->invalid_burst == 0)
                srv->rescue_mode = RESCUE_NONE;
        }

        fd_set fds; FD_ZERO(&fds); FD_SET(srv->server_fd, &fds);
        struct timeval tv = { 0, 200000 };
        if (select(srv->server_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        int client_fd = accept(srv->server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        /* Apply runtime timeout from config */
        struct timeval rcv_tv = { 0, (suseconds_t)(timeout_ms * 1000) };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof rcv_tv);

        /* AC 4.1: Connection cap (runtime value from config) */
        if (srv->active_conns >= max_conns) {
            srv->invalid_burst++;
            close(client_fd);
            continue;
        }

        /* AC 1.2 + UID check */
        if (!check_peer_uid(client_fd)) {
            /* RESCUE_HARD: don't increment burst — drain timer immune */
            if (srv->rescue_mode != RESCUE_HARD) {
                srv->invalid_burst++;
                if (srv->invalid_burst >= IPC_BURST_THRESHOLD &&
                    srv->rescue_mode == RESCUE_NONE) {
                    srv->rescue_mode = RESCUE_SOFT;
                    fprintf(stderr,
                        "[fluxa] ipc: RESCUE_SOFT activated — %d invalid "
                        "connections (flood detected)\n",
                        srv->invalid_burst);
                }
            }
            /* Silent drop in any RESCUE level; ERR_AUTH only when clean */
            if (srv->rescue_mode != RESCUE_NONE) {
                close(client_fd);
            } else {
                IpcResponse resp; memset(&resp, 0, sizeof resp);
                resp.magic  = IPC_MAGIC;
                resp.status = IPC_STATUS_ERR_AUTH;
                ipc_send_all(client_fd, &resp, sizeof resp);
                close(client_fd);
            }
            continue;
        }

        /* AC 4.2: Track active connections */
        srv->active_conns++;

        /* Any RESCUE level: peek magic — silent drop bad packets.
         * Valid magic always dispatched so operator commands work. */
        if (srv->rescue_mode != RESCUE_NONE) {
            uint16_t peek_magic = 0;
            ssize_t peeked = recv(client_fd, &peek_magic, sizeof(peek_magic),
                                  MSG_PEEK | MSG_DONTWAIT);
            if (peeked == sizeof(peek_magic) && peek_magic != IPC_MAGIC) {
                close(client_fd);
                srv->active_conns--;
                continue;
            }
        }

        dispatch(srv, client_fd);
        srv->active_conns--;
        close(client_fd);
    }

#else  /* !FLUXA_SECURE — standard dev/default server loop */

    while (srv->running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(srv->server_fd, &fds);
        struct timeval tv = { 0, 200000 };
        if (select(srv->server_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        int client_fd = accept(srv->server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        if (!check_peer_uid(client_fd)) {
            IpcResponse resp; memset(&resp, 0, sizeof resp);
            resp.magic  = IPC_MAGIC;
            resp.status = IPC_STATUS_ERR_AUTH;
            ipc_send_all(client_fd, &resp, sizeof resp);
            close(client_fd);
            continue;
        }

        dispatch(srv, client_fd);
        close(client_fd);
    }

#endif /* FLUXA_SECURE */

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
