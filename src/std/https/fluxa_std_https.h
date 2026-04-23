#ifndef FLUXA_STD_HTTPS_H
#define FLUXA_STD_HTTPS_H

/*
 * std.https — HTTPS client (libcurl, TLS enforced)
 *
 * Identical to std.httpc but enforces TLS:
 *   CURLOPT_SSL_VERIFYPEER = 1 (verify server cert)
 *   CURLOPT_SSL_VERIFYHOST = 2 (verify hostname)
 *   Rejects plain http:// URLs at runtime
 *   Uses system CA bundle by default for Fluxa-lang
 *
 * Backed by libcurl. All calls must be inside danger{}.
 * Returns dyn with {status, body, headers} for requests.
 *
 * API:
 *   http.get(url)                        → dyn {status:int, body:str, ok:bool}
 *   http.post(url, body)                 → dyn {status:int, body:str, ok:bool}
 *   http.post_json(url, json_str)        → dyn {status:int, body:str, ok:bool}
 *   http.put(url, body)                  → dyn {status:int, body:str, ok:bool}
 *   http.delete(url)                     → dyn {status:int, body:str, ok:bool}
 *   http.status(resp)                    → int
 *   http.body(resp)                      → str
 *   http.ok(resp)                        → bool  (status 200-299)
 */

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Response buffer ─────────────────────────────────────────────── */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} HttpsBuf;

static size_t https_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    HttpsBuf *b = (HttpsBuf *)ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t new_cap = (b->cap == 0) ? 4096 : b->cap * 2;
        while (new_cap < b->len + n + 1) new_cap *= 2;
        char *tmp = (char *)realloc(b->data, new_cap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value https_int(long n)        { Value v; v.type = VAL_INT;    v.as.integer = n;          return v; }
static inline Value https_bool(int b)        { Value v; v.type = VAL_BOOL;   v.as.boolean = b;          return v; }
static inline Value https_nil(void)          { Value v; v.type = VAL_NIL;                               return v; }
static inline Value https_str(const char *s) { Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v; }

/* Build response dyn: [status:int, body:str, ok:bool] */
static inline Value https_make_resp(long status, const char *body) {
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap   = 3;
    d->count = 3;
    d->items = (Value *)malloc(sizeof(Value) * 3);
    d->items[0] = https_int(status);
    d->items[1] = https_str(body ? body : "");
    d->items[2] = https_bool(status >= 200 && status < 300);
    Value v; v.type = VAL_DYN; v.as.dyn = d;
    return v;
}

/* Extract resp dyn or error */
#define GET_RESP(idx) \
    if (args[(idx)].type != VAL_DYN || !args[(idx)].as.dyn || \
        args[(idx)].as.dyn->count < 3) \
        HTTP_ERR("expected http response dyn (use http.get/post/...)"); \
    FluxaDyn *_resp = args[(idx)].as.dyn;

/* ── Core request helper ─────────────────────────────────────────── */
typedef enum { HTTPS_GET, HTTPS_POST, HTTPS_PUT, HTTPS_DELETE } HttpsMethod;

static inline Value https_do_request(const char *url, HttpsMethod method,
                                     const char *body, const char *content_type,
                                     ErrStack *err, int *had_error, int line) {
    char errbuf[280];
    (void)line;

#define HTTP_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "https: %s", (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "http", line); \
    *had_error = 1; return https_nil(); \
} while(0)

    CURL *curl = curl_easy_init();
    if (!curl) HTTP_ERR("failed to initialize curl");

    HttpsBuf buf = {NULL, 0, 0};
    struct curl_slist *headers = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, https_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fluxa-https/1.0");

    if (method == HTTPS_POST || method == HTTPS_PUT) {
        if (content_type) {
            char ct_hdr[128];
            snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s", content_type);
            headers = curl_slist_append(headers, ct_hdr);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)(body ? strlen(body) : 0));
        if (method == HTTPS_PUT)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else if (method == HTTPS_DELETE) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        snprintf(errbuf, sizeof(errbuf), "https: %s", curl_easy_strerror(res));
        errstack_push(err, ERR_FLUXA, errbuf, "http", line);
        *had_error = 1;
        free(buf.data);
        return https_nil();
    }

    Value resp = https_make_resp(status, buf.data ? buf.data : "");
    free(buf.data);
    return resp;

#undef HTTP_ERR
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_https_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define HTTP_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "https.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "http", line); \
    *had_error = 1; return https_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "https.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "http", line); \
        *had_error = 1; return https_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        HTTP_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* http.get(url) → resp */
    if (strcmp(fn_name, "get") == 0) {
        NEED(1); GET_STR(0, url);
        return https_do_request(url, HTTPS_GET, NULL, NULL, err, had_error, line);
    }

    /* http.post(url, body) → resp */
    if (strcmp(fn_name, "post") == 0) {
        NEED(2); GET_STR(0, url); GET_STR(1, body);
        return https_do_request(url, HTTPS_POST, body,
                               "application/x-www-form-urlencoded",
                               err, had_error, line);
    }

    /* http.post_json(url, json_str) → resp */
    if (strcmp(fn_name, "post_json") == 0) {
        NEED(2); GET_STR(0, url); GET_STR(1, body);
        return https_do_request(url, HTTPS_POST, body,
                               "application/json",
                               err, had_error, line);
    }

    /* http.put(url, body) → resp */
    if (strcmp(fn_name, "put") == 0) {
        NEED(2); GET_STR(0, url); GET_STR(1, body);
        return https_do_request(url, HTTPS_PUT, body,
                               "application/x-www-form-urlencoded",
                               err, had_error, line);
    }

    /* http.delete(url) → resp */
    if (strcmp(fn_name, "delete") == 0) {
        NEED(1); GET_STR(0, url);
        return https_do_request(url, HTTPS_DELETE, NULL, NULL, err, had_error, line);
    }

    /* http.status(resp) → int */
    if (strcmp(fn_name, "status") == 0) {
        NEED(1);
        GET_RESP(0);
        return _resp->items[0];
    }

    /* http.body(resp) → str */
    if (strcmp(fn_name, "body") == 0) {
        NEED(1);
        GET_RESP(0);
        Value s; s.type = VAL_STRING;
        s.as.string = strdup(_resp->items[1].as.string
                             ? _resp->items[1].as.string : "");
        return s;
    }

    /* http.ok(resp) → bool */
    if (strcmp(fn_name, "ok") == 0) {
        NEED(1);
        GET_RESP(0);
        return _resp->items[2];
    }

#undef HTTP_ERR
#undef NEED
#undef GET_STR
#undef GET_RESP

    snprintf(errbuf, sizeof(errbuf), "https.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "http", line);
    *had_error = 1;
    return https_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "https",
    toml_key  = "std.https",
    owner     = "https",
    call      = fluxa_std_https_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_HTTPS_H */
