#ifndef FLUXA_STD_WEBSOCKET_H
#define FLUXA_STD_WEBSOCKET_H

/*
 * std.websocket — WebSocket client for Fluxa-lang
 *
 * Two backends — selected at compile time:
 *
 *   FLUXA_WS_LIBWEBSOCKETS=1  libwebsockets backend (requires libssl-dev)
 *     Full RFC 6455 client + wss:// TLS + production-grade framing.
 *     Battle-tested in production; handles edge cases, ping/pong,
 *     fragmentation, per-message deflate, etc.
 *
 *   (default) pure C99 native backend
 *     POSIX sockets + manual RFC 6455 framing. Zero external deps.
 *     Works on Linux, macOS, RP2040 (lwIP), ESP32.
 *     ws:// only (no TLS). Sufficient for LAN dashboards/IoT.
 *
 * API (identical regardless of backend):
 *   ws.connect(url)           → dyn cursor  ("ws://host:port/path")
 *   ws.send(conn, msg)        → nil  (text frame)
 *   ws.send_bin(conn, data)   → nil  (binary frame)
 *   ws.recv(conn, timeout_ms) → str  (next message, "" on timeout)
 *   ws.poll(conn)             → bool (message available without blocking)
 *   ws.close(conn)            → nil
 *   ws.connected(conn)        → bool
 *   ws.url(conn)              → str
 *   ws.version()              → str  ("libwebsockets/x.y.z" or "fluxa-ws/1.0 RFC6455")
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: libwebsockets (when FLUXA_WS_LIBWEBSOCKETS=1)
 * ════════════════════════════════════════════════════════════════════ */
#ifdef FLUXA_WS_LIBWEBSOCKETS

#include <libwebsockets.h>

#define WS_RECV_BUF  65536
#define WS_MAX_CONNS 32

typedef struct {
    struct lws_context *ctx;
    struct lws         *wsi;
    char                url[512];
    int                 connected;
    int                 closed;
    char                recv_buf[WS_RECV_BUF];
    int                 recv_len;
    int                 recv_ready;
    unsigned char       send_buf[LWS_PRE + 65536];
} WsConn;

static WsConn *g_ws_conns[WS_MAX_CONNS];
static int     g_ws_count = 0;

static int ws_lws_cb(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len) {
    (void)user;
    WsConn *c = NULL;
    for (int i = 0; i < g_ws_count; i++)
        if (g_ws_conns[i] && g_ws_conns[i]->wsi == wsi) { c = g_ws_conns[i]; break; }
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (c) c->connected = 1; break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (c && in && len > 0) {
            int n = (int)len; if (n > WS_RECV_BUF-1) n = WS_RECV_BUF-1;
            memcpy(c->recv_buf, in, (size_t)n);
            c->recv_buf[n] = '\0'; c->recv_len = n; c->recv_ready = 1;
        }
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (c) { c->connected = 0; c->closed = 1; } break;
    default: break;
    }
    return 0;
}

static struct lws_protocols g_ws_protos[] = {
    { "fluxa-ws", ws_lws_cb, 0, 65536, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static int ws_parse_url_lws(const char *url, char *host, int *port,
                              char *path, int *tls) {
    *tls = 0;
    const char *p = url;
    if      (strncmp(p, "wss://", 6) == 0) { *tls = 1; p += 6; }
    else if (strncmp(p, "ws://",  5) == 0) {            p += 5; }
    else return -1;
    const char *colon = strchr(p, ':'), *slash = strchr(p, '/');
    if (colon && (!slash || colon < slash)) {
        int hl = (int)(colon-p); if (hl >= 256) return -1;
        memcpy(host, p, (size_t)hl); host[hl] = '\0';
        *port = atoi(colon+1);
        p = slash ? slash : p+strlen(p);
    } else if (slash) {
        int hl = (int)(slash-p); if (hl >= 256) return -1;
        memcpy(host, p, (size_t)hl); host[hl] = '\0';
        *port = *tls ? 443 : 80; p = slash;
    } else {
        strncpy(host, p, 255); host[255] = '\0';
        *port = *tls ? 443 : 80; p = "/";
    }
    strncpy(path, *p ? p : "/", 511); path[511] = '\0';
    return 0;
}

static Value ws_do_connect(const char *url, int force_tls,
                             ErrStack *err, int *had_error, int line) {
    char errbuf[280], host[256], path[512]; int port, tls;
    if (ws_parse_url_lws(url, host, &port, &path, &tls) != 0) {
        snprintf(errbuf, sizeof(errbuf),
            "websocket.connect: invalid URL '%s'", url); goto fail_msg; }
    if (force_tls) tls = 1;
    if (g_ws_count >= WS_MAX_CONNS) {
        snprintf(errbuf, sizeof(errbuf),
            "websocket.connect: pool full"); goto fail_msg; }

    { WsConn *c = (WsConn *)calloc(1, sizeof(WsConn));
      strncpy(c->url, url, sizeof(c->url)-1);
      struct lws_context_creation_info info; memset(&info, 0, sizeof(info));
      info.port = CONTEXT_PORT_NO_LISTEN; info.protocols = g_ws_protos;
      info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      c->ctx = lws_create_context(&info);
      if (!c->ctx) { free(c);
          snprintf(errbuf, sizeof(errbuf), "websocket.connect: context failed");
          goto fail_msg; }
      struct lws_client_connect_info ci; memset(&ci, 0, sizeof(ci));
      ci.context = c->ctx; ci.address = host; ci.port = port;
      ci.path = path; ci.host = host; ci.origin = host;
      ci.protocol = g_ws_protos[0].name;
      ci.ssl_connection = tls ? LCCSCF_USE_SSL : 0;
      c->wsi = lws_client_connect_via_info(&ci);
      if (!c->wsi) { lws_context_destroy(c->ctx); free(c);
          snprintf(errbuf, sizeof(errbuf), "websocket.connect: connect failed");
          goto fail_msg; }
      for (int w = 0; !c->connected && !c->closed && w < 300; w++)
          lws_service(c->ctx, 10);
      if (!c->connected) { lws_context_destroy(c->ctx); free(c);
          snprintf(errbuf, sizeof(errbuf), "websocket.connect: handshake timeout");
          goto fail_msg; }
      g_ws_conns[g_ws_count++] = c;
      Value v; v.type = VAL_DYN;
      FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
      d->items = (Value *)malloc(sizeof(Value));
      d->items[0].type = VAL_PTR; d->items[0].as.ptr = c;
      d->count = 1; d->cap = 1; v.as.dyn = d; return v; }
fail_msg:
    errstack_push(err, ERR_FLUXA, errbuf, "websocket", line);
    *had_error = 1; Value nil; nil.type = VAL_NIL; return nil;
}

static WsConn *ws_unwrap(const Value *v, ErrStack *err, int *had_error,
                           int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"websocket.%s: invalid cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"websocket",line); *had_error=1; return NULL; }
    return (WsConn *)v->as.dyn->items[0].as.ptr;
}

static Value ws_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }
static Value ws_bool(int b) {
    Value v; v.type=VAL_BOOL; v.as.boolean=b; return v; }

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: native pure C99 (default — no external deps)
 * ════════════════════════════════════════════════════════════════════ */
#else  /* !FLUXA_WS_LIBWEBSOCKETS */

#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

typedef struct {
    int  fd;
    char url[512];
    int  connected;
} WsConn;

static const char _ws_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void ws_b64enc(const unsigned char *src, int slen, char *dst) {
    int i=0,j=0;
    while (i<slen) {
        int rem=slen-i;
        unsigned a=src[i++];
        unsigned b=(rem>1)?src[i++]:0;
        unsigned c=(rem>2)?src[i++]:0;
        unsigned t=(a<<16)|(b<<8)|c;
        dst[j++]=_ws_b64[(t>>18)&0x3F];
        dst[j++]=_ws_b64[(t>>12)&0x3F];
        dst[j++]=(rem>1)?_ws_b64[(t>>6)&0x3F]:'=';
        dst[j++]=(rem>2)?_ws_b64[(t   )&0x3F]:'=';
    }
    dst[j]='\0';
}

static int ws_parse_url(const char *url, char *host, char *port_s, char *path) {
    if (strncmp(url,"ws://",5)!=0) return -1;
    const char *p=url+5, *colon=strchr(p,':'), *slash=strchr(p,'/');
    if (colon&&(!slash||colon<slash)) {
        int hl=(int)(colon-p); if(hl>=256) return -1;
        memcpy(host,p,(size_t)hl); host[hl]='\0';
        int pl=slash?(int)(slash-colon-1):(int)strlen(colon+1); if(pl>=16) return -1;
        memcpy(port_s,colon+1,(size_t)pl); port_s[pl]='\0';
        p=slash?slash:p+strlen(p);
    } else if (slash) {
        int hl=(int)(slash-p); if(hl>=256) return -1;
        memcpy(host,p,(size_t)hl); host[hl]='\0';
        strcpy(port_s,"80"); p=slash;
    } else {
        strncpy(host,p,255); host[255]='\0'; strcpy(port_s,"80"); p="/";
    }
    strncpy(path,*p?p:"/",511); path[511]='\0';
    return 0;
}

static int ws_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints,*res,*r; memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host,port,&hints,&res)!=0) return -1;
    int fd=-1;
    for(r=res;r;r=r->ai_next) {
        fd=socket(r->ai_family,r->ai_socktype,r->ai_protocol); if(fd<0) continue;
        struct timeval tv={5,0};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        if(connect(fd,r->ai_addr,r->ai_addrlen)==0) break;
        close(fd); fd=-1;
    }
    freeaddrinfo(res); return fd;
}

static int ws_handshake(int fd, const char *host, const char *path) {
    unsigned char raw[16];
    for(int i=0;i<16;i++) raw[i]=(unsigned char)(i*37+42);
    char key[32]; ws_b64enc(raw,16,key);
    char req[1024];
    int n=snprintf(req,sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        path,host,key);
    { size_t _s=0; while((ssize_t)_s<n) {
        ssize_t _w=send(fd,req+_s,(size_t)(n-_s),0);
        if(_w<=0) return -1;
        _s+=(size_t)_w;
    } }
    char resp[2048]; int rl=0;
    while(rl<(int)sizeof(resp)-1) {
        if(recv(fd,resp+rl,1,0)!=1) return -1;
        rl++;
        if(rl>=4&&resp[rl-4]=='\r'&&resp[rl-3]=='\n'&&
           resp[rl-2]=='\r'&&resp[rl-1]=='\n') break;
    }
    resp[rl]='\0';
    return strstr(resp,"101")?0:-1;
}

static int ws_send_frame(int fd, const char *data, size_t dlen, int opcode) {
    unsigned char frame[10]; int fi=0;
    frame[fi++]=(unsigned char)(0x80|(opcode&0x0F));
    unsigned char mask[4]={0x12,0x34,0x56,0x78};
    if(dlen<126) { frame[fi++]=(unsigned char)(0x80|dlen); }
    else if(dlen<65536) {
        frame[fi++]=0x80|126;
        frame[fi++]=(unsigned char)((dlen>>8)&0xFF);
        frame[fi++]=(unsigned char)(dlen&0xFF);
    } else return -1;
    memcpy(frame+fi,mask,4); fi+=4;
    if(send(fd,frame,(size_t)fi,0)!=fi) return -1;
    unsigned char buf[4096]; size_t sent=0;
    while(sent<dlen) {
        size_t chunk=dlen-sent; if(chunk>sizeof(buf)) chunk=sizeof(buf);
        for(size_t i=0;i<chunk;i++) buf[i]=(unsigned char)data[sent+i]^mask[i%4];
        if(send(fd,buf,chunk,0)!=(ssize_t)chunk) return -1;
        sent+=chunk;
    }
    return 0;
}

static int ws_recv_frame(int fd, char *out, int maxout, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    unsigned char hdr[2];
    if(recv(fd,hdr,2,MSG_WAITALL)!=2) return -1;
    int opcode=hdr[0]&0x0F, masked=(hdr[1]>>7)&1;
    int plen=hdr[1]&0x7F;
    if(opcode==8) return -2; /* close */
    if(opcode==9) { unsigned char pong[2]={0x8A,0x00}; send(fd,pong,2,0); return 0; }
    if(plen==126) {
        unsigned char ext[2]; if(recv(fd,ext,2,MSG_WAITALL)!=2) return -1;
        plen=(ext[0]<<8)|ext[1];
    } else if(plen==127) return -1;
    unsigned char mask[4]={0};
    if(masked&&recv(fd,mask,4,MSG_WAITALL)!=4) return -1;
    if(plen>=maxout) plen=maxout-1;
    int got=(int)recv(fd,out,(size_t)plen,MSG_WAITALL);
    if(got<=0) return -1;
    if(masked) for(int i=0;i<got;i++) out[i]^=mask[i%4];
    out[got]='\0'; return got;
}

static Value ws_do_connect(const char *url, int force_tls,
                             ErrStack *err, int *had_error, int line) {
    char errbuf[280];
    if (force_tls || strncmp(url,"wss://",6)==0) {
        snprintf(errbuf,sizeof(errbuf),
            "websocket.connect: wss:// requires FLUXA_WS_LIBWEBSOCKETS build "
            "(apt install libssl-dev libwebsockets-dev, then make FLUXA_WS_LWS=1 build)");
        errstack_push(err,ERR_FLUXA,errbuf,"websocket",line);
        *had_error=1; Value nil; nil.type=VAL_NIL; return nil; }
    char host[256],port_s[16],path[512];
    if(ws_parse_url(url,host,port_s,path)!=0) {
        snprintf(errbuf,sizeof(errbuf),
            "websocket.connect: invalid URL '%s' — use ws://host:port/path",url);
        goto fail; }
    { int fd=ws_tcp_connect(host,port_s);
      if(fd<0) { snprintf(errbuf,sizeof(errbuf),
          "websocket.connect: TCP connection failed");  /* host:port logged below */ goto fail; }
      if(ws_handshake(fd,host,path)!=0) { close(fd);
          snprintf(errbuf,sizeof(errbuf),
          "websocket.connect: WebSocket handshake failed"); goto fail; }
      WsConn *c=(WsConn *)calloc(1,sizeof(WsConn));
      c->fd=fd; c->connected=1; strncpy(c->url,url,sizeof(c->url)-1);
      FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
      d->items=(Value *)malloc(sizeof(Value));
      d->items[0].type=VAL_PTR; d->items[0].as.ptr=c;
      d->count=1; d->cap=1;
      Value v; v.type=VAL_DYN; v.as.dyn=d; return v; }
fail:
    errstack_push(err,ERR_FLUXA,errbuf,"websocket",line);
    *had_error=1; Value nil; nil.type=VAL_NIL; return nil;
}

static WsConn *ws_unwrap(const Value *v, ErrStack *err, int *had_error,
                           int line, const char *fn) {
    char eb[280];
    if(v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
       v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"websocket.%s: invalid cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"websocket",line); *had_error=1; return NULL; }
    return (WsConn *)v->as.dyn->items[0].as.ptr;
}
static Value ws_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }
static Value ws_bool(int b) { Value v; v.type=VAL_BOOL; v.as.boolean=b; return v; }

#endif /* FLUXA_WS_LIBWEBSOCKETS */

/* ════════════════════════════════════════════════════════════════════
 * DISPATCH — identical API for both backends
 * ════════════════════════════════════════════════════════════════════ */
static inline Value fluxa_std_websocket_call(const char *fn_name,
                                              const Value *args, int argc,
                                              ErrStack *err, int *had_error,
                                              int line) {
    char errbuf[280];

#define WS_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"websocket.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"websocket",line); \
    *had_error=1; Value _nil; _nil.type=VAL_NIL; return _nil; } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf), \
        "websocket.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"websocket",line); \
    *had_error=1; Value _nil; _nil.type=VAL_NIL; return _nil; } } while(0)

#define GET_STR(idx, var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) WS_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

#define GET_CONN(idx, var) \
    WsConn *(var)=ws_unwrap(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) { Value _nil; _nil.type=VAL_NIL; return _nil; }

#define GET_INT(idx, var) \
    if(args[(idx)].type!=VAL_INT) WS_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

    if (!strcmp(fn_name,"version")) {
#ifdef FLUXA_WS_LIBWEBSOCKETS
        return ws_str(lws_get_library_version());
#else
        return ws_str("fluxa-ws/1.0 RFC6455 (native)");
#endif
    }

    if (!strcmp(fn_name,"connect")) {
        NEED(1); GET_STR(0,url);
        return ws_do_connect(url, 0, err, had_error, line);
    }

    if (!strcmp(fn_name,"connect_tls")) {
        NEED(1); GET_STR(0,url);
        return ws_do_connect(url, 1, err, had_error, line);
    }

    if (!strcmp(fn_name,"connected")) {
        NEED(1); GET_CONN(0,c);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        return ws_bool(c->connected && !c->closed);
#else
        return ws_bool(c->connected && c->fd >= 0);
#endif
    }

    if (!strcmp(fn_name,"url")) {
        NEED(1); GET_CONN(0,c); return ws_str(c->url);
    }

    if (!strcmp(fn_name,"send")) {
        NEED(2); GET_CONN(0,c); GET_STR(1,msg);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        if(!c->connected||c->closed) WS_ERR("send: not connected");
        size_t n=strlen(msg); if(n>65535) WS_ERR("send: too large");
        memcpy(&c->send_buf[LWS_PRE],msg,n);
        if(lws_write(c->wsi,&c->send_buf[LWS_PRE],n,LWS_WRITE_TEXT)<0)
            WS_ERR("send: write failed");
        lws_service(c->ctx,0);
#else
        if(!c->connected||c->fd<0) WS_ERR("send: not connected");
        if(ws_send_frame(c->fd,msg,strlen(msg),0x01)<0) WS_ERR("send: failed");
#endif
        Value nil; nil.type=VAL_NIL; return nil;
    }

    if (!strcmp(fn_name,"send_bin")) {
        NEED(2); GET_CONN(0,c); GET_STR(1,msg);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        if(!c->connected||c->closed) WS_ERR("send_bin: not connected");
        size_t n=strlen(msg); if(n>65535) WS_ERR("send_bin: too large");
        memcpy(&c->send_buf[LWS_PRE],msg,n);
        lws_write(c->wsi,&c->send_buf[LWS_PRE],n,LWS_WRITE_BINARY);
        lws_service(c->ctx,0);
#else
        if(!c->connected||c->fd<0) WS_ERR("send_bin: not connected");
        if(ws_send_frame(c->fd,msg,strlen(msg),0x02)<0) WS_ERR("send_bin: failed");
#endif
        Value nil; nil.type=VAL_NIL; return nil;
    }

    if (!strcmp(fn_name,"recv")) {
        NEED(2); GET_CONN(0,c); GET_INT(1,timeout_ms);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        if(!c->connected||c->closed) WS_ERR("recv: not connected");
        c->recv_ready=0;
        int lim=(int)(timeout_ms/10)+1;
        for(int w=0;!c->recv_ready&&!c->closed&&w<lim;w++) lws_service(c->ctx,10);
        if(c->recv_ready) { c->recv_ready=0; return ws_str(c->recv_buf); }
        return ws_str("");
#else
        if(!c->connected||c->fd<0) WS_ERR("recv: not connected");
        static char rbuf[65536];
        int n=ws_recv_frame(c->fd,rbuf,(int)sizeof(rbuf),(int)timeout_ms);
        if(n>0) return ws_str(rbuf);
        if(n==-2) { c->connected=0; }
        return ws_str("");
#endif
    }

    if (!strcmp(fn_name,"poll")) {
        NEED(1); GET_CONN(0,c);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        if(!c->connected||c->closed) return ws_bool(0);
        lws_service(c->ctx,0); return ws_bool(c->recv_ready);
#else
        if(!c->connected||c->fd<0) return ws_bool(0);
        fd_set fds; FD_ZERO(&fds); FD_SET(c->fd,&fds);
        struct timeval tv={0,0};
        int r=select(c->fd+1,&fds,NULL,NULL,&tv);
        return ws_bool(r>0);
#endif
    }

    if (!strcmp(fn_name,"close")) {
        NEED(1); GET_CONN(0,c);
#ifdef FLUXA_WS_LIBWEBSOCKETS
        if(c->wsi&&c->connected)
            lws_close_reason(c->wsi,LWS_CLOSE_STATUS_NORMAL,NULL,0);
        c->connected=0;
        if(c->ctx) { lws_service(c->ctx,50); lws_context_destroy(c->ctx); }
        c->ctx=NULL; c->wsi=NULL; c->closed=1;
        for(int i=0;i<g_ws_count;i++)
            if(g_ws_conns[i]==c) { g_ws_conns[i]=g_ws_conns[--g_ws_count]; break; }
#else
        if(c->fd>=0) {
            ws_send_frame(c->fd,NULL,0,0x08); /* close frame */
            close(c->fd); c->fd=-1; }
        c->connected=0;
#endif
        free(c);
        if(argc>0&&args[0].type==VAL_DYN&&args[0].as.dyn&&args[0].as.dyn->count>0)
            args[0].as.dyn->items[0].as.ptr=NULL;
        Value nil; nil.type=VAL_NIL; return nil;
    }

#undef WS_ERR
#undef NEED
#undef GET_STR
#undef GET_CONN
#undef GET_INT

    snprintf(errbuf,sizeof(errbuf),"websocket.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"websocket",line);
    *had_error=1; Value nil; nil.type=VAL_NIL; return nil;
}

FLUXA_LIB_EXPORT(
    name      = "websocket",
    toml_key  = "std.websocket",
    owner     = "websocket",
    call      = fluxa_std_websocket_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_WEBSOCKET_H */
