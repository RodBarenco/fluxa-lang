#ifndef FLUXA_STD_HTTP_H
#define FLUXA_STD_HTTP_H

/*
 * std.http — HTTP server + client for Fluxa-lang (mongoose backend)
 *
 * mongoose 7.x: single-file C library, embedded-friendly, works on
 * RP2040 (Wi-Fi) and ESP32. Client + server + WebSocket in one.
 *
 * API:
 *   Server:
 *     http.serve(port)                  → dyn server cursor
 *     http.serve_tls(port, cert, key)   → dyn server cursor (HTTPS)
 *     http.poll(server, timeout_ms)     → dyn request | nil
 *     http.req_method(req)             → str  ("GET", "POST", ...)
 *     http.req_path(req)               → str  ("/api/sensor")
 *     http.req_body(req)               → str
 *     http.req_header(req, name)       → str  ("" if missing)
 *     http.reply(req, status, body)    → nil
 *     http.reply_json(req, status, json) → nil
 *     http.stop(server)                → nil
 *
 *   Client (same as std.httpc but via mongoose):
 *     http.get(url)                    → dyn response
 *     http.post(url, body)             → dyn response
 *     http.post_json(url, json)        → dyn response
 *     http.status(resp)                → int
 *     http.body(resp)                  → str
 *     http.ok(resp)                    → bool
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mongoose.h"
#include "../../scope.h"
#include "../../err.h"

/* ── Server state ────────────────────────────────────────────────── */
typedef struct {
    struct mg_mgr  mgr;
    int            running;
    char           listen_url[128];
    /* Pending request — filled by callback, consumed by http.poll */
    struct mg_connection *req_conn;
    struct mg_http_message req_msg_copy;
    char           req_body[65536];
    char           req_uri[512];
    char           req_method[16];
    int            req_ready;
} HttpServer;

/* ── Response state (client) ─────────────────────────────────────── */
typedef struct {
    int  status;
    char body[65536];
    int  done;
    int  error;
    /* Request to send on MG_EV_CONNECT */
    char req_line[8192];
} HttpResp;

/* ── Server event callback ───────────────────────────────────────── */
static void http_server_cb(struct mg_connection *c, int ev,
                            void *ev_data) {
    HttpServer *srv = (HttpServer *)c->fn_data;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (srv->req_ready) return; /* drop if previous not consumed */
        /* Copy request data */
        int blen = (int)hm->body.len;
        if (blen >= (int)sizeof(srv->req_body)) blen = sizeof(srv->req_body)-1;
        memcpy(srv->req_body, hm->body.buf, (size_t)blen);
        srv->req_body[blen] = '\0';
        int ulen = (int)hm->uri.len;
        if (ulen >= (int)sizeof(srv->req_uri)) ulen = sizeof(srv->req_uri)-1;
        memcpy(srv->req_uri, hm->uri.buf, (size_t)ulen);
        srv->req_uri[ulen] = '\0';
        int mlen = (int)hm->method.len;
        if (mlen >= (int)sizeof(srv->req_method)) mlen = sizeof(srv->req_method)-1;
        memcpy(srv->req_method, hm->method.buf, (size_t)mlen);
        srv->req_method[mlen] = '\0';
        srv->req_conn  = c;
        srv->req_ready = 1;
    }
}

/* ── Client event callback ───────────────────────────────────────── */
static void http_client_cb(struct mg_connection *c, int ev,
                             void *ev_data) {
    HttpResp *resp = (HttpResp *)c->fn_data;
    if (ev == MG_EV_CONNECT) {
        /* Send request immediately after TCP connect */
        mg_printf(c, "%s", resp->req_line);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        resp->status = mg_http_status(hm);
        int blen = (int)hm->body.len;
        if (blen >= (int)sizeof(resp->body)) blen = sizeof(resp->body)-1;
        memcpy(resp->body, hm->body.buf, (size_t)blen);
        resp->body[blen] = '\0';
        resp->done = 1;
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
        if (!resp->done) { resp->error = 1; resp->done = 1; }
    }
    (void)ev_data;
}

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value httpd_nil(void)   { Value v; v.type=VAL_NIL;    return v; }
static inline Value httpd_int(long n) { Value v; v.type=VAL_INT;    v.as.integer=n; return v; }
static inline Value httpd_bool(int b) { Value v; v.type=VAL_BOOL;   v.as.boolean=b; return v; }
static inline Value httpd_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }

static inline Value httpd_wrap_ptr(void *ptr, int tag) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(2*sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=ptr;
    d->items[1].type=VAL_INT; d->items[1].as.integer=tag;
    d->count=2; d->cap=2;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}

#define HTTP_TAG_SERVER 1
#define HTTP_TAG_RESP   2
#define HTTP_TAG_REQ    3

static inline void *httpd_unwrap(const Value *v, int expected_tag,
                                 ErrStack *err, int *had_error,
                                 int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<2||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr||
        v->as.dyn->items[1].type!=VAL_INT||
        v->as.dyn->items[1].as.integer!=expected_tag) {
        snprintf(eb,sizeof(eb),"http.%s: invalid cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"http",line); *had_error=1; return NULL; }
    return v->as.dyn->items[0].as.ptr;
}

/* ── Client helper: do a request synchronously (max 10s) ─────────── */
static Value httpd_do_request(const char *method, const char *url,
                              const char *body, const char *content_type,
                              ErrStack *err, int *had_error, int line) {
    char errbuf[280];
    mg_log_set(MG_LL_NONE);
    HttpResp *resp = (HttpResp *)calloc(1, sizeof(HttpResp));
    struct mg_mgr mgr; mg_mgr_init(&mgr);  /* suppress mongoose debug output */

    char req_hdr[256] = "";
    if (content_type)
        snprintf(req_hdr, sizeof(req_hdr), "Content-Type: %s\r\n", content_type);

    /* mg_url_host returns mg_str — extract to C string first */
    struct mg_str _mg_host = mg_url_host(url);
    char host_buf[256];
    int _hl = (int)_mg_host.len;
    if (_hl >= (int)sizeof(host_buf)) _hl = (int)sizeof(host_buf)-1;
    memcpy(host_buf, _mg_host.buf, (size_t)_hl); host_buf[_hl] = '\0';
    const char *_uri = mg_url_uri(url);

    /* Build request line — sent in MG_EV_CONNECT callback via resp->req_line */
    if (body && body[0]) {
        snprintf(resp->req_line, sizeof(resp->req_line),
            "%s %s HTTP/1.0\r\nHost: %s\r\n%sContent-Length: %zu\r\n\r\n%s",
            method, _uri, host_buf, req_hdr, strlen(body), body);
    } else {
        snprintf(resp->req_line, sizeof(resp->req_line),
            "%s %s HTTP/1.0\r\nHost: %s\r\n\r\n",
            method, _uri, host_buf);
    }

    struct mg_connection *conn = mg_http_connect(
        &mgr, url, http_client_cb, resp);
    if (!conn) {
        mg_mgr_free(&mgr); free(resp);
        snprintf(errbuf,sizeof(errbuf),"http.%s: connect failed to %s",method,url);
        errstack_push(err,ERR_FLUXA,errbuf,"http",line); *had_error=1;
        return httpd_nil();
    }

    /* Send request after connect is established — poll until connected */
    /* Poll until response received (10s) — request sent in MG_EV_CONNECT */
    for (int i = 0; !resp->done && i < 1000; i++)
        mg_mgr_poll(&mgr, 10);

    mg_mgr_free(&mgr);

    if (!resp->done || resp->error) {
        free(resp);
        snprintf(errbuf,sizeof(errbuf),"http.%s: request failed or timed out",method);
        errstack_push(err,ERR_FLUXA,errbuf,"http",line); *had_error=1;
        return httpd_nil();
    }

    Value v = httpd_wrap_ptr(resp, HTTP_TAG_RESP);
    return v;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_http_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define HTTP_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"http.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"http",line); \
    *had_error=1; return httpd_nil(); } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"http.%s: expected %d arg(s)",fn_name,(n)); \
    errstack_push(err,ERR_FLUXA,errbuf,"http",line); \
    *had_error=1; return httpd_nil(); } } while(0)

#define GET_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) HTTP_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

#define GET_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) HTTP_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

    /* ── Server ─────────────────────────────────────────── */

    if (!strcmp(fn_name,"serve") || !strcmp(fn_name,"serve_tls")) {
        NEED(1); GET_INT(0,port);
        mg_log_set(MG_LL_NONE);
        HttpServer *srv = (HttpServer *)calloc(1, sizeof(HttpServer));
        mg_mgr_init(&srv->mgr);
        int use_tls = !strcmp(fn_name,"serve_tls");
        if (use_tls && argc >= 3) {
            GET_STR(1,cert); GET_STR(2,key);
            snprintf(srv->listen_url, sizeof(srv->listen_url),
                     "https://0.0.0.0:%ld?ssl_cert=%s&ssl_key=%s",
                     port, cert, key);
        } else {
            snprintf(srv->listen_url, sizeof(srv->listen_url),
                     "http://0.0.0.0:%ld", port);
        }
        struct mg_connection *lc = mg_http_listen(
            &srv->mgr, srv->listen_url, http_server_cb, srv);
        if (!lc) {
            mg_mgr_free(&srv->mgr); free(srv);
            HTTP_ERR("serve: failed to bind port");
        }
        srv->running = 1;
        return httpd_wrap_ptr(srv, HTTP_TAG_SERVER);
    }

    if (!strcmp(fn_name,"poll")) {
        NEED(2);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_SERVER, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        GET_INT(1,timeout_ms);
        srv->req_ready = 0;
        /* Poll until request or timeout */
        long waited = 0;
        while (!srv->req_ready && waited < timeout_ms) {
            mg_mgr_poll(&srv->mgr, 10);
            waited += 10;
        }
        if (!srv->req_ready) return httpd_nil();
        /* Return a req cursor — pointer to server (conn embedded) */
        return httpd_wrap_ptr(srv, HTTP_TAG_REQ);
    }

    if (!strcmp(fn_name,"req_method")) {
        NEED(1);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        return httpd_str(srv->req_method);
    }

    if (!strcmp(fn_name,"req_path")) {
        NEED(1);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        return httpd_str(srv->req_uri);
    }

    if (!strcmp(fn_name,"req_body")) {
        NEED(1);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        return httpd_str(srv->req_body);
    }

    if (!strcmp(fn_name,"req_header")) {
        NEED(2);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        GET_STR(1, hname);
        /* We don't have full hm available — return empty for now */
        (void)hname;
        return httpd_str("");
    }

    if (!strcmp(fn_name,"reply")) {
        NEED(3);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv || !srv->req_conn) return httpd_nil();
        GET_INT(1, status);
        GET_STR(2, body);
        mg_http_reply(srv->req_conn, (int)status, "", "%s", body);
        /* Flush: poll until the reply is sent (max 500ms) */
        for (int _fi = 0; _fi < 50; _fi++) mg_mgr_poll(&srv->mgr, 10);
        srv->req_conn  = NULL;
        srv->req_ready = 0;
        return httpd_nil();
    }

    if (!strcmp(fn_name,"reply_json")) {
        NEED(3);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_REQ, err, had_error, line, fn_name);
        if (!srv || !srv->req_conn) return httpd_nil();
        GET_INT(1, status);
        GET_STR(2, json);
        mg_http_reply(srv->req_conn, (int)status,
                      "Content-Type: application/json\r\n", "%s", json);
        for (int _fi = 0; _fi < 50; _fi++) mg_mgr_poll(&srv->mgr, 10);
        srv->req_conn  = NULL;
        srv->req_ready = 0;
        return httpd_nil();
    }

    if (!strcmp(fn_name,"stop")) {
        NEED(1);
        HttpServer *srv = (HttpServer *)httpd_unwrap(
            &args[0], HTTP_TAG_SERVER, err, had_error, line, fn_name);
        if (!srv) return httpd_nil();
        mg_mgr_free(&srv->mgr);
        free(srv);
        if (args[0].type==VAL_DYN && args[0].as.dyn)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return httpd_nil();
    }

    /* ── Client ─────────────────────────────────────────── */

    if (!strcmp(fn_name,"get")) {
        NEED(1); GET_STR(0,url);
        return httpd_do_request("GET", url, NULL, NULL, err, had_error, line);
    }

    if (!strcmp(fn_name,"post")) {
        NEED(2); GET_STR(0,url); GET_STR(1,body);
        return httpd_do_request("POST", url, body,
            "application/x-www-form-urlencoded", err, had_error, line);
    }

    if (!strcmp(fn_name,"post_json")) {
        NEED(2); GET_STR(0,url); GET_STR(1,json);
        return httpd_do_request("POST", url, json,
            "application/json", err, had_error, line);
    }

    if (!strcmp(fn_name,"put")) {
        NEED(2); GET_STR(0,url); GET_STR(1,body);
        return httpd_do_request("PUT", url, body,
            "application/x-www-form-urlencoded", err, had_error, line);
    }

    if (!strcmp(fn_name,"delete")) {
        NEED(1); GET_STR(0,url);
        return httpd_do_request("DELETE", url, NULL, NULL, err, had_error, line);
    }

    if (!strcmp(fn_name,"status")) {
        NEED(1);
        HttpResp *resp = (HttpResp *)httpd_unwrap(
            &args[0], HTTP_TAG_RESP, err, had_error, line, fn_name);
        if (!resp) return httpd_nil();
        return httpd_int(resp->status);
    }

    if (!strcmp(fn_name,"body")) {
        NEED(1);
        HttpResp *resp = (HttpResp *)httpd_unwrap(
            &args[0], HTTP_TAG_RESP, err, had_error, line, fn_name);
        if (!resp) return httpd_nil();
        return httpd_str(resp->body);
    }

    if (!strcmp(fn_name,"ok")) {
        NEED(1);
        HttpResp *resp = (HttpResp *)httpd_unwrap(
            &args[0], HTTP_TAG_RESP, err, had_error, line, fn_name);
        if (!resp) return httpd_nil();
        return httpd_bool(resp->status >= 200 && resp->status < 300);
    }

    if (!strcmp(fn_name,"version")) {
        return httpd_str("mongoose/" MG_VERSION);
    }

#undef HTTP_ERR
#undef NEED
#undef GET_STR
#undef GET_INT

    snprintf(errbuf,sizeof(errbuf),"http.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"http",line);
    *had_error=1; return httpd_nil();
}

FLUXA_LIB_EXPORT(
    name      = "http",
    toml_key  = "std.http",
    owner     = "http",
    call      = fluxa_std_http_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_HTTP_H */
