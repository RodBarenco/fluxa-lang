#ifndef FLUXA_STD_WSERVER_H
#define FLUXA_STD_WSERVER_H

/*
 * std.wserver — Resilient HTTP server for Fluxa-lang
 *
 * Backend: libmicrohttpd (GNU MHD) with epoll + thread pool.
 * Design:  opaque int handles. No dyn cursors exposed to Fluxa code.
 *
 * Uses MHD_USE_THREAD_PER_CONNECTION — each request gets its own OS thread.
 * MHD_OPTION_LISTENING_ADDRESS_REUSE allows fast port reuse in tests.
 * Fluxa workers (ft.new) control parallelism at the application level.
 * 
 *
 * HTTP methods: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS.
 *
 * API (unchanged from v1):
 *   wserver.serve(int port)             → int  server handle (manual mode)
 *   wserver.serve(int port, bool auto)  → int  server handle
 *   wserver.accept(int server, int timeout_ms) → int  request handle (0 = timeout)
 *   wserver.req_method(int req)         → str
 *   wserver.req_path(int req)           → str
 *   wserver.req_body(int req)           → str
 *   wserver.req_header(int req, str name) → str  ("" if absent)
 *   wserver.reply(int req, int status, str body)       → nil
 *   wserver.reply_json(int req, int status, str json)  → nil
 *   wserver.reply_headers(int req, int status, str body, str arr h, int n) → nil
 *   wserver.connections(int server)     → int  active connection count
 *   wserver.wait(int server)            → nil  block until stop()
 *   wserver.stop(int server)            → nil
 *   wserver.version()                   → str
 *
 * Thread pool size:
 *   serve(port)        → pool of [libs.wserver] thread_pool_size threads (default: 4)
 *   serve(port, true)  → auto-scaling between min_threads and max_threads
 *
 * Configuration (fluxa.toml):
 *   [libs.wserver]
 *   max_servers       = 4        # hard cap 32
 *   max_requests      = 512      # hard cap 4096
 *   max_body_bytes    = 65536    # hard cap 16MB
 *   max_header_pairs  = 16       # hard cap 128
 *   max_header_bytes  = 4096     # hard cap 65536
 *   queue_depth       = 1024     # hard cap 16384
 *   thread_pool_size  = 4        # MHD thread pool (default 4, hard cap 64)
 *   # Auto-scaling (serve with true):
 *   min_threads       = 2
 *   max_threads       = 16
 *   scale_up_queue    = 8
 *   scale_down_idle   = 10
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"
#include "../../runtime.h"

/* ── Defaults and hard caps ──────────────────────────────────────── */
#define WS_DEFAULT_MAX_SERVERS       4
#define WS_DEFAULT_MAX_REQUESTS      512
#define WS_DEFAULT_MAX_BODY_BYTES    65536
#define WS_DEFAULT_MAX_HEADER_PAIRS  16
#define WS_DEFAULT_MAX_HEADER_BYTES  4096
#define WS_DEFAULT_QUEUE_DEPTH       1024
#define WS_DEFAULT_THREAD_POOL_SIZE  4
#define WS_DEFAULT_MIN_THREADS       2
#define WS_DEFAULT_MAX_THREADS       16
#define WS_DEFAULT_SCALE_UP_QUEUE    8
#define WS_DEFAULT_SCALE_DOWN_IDLE   10

#define WS_HARD_MAX_SERVERS       32
#define WS_HARD_MAX_REQUESTS      4096
#define WS_HARD_MAX_BODY_BYTES    (16*1024*1024)
#define WS_HARD_MAX_HEADER_PAIRS  128
#define WS_HARD_MAX_HEADER_BYTES  65536
#define WS_HARD_QUEUE_DEPTH       16384
#define WS_HARD_THREAD_POOL_SIZE  64
#define WS_HARD_MIN_THREADS       64
#define WS_HARD_MAX_THREADS       256

/* ── Value constructors ──────────────────────────────────────────── */
static inline Value wsrv_nil(void)          { Value v; v.type=VAL_NIL;            return v; }
static inline Value wsrv_int(long n)        { Value v; v.type=VAL_INT;   v.as.integer=n; return v; }
static inline Value wsrv_str(const char *s) {
    Value v; v.type=VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}

#ifdef FLUXA_WSERVER_MHD
/* ══════════════════════════════════════════════════════════════════
 * Real backend — libmicrohttpd with epoll + thread pool
 * ══════════════════════════════════════════════════════════════════ */

#include <microhttpd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/* ── Config (read once, protected by ws_cfg_mu) ──────────────────── */
static pthread_mutex_t ws_cfg_mu      = PTHREAD_MUTEX_INITIALIZER;
static int ws_cfg_initialized         = 0;
static int ws_max_servers             = 0;
static int ws_max_requests            = 0;
static int ws_max_body_bytes          = 0;
static int ws_max_header_pairs        = 0;
static int ws_max_header_bytes        = 0;
static int ws_queue_depth             = 0;
static int ws_thread_pool_size        = 0;
static int ws_min_threads             = 0;
static int ws_max_threads             = 0;
static int ws_scale_up_queue          = 0;
static int ws_scale_down_idle         = 0;

static void ws_ensure_config(const FluxaConfig *cfg) {
#define WS_CLAMP(v, def, hi) ((v) > 0 && (v) <= (hi) ? (v) : (def))
    ws_max_servers      = WS_CLAMP(cfg ? cfg->wserver_max_servers      : 0, WS_DEFAULT_MAX_SERVERS,      WS_HARD_MAX_SERVERS);
    ws_max_requests     = WS_CLAMP(cfg ? cfg->wserver_max_requests     : 0, WS_DEFAULT_MAX_REQUESTS,     WS_HARD_MAX_REQUESTS);
    ws_max_body_bytes   = WS_CLAMP(cfg ? cfg->wserver_max_body_bytes   : 0, WS_DEFAULT_MAX_BODY_BYTES,   WS_HARD_MAX_BODY_BYTES);
    ws_max_header_pairs = WS_CLAMP(cfg ? cfg->wserver_max_header_pairs : 0, WS_DEFAULT_MAX_HEADER_PAIRS, WS_HARD_MAX_HEADER_PAIRS);
    ws_max_header_bytes = WS_CLAMP(cfg ? cfg->wserver_max_header_bytes : 0, WS_DEFAULT_MAX_HEADER_BYTES, WS_HARD_MAX_HEADER_BYTES);
    ws_queue_depth      = WS_CLAMP(cfg ? cfg->wserver_queue_depth      : 0, WS_DEFAULT_QUEUE_DEPTH,      WS_HARD_QUEUE_DEPTH);
    ws_thread_pool_size = WS_CLAMP(cfg ? cfg->wserver_workers          : 0, WS_DEFAULT_THREAD_POOL_SIZE, WS_HARD_THREAD_POOL_SIZE);
    ws_min_threads      = WS_CLAMP(cfg ? cfg->wserver_min_threads      : 0, WS_DEFAULT_MIN_THREADS,      WS_HARD_MIN_THREADS);
    ws_max_threads      = WS_CLAMP(cfg ? cfg->wserver_max_threads      : 0, WS_DEFAULT_MAX_THREADS,      WS_HARD_MAX_THREADS);
    ws_scale_up_queue   = WS_CLAMP(cfg ? cfg->wserver_scale_up_queue   : 0, WS_DEFAULT_SCALE_UP_QUEUE,   ws_queue_depth);
    ws_scale_down_idle  = WS_CLAMP(cfg ? cfg->wserver_scale_down_idle  : 0, WS_DEFAULT_SCALE_DOWN_IDLE,  3600);
    if (ws_min_threads > ws_max_threads) ws_min_threads = ws_max_threads;
#undef WS_CLAMP
    ws_cfg_initialized = 1;
}

/* ── Request struct ──────────────────────────────────────────────── */
typedef struct {
    char    method[16];
    char   *path;
    char   *body;
    size_t  body_len;
    char   *headers_flat;   /* "Key1\0Val1\0Key2\0Val2\0\0" flat NUL-separated */
    int     header_count;
    int     http_status;    /* reply status set by Fluxa worker */
    char   *reply_body;
    char    reply_ct[256];
    int     reply_ready;    /* 1 when worker has called reply() */
    int     consumed;       /* 1 when MHD has finished sending */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} WsRequest;

/* ── Handle tables ───────────────────────────────────────────────── */
typedef struct {
    struct MHD_Daemon *daemon;
    int    port;
    int    running;
    int    auto_scale;
    volatile int auto_stop;
    /* Accept queue */
    pthread_mutex_t queue_mu;
    pthread_cond_t  queue_cv;
    WsRequest     **queue;
    int             q_head;
    int             q_tail;
    int             q_count;
    int             q_cap;
    int             conn_count;
    /* Auto-scaling state */
    int             cur_pool_size;
    pthread_t       monitor_tid;
    int             monitor_stop;
    /* Fluxa worker threads (when serve(port, true, fn_name)) */
    pthread_t      *worker_tids;
    int             worker_count;
} WsServer;

static WsServer  **ws_servers  = NULL;
static WsRequest **ws_requests = NULL;
static int         ws_tables_ok = 0;

static void ws_ensure_tables(void) {
    if (ws_tables_ok) return;
    ws_servers  = (WsServer  **)calloc((size_t)ws_max_servers,  sizeof(WsServer *));
    ws_requests = (WsRequest **)calloc((size_t)ws_max_requests, sizeof(WsRequest *));
    if (ws_servers && ws_requests) ws_tables_ok = 1;
    else { free(ws_servers); free(ws_requests); ws_servers = NULL; ws_requests = NULL; }
}

static int ws_alloc_server(WsServer *s) {
    for (int i = 0; i < ws_max_servers; i++)
        if (!ws_servers[i]) { ws_servers[i] = s; return i+1; }
    return 0;
}
static int ws_alloc_request(WsRequest *r) {
    for (int i = 0; i < ws_max_requests; i++)
        if (!ws_requests[i]) { ws_requests[i] = r; return i+1; }
    return 0;
}

static WsServer *ws_get_server(long h, ErrStack *e, int *he, int line, const char *fn) {
    char eb[280];
    pthread_mutex_lock(&ws_cfg_mu);
    int ok = ws_tables_ok && h >= 1 && h <= ws_max_servers && ws_servers[h-1];
    WsServer *s = ok ? ws_servers[h-1] : NULL;
    pthread_mutex_unlock(&ws_cfg_mu);
    if (!s) {
        snprintf(eb, sizeof(eb), "wserver.%s: invalid or stopped server handle %ld", fn, h);
        errstack_push(e, ERR_FLUXA, eb, "wserver", line); *he = 1;
    }
    return s;
}
static WsRequest *ws_get_request(long h, ErrStack *e, int *he, int line, const char *fn) {
    char eb[280];
    pthread_mutex_lock(&ws_cfg_mu);
    int ok = ws_tables_ok && h >= 1 && h <= ws_max_requests && ws_requests[h-1];
    WsRequest *r = ok ? ws_requests[h-1] : NULL;
    pthread_mutex_unlock(&ws_cfg_mu);
    if (!r) {
        snprintf(eb, sizeof(eb), "wserver.%s: invalid or consumed request handle %ld", fn, h);
        errstack_push(e, ERR_FLUXA, eb, "wserver", line); *he = 1;
    }
    return r;
}

static void ws_free_request(long h, WsRequest *req) {
    if (h >= 1 && h <= ws_max_requests) {
        pthread_mutex_lock(&ws_cfg_mu);
        ws_requests[h-1] = NULL;
        pthread_mutex_unlock(&ws_cfg_mu);
    }
    if (req) {
        free(req->path);
        free(req->body);
        free(req->headers_flat);
        free(req->reply_body);
        pthread_mutex_destroy(&req->mu);
        pthread_cond_destroy(&req->cv);
        free(req);
    }
}

/* ── MHD connection context ──────────────────────────────────────── */
typedef struct {
    WsServer  *srv;
    char      *body_buf;
    size_t     body_len;
    int        body_overflow;
    int        conn_counted;
    /* Set after the request is enqueued — used by ws_request_completed
     * to free the WsRequest slot without blocking the Fluxa worker. */
    WsRequest *req_ptr;
    long       req_handle;
} WsConnCtx;

/* ── Header collector ────────────────────────────────────────────── */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    count;
    int    overflow;
    int    max_bytes;
} WsHdrCtx;

static enum MHD_Result ws_hdr_iter(void *cls, enum MHD_ValueKind kind,
                                    const char *key, const char *value) {
    (void)kind;
    WsHdrCtx *hc = (WsHdrCtx *)cls;
    if (hc->overflow || !key || !value) return MHD_YES;
    size_t klen = strlen(key), vlen = strlen(value);
    if ((int)(klen + vlen + 2) > hc->max_bytes) { hc->overflow = 1; return MHD_YES; }
    /* Store as "key\0value\0" flat pairs */
    size_t needed = klen + 1 + vlen + 1;
    if (hc->len + needed + 1 > hc->cap) {
        size_t nc = hc->cap * 2 + needed + 64;
        char *nb = (char *)realloc(hc->buf, nc);
        if (!nb) { hc->overflow = 1; return MHD_YES; }
        hc->buf = nb; hc->cap = nc;
    }
    memcpy(hc->buf + hc->len, key, klen);   hc->len += klen;   hc->buf[hc->len++] = '\0';
    memcpy(hc->buf + hc->len, value, vlen); hc->len += vlen;   hc->buf[hc->len++] = '\0';
    hc->buf[hc->len] = '\0'; /* terminal sentinel */
    hc->count++;
    return MHD_YES;
}

/* ── MHD request handler ─────────────────────────────────────────── */
static enum MHD_Result ws_mhd_handler(
        void *cls, struct MHD_Connection *conn,
        const char *url, const char *method, const char *version,
        const char *upload_data, size_t *upload_data_size, void **con_cls) {

    WsServer *srv = (WsServer *)cls;
    (void)version;

    /* First call for this connection — allocate context */
    if (*con_cls == NULL) {
        WsConnCtx *ctx = (WsConnCtx *)calloc(1, sizeof(WsConnCtx));
        if (!ctx) return MHD_NO;
        ctx->srv      = srv;
        ctx->body_buf = (char *)malloc((size_t)ws_max_body_bytes + 1);
        if (!ctx->body_buf) { free(ctx); return MHD_NO; }
        ctx->body_buf[0] = '\0';
        *con_cls = ctx;
        return MHD_YES;
    }

    WsConnCtx *ctx = (WsConnCtx *)*con_cls;

    /* Accumulate body */
    if (*upload_data_size > 0) {
        if (!ctx->body_overflow) {
            size_t room = (size_t)ws_max_body_bytes - ctx->body_len;
            if (*upload_data_size > room) ctx->body_overflow = 1;
            else {
                memcpy(ctx->body_buf + ctx->body_len, upload_data, *upload_data_size);
                ctx->body_len += *upload_data_size;
                ctx->body_buf[ctx->body_len] = '\0';
            }
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Body overflow → 413 */
    if (ctx->body_overflow) {
        const char *b = "Request Entity Too Large";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 413, r);
        MHD_destroy_response(r);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        return rv;
    }

    /* Validate method */
    size_t mlen = method ? strlen(method) : 0;
    if (mlen == 0 || mlen > 15) {
        const char *b = "Bad Request";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 400, r);
        MHD_destroy_response(r);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        return rv;
    }

    /* Allocate request */
    WsRequest *req = (WsRequest *)calloc(1, sizeof(WsRequest));
    if (!req || pthread_mutex_init(&req->mu, NULL) != 0 || pthread_cond_init(&req->cv, NULL) != 0) {
        free(req); free(ctx->body_buf); free(ctx); *con_cls = NULL; return MHD_NO;
    }

    snprintf(req->method, sizeof(req->method), "%s", method);

    size_t ulen = url ? strlen(url) : 0;
    req->path = (char *)malloc(ulen + 1);
    if (!req->path) { ws_free_request(0,req); free(ctx->body_buf); free(ctx); *con_cls=NULL; return MHD_NO; }
    memcpy(req->path, url ? url : "", ulen); req->path[ulen] = '\0';

    req->body = (char *)malloc(ctx->body_len + 1);
    if (!req->body) { ws_free_request(0,req); free(ctx->body_buf); free(ctx); *con_cls=NULL; return MHD_NO; }
    memcpy(req->body, ctx->body_buf, ctx->body_len); req->body[ctx->body_len] = '\0';
    req->body_len = ctx->body_len;

    /* Collect headers as flat key\0val\0 buffer */
    WsHdrCtx hc; hc.cap=512; hc.len=0; hc.overflow=0; hc.count=0;
    hc.max_bytes = ws_max_header_bytes;
    hc.buf = (char *)malloc(hc.cap);
    if (!hc.buf) { ws_free_request(0,req); free(ctx->body_buf); free(ctx); *con_cls=NULL; return MHD_NO; }
    hc.buf[0] = '\0';
    MHD_get_connection_values(conn, MHD_HEADER_KIND, ws_hdr_iter, &hc);
    req->headers_flat  = hc.buf;
    req->header_count  = hc.count;

    /* Enqueue request */
    pthread_mutex_lock(&srv->queue_mu);
    int slot = 0;
    if (srv->q_count < srv->q_cap) {
        pthread_mutex_lock(&ws_cfg_mu);
        slot = ws_alloc_request(req);
        pthread_mutex_unlock(&ws_cfg_mu);
        if (slot > 0) {
            srv->queue[srv->q_tail] = req;
            req->http_status = slot; /* reuse field to carry slot# until accept() retrieves it */
            srv->q_tail = (srv->q_tail + 1) % srv->q_cap;
            srv->q_count++;
            srv->conn_count++;
            ctx->conn_counted = 1;
            ctx->req_ptr    = req;
            ctx->req_handle = slot;
        }
    }
    /* signal (not broadcast) — only one accept() worker needs to wake up
     * per enqueued request. Broadcast caused thundering herd with many
     * Fluxa worker threads waiting on the same cv. */
    pthread_cond_signal(&srv->queue_cv);
    pthread_mutex_unlock(&srv->queue_mu);

    if (!slot) {
        ws_free_request(0, req);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        const char *b = "Service Unavailable";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 503, r);
        MHD_destroy_response(r);
        return rv;
    }

    /* Wait for Fluxa worker to call reply() — 30s timeout */
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 30;

    pthread_mutex_lock(&req->mu);
    while (!req->reply_ready)
        if (pthread_cond_timedwait(&req->cv, &req->mu, &dl) == ETIMEDOUT) break;

    int    rstatus = req->reply_ready ? req->http_status : 504;
    char  *rbody   = req->reply_ready ? req->reply_body  : NULL;
    char   rct[256];
    snprintf(rct, sizeof(rct), "%s", req->reply_ct[0] ? req->reply_ct : "text/plain");
    pthread_mutex_unlock(&req->mu);

    const char *final_body = rbody ? rbody : (rstatus == 504 ? "Gateway Timeout" : "");
    struct MHD_Response *mhd_resp = MHD_create_response_from_buffer(
        strlen(final_body), (void *)final_body, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(mhd_resp, "Content-Type", rct);
    enum MHD_Result rv = MHD_queue_response(conn, (unsigned int)rstatus, mhd_resp);
    MHD_destroy_response(mhd_resp);

    /* Signal consumed so ws_finish_reply() can return. MHD has copied the
     * body (MHD_RESPMEM_MUST_COPY), so the worker can now safely free req. */
    pthread_mutex_lock(&req->mu);
    req->consumed = 1;
    pthread_cond_signal(&req->cv);
    pthread_mutex_unlock(&req->mu);

    *con_cls = ctx; /* keep for ws_request_completed */
    return rv;
}

static void ws_request_completed(void *cls, struct MHD_Connection *conn,
                                   void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)conn; (void)toe;
    if (*con_cls) {
        WsConnCtx *ctx = (WsConnCtx *)*con_cls;
        if (ctx && ctx->srv && ctx->conn_counted) {
            pthread_mutex_lock(&ctx->srv->queue_mu);
            ctx->srv->conn_count--;
            pthread_mutex_unlock(&ctx->srv->queue_mu);
        }
        if (ctx) { free(ctx->body_buf); free(ctx); }
        *con_cls = NULL;
    }
}

/* Wait for MHD to acknowledge our reply, then free the request slot.
 *
 * The wait MUST NOT time out early: if we destroy req->mu while the MHD
 * thread is still inside its post-queue lock sequence (line: "consumed = 1"),
 * the mutex_lock there hits a destroyed mutex and returns EINVAL — or
 * worse, segfaults. Under heavy thread contention on a single CPU, the
 * MHD thread's wakeup from its 30s wait can take 100ms+, so any short
 * timeout here races with that wakeup.
 *
 * 30s here matches the MHD response wait (CONNECTION_TIMEOUT). The wait
 * normally returns within microseconds (consumed is signaled right after
 * MHD_queue_response, which is non-blocking). The 30s is just an outer
 * bound to detect a truly stuck handler. */
static void ws_finish_reply(long req_handle, WsRequest *req) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 30;
    pthread_mutex_lock(&req->mu);
    while (!req->consumed)
        if (pthread_cond_timedwait(&req->cv, &req->mu, &dl) == ETIMEDOUT) break;
    pthread_mutex_unlock(&req->mu);
    ws_free_request(req_handle, req);
}

/* ── Auto-scaling monitor ────────────────────────────────────────── */
/* In auto mode we restart the MHD daemon with a larger thread pool when
 * the accept queue fills up. This is coarser than the v1 per-thread approach
 * but avoids the overhead of spawning an OS thread per scale step. */
static void *ws_monitor_thread(void *arg) {
    WsServer *srv = (WsServer *)arg;
    while (1) {
        for (int i = 0; i < 10; i++) {
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100000000L;
            nanosleep(&ts, NULL);
            if (srv->monitor_stop) goto done;
        }
        if (srv->monitor_stop) break;

        pthread_mutex_lock(&srv->queue_mu);
        int qdepth = srv->q_count;
        pthread_mutex_unlock(&srv->queue_mu);

        /* Scale up: grow pool by restarting daemon with more threads */
        if (qdepth >= ws_scale_up_queue && srv->cur_pool_size < ws_max_threads) {
            int new_size = srv->cur_pool_size * 2;
            if (new_size > ws_max_threads) new_size = ws_max_threads;
            struct MHD_Daemon *nd = MHD_start_daemon(
                MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG,
                (uint16_t)srv->port, NULL, NULL,
                ws_mhd_handler, srv,
                MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
                MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
                MHD_OPTION_THREAD_POOL_SIZE,        (unsigned int)new_size,
                MHD_OPTION_LISTEN_BACKLOG_SIZE,     (unsigned int)ws_queue_depth,
                MHD_OPTION_LISTENING_ADDRESS_REUSE, (unsigned int)1,
                MHD_OPTION_END);
            if (nd) {
                MHD_stop_daemon(srv->daemon);
                srv->daemon = nd;
                srv->cur_pool_size = new_size;
            }
        }
    }
done:
    return NULL;
}

/* ── Worker thread (for serve(port, true, fn_name)) ─────────────── */
typedef struct {
    WsServer *srv;
    void     *fn_node;
    void     *parent_rt;
    int       srv_handle;
} WsWorkerArg;

static void *ws_fluxa_worker(void *arg) {
    WsWorkerArg *wa     = (WsWorkerArg *)arg;
    ASTNode     *fn     = (ASTNode *)wa->fn_node;
    Runtime     *parent = (Runtime *)wa->parent_rt;
    int          srv_h  = wa->srv_handle;
    free(wa);

    Runtime *rt = runtime_clone_for_thread(parent);
    if (!rt) return NULL;

    if (fn->as.func_decl.param_count >= 1) {
        int zero_n = fn->as.func_decl.param_count + 4;
        if (zero_n > FLUXA_STACK_SIZE) zero_n = FLUXA_STACK_SIZE;
        for (int _z = 0; _z < zero_n; _z++) rt->stack[_z].type = VAL_NIL;
        rt->stack[0].type       = VAL_INT;
        rt->stack[0].as.integer = srv_h;
        rt->stack_size          = 1;
    }

    runtime_eval(rt, fn->as.func_decl.body);
    runtime_free_thread_clone(rt);
    return NULL;
}

/* ── Start server ────────────────────────────────────────────────── */
static WsServer *ws_start_server(int port, int auto_scale, char *errbuf, size_t ebsz) {
    WsServer *srv = (WsServer *)calloc(1, sizeof(WsServer));
    if (!srv) { snprintf(errbuf, ebsz, "out of memory"); return NULL; }

    srv->port       = port;
    srv->running    = 1;
    srv->auto_scale = auto_scale;
    srv->q_cap      = ws_queue_depth;
    srv->queue      = (WsRequest **)calloc((size_t)ws_queue_depth, sizeof(WsRequest *));
    if (!srv->queue) { free(srv); snprintf(errbuf, ebsz, "out of memory for queue"); return NULL; }

    if (pthread_mutex_init(&srv->queue_mu, NULL) != 0 ||
        pthread_cond_init(&srv->queue_cv, NULL)  != 0) {
        free(srv->queue); free(srv);
        snprintf(errbuf, ebsz, "pthread init failed"); return NULL;
    }

    /* Choose initial pool size */
    int init_pool = auto_scale ? ws_min_threads : ws_thread_pool_size;
    if (init_pool < 1) init_pool = 1;

    /* EPOLL + fixed thread pool — handles thousands of concurrent connections
     * with a small set of threads, instead of spawning one OS thread per request.
     * LISTEN_BACKLOG_SIZE raises the kernel TCP listen queue from default (~128)
     * to queue_depth so high-burst connect storms don't get RST.
     * LISTENING_ADDRESS_REUSE enables SO_REUSEPORT for fast port reclaim. */
    srv->daemon = MHD_start_daemon(
        MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG,
        (uint16_t)port, NULL, NULL,
        ws_mhd_handler, srv,
        MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
        MHD_OPTION_THREAD_POOL_SIZE,        (unsigned int)init_pool,
        MHD_OPTION_LISTEN_BACKLOG_SIZE,     (unsigned int)ws_queue_depth,
        MHD_OPTION_LISTENING_ADDRESS_REUSE, (unsigned int)1,
        MHD_OPTION_END);

    if (!srv->daemon) {
        pthread_mutex_destroy(&srv->queue_mu);
        pthread_cond_destroy(&srv->queue_cv);
        free(srv->queue); free(srv);
        snprintf(errbuf, ebsz, "wserver.serve: failed to bind port %d", port);
        return NULL;
    }

    srv->cur_pool_size = init_pool;

    /* Auto-scaling monitor */
    if (auto_scale) {
        if (pthread_create(&srv->monitor_tid, NULL, ws_monitor_thread, srv) != 0) {
            MHD_stop_daemon(srv->daemon);
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: monitor thread creation failed");
            return NULL;
        }
    }

    return srv;
}

/* ── Stop server ─────────────────────────────────────────────────── */
static void ws_stop_server(WsServer *srv) {
    if (!srv) return;

    /* Wait for in-flight responses (max 3s) */
    {
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 3;
        struct timespec step; step.tv_sec = 0; step.tv_nsec = 10000000L;
        for (;;) {
            pthread_mutex_lock(&srv->queue_mu);
            int n = srv->conn_count;
            pthread_mutex_unlock(&srv->queue_mu);
            if (n <= 0) break;
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) break;
            nanosleep(&step, NULL);
        }
    }

    /* Stop monitor */
    if (srv->auto_scale && srv->monitor_tid) {
        srv->monitor_stop = 1;
        pthread_join(srv->monitor_tid, NULL);
    }

    /* Stop MHD */
    if (srv->daemon) { MHD_stop_daemon(srv->daemon); srv->daemon = NULL; }

    /* Drain pending queue */
    pthread_mutex_lock(&srv->queue_mu);
    while (srv->q_count > 0) {
        WsRequest *r = srv->queue[srv->q_head];
        srv->q_head = (srv->q_head + 1) % srv->q_cap;
        srv->q_count--;
        if (r) {
            long rh = 0;
            pthread_mutex_lock(&ws_cfg_mu);
            for (int i = 0; i < ws_max_requests; i++)
                if (ws_requests[i] == r) { rh = i+1; break; }
            pthread_mutex_unlock(&ws_cfg_mu);
            ws_free_request(rh, r);
        }
    }
    pthread_mutex_unlock(&srv->queue_mu);

    pthread_mutex_destroy(&srv->queue_mu);
    pthread_cond_destroy(&srv->queue_cv);
    free(srv->queue);
    free(srv);
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static Value __attribute__((unused)) ws_dispatch(const char *fn_name,
                          const Value *args, int argc,
                          ErrStack *err, int *had_error,
                          int line, void *rt_ptr) {
    Runtime           *rt  = (Runtime *)rt_ptr;
    const FluxaConfig *cfg = rt ? &rt->config : NULL;
    char errbuf[512];

    pthread_mutex_lock(&ws_cfg_mu);
    if (!ws_cfg_initialized) ws_ensure_config(cfg);
    if (!ws_tables_ok) ws_ensure_tables();
    pthread_mutex_unlock(&ws_cfg_mu);

    if (!ws_tables_ok) {
        snprintf(errbuf, sizeof(errbuf),
                 "wserver.%s: handle tables not initialized (out of memory)", fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
        *had_error = 1; return wsrv_nil();
    }

#define WS_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "wserver.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "wserver", line); \
    *had_error = 1; return wsrv_nil(); } while(0)

#define NEED(n) do { if (argc < (n)) { \
    snprintf(errbuf, sizeof(errbuf), "wserver.%s: need %d arg(s), got %d", fn_name, (n), argc); \
    errstack_push(err, ERR_FLUXA, errbuf, "wserver", line); \
    *had_error = 1; return wsrv_nil(); } } while(0)

#define REQ_INT(idx, var) \
    if ((idx) >= argc || args[(idx)].type != VAL_INT) WS_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

#define REQ_STR(idx, var) \
    if ((idx) >= argc || args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        WS_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* ── wserver.serve(port [, bool auto]) → int ─────────────────── */
    if (!strcmp(fn_name, "serve")) {
        NEED(1); REQ_INT(0, port);
        if (port < 1 || port > 65535) WS_ERR("port out of range [1..65535]");

        int auto_scale = 0;
        if (argc >= 2 && args[1].type == VAL_BOOL)  auto_scale = args[1].as.boolean  ? 1 : 0;
        else if (argc >= 2 && args[1].type == VAL_INT)   auto_scale = args[1].as.integer  ? 1 : 0;

        char ebuf[256] = "";
        WsServer *srv = ws_start_server((int)port, auto_scale, ebuf, sizeof(ebuf));
        if (!srv) {
            errstack_push(err, ERR_FLUXA, ebuf[0] ? ebuf : "wserver.serve: failed", "wserver", line);
            *had_error = 1; return wsrv_int(0);
        }

        pthread_mutex_lock(&ws_cfg_mu);
        int slot = ws_alloc_server(srv);
        pthread_mutex_unlock(&ws_cfg_mu);

        if (!slot) { ws_stop_server(srv); WS_ERR("server table full"); }

        /* Optional Fluxa worker function (serve(port, true/false, "fn_name")) */
        const char *worker_fn = NULL;
        if (argc >= 3 && args[2].type == VAL_STRING && args[2].as.string)
            worker_fn = args[2].as.string;

        if (worker_fn && rt) {
            Value fn_val; fn_val.type = VAL_NIL;
            if (rt->global_table) scope_table_get(rt->global_table, worker_fn, &fn_val);
            if (fn_val.type != VAL_FUNC) {
                snprintf(errbuf, sizeof(errbuf), "wserver.serve: worker function '%s' not found", worker_fn);
                errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
                *had_error = 1;
                pthread_mutex_lock(&ws_cfg_mu); ws_servers[slot-1] = NULL; pthread_mutex_unlock(&ws_cfg_mu);
                ws_stop_server(srv);
                return wsrv_int(0);
            }
            int n_workers = (cfg && cfg->wserver_workers > 0) ? cfg->wserver_workers : 4;
            if (n_workers > 64) n_workers = 64;
            srv->worker_tids  = (pthread_t *)calloc((size_t)n_workers, sizeof(pthread_t));
            srv->worker_count = 0;
            if (srv->worker_tids) {
                for (int _wi = 0; _wi < n_workers; _wi++) {
                    WsWorkerArg *wa = (WsWorkerArg *)malloc(sizeof(WsWorkerArg));
                    if (!wa) break;
                    wa->srv = srv; wa->fn_node = fn_val.as.func;
                    wa->parent_rt = rt; wa->srv_handle = slot;
                    if (pthread_create(&srv->worker_tids[srv->worker_count], NULL, ws_fluxa_worker, wa) == 0)
                        srv->worker_count++;
                    else free(wa);
                }
            }
        }

        return wsrv_int(slot);
    }

    /* ── wserver.accept(server, timeout_ms) → int ───────────────── */
    if (!strcmp(fn_name, "accept")) {
        NEED(2); REQ_INT(0, h); REQ_INT(1, timeout_ms);
        WsServer *srv = ws_get_server(h, err, had_error, line, fn_name);
        if (!srv) return wsrv_int(0);
        if (timeout_ms < 0) timeout_ms = 0;

        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec  += timeout_ms / 1000;
        dl.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&srv->queue_mu);
        while (srv->q_count == 0) {
            if (srv->auto_stop) { pthread_mutex_unlock(&srv->queue_mu); return wsrv_int(0); }
            if (pthread_cond_timedwait(&srv->queue_cv, &srv->queue_mu, &dl) == ETIMEDOUT) {
                pthread_mutex_unlock(&srv->queue_mu); return wsrv_int(0);
            }
        }
        WsRequest *req = srv->queue[srv->q_head];
        srv->q_head = (srv->q_head + 1) % srv->q_cap;
        srv->q_count--;
        pthread_mutex_unlock(&srv->queue_mu);

        long req_handle = req->http_status; /* slot stored here by handler */
        req->http_status = 0;
        return wsrv_int(req_handle);
    }

    /* ── wserver.req_method(req) → str ──────────────────────────── */
    if (!strcmp(fn_name, "req_method")) {
        NEED(1); REQ_INT(0, h);
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        return wsrv_str(r->method);
    }

    /* ── wserver.req_path(req) → str ────────────────────────────── */
    if (!strcmp(fn_name, "req_path")) {
        NEED(1); REQ_INT(0, h);
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        return wsrv_str(r->path ? r->path : "");
    }

    /* ── wserver.req_body(req) → str ────────────────────────────── */
    if (!strcmp(fn_name, "req_body")) {
        NEED(1); REQ_INT(0, h);
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        return wsrv_str(r->body ? r->body : "");
    }

    /* ── wserver.req_header(req, name) → str ────────────────────── */
    if (!strcmp(fn_name, "req_header")) {
        NEED(2); REQ_INT(0, h); REQ_STR(1, hname);
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r || !r->headers_flat) return wsrv_str("");
        /* Walk flat "key\0val\0..." buffer */
        const char *p = r->headers_flat;
        while (*p) {
            const char *k = p;
            while (*p) { p++; } p++; /* skip key */
            const char *v = p;
            while (*p) { p++; } p++; /* skip val */
            if (strcasecmp(k, hname) == 0) return wsrv_str(v);
        }
        return wsrv_str("");
    }

    /* ── wserver.reply(req, status, body) → nil ─────────────────── */
    if (!strcmp(fn_name, "reply")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, status); REQ_STR(2, body);
        if (status < 100 || status > 599) WS_ERR("HTTP status must be 100-599");
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        /* Idempotent: only the first reply wins. Subsequent calls are no-ops. */
        pthread_mutex_lock(&r->mu);
        if (r->reply_ready) { pthread_mutex_unlock(&r->mu); return wsrv_nil(); }
        pthread_mutex_unlock(&r->mu);
        r->reply_body  = strdup(body);
        r->reply_ct[0] = '\0';
        pthread_mutex_lock(&r->mu);
        r->http_status = (int)status; r->reply_ready = 1;
        pthread_cond_signal(&r->cv);
        pthread_mutex_unlock(&r->mu);
        ws_finish_reply(h, r);
        return wsrv_nil();
    }

    /* ── wserver.reply_json(req, status, json) → nil ────────────── */
    if (!strcmp(fn_name, "reply_json")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, status); REQ_STR(2, json);
        if (status < 100 || status > 599) WS_ERR("HTTP status must be 100-599");
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        /* Idempotent: only the first reply wins. Subsequent calls are no-ops. */
        pthread_mutex_lock(&r->mu);
        if (r->reply_ready) { pthread_mutex_unlock(&r->mu); return wsrv_nil(); }
        pthread_mutex_unlock(&r->mu);
        r->reply_body = strdup(json);
        snprintf(r->reply_ct, sizeof(r->reply_ct), "application/json");
        pthread_mutex_lock(&r->mu);
        r->http_status = (int)status; r->reply_ready = 1;
        pthread_cond_signal(&r->cv);
        pthread_mutex_unlock(&r->mu);
        ws_finish_reply(h, r);
        return wsrv_nil();
    }

    /* ── wserver.reply_headers(req, status, body, str arr h, int n) */
    if (!strcmp(fn_name, "reply_headers")) {
        NEED(5); REQ_INT(0, h); REQ_INT(1, status); REQ_STR(2, body);
        if (status < 100 || status > 599) WS_ERR("HTTP status must be 100-599");
        if (args[3].type != VAL_ARR || !args[3].as.arr.data)
            WS_ERR("reply_headers: arg 4 must be str arr");
        if (args[4].type != VAL_INT)
            WS_ERR("reply_headers: arg 5 must be int (pair count)");
        long npairs = args[4].as.integer;
        if (npairs < 0) WS_ERR("reply_headers: pair count cannot be negative");
        if (npairs > ws_max_header_pairs) {
            snprintf(errbuf, sizeof(errbuf),
                     "wserver.reply_headers: %ld pairs > max_header_pairs %d",
                     npairs, ws_max_header_pairs);
            errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
            *had_error = 1; return wsrv_nil();
        }
        if (npairs * 2 > args[3].as.arr.size) {
            snprintf(errbuf, sizeof(errbuf),
                     "wserver.reply_headers: need %ld elements for %ld pairs, arr has %d",
                     npairs*2, npairs, args[3].as.arr.size);
            errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
            *had_error = 1; return wsrv_nil();
        }
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        /* Idempotent: only the first reply wins. */
        pthread_mutex_lock(&r->mu);
        if (r->reply_ready) { pthread_mutex_unlock(&r->mu); return wsrv_nil(); }
        pthread_mutex_unlock(&r->mu);
        char ct[256] = "";
        Value *arr = args[3].as.arr.data;
        for (long i = 0; i < npairs; i++) {
            Value *kv = &arr[i*2], *vv = &arr[i*2+1];
            if (kv->type != VAL_STRING || !kv->as.string ||
                vv->type != VAL_STRING || !vv->as.string)
                WS_ERR("reply_headers: all arr elements must be str");
            if ((int)(strlen(kv->as.string)+strlen(vv->as.string)) > ws_max_header_bytes)
                WS_ERR("reply_headers: header key+value exceeds max_header_bytes");
            if (!strcasecmp(kv->as.string, "Content-Type"))
                snprintf(ct, sizeof(ct), "%s", vv->as.string);
        }
        r->reply_body = strdup(body);
        snprintf(r->reply_ct, sizeof(r->reply_ct), "%s", ct[0] ? ct : "text/plain");
        pthread_mutex_lock(&r->mu);
        r->http_status = (int)status; r->reply_ready = 1;
        pthread_cond_signal(&r->cv);
        pthread_mutex_unlock(&r->mu);
        ws_finish_reply(h, r);
        return wsrv_nil();
    }

    /* ── wserver.connections(server) → int ──────────────────────── */
    if (!strcmp(fn_name, "connections")) {
        NEED(1); REQ_INT(0, h);
        WsServer *srv = ws_get_server(h, err, had_error, line, fn_name);
        if (!srv) return wsrv_nil();
        pthread_mutex_lock(&srv->queue_mu);
        long n = srv->conn_count;
        pthread_mutex_unlock(&srv->queue_mu);
        return wsrv_int(n);
    }

    /* ── wserver.wait(server) → nil ─────────────────────────────── */
    if (!strcmp(fn_name, "wait")) {
        NEED(1); REQ_INT(0, h);
        WsServer *srv = ws_get_server(h, err, had_error, line, fn_name);
        if (!srv) return wsrv_nil();
        struct timespec _step; _step.tv_sec = 0; _step.tv_nsec = 50000000L;
        while (!srv->auto_stop) nanosleep(&_step, NULL);
        return wsrv_nil();
    }

    /* ── wserver.stop(server) → nil ─────────────────────────────── */
    if (!strcmp(fn_name, "stop")) {
        NEED(1); REQ_INT(0, h);
        if (h < 1 || h > ws_max_servers) return wsrv_nil();
        pthread_mutex_lock(&ws_cfg_mu);
        WsServer *srv = ws_servers[h-1]; ws_servers[h-1] = NULL;
        pthread_mutex_unlock(&ws_cfg_mu);
        if (srv) {
            srv->auto_stop = 1;
            if (srv->worker_tids && srv->worker_count > 0) {
                for (int _wi = 0; _wi < srv->worker_count; _wi++)
                    pthread_join(srv->worker_tids[_wi], NULL);
                free(srv->worker_tids);
                srv->worker_tids  = NULL;
                srv->worker_count = 0;
            }
        }
        ws_stop_server(srv);
        return wsrv_nil();
    }

    /* ── wserver.version() → str ─────────────────────────────────── */
    if (!strcmp(fn_name, "version")) {
        char buf[64];
        snprintf(buf, sizeof(buf), "libmicrohttpd/%s (epoll+pool)", MHD_get_version());
        return wsrv_str(buf);
    }

#undef WS_ERR
#undef NEED
#undef REQ_INT
#undef REQ_STR

    snprintf(errbuf, sizeof(errbuf), "wserver.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
    *had_error = 1;
    return wsrv_nil();
}

#else /* !FLUXA_WSERVER_MHD — stub backend */
static Value __attribute__((unused)) ws_stub_dispatch(const char *fn_name,
                               const Value *args, int argc,
                               ErrStack *err, int *had_error,
                               int line, void *cfg) {
    char errbuf[280];
    (void)args; (void)argc; (void)cfg;
    fprintf(stderr, "[fluxa] std.wserver: stub backend — "
            "install libmicrohttpd-dev and rebuild with make build\n");
    snprintf(errbuf, sizeof(errbuf),
             "wserver.%s: backend not available (libmicrohttpd not found at build time)",
             fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
    *had_error = 1;
    Value v; v.type = VAL_NIL; return v;
}
#endif /* FLUXA_WSERVER_MHD */

FLUXA_LIB_EXPORT(
    name      = "wserver",
    toml_key  = "std.wserver",
    owner     = "wserver",
    call      = fluxa_std_wserver_call,
    rt_aware  = 1,
    cfg_aware = 0
)

#endif /* FLUXA_STD_WSERVER_H */
