#ifndef FLUXA_STD_WSERVER_H
#define FLUXA_STD_WSERVER_H

/*
 * std.wserver — Resilient HTTP server for Fluxa-lang
 *
 * Backend: libmicrohttpd (GNU MHD).
 * Design: opaque int handles. No dyn cursors exposed to Fluxa code.
 *
 * HTTP methods: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS.
 * wserver.req_method() returns the method string exactly as received.
 *
 * API:
 *   wserver.serve(int port)             → int  server handle (manual mode)
 *   wserver.serve(int port, bool auto)  → int  server handle
 *       auto=false: manual — user controls workers via ft + Block
 *       auto=true:  auto-scaling — lib manages MHD thread pool internally.
 *                   The Fluxa accept/reply loop is identical in both modes.
 *                   The difference is only how many MHD threads receive
 *                   connections simultaneously — invisible to Fluxa code.
 *   wserver.accept(int server, int timeout_ms)            → int  request handle (0 = timeout)
 *   wserver.req_method(int req)                           → str
 *   wserver.req_path(int req)                             → str
 *   wserver.req_body(int req)                             → str
 *   wserver.req_header(int req, str name)                 → str  ("" if absent)
 *   wserver.reply(int req, int status, str body)          → nil
 *   wserver.reply_json(int req, int status, str json)     → nil
 *   wserver.reply_headers(int req, int status, str body,
 *                         str arr headers, int n)         → nil
 *   wserver.connections(int server)                       → int  active count
 *   wserver.stop(int server)                              → nil
 *   wserver.version()                                     → str
 *
 * Configuration (fluxa.toml):
 *   [libs.wserver]
 *   max_servers       = 4      # max simultaneous servers       (hard cap 32)
 *   max_requests      = 128    # max simultaneous live requests (hard cap 4096)
 *   max_body_bytes    = 65536  # max request body size          (hard cap 16MB)
 *   max_header_pairs  = 16     # max header pairs in reply_headers (hard cap 128)
 *   max_header_bytes  = 4096   # max bytes per header key+value (hard cap 65536)
 *   queue_depth       = 256    # per-server accept queue depth  (hard cap 4096)
 *   # Auto-scaling pool (only relevant when serve(..., true)):
 *   min_threads       = 2      # min MHD threads in pool        (hard cap 64)
 *   max_threads       = 16     # max MHD threads in pool        (hard cap 256)
 *   scale_up_queue    = 4      # queue depth to trigger scale-up
 *   scale_down_idle   = 10     # idle seconds to trigger scale-down
 *
 * Manual mode — user controls workers:
 *
 *   import std wserver
 *   import std flxthread as ft
 *
 *   fn worker_loop(int srv) nil {
 *       while !ft.should_stop() {
 *           danger {
 *               int req = wserver.accept(srv, 100)
 *               if req != 0 {
 *                   str method = wserver.req_method(req)
 *                   if method == "GET"    { wserver.reply(req, 200, "ok") }
 *                   if method == "POST"   { wserver.reply(req, 201, wserver.req_body(req)) }
 *                   if method == "PUT"    { wserver.reply(req, 200, wserver.req_body(req)) }
 *                   if method == "PATCH"  { wserver.reply(req, 200, wserver.req_body(req)) }
 *                   if method == "DELETE" { wserver.reply(req, 204, "") }
 *               }
 *           }
 *       }
 *   }
 *
 *   int srv = 0
 *   danger { srv = wserver.serve(8080, false) }
 *
 *   Block w1 typeof Worker
 *   Block w2 typeof Worker
 *   ft.new("t1", w1, "run", srv)
 *   ft.new("t2", w2, "run", srv)
 *   ft.resolve_all()
 *   wserver.stop(srv)
 *
 * Auto-scaling mode — lib manages MHD thread pool:
 *
 *   import std wserver
 *   import std flxthread as ft
 *
 *   fn worker_loop(int srv) nil {
 *       while !ft.should_stop() {
 *           danger {
 *               int req = wserver.accept(srv, 100)
 *               if req != 0 {
 *                   str method = wserver.req_method(req)
 *                   if method == "GET"  { wserver.reply(req, 200, "ok") }
 *                   if method == "POST" { wserver.reply(req, 201, wserver.req_body(req)) }
 *               }
 *           }
 *       }
 *   }
 *
 *   int srv = 0
 *   danger { srv = wserver.serve(8080, true) }
 *
 *   Block w1 typeof Worker
 *   ft.new("t1", w1, "run", srv)
 *   ft.resolve_all()
 *   wserver.stop(srv)
 *
 * The Fluxa code is identical in both modes. With auto=true the lib adds
 * and removes MHD threads based on queue pressure — the Fluxa worker sees
 * the same accept/reply interface regardless.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Defaults and hard caps ──────────────────────────────────────── */
#define WS_DEFAULT_MAX_SERVERS       4
#define WS_DEFAULT_MAX_REQUESTS      128
#define WS_DEFAULT_MAX_BODY_BYTES    65536
#define WS_DEFAULT_MAX_HEADER_PAIRS  16
#define WS_DEFAULT_MAX_HEADER_BYTES  4096
#define WS_DEFAULT_QUEUE_DEPTH       256
#define WS_DEFAULT_MIN_THREADS       2
#define WS_DEFAULT_MAX_THREADS       16
#define WS_DEFAULT_SCALE_UP_QUEUE    4
#define WS_DEFAULT_SCALE_DOWN_IDLE   10

#define WS_HARD_MAX_SERVERS       32
#define WS_HARD_MAX_REQUESTS      4096
#define WS_HARD_MAX_BODY_BYTES    (16*1024*1024)
#define WS_HARD_MAX_HEADER_PAIRS  128
#define WS_HARD_MAX_HEADER_BYTES  65536
#define WS_HARD_QUEUE_DEPTH       4096
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
 * Real backend — libmicrohttpd
 * ══════════════════════════════════════════════════════════════════ */

#include <microhttpd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/* ── Config ──────────────────────────────────────────────────────── */
static pthread_mutex_t ws_cfg_mu       = PTHREAD_MUTEX_INITIALIZER;
static int ws_cfg_initialized          = 0;
static int ws_max_servers              = 0;
static int ws_max_requests             = 0;
static int ws_max_body_bytes           = 0;
static int ws_max_header_pairs         = 0;
static int ws_max_header_bytes         = 0;
static int ws_queue_depth              = 0;
static int ws_min_threads              = 0;
static int ws_max_threads              = 0;
static int ws_scale_up_queue           = 0;
static int ws_scale_down_idle          = 0;

static void ws_ensure_config(const FluxaConfig *cfg) {
#define WS_CLAMP(v, def, hi) ((v) > 0 && (v) <= (hi) ? (v) : (def))
    ws_max_servers      = WS_CLAMP(cfg ? cfg->wserver_max_servers      : 0, WS_DEFAULT_MAX_SERVERS,      WS_HARD_MAX_SERVERS);
    ws_max_requests     = WS_CLAMP(cfg ? cfg->wserver_max_requests     : 0, WS_DEFAULT_MAX_REQUESTS,     WS_HARD_MAX_REQUESTS);
    ws_max_body_bytes   = WS_CLAMP(cfg ? cfg->wserver_max_body_bytes   : 0, WS_DEFAULT_MAX_BODY_BYTES,   WS_HARD_MAX_BODY_BYTES);
    ws_max_header_pairs = WS_CLAMP(cfg ? cfg->wserver_max_header_pairs : 0, WS_DEFAULT_MAX_HEADER_PAIRS, WS_HARD_MAX_HEADER_PAIRS);
    ws_max_header_bytes = WS_CLAMP(cfg ? cfg->wserver_max_header_bytes : 0, WS_DEFAULT_MAX_HEADER_BYTES, WS_HARD_MAX_HEADER_BYTES);
    ws_queue_depth      = WS_CLAMP(cfg ? cfg->wserver_queue_depth      : 0, WS_DEFAULT_QUEUE_DEPTH,      WS_HARD_QUEUE_DEPTH);
    ws_min_threads      = WS_CLAMP(cfg ? cfg->wserver_min_threads      : 0, WS_DEFAULT_MIN_THREADS,      WS_HARD_MIN_THREADS);
    ws_max_threads      = WS_CLAMP(cfg ? cfg->wserver_max_threads      : 0, WS_DEFAULT_MAX_THREADS,      WS_HARD_MAX_THREADS);
    ws_scale_up_queue   = WS_CLAMP(cfg ? cfg->wserver_scale_up_queue   : 0, WS_DEFAULT_SCALE_UP_QUEUE,   ws_queue_depth);
    ws_scale_down_idle  = WS_CLAMP(cfg ? cfg->wserver_scale_down_idle  : 0, WS_DEFAULT_SCALE_DOWN_IDLE,  3600);
    /* min_threads must not exceed max_threads */
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
    char   *headers_json;
    size_t  headers_json_len;
    int     status;
    char   *reply_body;
    char    reply_ct[256];
    int     reply_ready;
    int     consumed;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} WsRequest;

/* ── Pool thread state ───────────────────────────────────────────── */
typedef struct WsPoolThread {
    pthread_t          tid;
    struct MHD_Daemon *daemon;   /* each pool thread is its own MHD daemon */
    int                port;
    int                active;   /* 1 = running */
    int                idle_sec; /* incremented by monitor each second of idleness */
    struct WsServer_  *srv;      /* back-pointer */
    struct WsPoolThread *next;
} WsPoolThread;

/* ── Server struct ───────────────────────────────────────────────── */
typedef struct WsServer_ {
    int                port;
    int                running;
    int                auto_scale;
    /* Accept queue — shared across all pool threads */
    pthread_mutex_t    queue_mu;
    pthread_cond_t     queue_cv;
    WsRequest        **queue;
    int                q_head;
    int                q_tail;
    int                q_count;
    int                q_cap;
    int                conn_count;
    /* Auto-scaling pool */
    pthread_mutex_t    pool_mu;
    WsPoolThread      *pool_head; /* linked list of pool threads */
    int                pool_size; /* current thread count */
    pthread_t          monitor_tid;
    int                monitor_stop;
} WsServer;

/* ── Handle tables ───────────────────────────────────────────────── */
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
        free(req->headers_json);
        free(req->reply_body);
        pthread_mutex_destroy(&req->mu);
        pthread_cond_destroy(&req->cv);
        free(req);
    }
}

/* ── MHD connection context ──────────────────────────────────────── */
typedef struct {
    WsServer *srv;
    char     *body_buf;
    size_t    body_len;
    int       body_overflow;
    int       conn_counted; /* 1 = conn_count was incremented */
} WsConnCtx;

/* ── Header iterator ─────────────────────────────────────────────── */
typedef struct { char *buf; size_t cap; size_t len; int overflow; } WsHdrCtx;

static enum MHD_Result ws_hdr_iter(void *cls, enum MHD_ValueKind kind,
                                    const char *key, const char *value) {
    (void)kind;
    WsHdrCtx *hc = (WsHdrCtx *)cls;
    if (hc->overflow || !key || !value) return MHD_YES;
    size_t klen = strlen(key), vlen = strlen(value);
    if ((int)(klen + vlen) > ws_max_header_bytes) { hc->overflow = 1; return MHD_YES; }
    size_t needed = klen + vlen + 8;
    if (hc->len + needed + 2 > hc->cap) {
        size_t nc = hc->cap * 2 + needed + 64;
        char *nb = (char *)realloc(hc->buf, nc);
        if (!nb) { hc->overflow = 1; return MHD_YES; }
        hc->buf = nb; hc->cap = nc;
    }
    if (hc->len > 1) hc->buf[hc->len - 1] = ',';
    else { hc->buf[0] = '{'; hc->len = 1; }
    char kbuf[1024], vbuf[8192];
    size_t ki = 0, vi = 0;
    for (size_t i = 0; i < klen && ki < sizeof(kbuf)-1; i++)
        kbuf[ki++] = (key[i]=='"'||key[i]=='\\') ? '_' : key[i];
    kbuf[ki] = '\0';
    for (size_t i = 0; i < vlen && vi < sizeof(vbuf)-1; i++)
        vbuf[vi++] = (value[i]=='"'||value[i]=='\\') ? '_' : value[i];
    vbuf[vi] = '\0';
    int w = snprintf(hc->buf + hc->len, hc->cap - hc->len, "\"%s\":\"%s\"}", kbuf, vbuf);
    if (w > 0) hc->len += (size_t)w;
    return MHD_YES;
}

/* ── MHD request handler — shared by all pool threads ───────────── */
static enum MHD_Result ws_mhd_handler(
        void *cls, struct MHD_Connection *conn,
        const char *url, const char *method, const char *version,
        const char *upload_data, size_t *upload_data_size, void **con_cls) {

    WsServer *srv = (WsServer *)cls;
    (void)version;

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

    if (ctx->body_overflow) {
        const char *b = "Request Entity Too Large";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 413, r);
        MHD_destroy_response(r);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        return rv;
    }

    size_t mlen = method ? strlen(method) : 0;
    if (mlen == 0 || mlen > 15) {
        const char *b = "Bad Request";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 400, r);
        MHD_destroy_response(r);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        return rv;
    }
    for (size_t i = 0; i < mlen; i++) {
        if (method[i] < 'A' || method[i] > 'Z') {
            const char *b = "Bad Request";
            struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
            enum MHD_Result rv = MHD_queue_response(conn, 400, r);
            MHD_destroy_response(r);
            free(ctx->body_buf); free(ctx); *con_cls = NULL;
            return rv;
        }
    }

    WsRequest *req = (WsRequest *)calloc(1, sizeof(WsRequest));
    if (!req) { free(ctx->body_buf); free(ctx); *con_cls = NULL; return MHD_NO; }
    if (pthread_mutex_init(&req->mu, NULL) != 0 || pthread_cond_init(&req->cv, NULL) != 0) {
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

    WsHdrCtx hc; hc.cap=512; hc.len=0; hc.overflow=0;
    hc.buf = (char *)malloc(hc.cap);
    if (!hc.buf) { ws_free_request(0,req); free(ctx->body_buf); free(ctx); *con_cls=NULL; return MHD_NO; }
    hc.buf[0]='{'; hc.buf[1]='}'; hc.len=2;
    MHD_get_connection_values(conn, MHD_HEADER_KIND, ws_hdr_iter, &hc);
    req->headers_json     = hc.buf;
    req->headers_json_len = hc.len;

    pthread_mutex_lock(&srv->queue_mu);
    int enqueued = 0;
    if (srv->q_count < srv->q_cap) {
        pthread_mutex_lock(&ws_cfg_mu);
        int slot = ws_alloc_request(req);
        pthread_mutex_unlock(&ws_cfg_mu);
        if (slot > 0) {
            srv->queue[srv->q_tail] = req;
            req->status = slot;
            srv->q_tail = (srv->q_tail + 1) % srv->q_cap;
            srv->q_count++;
            srv->conn_count++;
            ctx->conn_counted = 1;
            enqueued = slot;
        }
    }
    pthread_cond_broadcast(&srv->queue_cv);
    pthread_mutex_unlock(&srv->queue_mu);

    if (!enqueued) {
        ws_free_request(0, req);
        free(ctx->body_buf); free(ctx); *con_cls = NULL;
        const char *b = "Service Unavailable";
        struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
        enum MHD_Result rv = MHD_queue_response(conn, 503, r);
        MHD_destroy_response(r);
        return rv;
    }

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 30;

    pthread_mutex_lock(&req->mu);
    while (!req->reply_ready)
        if (pthread_cond_timedwait(&req->cv, &req->mu, &dl) == ETIMEDOUT) break;

    int    rstatus = req->reply_ready ? req->status : 504;
    char  *rbody   = req->reply_ready ? req->reply_body : NULL;
    char   rct[256];
    snprintf(rct, sizeof(rct), "%s", req->reply_ct[0] ? req->reply_ct : "text/plain");
    pthread_mutex_unlock(&req->mu);

    const char *final_body = rbody ? rbody : (rstatus==504 ? "Gateway Timeout" : "");
    struct MHD_Response *mhd_resp = MHD_create_response_from_buffer(
        strlen(final_body), (void *)final_body, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(mhd_resp, "Content-Type", rct);
    enum MHD_Result rv = MHD_queue_response(conn, (unsigned int)rstatus, mhd_resp);
    MHD_destroy_response(mhd_resp);

    /* conn_count and ctx freed in ws_request_completed after MHD closes
     * the connection — guarantees all bytes are sent before stop() proceeds. */
    pthread_mutex_lock(&req->mu);
    req->consumed = 1;
    pthread_cond_broadcast(&req->cv);
    pthread_mutex_unlock(&req->mu);

    *con_cls = ctx; /* keep ctx alive for ws_request_completed */
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

static void ws_finish_reply(long req_handle, WsRequest *req) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 5;
    pthread_mutex_lock(&req->mu);
    while (!req->consumed)
        if (pthread_cond_timedwait(&req->cv, &req->mu, &dl) == ETIMEDOUT) break;
    pthread_mutex_unlock(&req->mu);
    ws_free_request(req_handle, req);
}

/* ── Auto-scaling pool helpers ───────────────────────────────────── */

/* Start one additional MHD daemon on the same port as the server.
 * MHD_USE_THREAD_PER_CONNECTION means each daemon handles concurrent
 * connections on its own thread pool. All daemons share the same
 * ws_mhd_handler which enqueues into srv->queue.
 * Called while holding srv->pool_mu. */
static int ws_pool_add_thread(WsServer *srv) {
    if (srv->pool_size >= ws_max_threads) return 0;

    WsPoolThread *pt = (WsPoolThread *)calloc(1, sizeof(WsPoolThread));
    if (!pt) return 0;

    pt->srv    = srv;
    pt->port   = srv->port;
    pt->active = 1;

    pt->daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
        (uint16_t)srv->port,
        NULL, NULL,
        ws_mhd_handler, srv,
        MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
        MHD_OPTION_END);

    if (!pt->daemon) { free(pt); return 0; }

    pt->next        = srv->pool_head;
    srv->pool_head  = pt;
    srv->pool_size++;
    return 1;
}

/* Remove one idle thread from the pool tail (LIFO — newest first).
 * Called while holding srv->pool_mu. */
static void ws_pool_remove_one(WsServer *srv) {
    if (srv->pool_size <= ws_min_threads) return;

    /* Find a thread that has been idle long enough */
    WsPoolThread *prev = NULL, *cur = srv->pool_head;
    while (cur) {
        if (cur->idle_sec >= ws_scale_down_idle) {
            MHD_stop_daemon(cur->daemon);
            if (prev) prev->next = cur->next;
            else       srv->pool_head = cur->next;
            free(cur);
            srv->pool_size--;
            return;
        }
        prev = cur; cur = cur->next;
    }
}

/* Monitor thread — runs for the lifetime of the server when auto=true.
 * Wakes every second, adjusts pool_size based on queue pressure. */
static void *ws_monitor_thread(void *arg) {
    WsServer *srv = (WsServer *)arg;

    while (1) {
        /* Sleep 1s in short intervals to catch stop quickly */
        for (int i = 0; i < 10; i++) {
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100000000L; /* 100ms */
            nanosleep(&ts, NULL);
            if (srv->monitor_stop) goto done;
        }
        if (srv->monitor_stop) break;

        /* Sample queue depth */
        pthread_mutex_lock(&srv->queue_mu);
        int qdepth = srv->q_count;
        pthread_mutex_unlock(&srv->queue_mu);

        pthread_mutex_lock(&srv->pool_mu);

        /* Scale up: queue pressure exceeds threshold */
        if (qdepth >= ws_scale_up_queue && srv->pool_size < ws_max_threads) {
            ws_pool_add_thread(srv);
            /* Reset idle counters — new thread means activity */
            for (WsPoolThread *p = srv->pool_head; p; p = p->next)
                p->idle_sec = 0;
        } else if (qdepth == 0) {
            /* Increment idle counter for all threads when queue empty */
            for (WsPoolThread *p = srv->pool_head; p; p = p->next)
                p->idle_sec++;
            ws_pool_remove_one(srv);
        } else {
            /* Queue has items but not overwhelming — reset idle counters */
            for (WsPoolThread *p = srv->pool_head; p; p = p->next)
                p->idle_sec = 0;
        }

        pthread_mutex_unlock(&srv->pool_mu);
    }
done:
    return NULL;
}

/* Stop and free all pool threads. Called from ws_stop_server. */
static void ws_pool_destroy(WsServer *srv) {
    /* Signal monitor to stop */
    srv->monitor_stop = 1;
    pthread_join(srv->monitor_tid, NULL);

    pthread_mutex_lock(&srv->pool_mu);
    WsPoolThread *cur = srv->pool_head;
    while (cur) {
        WsPoolThread *next = cur->next;
        MHD_stop_daemon(cur->daemon);
        free(cur);
        cur = next;
    }
    srv->pool_head = NULL;
    srv->pool_size = 0;
    pthread_mutex_unlock(&srv->pool_mu);
    pthread_mutex_destroy(&srv->pool_mu);
}

/* ── Start a server with or without auto-scaling ─────────────────── */
static WsServer *ws_start_server(int port, int auto_scale, char *errbuf, size_t ebsz) {
    WsServer *srv = (WsServer *)calloc(1, sizeof(WsServer));
    if (!srv) { snprintf(errbuf, ebsz, "out of memory"); return NULL; }

    srv->port       = port;
    srv->running    = 1;
    srv->auto_scale = auto_scale;
    srv->q_cap      = ws_queue_depth;
    srv->queue      = (WsRequest **)calloc((size_t)ws_queue_depth, sizeof(WsRequest *));
    if (!srv->queue) { free(srv); snprintf(errbuf, ebsz, "out of memory allocating queue"); return NULL; }

    if (pthread_mutex_init(&srv->queue_mu, NULL) != 0 ||
        pthread_cond_init(&srv->queue_cv, NULL)  != 0) {
        free(srv->queue); free(srv);
        snprintf(errbuf, ebsz, "pthread init failed"); return NULL;
    }

    if (auto_scale) {
        if (pthread_mutex_init(&srv->pool_mu, NULL) != 0) {
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "pthread pool_mu init failed"); return NULL;
        }
        /* Start min_threads daemons */
        pthread_mutex_lock(&srv->pool_mu);
        for (int i = 0; i < ws_min_threads; i++) {
            if (!ws_pool_add_thread(srv)) {
                /* Partial start — still usable, just fewer initial threads */
                break;
            }
        }
        pthread_mutex_unlock(&srv->pool_mu);

        if (srv->pool_size == 0) {
            ws_pool_destroy(srv); /* frees pool_mu too */
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: failed to bind port %d (auto)", port);
            return NULL;
        }

        /* Start monitor */
        if (pthread_create(&srv->monitor_tid, NULL, ws_monitor_thread, srv) != 0) {
            ws_pool_destroy(srv);
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: monitor thread creation failed");
            return NULL;
        }
    } else {
        /* Manual mode — single MHD daemon */
        struct MHD_Daemon *d = MHD_start_daemon(
            MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
            (uint16_t)port, NULL, NULL,
            ws_mhd_handler, srv,
            MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
            MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
            MHD_OPTION_END);

        if (!d) {
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: failed to bind port %d", port);
            return NULL;
        }

        /* Store as single-entry pool so stop logic is uniform */
        if (pthread_mutex_init(&srv->pool_mu, NULL) != 0) {
            MHD_stop_daemon(d);
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "pthread pool_mu init failed"); return NULL;
        }

        WsPoolThread *pt = (WsPoolThread *)calloc(1, sizeof(WsPoolThread));
        if (!pt) {
            MHD_stop_daemon(d);
            pthread_mutex_destroy(&srv->pool_mu);
            pthread_mutex_destroy(&srv->queue_mu);
            pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "out of memory"); return NULL;
        }
        pt->daemon      = d;
        pt->port        = port;
        pt->active      = 1;
        pt->srv         = srv;
        srv->pool_head  = pt;
        srv->pool_size  = 1;
    }

    return srv;
}

/* ── Stop and free a server ──────────────────────────────────────── */
static void ws_stop_server(WsServer *srv) {
    if (!srv) return;

    /* Wait for in-flight responses to be fully sent (conn_count → 0).
     * This prevents MHD_stop_daemon from closing sockets while bytes are
     * still being flushed to the client. Timeout: 3s. */
    {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 3;
        struct timespec step; step.tv_sec = 0; step.tv_nsec = 10000000L; /* 10ms */
        for (;;) {
            pthread_mutex_lock(&srv->queue_mu);
            int n = srv->conn_count;
            pthread_mutex_unlock(&srv->queue_mu);
            if (n <= 0) break;
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > dl.tv_sec ||
                (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) break;
            nanosleep(&step, NULL);
        }
    }

    /* In auto mode, monitor_stop + join is done in ws_pool_destroy.
     * In manual mode monitor_tid is zero — join is a no-op. */
    if (srv->auto_scale) {
        ws_pool_destroy(srv);
    } else {
        /* Stop the single daemon via pool_head */
        srv->monitor_stop = 1; /* harmless in manual mode */
        pthread_mutex_lock(&srv->pool_mu);
        WsPoolThread *cur = srv->pool_head;
        while (cur) {
            WsPoolThread *next = cur->next;
            MHD_stop_daemon(cur->daemon);
            free(cur);
            cur = next;
        }
        srv->pool_head = NULL;
        srv->pool_size = 0;
        pthread_mutex_unlock(&srv->pool_mu);
        pthread_mutex_destroy(&srv->pool_mu);
    }

    /* Drain pending queued requests */
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
static inline Value fluxa_std_wserver_call(const char *fn_name,
                                            const Value *args, int argc,
                                            ErrStack *err, int *had_error,
                                            int line,
                                            const FluxaConfig *cfg) {
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

        /* Optional second arg: bool auto_scale (default false) */
        int auto_scale = 0;
        if (argc >= 2 && args[1].type == VAL_BOOL)
            auto_scale = args[1].as.boolean ? 1 : 0;
        else if (argc >= 2 && args[1].type == VAL_INT)
            auto_scale = args[1].as.integer ? 1 : 0;

        char ebuf[256] = "";
        WsServer *srv = ws_start_server((int)port, auto_scale, ebuf, sizeof(ebuf));
        if (!srv) {
            errstack_push(err, ERR_FLUXA, ebuf[0] ? ebuf : "wserver.serve: failed",
                          "wserver", line);
            *had_error = 1; return wsrv_int(0);
        }

        pthread_mutex_lock(&ws_cfg_mu);
        int slot = ws_alloc_server(srv);
        pthread_mutex_unlock(&ws_cfg_mu);

        if (!slot) {
            ws_stop_server(srv);
            WS_ERR("server table full — increase [libs.wserver] max_servers");
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
            if (pthread_cond_timedwait(&srv->queue_cv, &srv->queue_mu, &dl) == ETIMEDOUT) {
                pthread_mutex_unlock(&srv->queue_mu);
                return wsrv_int(0);
            }
        }
        WsRequest *req = srv->queue[srv->q_head];
        srv->q_head = (srv->q_head + 1) % srv->q_cap;
        srv->q_count--;
        pthread_mutex_unlock(&srv->queue_mu);

        long req_handle = req->status;
        req->status = 0;
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
        if (!r || !r->headers_json) return wsrv_str("");
        if (strlen(hname) > 256) return wsrv_str("");
        char needle[270];
        snprintf(needle, sizeof(needle), "\"%s\":\"", hname);
        const char *pos = strstr(r->headers_json, needle);
        if (!pos) return wsrv_str("");
        pos += strlen(needle);
        const char *end = strchr(pos, '"');
        if (!end) return wsrv_str("");
        size_t vlen = (size_t)(end - pos);
        if ((int)vlen > ws_max_header_bytes) vlen = (size_t)ws_max_header_bytes;
        char *val = (char *)malloc(vlen + 1);
        if (!val) return wsrv_str("");
        memcpy(val, pos, vlen); val[vlen] = '\0';
        Value ret = wsrv_str(val);
        free(val);
        return ret;
    }

    /* ── wserver.reply(req, status, body) → nil ─────────────────── */
    if (!strcmp(fn_name, "reply")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, status); REQ_STR(2, body);
        if (status < 100 || status > 599) WS_ERR("HTTP status must be 100-599");
        WsRequest *r = ws_get_request(h, err, had_error, line, fn_name);
        if (!r) return wsrv_nil();
        r->reply_body  = strdup(body);
        r->reply_ct[0] = '\0';
        pthread_mutex_lock(&r->mu);
        r->status = (int)status; r->reply_ready = 1;
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
        r->reply_body = strdup(json);
        snprintf(r->reply_ct, sizeof(r->reply_ct), "application/json");
        pthread_mutex_lock(&r->mu);
        r->status = (int)status; r->reply_ready = 1;
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
                     "wserver.reply_headers: %ld pairs > max_header_pairs %d"
                     " — increase [libs.wserver] max_header_pairs", npairs, ws_max_header_pairs);
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
        r->status = (int)status; r->reply_ready = 1;
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

    /* ── wserver.stop(server) → nil ─────────────────────────────── */
    if (!strcmp(fn_name, "stop")) {
        NEED(1); REQ_INT(0, h);
        if (h < 1 || h > ws_max_servers) return wsrv_nil();
        pthread_mutex_lock(&ws_cfg_mu);
        WsServer *srv = ws_servers[h-1]; ws_servers[h-1] = NULL;
        pthread_mutex_unlock(&ws_cfg_mu);
        ws_stop_server(srv);
        return wsrv_nil();
    }

    /* ── wserver.version() → str ─────────────────────────────────── */
    if (!strcmp(fn_name, "version")) {
        char buf[64];
        snprintf(buf, sizeof(buf), "libmicrohttpd/%s", MHD_get_version());
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
/* ══════════════════════════════════════════════════════════════════
 * Stub backend
 * ══════════════════════════════════════════════════════════════════ */
static inline Value fluxa_std_wserver_call(const char *fn_name,
                                            const Value *args, int argc,
                                            ErrStack *err, int *had_error,
                                            int line,
                                            const FluxaConfig *cfg) {
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
    rt_aware  = 0,
    cfg_aware = 1
)

#endif /* FLUXA_STD_WSERVER_H */
