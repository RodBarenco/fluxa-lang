#ifndef FLUXA_STD_MCP_H
#define FLUXA_STD_MCP_H

/*
 * std.mcp — Fluxa as MCP server (Model Context Protocol)
 *
 * Exposes the Fluxa runtime as an MCP server over HTTP (mongoose backend).
 * AI agents (Claude, GPT, Gemini, local llama.cpp) can discover and call
 * Fluxa tools via the standard MCP protocol (JSON-RPC 2.0 over HTTP).
 *
 * MCP tools exposed:
 *   fluxa/observe   → read current value of a prst variable
 *   fluxa/set       → mutate a prst variable at next safe point
 *   fluxa/apply     → hot reload: swap script preserving prst state
 *   fluxa/handover  → atomic handover (5-step protocol)
 *   fluxa/status    → cycle count, prst count, errors, mode
 *   fluxa/logs      → last N error entries
 *
 * API:
 *   mcp.serve(port)           → dyn server cursor
 *   mcp.poll(server, ms)      → nil (process one request cycle)
 *   mcp.stop(server)          → nil
 *   mcp.version()             → str
 *
 * Internally: mcp.serve() starts an HTTP server on the given port.
 * All POST /mcp requests are dispatched as JSON-RPC 2.0.
 * The server connects to the running Fluxa IPC socket to execute tools.
 *
 * Depends on std.http (mongoose).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "mongoose.h"
#include "../../scope.h"
#include "../../err.h"
#include "../../fluxa_ipc.h"

/* ── JSON-RPC 2.0 helpers ────────────────────────────────────────── */
static void mcp_json_str(const char *s, char *out, int outsz) {
    int i=0, j=0;
    out[j++]='"';
    while(s[i]&&j<outsz-4) {
        if(s[i]=='"'||s[i]=='\\') out[j++]='\\';
        if(s[i]=='\n') { out[j++]='\\'; out[j++]='n'; i++; continue; }
        out[j++]=s[i++];
    }
    out[j++]='"'; out[j]='\0';
}

/* Extract a JSON string field (very simple, no full parser) */
static int mcp_json_get(const char *json, const char *key, char *val, int vsz) {
    char needle[64]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle); if(!p) return 0;
    p+=strlen(needle);
    while(*p==' '||*p==':') p++;
    if(*p=='"') {
        p++;
        int i=0;
        while(*p&&*p!='"'&&i<vsz-1) val[i++]=*p++;
        val[i]='\0'; return 1;
    }
    /* Also handle number/bool */
    int i=0;
    while(*p&&*p!=','&&*p!='}'&&*p!='\n'&&i<vsz-1) val[i++]=*p++;
    while(i>0&&(val[i-1]==' '||val[i-1]=='\r')) i--;
    val[i]='\0'; return i>0;
}

/* ── MCP server state ────────────────────────────────────────────── */
typedef struct {
    struct mg_mgr mgr;
    int           running;
    int           port;
} McpServer;

/* ── IPC call helper — sends one request to local runtime ─────────── */
static int mcp_ipc_call(IpcRequest *req, IpcResponse *resp) {
    /* Find the socket — try common PIDs via /tmp/fluxa-*.sock */
    char sock_path[108];  /* sun_path limit on Linux */
    /* Try to find via /proc */
    FILE *fp = popen("ls /tmp/fluxa-*.sock 2>/dev/null | head -1", "r");
    if (!fp) return -1;
    char line[128] = "";
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        /* snprintf: explicit truncate to sun_path limit, always NUL-terminates */
        snprintf(sock_path, sizeof(sock_path), "%s", line);
    }
    pclose(fp);
    if (!sock_path[0]) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa; memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }
    if (send(fd, req, sizeof(*req), 0) != sizeof(*req)) { close(fd); return -1; }
    if (recv(fd, resp, sizeof(*resp), MSG_WAITALL) != sizeof(*resp)) {
        close(fd); return -1;
    }
    close(fd);
    return 0;
}

/* ── MCP dispatch: handle one JSON-RPC 2.0 call ────────────────────── */
static void mcp_dispatch(struct mg_connection *c, struct mg_http_message *hm) {
    char body_buf[4096];
    int blen = (int)hm->body.len;
    if (blen >= (int)sizeof(body_buf)) blen = sizeof(body_buf)-1;
    memcpy(body_buf, hm->body.buf, (size_t)blen);
    body_buf[blen] = '\0';

    char method[64]="", params_name[128]="", params_val[256]="", id_str[32]="1";
    mcp_json_get(body_buf, "method", method, sizeof(method));
    mcp_json_get(body_buf, "id", id_str, sizeof(id_str));

    /* Extract params.name and params.value */
    const char *params_p = strstr(body_buf, "\"params\"");
    if (params_p) {
        mcp_json_get(params_p, "name",  params_name, sizeof(params_name));
        mcp_json_get(params_p, "value", params_val,  sizeof(params_val));
    }

    char result_json[1024] = "{\"result\":\"ok\"}";
    int  rpc_err = 0;
    char err_msg[256] = "";

    IpcRequest  req;
    IpcResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    /* ── MCP tools list (initialize) ── */
    if (!strcmp(method, "initialize") || !strcmp(method, "tools/list")) {
        snprintf(result_json, sizeof(result_json),
            "{\"tools\":["
            "{\"name\":\"fluxa/observe\",\"description\":\"Read a prst variable\","
             "\"inputSchema\":{\"type\":\"object\",\"properties\":"
             "{\"name\":{\"type\":\"string\"}}}},"
            "{\"name\":\"fluxa/set\",\"description\":\"Mutate a prst variable\","
             "\"inputSchema\":{\"type\":\"object\",\"properties\":"
             "{\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}}}},"
            "{\"name\":\"fluxa/status\",\"description\":\"Runtime status\","
             "\"inputSchema\":{\"type\":\"object\"}},"
            "{\"name\":\"fluxa/logs\",\"description\":\"Last error log entries\","
             "\"inputSchema\":{\"type\":\"object\"}}"
            "]}");
    }
    /* ── fluxa/observe ── */
    else if (!strcmp(method, "tools/call") &&
             strstr(body_buf, "fluxa/observe")) {
        if (!params_name[0]) { rpc_err=1; strcpy(err_msg,"params.name required"); }
        else {
            ipc_req_observe(&req, 1, params_name);
            if (mcp_ipc_call(&req, &resp) < 0) {
                rpc_err=1; strcpy(err_msg,"IPC unavailable — is fluxa -prod running?");
            } else if (resp.status == IPC_STATUS_OK) {
                char val_j[256];
                mcp_json_str(resp.message, val_j, sizeof(val_j));
                snprintf(result_json, sizeof(result_json),
                    "{\"content\":[{\"type\":\"text\",\"text\":%s}]}", val_j);
            } else {
                rpc_err=1;
                snprintf(err_msg, sizeof(err_msg), "observe failed: %s", resp.message);
            }
        }
    }
    /* ── fluxa/set ── */
    else if (!strcmp(method, "tools/call") &&
             strstr(body_buf, "fluxa/set")) {
        if (!params_name[0]) { rpc_err=1; strcpy(err_msg,"params.name required"); }
        else {
            ipc_req_set_str(&req, 1, params_name, params_val);
            if (mcp_ipc_call(&req, &resp) < 0) {
                rpc_err=1; strcpy(err_msg,"IPC unavailable");
            } else if (resp.status == IPC_STATUS_OK) {
                snprintf(result_json, sizeof(result_json),
                    "{\"content\":[{\"type\":\"text\",\"text\":\"set ok\"}]}");
            } else {
                rpc_err=1;
                snprintf(err_msg, sizeof(err_msg), "set failed: %s", resp.message);
            }
        }
    }
    /* ── fluxa/status ── */
    else if (!strcmp(method, "tools/call") &&
             strstr(body_buf, "fluxa/status")) {
        ipc_req_status(&req, 1);
        if (mcp_ipc_call(&req, &resp) < 0) {
            rpc_err=1; strcpy(err_msg,"IPC unavailable");
        } else {
            snprintf(result_json, sizeof(result_json),
                "{\"content\":[{\"type\":\"text\",\"text\":"
                "\"cycle=%d prst=%d errors=%d mode=%d dry_run=%d\"}]}",
                resp.cycle_count, resp.prst_count, resp.err_count,
                resp.mode, resp.dry_run);
        }
    }
    /* ── fluxa/logs ── */
    else if (!strcmp(method, "tools/call") &&
             strstr(body_buf, "fluxa/logs")) {
        ipc_req_logs(&req, 1);
        if (mcp_ipc_call(&req, &resp) < 0) {
            rpc_err=1; strcpy(err_msg,"IPC unavailable");
        } else {
            char msg_j[512];
            mcp_json_str(resp.message, msg_j, sizeof(msg_j));
            snprintf(result_json, sizeof(result_json),
                "{\"content\":[{\"type\":\"text\",\"text\":%s}]}", msg_j);
        }
    }

    /* ── Build JSON-RPC 2.0 response ── */
    char full_resp[2048];
    if (rpc_err) {
        char err_j[512]; mcp_json_str(err_msg, err_j, sizeof(err_j));
        snprintf(full_resp, sizeof(full_resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":"
            "{\"code\":-32600,\"message\":%s}}",
            id_str, err_j);
    } else {
        snprintf(full_resp, sizeof(full_resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
            id_str, result_json);
    }

    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "%s", full_resp);
}

/* ── Event callback ──────────────────────────────────────────────── */
static void mcp_server_cb(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 200,
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n", "");
            return;
        }
        mcp_dispatch(c, hm);
    }
}

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value mcp_nil(void) { Value v; v.type=VAL_NIL; return v; }
static inline Value mcp_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }

static inline Value mcp_wrap(McpServer *srv) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=srv;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline McpServer *mcp_unwrap(const Value *v, ErrStack *err,
                                     int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"mcp.%s: invalid server cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"mcp",line); *had_error=1; return NULL; }
    return (McpServer *)v->as.dyn->items[0].as.ptr;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_mcp_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[280];

#define MCP_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"mcp.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"mcp",line); \
    *had_error=1; return mcp_nil(); } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"mcp.%s: expected %d arg(s)",fn_name,(n)); \
    errstack_push(err,ERR_FLUXA,errbuf,"mcp",line); \
    *had_error=1; return mcp_nil(); } } while(0)

#define GET_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) MCP_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

    if (!strcmp(fn_name,"version")) {
        return mcp_str("fluxa-mcp/1.0 MCP-2024-11-05 mongoose/" MG_VERSION);
    }

    if (!strcmp(fn_name,"serve")) {
        NEED(1); GET_INT(0,port);
        McpServer *srv = (McpServer *)calloc(1, sizeof(McpServer));
        srv->port = (int)port;
        mg_mgr_init(&srv->mgr);
        mg_log_set(MG_LL_NONE);
        char url[64]; snprintf(url, sizeof(url), "http://0.0.0.0:%ld", port);
        struct mg_connection *lc = mg_http_listen(
            &srv->mgr, url, mcp_server_cb, srv);
        if (!lc) {
            mg_mgr_free(&srv->mgr); free(srv);
            MCP_ERR("serve: failed to bind port");
        }
        srv->running = 1;
        fprintf(stderr, "[fluxa] mcp: listening on http://0.0.0.0:%ld\n", port);
        return mcp_wrap(srv);
    }

    if (!strcmp(fn_name,"poll")) {
        NEED(2);
        McpServer *srv = mcp_unwrap(&args[0], err, had_error, line, fn_name);
        if (!srv) return mcp_nil();
        if(args[1].type!=VAL_INT) MCP_ERR("expected int timeout");
        long ms = args[1].as.integer;
        mg_mgr_poll(&srv->mgr, (int)ms);
        return mcp_nil();
    }

    if (!strcmp(fn_name,"stop")) {
        NEED(1);
        McpServer *srv = mcp_unwrap(&args[0], err, had_error, line, fn_name);
        if (!srv) return mcp_nil();
        mg_mgr_free(&srv->mgr);
        free(srv);
        if (args[0].type==VAL_DYN && args[0].as.dyn)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return mcp_nil();
    }

#undef MCP_ERR
#undef NEED
#undef GET_INT

    snprintf(errbuf,sizeof(errbuf),"mcp.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"mcp",line);
    *had_error=1; return mcp_nil();
}

FLUXA_LIB_EXPORT(
    name      = "mcp",
    toml_key  = "std.mcp",
    owner     = "mcp",
    call      = fluxa_std_mcp_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_MCP_H */
