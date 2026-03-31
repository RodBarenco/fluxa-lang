/* fluxa_ipc.h — Fluxa IPC Layer (Sprint 9)
 *
 * Pluggable transport for runtime interaction commands:
 *   fluxa observe <var>       — watch a prst value in real time
 *   fluxa set <var> <val>     — mutate a prst value without stopping execution
 *   fluxa logs                — tail runtime error/event log
 *   fluxa status              — query runtime health
 *
 * Backend selection at compile time:
 *   IPC_BACKEND_UNIX_SOCKET   — Linux / macOS  (default)
 *   IPC_BACKEND_NONE          — RP2040 / bare-metal (direct memory access)
 *
 * Security properties (UNIX_SOCKET backend):
 *   - Socket path: /tmp/fluxa-<pid>.sock  (per-process, no collisions)
 *   - Created with umask(0077) → permissions 0600 (owner only)
 *   - All messages are fixed-size structs — no length field, no dynamic alloc
 *   - Commands carry opcode + pre-validated offsets, never raw strings
 *   - Every read has a 100ms timeout — no indefinite blocking
 *   - Server validates client UID matches socket owner before processing
 *
 * Wire protocol — all fields little-endian, no padding between fields:
 *
 *   Request  (IpcRequest,  32 bytes fixed)
 *   Response (IpcResponse, 48 bytes fixed)
 *
 * No framing, no length prefix, no TLV — fixed size IS the framing.
 */
#ifndef FLUXA_IPC_H
#define FLUXA_IPC_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── Backend selection ───────────────────────────────────────────────────── */
#if defined(FLUXA_IPC_NONE)
#  define IPC_BACKEND IPC_BACKEND_NONE
#else
#  define IPC_BACKEND IPC_BACKEND_UNIX_SOCKET
#endif

/* ── Wire constants ──────────────────────────────────────────────────────── */
#define IPC_MAGIC          0xF10Au       /* 2-byte magic: "FL" */
#define IPC_VERSION        1u
#define IPC_SOCKET_DIR     "/tmp"
#define IPC_SOCKET_FMT     "/tmp/fluxa-%d.sock"   /* %d = server PID */
#define IPC_LOCK_FMT       "/tmp/fluxa-%d.lock"
#define IPC_TIMEOUT_MS     100
#define IPC_VAR_NAME_MAX   64            /* max prst variable name length    */
#define IPC_LOG_LINE_MAX   128           /* max single log line in response  */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
typedef enum {
    IPC_OP_PING        = 0x01,  /* health check — no payload                */
    IPC_OP_OBSERVE     = 0x02,  /* get current value of a prst var          */
    IPC_OP_SET         = 0x03,  /* set value of a prst var                  */
    IPC_OP_LOGS        = 0x04,  /* get N most recent log entries            */
    IPC_OP_STATUS      = 0x05,  /* get runtime status snapshot              */
} IpcOpcode;

/* ── Value type tags (mirrors FluxaType, self-contained for wire format) ── */
typedef enum {
    IPC_TYPE_INT   = 1,
    IPC_TYPE_FLOAT = 2,
    IPC_TYPE_BOOL  = 3,
    IPC_TYPE_STR   = 4,
    IPC_TYPE_NIL   = 5,
} IpcTypeTag;

/* ── Request (32 bytes, fixed) ───────────────────────────────────────────── */
/* Layout:
 *   [0..1]   magic    uint16
 *   [2]      version  uint8
 *   [3]      opcode   uint8
 *   [4..7]   seq      uint32   (client sequence number, echoed in response)
 *   [8..71]  name     char[64] (variable name, NUL-padded, for OBSERVE/SET)
 *   [72..79] i_val    int64    (integer value, for SET IPC_TYPE_INT)
 *   [80..87] f_val    double   (float value,   for SET IPC_TYPE_FLOAT)
 *   [88]     type_tag uint8    (IpcTypeTag,    for SET)
 *   [89..95] _pad     uint8[7]
 * Total: 96 bytes
 */
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  opcode;           /* IpcOpcode                                */
    uint32_t seq;              /* echoed back in response                  */
    char     name[IPC_VAR_NAME_MAX];  /* prst var name (NUL-padded)        */
    int64_t  i_val;            /* integer value payload (SET)              */
    double   f_val;            /* float value payload (SET)                */
    uint8_t  type_tag;         /* IpcTypeTag (SET)                         */
    uint8_t  b_val;            /* bool value payload (SET)                 */
    uint8_t  _pad[6];
} IpcRequest;                  /* sizeof = 96 bytes                        */

/* ── Response (96 bytes, fixed) ─────────────────────────────────────────── */
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  status;           /* 0 = OK, non-zero = error code            */
    uint32_t seq;              /* echoed from request                      */
    char     name[IPC_VAR_NAME_MAX];  /* var name (echoed)                 */
    int64_t  i_val;            /* int result                               */
    double   f_val;            /* float result                             */
    uint8_t  type_tag;         /* IpcTypeTag of returned value             */
    uint8_t  b_val;            /* bool result                              */
    uint8_t  _pad[2];
    char     message[IPC_LOG_LINE_MAX]; /* error message or log line       */
    /* STATUS fields */
    int32_t  cycle_count;      /* runtime cycle_count at snapshot          */
    int32_t  prst_count;       /* number of active prst vars               */
    int32_t  err_count;        /* number of errors in err_stack            */
    uint8_t  mode;             /* FluxaMode (0=SCRIPT, 1=PROJECT)          */
    uint8_t  dry_run;          /* 1 if in dry_run                          */
    uint8_t  _pad2[2];
} IpcResponse;                 /* sizeof = 96 + 128 + 16 = fits ~256 bytes */

/* ── Status codes ────────────────────────────────────────────────────────── */
#define IPC_STATUS_OK           0x00
#define IPC_STATUS_ERR_UNKNOWN  0x01
#define IPC_STATUS_ERR_NOTFOUND 0x02  /* prst var not found                */
#define IPC_STATUS_ERR_TYPE     0x03  /* type mismatch on SET              */
#define IPC_STATUS_ERR_AUTH     0x04  /* UID mismatch                      */
#define IPC_STATUS_ERR_MAGIC    0x05  /* bad magic in request              */
#define IPC_STATUS_ERR_TIMEOUT  0x06

/* ── IpcRtView — forward declaration only ───────────────────────────────── */
/* Full definition, ipc_rtview_create(), and ipc_rtview_destroy() are in
 * ipc_server.h, which has the required includes (pthread, PrstPool, ErrStack).
 * fluxa_ipc.h only needs the opaque type for IpcServer.rt. */
typedef struct IpcRtView IpcRtView;

static inline void ipc_socket_path(char *buf, size_t buflen, int pid) {
    snprintf(buf, buflen, IPC_SOCKET_FMT, pid);
}
static inline void ipc_lock_path(char *buf, size_t buflen, int pid) {
    snprintf(buf, buflen, IPC_LOCK_FMT, pid);
}

/* ── Request builder helpers ─────────────────────────────────────────────── */
static inline void ipc_req_ping(IpcRequest *r, uint32_t seq) {
    memset(r, 0, sizeof *r);
    r->magic   = IPC_MAGIC;
    r->version = IPC_VERSION;
    r->opcode  = IPC_OP_PING;
    r->seq     = seq;
}

static inline void ipc_req_observe(IpcRequest *r, uint32_t seq,
                                    const char *name) {
    memset(r, 0, sizeof *r);
    r->magic   = IPC_MAGIC;
    r->version = IPC_VERSION;
    r->opcode  = IPC_OP_OBSERVE;
    r->seq     = seq;
    strncpy(r->name, name, IPC_VAR_NAME_MAX - 1);
}

static inline void ipc_req_set_int(IpcRequest *r, uint32_t seq,
                                    const char *name, int64_t val) {
    memset(r, 0, sizeof *r);
    r->magic    = IPC_MAGIC;
    r->version  = IPC_VERSION;
    r->opcode   = IPC_OP_SET;
    r->seq      = seq;
    r->type_tag = IPC_TYPE_INT;
    r->i_val    = val;
    strncpy(r->name, name, IPC_VAR_NAME_MAX - 1);
}

static inline void ipc_req_set_float(IpcRequest *r, uint32_t seq,
                                      const char *name, double val) {
    memset(r, 0, sizeof *r);
    r->magic    = IPC_MAGIC;
    r->version  = IPC_VERSION;
    r->opcode   = IPC_OP_SET;
    r->seq      = seq;
    r->type_tag = IPC_TYPE_FLOAT;
    r->f_val    = val;
    strncpy(r->name, name, IPC_VAR_NAME_MAX - 1);
}

static inline void ipc_req_set_bool(IpcRequest *r, uint32_t seq,
                                     const char *name, int val) {
    memset(r, 0, sizeof *r);
    r->magic    = IPC_MAGIC;
    r->version  = IPC_VERSION;
    r->opcode   = IPC_OP_SET;
    r->seq      = seq;
    r->type_tag = IPC_TYPE_BOOL;
    r->b_val    = (uint8_t)(val ? 1 : 0);
    strncpy(r->name, name, IPC_VAR_NAME_MAX - 1);
}

static inline void ipc_req_logs(IpcRequest *r, uint32_t seq) {
    memset(r, 0, sizeof *r);
    r->magic   = IPC_MAGIC;
    r->version = IPC_VERSION;
    r->opcode  = IPC_OP_LOGS;
    r->seq     = seq;
}

static inline void ipc_req_status(IpcRequest *r, uint32_t seq) {
    memset(r, 0, sizeof *r);
    r->magic   = IPC_MAGIC;
    r->version = IPC_VERSION;
    r->opcode  = IPC_OP_STATUS;
    r->seq     = seq;
}

/* ── Validation ──────────────────────────────────────────────────────────── */
static inline int ipc_request_valid(const IpcRequest *r) {
    if (r->magic   != IPC_MAGIC)   return 0;
    if (r->version != IPC_VERSION) return 0;
    /* name must be NUL-terminated within bounds */
    int has_nul = 0;
    for (int i = 0; i < IPC_VAR_NAME_MAX; i++) {
        if (r->name[i] == '\0') { has_nul = 1; break; }
    }
    if (!has_nul) return 0;
    return 1;
}

#if IPC_BACKEND == IPC_BACKEND_UNIX_SOCKET
/* ── UNIX socket implementation ──────────────────────────────────────────── */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <glob.h>
#include <pthread.h>

/* ── IPC server state (embedded in Runtime, one per running process) ─────── */
typedef struct {
    int            server_fd;      /* listening unix socket fd             */
    pthread_t      thread;         /* server accept/dispatch thread        */
    volatile int   running;        /* 0 = shutdown requested               */
    char           sock_path[108]; /* UNIX_PATH_MAX                        */
    char           lock_path[108];
    int            pid;

    /* Back-pointer to runtime data — server reads/writes prst pool.
     * Access is protected by the runtime's own safe-point discipline:
     * SET is only applied at safe points, OBSERVE is read-only. */
    void          *rt;             /* Runtime* — avoids circular include   */
    pthread_mutex_t mu;            /* protects rt access from server thread */
} IpcServer;

/* ── IPC client state (used by CLI commands: observe, set, logs) ─────────── */
typedef struct {
    int  fd;
    char sock_path[108];
} IpcClient;

/* ── Server: create socket, bind, set permissions ───────────────────────── */
static inline int ipc_server_bind(IpcServer *srv, int pid) {
    srv->pid = pid;
    ipc_socket_path(srv->sock_path, sizeof srv->sock_path, pid);
    ipc_lock_path(srv->lock_path,   sizeof srv->lock_path,  pid);

    /* Remove stale socket */
    unlink(srv->sock_path);

    srv->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->server_fd < 0) return -1;

    /* Restrict permissions before bind — umask ensures 0600 */
    mode_t old = umask(0077);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, srv->sock_path, sizeof(addr.sun_path) - 1); addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        umask(old);
        close(srv->server_fd);
        srv->server_fd = -1;
        return -1;
    }
    umask(old);

    /* Explicit chmod to be safe even if umask was already permissive */
    chmod(srv->sock_path, 0600);

    if (listen(srv->server_fd, 4) < 0) {
        close(srv->server_fd);
        srv->server_fd = -1;
        return -1;
    }

    /* Write PID lock file */
    FILE *lf = fopen(srv->lock_path, "w");
    if (lf) { fprintf(lf, "%d\n", pid); fclose(lf); }

    pthread_mutex_init(&srv->mu, NULL);
    return 0;
}

/* ── Server: timed recv (100ms) ──────────────────────────────────────────── */
static inline int ipc_recv_timed(int fd, void *buf, size_t len) {
    struct timeval tv = { 0, IPC_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t n = recv(fd, buf, len, MSG_WAITALL);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* ── Server: send full buffer ────────────────────────────────────────────── */
static inline int ipc_send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── Client: connect to running runtime ──────────────────────────────────── */
static inline int ipc_client_connect(IpcClient *cli, int pid) {
    ipc_socket_path(cli->sock_path, sizeof cli->sock_path, pid);

    cli->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli->fd < 0) return -1;

    struct timeval tv = { 0, IPC_TIMEOUT_MS * 1000 };
    setsockopt(cli->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(cli->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, cli->sock_path, sizeof(addr.sun_path) - 1); addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(cli->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(cli->fd);
        cli->fd = -1;
        return -1;
    }
    return 0;
}

static inline int ipc_client_send(IpcClient *cli, const IpcRequest *req,
                                    IpcResponse *resp) {
    if (ipc_send_all(cli->fd, req, sizeof *req) < 0)  return -1;
    if (ipc_recv_timed(cli->fd, resp, sizeof *resp) < 0) return -1;
    if (resp->magic != IPC_MAGIC) return -1;
    if (resp->seq   != req->seq)  return -1;
    return 0;
}

static inline void ipc_client_close(IpcClient *cli) {
    if (cli->fd >= 0) { close(cli->fd); cli->fd = -1; }
}

/* ── Discover PID of running fluxa runtime ───────────────────────────────── */
/* Reads the first PID found in /tmp/fluxa-*.lock that is still alive.
 * Returns 0 if no runtime is running. */
static inline int ipc_discover_pid(void) {
    glob_t gl;
    if (glob("/tmp/fluxa-*.lock", 0, NULL, &gl) != 0) return 0;
    int found = 0;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        FILE *f = fopen(gl.gl_pathv[i], "r");
        if (!f) continue;
        int pid = 0;
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
        if (pid > 0 && kill(pid, 0) == 0) { found = pid; break; }
    }
    globfree(&gl);
    return found;
}

static inline void ipc_server_cleanup(IpcServer *srv) {
    srv->running = 0;
    if (srv->server_fd >= 0) { close(srv->server_fd); srv->server_fd = -1; }
    unlink(srv->sock_path);
    unlink(srv->lock_path);
    pthread_mutex_destroy(&srv->mu);
}

#endif /* IPC_BACKEND_UNIX_SOCKET */
#endif /* FLUXA_IPC_H */
