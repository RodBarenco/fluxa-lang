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
 * The VM updates it at every top-level safe point via ipc_rtview_update(). */
struct IpcRtView {
    volatile int    ready;
    pthread_mutex_t mu;
    long            cycle_count;
    int             prst_count;
    int             err_count;
    int             mode;
    int             dry_run;
    PrstPool        prst_snapshot;
    ErrStack        err_snapshot;
};

static inline IpcRtView *ipc_rtview_create(void) {
    IpcRtView *v = (IpcRtView *)calloc(1, sizeof(IpcRtView));
    if (!v) return NULL;
    pthread_mutex_init(&v->mu, NULL);
    return v;
}

static inline void ipc_rtview_destroy(IpcRtView *v) {
    if (!v) return;
    if (v->prst_snapshot.entries) free(v->prst_snapshot.entries);
    pthread_mutex_destroy(&v->mu);
    free(v);
}

/* Start IPC server background thread. rt_view = IpcRtView* from ipc_rtview_create(). */
IpcServer  *ipc_server_start(void *rt_view);

/* Stop server, close socket, remove lock file. */
void        ipc_server_stop(IpcServer *srv);

/* Copy live Runtime state into the stable view at each safe point. */
void        ipc_rtview_update(IpcRtView *view, Runtime *rt);

/* Apply any queued fluxa set command. No-op when nothing pending. */
void        ipc_apply_pending_set(Runtime *rt);

#endif /* FLUXA_IPC_SERVER_H */
