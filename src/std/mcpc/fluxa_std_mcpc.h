#ifndef FLUXA_STD_MCPC_H
#define FLUXA_STD_MCPC_H

/*
 * std.mcpc — MCP client (calls external MCP servers) for Fluxa-lang
 *
 * MCP (https://modelcontextprotocol.io) enables Fluxa programs to call
 * tools exposed by MCP servers (Claude, filesystem, databases, etc).
 *
 * Transport: HTTP POST (JSON-RPC 2.0). Requires std.http (libcurl).
 *
 * API:
 *   mcp.connect(url)                     → dyn cursor
 *   mcp.connect_auth(url, token)         → dyn cursor
 *   mcp.list_tools(cursor)               → dyn of str (tool names)
 *   mcp.call(cursor, tool, args_json)    → str (result JSON)
 *   mcp.call_text(cursor, tool, args_json) → str (text content only)
 *   mcp.disconnect(cursor)               → nil
 *
 * Example:
 *   prst dyn claude = mcp.connect("http://localhost:3000")
 *   dyn tools = mcp.list_tools(claude)
 *   str result = mcp.call_text(claude, "read_file", "{\"path\":\"/etc/hostname\"}")
 */

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Cursor ──────────────────────────────────────────────────────── */
typedef struct {
    char *url;    /* MCP server base URL */
    char *token;  /* Bearer token (optional, NULL if none) */
    int   req_id; /* JSON-RPC request ID counter */
} McpcClient;

/* ── Response buffer (shared with http pattern) ──────────────────── */
typedef struct { char *data; size_t len; size_t cap; } McpcBuf;

static size_t mcpc_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    McpcBuf *b = (McpcBuf *)ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *t = (char *)realloc(b->data, nc);
        if (!t) return 0;
        b->data = t; b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value mcpc_nil(void)          { Value v; v.type = VAL_NIL;  return v; }
static inline Value mcpc_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : ""); return v;
}

static inline Value mcpc_wrap(McpcClient *c) {
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap = 1; d->count = 1;
    d->items = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = c;
    Value v; v.type = VAL_DYN; v.as.dyn = d;
    return v;
}

static inline McpcClient *mcpc_unwrap(const Value *v, ErrStack *err,
                                     int *had_error, int line,
                                     const char *fn) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn || v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR || !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "mcpc.%s: invalid cursor — use mcp.connect() first", fn);
        errstack_push(err, ERR_FLUXA, errbuf, "mcp", line);
        *had_error = 1; return NULL;
    }
    return (McpcClient *)v->as.dyn->items[0].as.ptr;
}

/* ── JSON-RPC POST helper ────────────────────────────────────────── */
static char *mcpc_post(McpcClient *c, const char *body,
                       ErrStack *err, int *had_error, int line) {
    char errbuf[280];
    CURL *curl = curl_easy_init();
    if (!curl) {
        errstack_push(err, ERR_FLUXA, "mcpc: curl init failed", "mcp", line);
        *had_error = 1; return NULL;
    }

    McpcBuf buf = {NULL, 0, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    if (c->token) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->token);
        headers = curl_slist_append(headers, auth);
    }

    /* MCP endpoint: POST to base URL */
    char endpoint[1024];
    snprintf(endpoint, sizeof(endpoint), "%s", c->url);

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mcpc_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fluxa-mcpc/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        snprintf(errbuf, sizeof(errbuf), "mcpc: %s", curl_easy_strerror(res));
        errstack_push(err, ERR_FLUXA, errbuf, "mcp", line);
        *had_error = 1;
        free(buf.data);
        return NULL;
    }
    return buf.data; /* caller frees */
}

/* ── Minimal JSON helpers (no external dep) ──────────────────────── */

/* Extract string value of a top-level JSON key (shallow, no nesting) */
static char *mcpc_json_str(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return NULL;
        size_t len = (size_t)(end - p);
        char *result = (char *)malloc(len + 1);
        memcpy(result, p, len);
        result[len] = '\0';
        return result;
    }
    return NULL;
}

/* Check if JSON response has an "error" key */
static int mcpc_json_has_error(const char *json) {
    return json && strstr(json, "\"error\"") != NULL &&
           strstr(json, "\"result\"") == NULL;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_mcpc_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[280];

#define MCP_ERR(msg) do { \
    char _eb[512]; \
    snprintf(_eb, sizeof(_eb), "mcpc.%s (line %d): %.400s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, _eb, "mcp", line); \
    *had_error = 1; return mcpc_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "mcpc.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "mcp", line); \
        *had_error = 1; return mcpc_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        MCP_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_CLIENT(idx) \
    McpcClient *client = mcpc_unwrap(&args[(idx)], err, had_error, line, fn_name); \
    if (!client) return mcpc_nil();

    /* mcp.connect(url) → dyn */
    if (strcmp(fn_name, "connect") == 0) {
        NEED(1); GET_STR(0, url);
        McpcClient *c = (McpcClient *)malloc(sizeof(McpcClient));
        c->url    = strdup(url);
        c->token  = NULL;
        c->req_id = 1;
        return mcpc_wrap(c);
    }

    /* mcp.connect_auth(url, token) → dyn */
    if (strcmp(fn_name, "connect_auth") == 0) {
        NEED(2); GET_STR(0, url); GET_STR(1, token);
        McpcClient *c = (McpcClient *)malloc(sizeof(McpcClient));
        c->url    = strdup(url);
        c->token  = strdup(token);
        c->req_id = 1;
        return mcpc_wrap(c);
    }

    /* mcp.list_tools(cursor) → dyn of str */
    if (strcmp(fn_name, "list_tools") == 0) {
        NEED(1); GET_CLIENT(0);

        char body[256];
        snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"method\":\"tools/list\",\"params\":{}}", client->req_id++);

        char *resp = mcpc_post(client, body, err, had_error, line);
        if (!resp) return mcpc_nil();

        /* Parse tools array — extract "name" fields */
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        d->cap = 8; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);

        if (mcpc_json_has_error(resp)) {
            char *emsg = mcpc_json_str(resp, "message");
            snprintf(errbuf, sizeof(errbuf), "mcp.list_tools: %s",
                     emsg ? emsg : "server error");
            free(emsg); free(resp); free(d->items); free(d);
            MCP_ERR(errbuf);
        }

        /* Scan for "name":"toolname" patterns in the tools array */
        const char *p = resp;
        while ((p = strstr(p, "\"name\"")) != NULL) {
            p += 6;
            while (*p == ' ' || *p == ':') p++;
            if (*p != '"') { p++; continue; }
            p++;
            const char *end = strchr(p, '"');
            if (!end) break;
            size_t len = (size_t)(end - p);
            char *name = (char *)malloc(len + 1);
            memcpy(name, p, len); name[len] = '\0';

            if (d->count >= d->cap) {
                d->cap *= 2;
                d->items = (Value *)realloc(d->items,
                    sizeof(Value) * (size_t)d->cap);
            }
            d->items[d->count].type      = VAL_STRING;
            d->items[d->count].as.string = name;
            d->count++;
            p = end + 1;
        }

        free(resp);
        Value v; v.type = VAL_DYN; v.as.dyn = d;
        return v;
    }

    /* mcp.call(cursor, tool, args_json) → str (full JSON result) */
    if (strcmp(fn_name, "call") == 0) {
        NEED(3); GET_CLIENT(0); GET_STR(1, tool); GET_STR(2, args_json);

        /* Build JSON-RPC request */
        size_t blen = strlen(tool) + strlen(args_json) + 128;
        char *body = (char *)malloc(blen);
        snprintf(body, blen,
            "{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"method\":\"tools/call\","
            "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
            client->req_id++, tool, args_json);

        char *resp = mcpc_post(client, body, err, had_error, line);
        free(body);
        if (!resp) return mcpc_nil();

        if (mcpc_json_has_error(resp)) {
            char *emsg = mcpc_json_str(resp, "message");
            snprintf(errbuf, sizeof(errbuf), "mcp.call '%s': %s",
                     tool, emsg ? emsg : "server error");
            free(emsg); free(resp);
            MCP_ERR(errbuf);
        }

        Value result = mcpc_str(resp);
        free(resp);
        return result;
    }

    /* mcp.call_text(cursor, tool, args_json) → str (text content only) */
    if (strcmp(fn_name, "call_text") == 0) {
        NEED(3); GET_CLIENT(0); GET_STR(1, tool); GET_STR(2, args_json);

        size_t blen = strlen(tool) + strlen(args_json) + 128;
        char *body = (char *)malloc(blen);
        snprintf(body, blen,
            "{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"method\":\"tools/call\","
            "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
            client->req_id++, tool, args_json);

        char *resp = mcpc_post(client, body, err, had_error, line);
        free(body);
        if (!resp) return mcpc_nil();

        if (mcpc_json_has_error(resp)) {
            char *emsg = mcpc_json_str(resp, "message");
            snprintf(errbuf, sizeof(errbuf), "mcp.call_text '%s': %s",
                     tool, emsg ? emsg : "server error");
            free(emsg); free(resp);
            MCP_ERR(errbuf);
        }

        /* Extract "text" field from content array */
        char *text = mcpc_json_str(resp, "text");
        Value result = mcpc_str(text ? text : resp);
        free(text); free(resp);
        return result;
    }

    /* mcp.disconnect(cursor) → nil */
    if (strcmp(fn_name, "disconnect") == 0) {
        NEED(1); GET_CLIENT(0);
        free(client->url);
        free(client->token);
        free(client);
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return mcpc_nil();
    }

#undef MCP_ERR
#undef NEED
#undef GET_STR
#undef GET_CLIENT

    snprintf(errbuf, sizeof(errbuf), "mcpc.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "mcp", line);
    *had_error = 1;
    return mcpc_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "mcpc",
    toml_key  = "std.mcpc",
    owner     = "mcpc",
    call      = fluxa_std_mcpc_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_MCPC_H */
