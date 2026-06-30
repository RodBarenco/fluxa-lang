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
/* ── direct-socket mode (opt-in: [libs.wserver] direct=1) ───────────── */
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

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
static int ws_nonblocking             = 0;   /* [libs.wserver] nonblocking: suspend/resume handoff */
static int ws_direct                  = 0;   /* [libs.wserver] direct: socket I/O on the worker thread */
static void ws_conn_notify(void *cls, struct MHD_Connection *connection, void **socket_context, enum MHD_ConnectionNotificationCode toe);

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
    ws_nonblocking = (cfg && cfg->wserver_nonblocking) ? 1 : 0;
    ws_direct      = (cfg && cfg->wserver_direct) ? 1 : 0;
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
    int     inline_mode;    /* 1 when handled inline on the MHD epoll thread (no handoff) */
    int     suspend_mode;   /* 1 when the epoll thread suspended the conn instead of blocking */
    struct MHD_Connection *conn;  /* connection to resume from reply() (suspend mode) */
    int     conn_fd;        /* direct mode: the client socket (-1 otherwise) */
    int     keep_alive;     /* direct mode: 1 to keep the connection after reply */
    int     direct;         /* 1 only for direct-socket requests (calloc => 0 for MHD path) */
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
    /* Inline mode (serve_inline): run the handler on the MHD epoll thread,
     * with a per-thread runtime clone — no queue, no condvar handoff. */
    int             inline_mode;
    int             suspend_mode;  /* 1 = suspend/resume handoff (epoll thread doesn't block) */
    int             direct;        /* 1 = direct-socket mode (no MHD; accept/reply do socket I/O) */
    int             listen_fd;     /* direct mode: the listening socket (-1 otherwise) */
    void           *parent_rt;     /* Runtime*  to clone per epoll thread */
    void           *handler_fn;    /* ASTNode*  fn_decl of the handler     */
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
    int        suspended;   /* 1 after enqueue+MHD_suspend_connection: next handler call is the resume */
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

/* ── Inline-mode per-thread runtime clone ────────────────────────────────
 * In inline mode each MHD epoll thread runs the Fluxa handler directly on its
 * own runtime clone (created lazily on first use, reused after, freed when the
 * thread exits — MHD joins its pool threads on daemon shutdown). This removes
 * the queue + condvar handoff (and its cross-thread wakeups) entirely. */
static pthread_key_t  ws_inline_clone_key;
static pthread_once_t ws_inline_clone_once = PTHREAD_ONCE_INIT;
static void ws_inline_clone_dtor(void *p) { if (p) runtime_free_thread_clone((Runtime *)p); }
static void ws_inline_clone_key_init(void) { pthread_key_create(&ws_inline_clone_key, ws_inline_clone_dtor); }
static Runtime *ws_inline_get_clone(Runtime *parent) {
    pthread_once(&ws_inline_clone_once, ws_inline_clone_key_init);
    Runtime *rt = (Runtime *)pthread_getspecific(ws_inline_clone_key);
    if (!rt) { rt = runtime_clone_for_thread(parent); if (rt) pthread_setspecific(ws_inline_clone_key, rt); }
    return rt;
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

    /* ── Suspend/resume: this is the RESUMED call — the Fluxa worker finished
     *    and called MHD_resume_connection(). Queue the response it produced and
     *    free the request slot. No new body arrives on a resumed call. ── */
    if (ctx->suspended) {
        WsRequest  *rq      = ctx->req_ptr;
        int         rstatus = 504;
        const char *rbody   = "";
        char        rct[256];
        snprintf(rct, sizeof(rct), "application/json");
        if (rq) {
            pthread_mutex_lock(&rq->mu);
            rstatus = rq->reply_ready ? rq->http_status : 504;
            rbody   = (rq->reply_ready && rq->reply_body) ? rq->reply_body : "";
            snprintf(rct, sizeof(rct), "%s", rq->reply_ct[0] ? rq->reply_ct : "application/json");
            pthread_mutex_unlock(&rq->mu);
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(rbody), (void *)rbody, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(resp, "Content-Type", rct);
        enum MHD_Result rv = MHD_queue_response(conn, (unsigned int)rstatus, resp);
        MHD_destroy_response(resp);
        if (rq) ws_free_request(ctx->req_handle, rq);   /* MUST_COPY done → safe to free */
        ctx->req_ptr = NULL; ctx->req_handle = 0; ctx->suspended = 0;
        return rv;
    }

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

    /* ── Inline mode: run the Fluxa handler on THIS epoll thread (no handoff) ──
     * Allocate a request slot, run the registered handler on this thread's
     * runtime clone, then queue the response it produced and free the slot.
     * No queue, no condvar wait — the response is ready when the handler
     * returns, on the same thread that will send it. */
    if (srv->inline_mode) {
        pthread_mutex_lock(&ws_cfg_mu);
        int islot = ws_alloc_request(req);
        pthread_mutex_unlock(&ws_cfg_mu);
        if (!islot) {
            ws_free_request(0, req);
            free(ctx->body_buf); free(ctx); *con_cls = NULL;
            const char *b = "Service Unavailable";
            struct MHD_Response *r = MHD_create_response_from_buffer(strlen(b),(void*)b,MHD_RESPMEM_PERSISTENT);
            enum MHD_Result rv = MHD_queue_response(conn, 503, r);
            MHD_destroy_response(r);
            return rv;
        }
        req->inline_mode = 1;
        /* The request slot is freed below by this thread; make sure
         * ws_request_completed (which only frees ctx) never touches it. */
        ctx->req_ptr = NULL; ctx->req_handle = 0; ctx->conn_counted = 0;

        Runtime *clone = ws_inline_get_clone((Runtime *)srv->parent_rt);
        if (clone && srv->handler_fn) {
            ASTNode *hfn = (ASTNode *)srv->handler_fn;
            if (hfn->as.func_decl.param_count >= 1) {
                int zn = hfn->as.func_decl.param_count + 4;
                if (zn > FLUXA_STACK_SIZE) zn = FLUXA_STACK_SIZE;
                for (int _z = 0; _z < zn; _z++) clone->stack[_z].type = VAL_NIL;
                clone->stack[0].type = VAL_INT; clone->stack[0].as.integer = islot;
                clone->stack_size = 1;
            }
            runtime_eval(clone, hfn->as.func_decl.body);
        }

        int         rstatus = req->reply_ready ? req->http_status : 500;
        const char *rbody   = (req->reply_ready && req->reply_body) ? req->reply_body : "";
        char        rct[256];
        snprintf(rct, sizeof(rct), "%s", req->reply_ct[0] ? req->reply_ct : "application/json");
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(rbody), (void *)rbody, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(resp, "Content-Type", rct);
        enum MHD_Result rv = MHD_queue_response(conn, (unsigned int)rstatus, resp);
        MHD_destroy_response(resp);
        ws_free_request(islot, req);   /* frees req (incl. reply_body) + slot */
        *con_cls = ctx;                /* ws_request_completed frees ctx */
        return rv;
    }

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

    /* ── Suspend/resume mode: do NOT block this epoll thread. Park the
     *    connection and return; the Fluxa worker will resume it from reply().
     *    The request is already enqueued and ctx->req_ptr/req_handle are set,
     *    so the resumed call (handled at the top) sends the response. ── */
    if (srv->suspend_mode) {
        req->conn         = conn;
        req->suspend_mode = 1;
        ctx->suspended    = 1;
        *con_cls          = ctx;
        MHD_suspend_connection(conn);
        return MHD_YES;
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

/* ════════════════════════════════════════════════════════════════════
 *  Direct-socket mode  ([libs.wserver] direct = 1)
 *
 *  accept()/reply() do socket I/O on the calling worker thread — no MHD,
 *  no epoll pool, no cross-thread queue/handoff. Each worker runs the full
 *  accept → parse → handler → reply → (keep-alive) cycle on its own thread,
 *  which removes the cross-thread scheduling latency that dominates the tail
 *  on a fractional core. The .flx script (serve(false) + ft.new workers +
 *  accept loop) is unchanged; only the backend behind accept/reply changes.
 * ════════════════════════════════════════════════════════════════════ */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Per-worker connection reader (one/thread). Holds the current keep-alive
 * connection plus any bytes already received beyond the last consumed request
 * (so pipelined requests aren't lost). Stored in TLS via pthread_key, which
 * avoids the non-ISO __thread keyword and keeps -pedantic clean. */
typedef struct { int fd; char *buf; size_t len; size_t cap; } WsRd;
static pthread_key_t  ws_rd_key;
static pthread_once_t ws_rd_once = PTHREAD_ONCE_INIT;
static void ws_rd_dtor(void *p) {
    if (p) { WsRd *r = (WsRd *)p; if (r->fd >= 0) close(r->fd); free(r->buf); free(r); }
}
static void ws_rd_key_init(void) { pthread_key_create(&ws_rd_key, ws_rd_dtor); }
static WsRd *ws_rd_get(void) {
    pthread_once(&ws_rd_once, ws_rd_key_init);
    WsRd *r = (WsRd *)pthread_getspecific(ws_rd_key);
    if (!r) {
        r = (WsRd *)calloc(1, sizeof(WsRd));
        if (r) { r->fd = -1; pthread_setspecific(ws_rd_key, r); }
    }
    return r;
}
/* accept a fresh connection from the listener; -1 on none/error */
static int ws_accept_new(WsServer *srv) {
    int cfd = accept(srv->listen_fd, NULL, NULL);
    if (cfd < 0) return -1;                          /* EAGAIN: another worker took it */
    struct timeval tv; tv.tv_sec = 15; tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    { int one = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    int fl = fcntl(cfd, F_GETFD, 0); if (fl >= 0) fcntl(cfd, F_SETFD, fl | FD_CLOEXEC);
    return cfd;
}

/* locate needle in hay (memmem isn't guaranteed under _POSIX_C_SOURCE) */
static const char *ws_memfind(const char *hay, size_t hlen, const char *nee, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (hay[i] == nee[0] && memcmp(hay + i, nee, nlen) == 0) return hay + i;
    return NULL;
}
/* bounded, case-insensitive substring test within [hay, hay+hlen) */
static int ws_region_has(const char *hay, size_t hlen, const char *lit) {
    size_t nlen = strlen(lit);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != lit[j]) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}
/* lowercase, NUL-terminated copy of [p, p+n) for header-name search */
static char *ws_lower_copy(const char *p, size_t n) {
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        o[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    o[n] = '\0';
    return o;
}
static const char *ws_http_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}
static int ws_write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    return 0;
}
static int ws_http_write_response(int fd, int status, const char *ct,
                                  const char *body, int keep) {
    size_t blen = body ? strlen(body) : 0;
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
        status, ws_http_reason(status), ct ? ct : "text/plain",
        blen, keep ? "keep-alive" : "close");
    if (hl < 0 || hl >= (int)sizeof(hdr)) return -1;
    if (ws_write_all(fd, hdr, (size_t)hl) != 0) return -1;
    if (blen && ws_write_all(fd, body, blen) != 0) return -1;
    return 0;
}

/* Read one HTTP request from fd into req (method/path/body/keep_alive).
 * Returns 1 ok, 0 on clean EOF (peer closed before sending anything),
 * -1 on error/timeout/malformed. Bounded by ws_max_header_bytes/body_bytes. */
static int ws_http_read_request(WsRd *rd, int fd, WsRequest *req) {
    size_t hard_cap = (size_t)ws_max_header_bytes + (size_t)ws_max_body_bytes + 1024;
    if (rd->cap == 0) {
        rd->cap = (size_t)ws_max_header_bytes + 1024;
        rd->buf = (char *)malloc(rd->cap);
        if (!rd->buf) { rd->cap = 0; return -1; }
        rd->len = 0;
    }

    /* 1) read until the header terminator is buffered (leftover may already hold it) */
    const char *term = ws_memfind(rd->buf, rd->len, "\r\n\r\n", 4);
    while (!term) {
        if (rd->len + 1 >= rd->cap) {
            if (rd->cap >= hard_cap) return -1;            /* headers too large */
            size_t nc = rd->cap * 2; if (nc > hard_cap) nc = hard_cap;
            char *nb = (char *)realloc(rd->buf, nc); if (!nb) return -1;
            rd->buf = nb; rd->cap = nc;
        }
        ssize_t n = recv(fd, rd->buf + rd->len, rd->cap - 1 - rd->len, 0);
        if (n == 0) return (rd->len == 0) ? 0 : -1;        /* clean EOF vs truncated */
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        rd->len += (size_t)n;
        term = ws_memfind(rd->buf, rd->len, "\r\n\r\n", 4);
    }
    size_t hdr_len = (size_t)(term - rd->buf);

    /* request line: METHOD SP PATH SP HTTP/x.x */
    const char *sp1 = (const char *)memchr(rd->buf, ' ', hdr_len);
    if (!sp1) return -1;
    size_t mlen = (size_t)(sp1 - rd->buf);
    if (mlen == 0 || mlen > 15) return -1;
    memcpy(req->method, rd->buf, mlen); req->method[mlen] = '\0';

    const char *ps  = sp1 + 1;
    const char *sp2 = (const char *)memchr(ps, ' ', (size_t)(rd->buf + hdr_len - ps));
    if (!sp2) return -1;
    size_t plen = (size_t)(sp2 - ps);
    req->path = (char *)malloc(plen + 1);
    if (!req->path) return -1;
    memcpy(req->path, ps, plen); req->path[plen] = '\0';

    /* headers (lowercased copy for case-insensitive name search) */
    char *lc = ws_lower_copy(rd->buf, hdr_len);
    if (!lc) return -1;
    int http11 = (ws_memfind(lc, hdr_len, "http/1.1", 8) != NULL);
    long clen = 0;
    const char *cl = ws_memfind(lc, hdr_len, "\r\ncontent-length:", 17);
    if (cl) clen = strtol(cl + 17, NULL, 10);
    int conn_close = 0, conn_keep = 0;
    const char *cn = ws_memfind(lc, hdr_len, "\r\nconnection:", 13);
    if (cn) {
        const char *v   = cn + 13;
        const char *eol = ws_memfind(v, (size_t)(lc + hdr_len - v), "\r\n", 2);
        size_t vl = eol ? (size_t)(eol - v) : (size_t)(lc + hdr_len - v);
        if (ws_region_has(v, vl, "close"))      conn_close = 1;
        if (ws_region_has(v, vl, "keep-alive")) conn_keep  = 1;
    }
    free(lc);
    req->keep_alive = http11 ? (conn_close ? 0 : 1) : (conn_keep ? 1 : 0);

    if (clen < 0) clen = 0;
    if (clen > ws_max_body_bytes) return -1;
    size_t total = hdr_len + 4 + (size_t)clen;

    /* 2) read until the full body is buffered. realloc may move rd->buf, so
     *    from here on everything uses offsets — never the stale term/sp pointers. */
    if (total + 1 > rd->cap) {
        size_t nc = total + 1; if (nc > hard_cap) return -1;
        char *nb = (char *)realloc(rd->buf, nc); if (!nb) return -1;
        rd->buf = nb; rd->cap = nc;
    }
    while (rd->len < total) {
        ssize_t n = recv(fd, rd->buf + rd->len, rd->cap - rd->len, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        rd->len += (size_t)n;
    }

    /* 3) hand the body to the request */
    req->body = (char *)malloc((size_t)clen + 1);
    if (!req->body) return -1;
    if (clen) memcpy(req->body, rd->buf + hdr_len + 4, (size_t)clen);
    req->body[clen] = '\0';
    req->body_len   = (size_t)clen;

    /* 4) keep any bytes past this request (a pipelined next request) for later */
    size_t leftover = rd->len - total;
    if (leftover) memmove(rd->buf, rd->buf + total, leftover);
    rd->len = leftover;
    return 1;
}

/* Write the response the worker produced, then keep-alive or close.
 * On keep-alive the connection (and any buffered pipelined bytes) stays in the
 * thread's WsRd for the next accept(); on close the conn is dropped. */
static void ws_direct_send(long req_handle, WsRequest *req) {
    WsRd *rd = ws_rd_get();
    int  fd   = req->conn_fd;
    int  keep = req->keep_alive;
    int  st   = req->http_status ? req->http_status : 200;
    const char *body = req->reply_body ? req->reply_body : "";
    const char *ct   = req->reply_ct[0] ? req->reply_ct  : "text/plain";
    int rc = ws_http_write_response(fd, st, ct, body, keep);
    if (rc == 0 && keep) {
        if (rd) rd->fd = fd;                 /* reuse; leftover bytes in rd->buf preserved */
    } else {
        close(fd);
        if (rd) { rd->fd = -1; rd->len = 0; } /* write error or Connection: close */
    }
    req->conn_fd = -1;                        /* detach before free (fd already handled) */
    ws_free_request(req_handle, req);
}

/* accept() for direct mode — runs on a worker thread. Returns a request
 * handle, or 0 on timeout / no work. A held keep-alive connection is preferred
 * (and its buffered bytes consumed first) but never blocks out new work:
 * the keep-alive fd and the listener are polled together. */
static long ws_direct_accept(WsServer *srv, int timeout_ms) {
    WsRd *rd = ws_rd_get();
    if (!rd) return 0;

    int fd = -1;
    if (rd->fd >= 0) {
        /* Hold a keep-alive conn. If bytes are already buffered, handle them
         * now (poll with 0 timeout); otherwise poll it with the listener so an
         * idle conn doesn't starve new connections. */
        struct pollfd pfds[2];
        pfds[0].fd = rd->fd;         pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = srv->listen_fd; pfds[1].events = POLLIN; pfds[1].revents = 0;
        int pr = poll(pfds, 2, (rd->len > 0) ? 0 : timeout_ms);
        if (srv->auto_stop) return 0;
        if (rd->len > 0 || (pr > 0 && (pfds[0].revents & (POLLIN|POLLHUP|POLLERR|POLLNVAL)))) {
            fd = rd->fd;                                  /* continue on keep-alive conn */
        } else if (pr > 0 && (pfds[1].revents & POLLIN)) {
            close(rd->fd); rd->fd = -1; rd->len = 0;      /* abandon idle keep-alive */
            fd = ws_accept_new(srv);
            if (fd < 0) return 0;
        } else {
            return 0;
        }
    } else {
        struct pollfd pfd; pfd.fd = srv->listen_fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0 || srv->auto_stop) return 0;
        if (!(pfd.revents & POLLIN)) return 0;
        fd = ws_accept_new(srv);
        if (fd < 0) return 0;
    }
    rd->fd = fd;                                          /* active conn for this cycle */

    WsRequest *req = (WsRequest *)calloc(1, sizeof(WsRequest));
    if (!req || pthread_mutex_init(&req->mu, NULL) != 0 || pthread_cond_init(&req->cv, NULL) != 0) {
        free(req); close(fd); rd->fd = -1; rd->len = 0; return 0;
    }
    req->conn_fd = -1;
    int rr = ws_http_read_request(rd, fd, req);
    if (rr <= 0) {
        close(fd); rd->fd = -1; rd->len = 0;
        pthread_mutex_destroy(&req->mu); pthread_cond_destroy(&req->cv);
        free(req->path); free(req->body); free(req);
        return 0;                                         /* keep-alive peer closed, or bad request */
    }
    req->conn_fd = fd;
    req->direct  = 1;
    pthread_mutex_lock(&ws_cfg_mu);
    long h = ws_alloc_request(req);
    pthread_mutex_unlock(&ws_cfg_mu);
    if (!h) {
        close(fd); rd->fd = -1; rd->len = 0;
        pthread_mutex_destroy(&req->mu); pthread_cond_destroy(&req->cv);
        free(req->path); free(req->body); free(req);
        return 0;
    }
    return h;
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
    /* Inline mode: the epoll thread queues the response and frees the slot
     * itself after the handler returns, so reply() must not block here (the
     * "consumed" signal would never come on this same thread) nor free req. */
    if (req && req->direct) { ws_direct_send(req_handle, req); return; }
    if (req && req->inline_mode) return;
    if (req && req->suspend_mode) {
        /* Suspend/resume: the worker has set the response (reply_ready under
         * req->mu). Wake the parked connection; the resumed handler call queues
         * the response and frees the slot. Do NOT block on consumed or free. */
        if (req->conn) MHD_resume_connection(req->conn);
        return;
    }
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
            unsigned int ws_dflags2 = MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG;
            if (srv->suspend_mode) ws_dflags2 |= MHD_ALLOW_SUSPEND_RESUME;
            struct MHD_Daemon *nd = MHD_start_daemon(
                ws_dflags2,
                (uint16_t)srv->port, NULL, NULL,
                ws_mhd_handler, srv,
                MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
                MHD_OPTION_NOTIFY_CONNECTION, ws_conn_notify, NULL,
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
/* Disable Nagle on each new MHD connection: small JSON responses must not wait
 * on the peer's delayed-ACK timer (a classic ~40-100ms tail behind a proxy). */
static void ws_conn_notify(void *cls, struct MHD_Connection *connection,
                           void **socket_context,
                           enum MHD_ConnectionNotificationCode toe) {
    (void)cls; (void)socket_context;
    if (toe == MHD_CONNECTION_NOTIFY_STARTED) {
        const union MHD_ConnectionInfo *ci =
            MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CONNECTION_FD);
        if (ci) { int one = 1; setsockopt(ci->connect_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    }
}

static WsServer *ws_start_server(int port, int auto_scale, int inline_mode,
                                 void *parent_rt, void *handler_fn,
                                 char *errbuf, size_t ebsz) {
    WsServer *srv = (WsServer *)calloc(1, sizeof(WsServer));
    if (!srv) { snprintf(errbuf, ebsz, "out of memory"); return NULL; }

    srv->port       = port;
    srv->running    = 1;
    srv->auto_scale = auto_scale;
    srv->inline_mode = inline_mode;
    srv->parent_rt   = parent_rt;
    srv->handler_fn  = handler_fn;
    srv->suspend_mode = inline_mode ? 0 : ws_nonblocking;
    srv->direct       = (!inline_mode && !auto_scale) ? ws_direct : 0;
    srv->listen_fd    = -1;
    srv->q_cap      = ws_queue_depth;
    srv->queue      = (WsRequest **)calloc((size_t)ws_queue_depth, sizeof(WsRequest *));
    if (!srv->queue) { free(srv); snprintf(errbuf, ebsz, "out of memory for queue"); return NULL; }

    if (pthread_mutex_init(&srv->queue_mu, NULL) != 0 ||
        pthread_cond_init(&srv->queue_cv, NULL)  != 0) {
        free(srv->queue); free(srv);
        snprintf(errbuf, ebsz, "pthread init failed"); return NULL;
    }

    /* ── Direct-socket mode: bind a listening socket; no MHD, no monitor.
     *    Worker threads accept/read/reply on it directly. ── */
    if (srv->direct) {
        signal(SIGPIPE, SIG_IGN);
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) {
            pthread_mutex_destroy(&srv->queue_mu); pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: socket() failed"); return NULL;
        }
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port        = htons((uint16_t)port);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
            listen(lfd, ws_queue_depth) != 0) {
            close(lfd);
            pthread_mutex_destroy(&srv->queue_mu); pthread_cond_destroy(&srv->queue_cv);
            free(srv->queue); free(srv);
            snprintf(errbuf, ebsz, "wserver.serve: failed to bind port %d", port); return NULL;
        }
        int fl = fcntl(lfd, F_GETFL, 0); if (fl >= 0) fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(lfd, F_GETFD, 0); if (fd >= 0) fcntl(lfd, F_SETFD, fd | FD_CLOEXEC);
        srv->listen_fd     = lfd;
        srv->cur_pool_size = 0;
        return srv;
    }

    /* Choose initial pool size */
    int init_pool = auto_scale ? ws_min_threads : ws_thread_pool_size;
    if (init_pool < 1) init_pool = 1;

    /* EPOLL + fixed thread pool — handles thousands of concurrent connections
     * with a small set of threads, instead of spawning one OS thread per request.
     * LISTEN_BACKLOG_SIZE raises the kernel TCP listen queue from default (~128)
     * to queue_depth so high-burst connect storms don't get RST.
     * LISTENING_ADDRESS_REUSE enables SO_REUSEPORT for fast port reclaim. */
    unsigned int ws_dflags = MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG;
    if (srv->suspend_mode) ws_dflags |= MHD_ALLOW_SUSPEND_RESUME;
    srv->daemon = MHD_start_daemon(
        ws_dflags,
        (uint16_t)port, NULL, NULL,
        ws_mhd_handler, srv,
        MHD_OPTION_NOTIFY_COMPLETED, ws_request_completed, NULL,
                MHD_OPTION_NOTIFY_CONNECTION, ws_conn_notify, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
        MHD_OPTION_THREAD_POOL_SIZE,        (unsigned int)init_pool,
        MHD_OPTION_LISTEN_BACKLOG_SIZE,     (unsigned int)ws_queue_depth,
        MHD_OPTION_LISTENING_ADDRESS_REUSE, (unsigned int)1,
        MHD_OPTION_END);
    if (srv->daemon) fprintf(stderr, "[wserver] MHD started: thread_pool=%d, TCP_NODELAY=on (build OK)\n", (int)init_pool), fflush(stderr);

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

    /* Direct-socket mode: close the listener and free; no MHD/queue/monitor. */
    if (srv->direct) {
        if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
        pthread_mutex_destroy(&srv->queue_mu);
        pthread_cond_destroy(&srv->queue_cv);
        free(srv->queue);
        free(srv);
        return;
    }

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
        WsServer *srv = ws_start_server((int)port, auto_scale, 0, NULL, NULL, ebuf, sizeof(ebuf));
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

    /* ── wserver.serve_inline(port, "handler_fn") → int ──────────────
     * Like serve(), but the handler runs INLINE on the MHD epoll threads,
     * each on its own runtime clone — no worker pool, no queue, no condvar
     * handoff. The handler is a single-request function that replies once:
     *     fn handler(int req) nil { ... wserver.reply_json(req, ...) ... }
     * MHD thread-pool size (number of epoll threads) = [libs.wserver] workers. */
    if (!strcmp(fn_name, "serve_inline")) {
        NEED(2); REQ_INT(0, port); REQ_STR(1, hname);
        if (port < 1 || port > 65535) WS_ERR("port out of range [1..65535]");
        if (!rt) WS_ERR("serve_inline: no runtime context");

        Value fn_val; fn_val.type = VAL_NIL;
        if (rt->global_table) scope_table_get(rt->global_table, hname, &fn_val);
        if (fn_val.type != VAL_FUNC) {
            snprintf(errbuf, sizeof(errbuf), "wserver.serve_inline: handler function '%s' not found", hname);
            errstack_push(err, ERR_FLUXA, errbuf, "wserver", line);
            *had_error = 1; return wsrv_int(0);
        }

        char ebuf[256] = "";
        WsServer *srv = ws_start_server((int)port, 0, 1, rt, fn_val.as.func, ebuf, sizeof(ebuf));
        if (!srv) {
            errstack_push(err, ERR_FLUXA, ebuf[0] ? ebuf : "wserver.serve_inline: failed", "wserver", line);
            *had_error = 1; return wsrv_int(0);
        }
        pthread_mutex_lock(&ws_cfg_mu);
        int slot = ws_alloc_server(srv);
        pthread_mutex_unlock(&ws_cfg_mu);
        if (!slot) { ws_stop_server(srv); WS_ERR("server table full"); }
        return wsrv_int(slot);
    }

    /* ── wserver.accept(server, timeout_ms) → int ───────────────── */
    if (!strcmp(fn_name, "accept")) {
        NEED(2); REQ_INT(0, h); REQ_INT(1, timeout_ms);
        WsServer *srv = ws_get_server(h, err, had_error, line, fn_name);
        if (!srv) return wsrv_int(0);
        if (timeout_ms < 0) timeout_ms = 0;
        if (srv->direct) return wsrv_int(ws_direct_accept(srv, (int)timeout_ms));

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
