/* ipc_server.h — Fluxa IPC Server public API (Sprint 9)
 *
 * Owns the full IpcRtView definition — needs pthread_mutex_t, PrstPool,
 * ErrStack which are available here after the correct include chain.
 *
 * Include order for callers: fluxa_ipc.h is included by runtime.h;
 * include ipc_server.h after runtime.h.
 */
#ifndef FLUXA_IPC_SERVER_H
#define FLUXA_IPC_SERVER_H

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdlib.h>
#include "fluxa_ipc.h"
#include "runtime.h"

/* ── IpcRtView — stable snapshot for IPC server in -dev mode ─────────────── */
/* In -dev mode the Runtime struct is re-created on the C stack for every
 * reload.  The IPC server holds a pointer to this stable heap struct instead.
 * The VM updates it at every top-level safe point via ipc_rtview_update().
 *
 * live_rt: direct pointer to the executing Runtime — set at each safe point,
 * cleared to NULL when the runtime exits.  The SET handler uses this to write
 * pool + scope + stack[offset] immediately, without waiting for the next
 * top-level safe point (which never comes inside an infinite loop).
 * Reads/writes to live_rt are protected by mu. */
struct IpcRtView {
    volatile int      ready;
    pthread_mutex_t   mu;
    long              cycle_count;
    int               prst_count;
    int               err_count;
    int               mode;
    int               dry_run;
    int               pid;
    volatile Runtime *live_rt;            /* direct pointer — see note above */
    PrstPool          prst_snapshot;
    PrstGraph         prst_graph_snapshot;
    ErrStack          err_snapshot;
};

static inline IpcRtView *ipc_rtview_create(void) {
    IpcRtView *v = (IpcRtView *)calloc(1, sizeof(IpcRtView));
    if (!v) return NULL;
    pthread_mutex_init(&v->mu, NULL);
    v->pid = getpid();
    return v;
}

static inline void ipc_rtview_destroy(IpcRtView *v) {
    if (!v) return;
    if (v->prst_snapshot.entries) free(v->prst_snapshot.entries);
    if (v->prst_graph_snapshot.deps) free(v->prst_graph_snapshot.deps);
    pthread_mutex_destroy(&v->mu);
    free(v);
}

/* Start IPC server background thread. rt_view = IpcRtView* from ipc_rtview_create(). */
IpcServer  *ipc_server_start(void *rt_view);

/* Stop server, close socket, remove lock file. */
void        ipc_server_stop(IpcServer *srv);

/* Copy live Runtime state into the stable view at each safe point.
 * Also sets live_rt so SET can write directly to the running Runtime. */
void        ipc_rtview_update(IpcRtView *view, Runtime *rt);

/* Clear live_rt when runtime exits — prevents dangling pointer writes.
 * Called by runtime_exec / runtime_apply before teardown. */
void        ipc_rtview_clear_live(IpcRtView *view);

/* Apply any queued SET that arrived while live_rt was NULL (between reloads).
 * No-op if the SET was already applied directly by the IPC dispatch. */
void        ipc_apply_pending_set(Runtime *rt);

#endif /* FLUXA_IPC_SERVER_H */
