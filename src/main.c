/* main.c — Fluxa CLI entry point (Sprint 9 / Sprint 9.b)
 *
 * Commands:
 *   fluxa run <file.flx>                     execute (auto-detects script vs project)
 *   fluxa run <file.flx> -dev                dev mode: watch + auto-reload on change
 *   fluxa run <file.flx> -dev -p             dev mode with preflight validation
 *   fluxa run <file.flx> -prod               prod mode (manual apply via IPC)
 *   fluxa explain                            live prst state from running runtime (IPC)
 *   fluxa explain <file.flx>                 execute file and print prst state + dep graph
 *   fluxa apply <file.flx>                   one-shot reload preserving prst state
 *   fluxa apply <file.flx> -p               preflight before applying
 *   fluxa apply <file.flx> -p --force       force apply even with warnings (prod only)
 *   fluxa handover <old.flx> <new.flx>      Atomic Handover (5-step protocol)
 *   fluxa observe <var>                      watch prst value in real time
 *   fluxa set <var> <val>                    mutate prst value without stopping execution
 *   fluxa logs                               tail runtime error/event log
 *   fluxa status                             runtime health snapshot
 *   fluxa init [dir]                         create project structure with fluxa.toml
 *
 * Sprint 9 additions:
 *   IPC layer: unix socket /tmp/fluxa-<pid>.sock, 0600 permissions, fixed-size protocol
 *   fluxa observe / set / logs / status: connect to running runtime via IPC
 *   fluxa init: scaffold project directory
 *   preflight (-p): validate before applying, operator decides
 *   --force: apply with warnings (prod only)
 *
 * Sprint 9.b additions:
 *   Issue #95: safe point on every while back-edge (fluxa set works in infinite loops)
 *   Issue #96: fluxa explain without file → IPC_OP_EXPLAIN streaming from live runtime
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pool.h"
#include "resolver.h"
#include "runtime.h"
#include "handover.h"
#include "watcher.h"
#include "fluxa_ipc.h"
#ifdef FLUXA_SECURE
#include "fluxa_keygen.h"
#endif
/* fluxa dis — forward declaration; linked from dis.c via Makefile */
int fluxa_dis_file(const char *inpath, const char *outpath);
#include "ipc_server.h"
#include <pthread.h>

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[fluxa] cannot open file: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = (char*)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, size, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  fluxa run <file.flx>                    execute (auto script vs project)\n"
        "  fluxa run <file.flx> -dev               dev mode: watch + reload on save\n"
        "  fluxa run <file.flx> -dev -p            dev mode with preflight validation\n"
        "  fluxa run <file.flx> -prod              prod mode (manual apply)\n"
        "  fluxa explain                           live state from running runtime (IPC)\n"
        "  fluxa explain <file.flx>                execute file and show prst + deps\n"
        "  fluxa dis <file.flx>                    disassemble: AST, warm forecast,\n"
        "                                            hot bytecode, call order, prst fork\n"
        "  fluxa dis <file.flx> -o out.txt         write dis report to explicit path\n"
        "  fluxa apply <file.flx>                  reload preserving prst state\n"
        "  fluxa apply <file.flx> -p               preflight before applying\n"
        "  fluxa apply <file.flx> -p --force       force apply with warnings (prod only)\n"
        "  fluxa handover <old.flx> <new.flx>      Atomic Handover (5-step protocol)\n"
        "  fluxa observe <var>                      watch prst value in real time\n"
        "  fluxa set <var> <val>                    mutate prst value without stopping\n"
        "  fluxa logs                               tail runtime error/event log\n"
        "  fluxa status                             runtime health snapshot\n"
        "  fluxa init <name>                        Create project: main.flx, fluxa.toml,\n"\
        "                                             fluxa.libs, live/, static/, tests/\n"
        "  fluxa update                             reload fluxa.toml [ffi]/[libs] live\n"
        "  fluxa ffi list                           list shared libs available on system\n"
        "  fluxa ffi inspect <lib>                  generate toml signatures from lib\n"
        "  fluxa runtime info                       show runtime state + config\n"
        "  fluxa keygen [--dir <path>]              generate Ed25519 + HMAC keys\n"
        "                                             (FLUXA_SECURE builds only)\n"
    );
}

/* Parse a .flx file and return the program AST.
 * pool must be initialized by the caller. Returns NULL on parse error. */
static ASTNode *parse_file(const char *path, ASTPool *pool) {
    char *source = load_file(path);
    if (!source) return NULL;
    pool_init(pool);
    Parser   parser  = parser_new(source, pool);
    ASTNode *program = parser_parse(&parser);
    free(source);
    parser_free(&parser);
    if (!program) {
        fprintf(stderr, "[fluxa] aborting due to parse errors.\n");
        pool_free(pool);
    }
    return program;
}

/* Single run — no watcher */
static int run_once(const char *path, int explain) {
    static ASTPool pool;
    ASTNode *program = parse_file(path, &pool);
    if (!program) return 1;
    int result = explain ? runtime_exec_explain(program) : runtime_exec(program);
    pool_free(&pool);
    return result;
}

/* Dev mode: watch file and reload on every save, preserving prst state */
/* ── -dev mode: execution context passed to worker thread ───────────────── */
typedef struct {
    const char    *path;
    ASTPool       *ast_pool;
    PrstPool      *pool;          /* prst state preserved across reloads   */
    int            first_run;
    volatile int   cancel;        /* watcher sets 1 → VM stops at next check */
    volatile int   done;          /* worker sets 1 when finished           */
    int            exit_code;
    IpcRtView     *ipc_view;      /* Sprint 9: stable view for IPC server  */
} DevCtx;

static void *dev_exec_thread(void *arg) {
    DevCtx *ctx = (DevCtx *)arg;

    ASTNode *program = parse_file(ctx->path, ctx->ast_pool);
    if (!program) {
        fprintf(stderr, "[fluxa] -dev: parse error — waiting for fix...\n");
        ctx->exit_code = 1;
        ctx->done = 1;
        return NULL;
    }

    fprintf(stderr, "[fluxa] -dev: running %s\n", ctx->path);

    /* Register the cancel flag so every runtime created here picks it up
     * via g_cancel_flag. Only one runtime runs at a time in -dev mode. */
    runtime_set_cancel_flag(&ctx->cancel);
    /* Sprint 9: register the IPC view so the exec loop updates it each cycle */
    runtime_set_ipc_view(ctx->ipc_view);

    int r;
    if (ctx->first_run) {
        r = runtime_exec(program);
        ctx->first_run = 0;
    } else {
        r = runtime_apply(program, ctx->pool);
        if (!ctx->cancel)
            fprintf(stderr, "[fluxa] -dev: reload done (exit=%d)\n", r);
    }
    runtime_set_cancel_flag(NULL);
    runtime_set_ipc_view(NULL);

    pool_free(ctx->ast_pool);
    ctx->exit_code = r;
    ctx->done = 1;   /* signal watcher that script finished on its own */
    return NULL;
}

static int run_dev(const char *path) {
    fprintf(stderr, "[fluxa] -dev: watching %s (Ctrl-C to stop)\n", path);

    static ASTPool ast_pool;
    static PrstPool pool;
    pool.entries = NULL; pool.count = 0; pool.cap = 0;

    /* Sprint 9: create stable IPC view — survives across reloads */
    IpcRtView *ipc_view = ipc_rtview_create();
    IpcServer *ipc = ipc_view ? ipc_server_start(ipc_view) : NULL;

    DevCtx ctx;
    ctx.path      = path;
    ctx.ast_pool  = &ast_pool;
    ctx.pool      = &pool;
    ctx.first_run = 1;
    ctx.cancel    = 0;
    ctx.done      = 0;
    ctx.exit_code = 0;
    ctx.ipc_view  = ipc_view;  /* passed to runtime so it can update the view */

    while (1) {
        ctx.cancel = 0;
        ctx.done   = 0;

        pthread_t tid;
        if (pthread_create(&tid, NULL, dev_exec_thread, &ctx) != 0) {
            fprintf(stderr, "[fluxa] -dev: pthread_create failed\n");
            if (ipc) ipc_server_stop(ipc);
            if (ipc_view) ipc_rtview_destroy(ipc_view);
            return 1;
        }

        FWatcher *fw = fw_open(path);
        if (!fw) {
            fprintf(stderr, "[fluxa] -dev: cannot open watcher for %s\n", path);
            ctx.cancel = 1;
            pthread_join(tid, NULL);
            if (ipc) ipc_server_stop(ipc);
            if (ipc_view) ipc_rtview_destroy(ipc_view);
            return 1;
        }

        int reload = 0;
        while (!reload) {
            int wr = fw_wait(fw, 200);
            if (wr == 1)  reload = 1;
            else if (wr == -1) {
                fw_close(fw);
                fw = fw_open(path);
                if (!fw) break;
            }
            if (ctx.done) reload = 1;
        }
        fw_close(fw);

        if (!ctx.done) {
            ctx.cancel = 1;
            pthread_join(tid, NULL);
            fprintf(stderr, "[fluxa] -dev: reload triggered\n");
        } else {
            pthread_join(tid, NULL);
            fprintf(stderr, "[fluxa] -dev: waiting for changes...\n");
        }
    }
    if (ipc) ipc_server_stop(ipc);
    if (ipc_view) ipc_rtview_destroy(ipc_view);
    return 0;
}

/* Prod mode: start IPC server, run program once (no file watcher, no reload).
 * Stays alive as long as the program runs (e.g. infinite loop).
 * IPC commands (observe, set, logs, status, explain) all work. */
static int run_prod(const char *path) {
    fprintf(stderr, "[fluxa] -prod: running %s\n", path);

#ifdef FLUXA_SECURE
    /* Load config to check security settings before starting IPC server */
    {
        char proj_dir[512]; strncpy(proj_dir, path, sizeof(proj_dir)-1);
        char *sl = strrchr(proj_dir, '/');
        if (sl) *sl = '\0'; else strncpy(proj_dir, ".", sizeof(proj_dir)-1);
        char toml_path[600];
        snprintf(toml_path, sizeof(toml_path), "%s/fluxa.toml", proj_dir);
        FluxaConfig cfg = fluxa_config_load(toml_path);
        if (fluxa_security_check(
                cfg.security.signing_key_path[0] ? cfg.security.signing_key_path : NULL,
                cfg.security.ipc_hmac_key_path[0] ? cfg.security.ipc_hmac_key_path : NULL,
                cfg.security.mode) != 0) {
            fprintf(stderr,
                "[fluxa] -prod: security check failed. Fix [security] in fluxa.toml\n"
                "  or run with standard ./fluxa (non-secure build) for development.\n");
            return 1;
        }
        if (cfg.security.mode != FLUXA_SEC_MODE_OFF) {
            fprintf(stderr, "[fluxa] -prod: security mode=%s\n",
                cfg.security.mode == FLUXA_SEC_MODE_STRICT ? "strict" : "warn");
        }
    }
#endif

    static ASTPool ast_pool;

    /* Create stable IPC view — IPC server uses this for all queries */
    IpcRtView *ipc_view = ipc_rtview_create();
    IpcServer *ipc = ipc_view ? ipc_server_start(ipc_view) : NULL;

    if (!ipc) {
        fprintf(stderr, "[fluxa] -prod: IPC server failed to start\n");
        if (ipc_view) ipc_rtview_destroy(ipc_view);
        return 1;
    }

#ifdef FLUXA_SECURE
    /* Apply [security] config values to IPC server runtime parameters.
     * These override compile-time defaults — set before the first accept(). */
    {
        char proj_dir2[512]; strncpy(proj_dir2, path, sizeof(proj_dir2)-1);
        char *sl2 = strrchr(proj_dir2, '/');
        if (sl2) *sl2 = '\0'; else strncpy(proj_dir2, ".", sizeof(proj_dir2)-1);
        char toml_path2[600];
        snprintf(toml_path2, sizeof(toml_path2), "%s/fluxa.toml", proj_dir2);
        FluxaConfig cfg2 = fluxa_config_load(toml_path2);
        ipc->handshake_timeout_ms = cfg2.security.handshake_timeout_ms;
        ipc->ipc_max_conns        = cfg2.security.ipc_max_conns;
        if (cfg2.security.handshake_timeout_ms != 50 ||
            cfg2.security.ipc_max_conns != 16) {
            fprintf(stderr,
                "[fluxa] -prod: ipc config: timeout=%dms max_conns=%d\n",
                ipc->handshake_timeout_ms, ipc->ipc_max_conns);
        }
    }
#endif

    ASTNode *program = parse_file(path, &ast_pool);
    if (!program) {
        ipc_server_stop(ipc);
        ipc_rtview_destroy(ipc_view);
        return 1;
    }

    runtime_set_ipc_view(ipc_view);
    int result = runtime_exec(program);
    runtime_set_ipc_view(NULL);

    pool_free(&ast_pool);
    ipc_server_stop(ipc);
    ipc_rtview_destroy(ipc_view);
    return result;
}
static int run_preflight(const char *path) {
    fprintf(stderr, "[fluxa] preflight: validating %s\n", path);
    static ASTPool pool;
    ASTNode *program = parse_file(path, &pool);
    if (!program) {
        fprintf(stderr, "[fluxa] preflight: FAIL — parse error\n");
        pool_free(&pool);
        return 1;
    }
    int slots = resolver_run(program);
    pool_free(&pool);
    if (slots < 0) {
        fprintf(stderr, "[fluxa] preflight: FAIL — resolve error\n");
        return 1;
    }
    fprintf(stderr, "[fluxa] preflight: OK (slots=%d)\n", slots);
    return 0;
}

/* Apply: one-shot reload of an existing project, preserving prst state.
 * Flags:
 *   preflight=1  — validate before applying; prompt operator
 *   force=1      — apply even if preflight warns (prod only, no prompt)
 */
/* Forward declaration — run_apply_flags is defined below run_preflight */
static int run_apply_flags(const char *path, int preflight, int force);

static int run_apply_flags(const char *path, int preflight, int force) {
    if (preflight) {
        int pf = run_preflight(path);
        if (pf != 0) {
            if (force) {
                fprintf(stderr,
                    "[fluxa] apply: preflight failed — --force override, proceeding\n");
            } else {
                fprintf(stderr,
                    "[fluxa] apply: preflight failed — apply aborted\n"
                    "               use --force to override (prod only)\n");
                return 1;
            }
        }
    }
    fprintf(stderr, "[fluxa] apply: reloading %s with prst preservation\n", path);
    static ASTPool pool;
    ASTNode *program = parse_file(path, &pool);
    if (!program) return 1;
    int result = runtime_apply(program, NULL);
    pool_free(&pool);
    return result;
}


/* ── Sprint 9: IPC client helpers ────────────────────────────────────────── */

/* Connect to a running runtime.  Discovers PID from /tmp/fluxa-*.lock.
 * Prints a user-friendly error and returns -1 if no runtime is found. */
static int ipc_connect_auto(IpcClient *cli) {
    int pid = ipc_discover_pid();
    if (pid <= 0) {
        fprintf(stderr,
            "[fluxa] no running runtime found\n"
            "        start one with: fluxa run <file.flx> -dev\n"
            "                     or: fluxa run <file.flx> -prod\n");
        return -1;
    }
    if (ipc_client_connect(cli, pid) < 0) {
        fprintf(stderr, "[fluxa] cannot connect to runtime (pid %d)\n", pid);
        return -1;
    }
    return pid;
}

/* fluxa observe <var>
 * Watch a prst variable — polls every 500ms until Ctrl-C. */
static int run_observe(const char *varname) {
    IpcClient cli;
    if (ipc_connect_auto(&cli) < 0) return 1;

    fprintf(stderr, "[fluxa] observing '%s' (Ctrl-C to stop)\n", varname);

    uint32_t seq = 1;
    char last_val[IPC_LOG_LINE_MAX] = "";

    /* Install SIGINT handler so we can print a final newline */
    signal(SIGINT, SIG_DFL);

    while (1) {
        IpcRequest  req;
        IpcResponse resp;
        ipc_req_observe(&req, seq++, varname);

        if (ipc_client_send(&cli, &req, &resp) < 0) {
            /* Runtime may have reloaded and restarted its socket — retry once */
            ipc_client_close(&cli);
            int pid = ipc_discover_pid();
            if (pid <= 0 || ipc_client_connect(&cli, pid) < 0) {
                fprintf(stderr, "\n[fluxa] runtime disconnected\n");
                break;
            }
            continue;
        }

        if (resp.status == IPC_STATUS_OK) {
            /* Only print when value changes — avoids flooding stdout */
            if (strcmp(resp.message, last_val) != 0) {
                printf("%s\n", resp.message);
                fflush(stdout);
                memcpy(last_val, resp.message, sizeof(last_val) - 1); last_val[sizeof(last_val)-1] = '\0';
            }
        } else if (resp.status == IPC_STATUS_ERR_NOTFOUND) {
            fprintf(stderr, "[fluxa] observe: variable not found: %s\n", varname);
            ipc_client_close(&cli);
            return 1;
        }

        /* 500ms poll interval */
        struct timespec ts = { 0, 500000000L };
        nanosleep(&ts, NULL);
    }

    ipc_client_close(&cli);
    return 0;
}

/* fluxa set <var> <val>
 * Mutate a prst variable in the running runtime without stopping execution.
 * Type is inferred from the value string: integer, float, or bool. */
static int run_set(const char *varname, const char *valstr) {
    IpcClient cli;
    if (ipc_connect_auto(&cli) < 0) return 1;

    IpcRequest  req;
    IpcResponse resp;

    /* Infer type from value string */
    if (strcmp(valstr, "true") == 0 || strcmp(valstr, "false") == 0) {
        ipc_req_set_bool(&req, 1, varname, strcmp(valstr, "true") == 0);
    } else {
        /* Try integer first, then float */
        char *end = NULL;
        long long ival = strtoll(valstr, &end, 10);
        if (end && *end == '\0') {
            ipc_req_set_int(&req, 1, varname, (int64_t)ival);
        } else {
            double fval = strtod(valstr, &end);
            if (end && *end == '\0') {
                ipc_req_set_float(&req, 1, varname, fval);
            } else {
                fprintf(stderr,
                    "[fluxa] set: cannot parse value '%s'\n"
                    "        supported types: int, float, bool\n", valstr);
                ipc_client_close(&cli);
                return 1;
            }
        }
    }

    if (ipc_client_send(&cli, &req, &resp) < 0) {
        fprintf(stderr, "[fluxa] set: IPC error\n");
        ipc_client_close(&cli);
        return 1;
    }

    if (resp.status == IPC_STATUS_OK) {
        printf("%s\n", resp.message);
    } else if (resp.status == IPC_STATUS_ERR_NOTFOUND) {
        fprintf(stderr, "[fluxa] set: variable not found: %s\n", varname);
        ipc_client_close(&cli);
        return 1;
    } else if (resp.status == IPC_STATUS_ERR_TYPE) {
        fprintf(stderr, "[fluxa] set: type mismatch — %s\n", resp.message);
        ipc_client_close(&cli);
        return 1;
    } else {
        fprintf(stderr, "[fluxa] set: error %d — %s\n", resp.status, resp.message);
        ipc_client_close(&cli);
        return 1;
    }

    ipc_client_close(&cli);
    return 0;
}

/* fluxa logs
 * Print all entries from the runtime err_stack (most recent first). */
static int run_logs(void) {
    IpcClient cli;
    int pid = ipc_connect_auto(&cli);
    if (pid < 0) return 1;

    fprintf(stderr, "[fluxa] logs from runtime pid=%d\n", pid);

    IpcRequest  req;
    IpcResponse resp;
    ipc_req_logs(&req, 1);

    if (ipc_client_send(&cli, &req, &resp) < 0) {
        fprintf(stderr, "[fluxa] logs: IPC error\n");
        ipc_client_close(&cli);
        return 1;
    }

    if (resp.status == IPC_STATUS_OK) {
        if (resp.err_count == 0) {
            printf("(no errors)\n");
        } else {
            printf("[%d error(s) in stack]\n", resp.err_count);
            printf("%s\n", resp.message);
        }
    } else {
        fprintf(stderr, "[fluxa] logs: error %d\n", resp.status);
    }

    ipc_client_close(&cli);
    return 0;
}

/* fluxa status
 * Print a health snapshot of the running runtime. */
static int run_status(void) {
    IpcClient cli;
    int pid = ipc_connect_auto(&cli);
    if (pid < 0) return 1;

    IpcRequest  req;
    IpcResponse resp;
    ipc_req_status(&req, 1);

    if (ipc_client_send(&cli, &req, &resp) < 0) {
        fprintf(stderr, "[fluxa] status: IPC error\n");
        ipc_client_close(&cli);
        return 1;
    }

    if (resp.status == IPC_STATUS_OK) {
        printf("pid      : %d\n", pid);
        printf("mode     : %s\n", resp.mode == 1 ? "project" : "script");
        printf("cycle    : %d\n", resp.cycle_count);
        printf("prst     : %d vars\n", resp.prst_count);
        printf("errors   : %d\n", resp.err_count);
        printf("dry_run  : %s\n", resp.dry_run ? "yes" : "no");
    } else {
        fprintf(stderr, "[fluxa] status: error %d\n", resp.status);
    }

    ipc_client_close(&cli);
    return 0;
}

/* fluxa init [dir]
 * Scaffold a new Fluxa project: creates dir/main.flx + dir/fluxa.toml */
/* ── fluxa init helpers ─────────────────────────────────────────────────── */
static int fluxa_mkdir(const char *path) {
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "[fluxa] init: cannot create directory: %s\n", path);
        return 1;
    }
    return 0;
}

static int fluxa_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[fluxa] init: cannot create %s\n", path);
        return 1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

static int run_init(const char *name) {
    if (!name || !*name) {
        fprintf(stderr,
            "[fluxa] init: project name required.\n"
            "  Usage: fluxa init <project_name>\n");
        return 1;
    }
    /* Allow full paths: "path/to/myproject" — use last component as name */
    const char *proj_name = name;
    const char *last_slash = strrchr(name, '/');
    if (last_slash) proj_name = last_slash + 1;
    if (!proj_name || !*proj_name) {
        fprintf(stderr, "[fluxa] init: invalid path '%s'\n", name);
        return 1;
    }

    struct stat st;
    if (stat(name, &st) == 0) {
        fprintf(stderr,
            "[fluxa] init: directory '%s' already exists.\n"
            "  Remove it or choose a different name.\n", name);
        return 1;
    }

    char path[512];

    /* Directory structure */
    if (fluxa_mkdir(name)) return 1;
    snprintf(path, sizeof path, "%s/live",   name); if (fluxa_mkdir(path)) return 1;
    snprintf(path, sizeof path, "%s/static", name); if (fluxa_mkdir(path)) return 1;
    snprintf(path, sizeof path, "%s/tests",  name); if (fluxa_mkdir(path)) return 1;

    /* main.flx */
    {
        char content[512];
        snprintf(content, sizeof content,
            "// %s — entry point\n"
            "//\n"
            "// Run:         fluxa run main.flx\n"
            "// Dev reload:  fluxa run main.flx -dev\n"
            "// Production:  fluxa run main.flx -prod\n"
            "\n"
            "prst int counter = 0\n"
            "\n"
            "counter = counter + 1\n"
            "print(counter)\n", proj_name);
        snprintf(path, sizeof path, "%s/main.flx", name);
        if (fluxa_write_file(path, content)) return 1;
    }

    /* fluxa.toml */
    {
        char content[2048];
        snprintf(content, sizeof content,
            "# fluxa.toml — project configuration\n"
            "# Generated by: fluxa init %s\n"
            "\n"
            "[project]\n"
            "name  = \"%s\"\n"
            "entry = \"main.flx\"\n"
            "\n"
            "[runtime]\n"
            "gc_cap         = 1024   # GC table initial capacity\n"
            "prst_cap       = 64     # PrstPool initial capacity (grows via realloc)\n"
            "prst_graph_cap = 256    # PrstGraph initial capacity (grows via realloc)\n"
            "warm_func_cap  = 32     # WarmProfile hash table size (power of 2, max 256)\n"
            "\n"
            "[libs]\n"
            "# Declare libs your program uses. Uncomment to enable.\n"
            "# Libs must also be set to true in fluxa.libs to enter the binary.\n"
            "#\n"
            "# std.math      = \"1.0\"\n"
            "# std.csv       = \"1.0\"\n"
            "# std.json      = \"1.0\"\n"
            "# std.strings   = \"1.0\"\n"
            "# std.time      = \"1.0\"\n"
            "# std.flxthread = \"1.0\"\n"
            "# std.pid       = \"1.0\"\n"
            "# std.crypto    = \"1.0\"\n"
            "# std.sqlite    = \"1.0\"\n"
            "# std.serial    = \"1.0\"\n"
            "# std.i2c       = \"1.0\"\n"
            "# std.http      = \"1.0\"\n"
            "# std.mqtt      = \"1.0\"\n"
            "# std.mcp       = \"1.0\"\n", name, proj_name);
        snprintf(path, sizeof path, "%s/fluxa.toml", name);
        if (fluxa_write_file(path, content)) return 1;
    }

    /* fluxa.libs */
    {
        char content[1536];
        snprintf(content, sizeof content,
            "# fluxa.libs — build-time library configuration\n"
            "# Generated by: fluxa init %s\n"
            "#\n"
            "# Controls which libs are compiled into the Fluxa binary.\n"
            "# false = excluded from binary (zero size, zero overhead).\n"
            "# After changing this file run: make build\n"
            "\n"
            "[libs.build]\n"
            "std.math      = true    # no external deps\n"
            "std.csv       = true    # no external deps\n"
            "std.json      = true    # no external deps\n"
            "std.strings   = true    # no external deps\n"
            "std.time      = true    # POSIX\n"
            "std.flxthread = true    # pthreads\n"
            "std.pid       = true    # no external deps\n"
            "std.crypto    = false   # requires: libsodium-dev\n"
            "std.sqlite    = false   # requires: libsqlite3-dev\n"
            "std.serial    = false   # requires: libserialport-dev\n"
            "std.i2c       = true    # Linux kernel header (no external lib)\n"
            "std.http      = false   # requires: libcurl\n"
            "std.mqtt      = false   # requires: libmosquitto\n"
            "std.mcp       = false   # requires: libcurl (via std.http)\n", proj_name);
        snprintf(path, sizeof path, "%s/fluxa.libs", name);
        if (fluxa_write_file(path, content)) return 1;
    }

    /* .gitkeep files */
    snprintf(path, sizeof path, "%s/live/.gitkeep",   name);
    fluxa_write_file(path, "");
    snprintf(path, sizeof path, "%s/static/.gitkeep", name);
    fluxa_write_file(path, "");
    snprintf(path, sizeof path, "%s/tests/.gitkeep",  name);
    fluxa_write_file(path, "");

    fprintf(stdout,
        "[fluxa] init: project '%s' created\n"
        "\n"
        "  %s/\n"
        "  ├── main.flx          entry point\n"
        "  ├── fluxa.toml        project config\n"
        "  ├── fluxa.libs        build-time lib selection\n"
        "  ├── live/             reloadable modules (good place for prst state)\n"
        "  ├── static/           stable modules\n"
        "  └── tests/            project tests\n"
        "\n"
        "  cd %s && fluxa run main.flx\n",
        name, name, name);

    return 0;
}

/* ── test-reload: internal dev tool, not in usage() ─────────────────────── */
/* Simulates 3 successive -dev reloads with a shared pool and cancel_flag=1.
 * Usage: fluxa test-reload (no file arg needed) */
static int run_test_reload(void) {
    static const char *srcs[3] = {
        "prst int number = 12\nbool key = true\nwhile key == true {\n    print(number)\n}\n",
        "prst int number = 99\nbool key = true\nwhile key == true {\n    print(number)\n}\n",
        "prst int number = 7\nbool key = true\nwhile key == true {\n    print(number)\n}\n",
    };
    static const char *labels[3] = {
        "run1 (first, pool empty, cancel immediately)",
        "run2 (reload, src changed 12->99)",
        "run3 (reload, src changed 99->7)",
    };
    static int expected[3] = { 12, 99, 7 };

    static ASTPool ap;
    PrstPool pool;
    pool.entries = NULL; pool.count = 0; pool.cap = 0;

    volatile int cancel = 1;   /* always cancel at first OP_JUMP */
    runtime_set_cancel_flag(&cancel);

    int all_ok = 1;
    for (int r = 0; r < 3; r++) {
        fprintf(stderr, "--- %s ---\n", labels[r]);

        FILE *f = fopen("/tmp/_fluxa_tr.flx", "w");
        fputs(srcs[r], f);
        fclose(f);

        pool_free(&ap);
        ASTNode *prog = parse_file("/tmp/_fluxa_tr.flx", &ap);
        if (!prog) { fprintf(stderr, "FAIL: parse error\n"); return 1; }

        runtime_apply(prog, &pool);
        pool_free(&ap);

        /* Check pool has correct value */
        int found = 0;
        for (int i = 0; i < pool.count; i++) {
            if (strcmp(pool.entries[i].name, "number") == 0) {
                long long got = pool.entries[i].value.as.integer;
                if (got == expected[r]) {
                    fprintf(stderr, "  PASS number=%lld\n", got);
                } else {
                    fprintf(stderr, "  FAIL number=%lld (expected %d)\n",
                            got, expected[r]);
                    all_ok = 0;
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "  FAIL 'number' not in pool\n");
            all_ok = 0;
        }
    }

    runtime_set_cancel_flag(NULL);
    prst_pool_free(&pool);

    fprintf(stderr, "\n%s\n", all_ok ? "ALL PASS" : "SOME FAILED");
    return all_ok ? 0 : 1;
}

/* ── fluxa explain — Sprint 9.b (Issue #96) ──────────────────────────────── */
/* Mode 1 (no file): connect via IPC to running runtime — live values.
 * Mode 2 (with file): execute file and print final prst state. */
static int run_explain_live(void) {
    IpcClient cli;
    int pid = ipc_connect_auto(&cli);
    if (pid < 0) return 1;

    IpcRequest  req;
    IpcResponse resp;
    ipc_req_explain(&req, 1);

    if (ipc_send_all(cli.fd, &req, sizeof req) < 0) {
        fprintf(stderr, "[fluxa] explain: IPC send error\n");
        ipc_client_close(&cli);
        return 1;
    }

    printf("\n── prst (persist across reloads) ──────────────────────────────────\n");

    int var_count = 0;
    int done = 0;

    while (!done) {
        /* Each var packet has its own timeout — allow up to 2s total */
        if (ipc_recv_timed(cli.fd, &resp, sizeof resp) < 0) {
            if (var_count == 0)
                fprintf(stderr, "[fluxa] explain: IPC recv timeout\n");
            break;
        }
        if (resp.magic != IPC_MAGIC) {
            fprintf(stderr, "[fluxa] explain: bad magic in response\n");
            ipc_client_close(&cli);
            return 1;
        }

        if (resp.status == IPC_STATUS_EXPLAIN_VAR) {
            /* resp.name has the var name; resp.message has "name  type  = val"
             * Re-format with alignment using the raw fields for precision */
            switch (resp.type_tag) {
                case IPC_TYPE_INT:
                    printf("  %-20s int   = %ld\n",  resp.name, (long)resp.i_val);
                    break;
                case IPC_TYPE_FLOAT:
                    printf("  %-20s float = %g\n",   resp.name, resp.f_val);
                    break;
                case IPC_TYPE_BOOL:
                    printf("  %-20s bool  = %s\n",   resp.name,
                           resp.b_val ? "true" : "false");
                    break;
                default:
                    /* str or nil — message already formatted */
                    printf("  %s\n", resp.message);
                    break;
            }
            var_count++;

        } else if (resp.status == IPC_STATUS_EXPLAIN_DONE) {
            if (var_count == 0)
                printf("  (none)\n");

            printf("\n── Registered dependencies ─────────────────────────────────────────\n");
            printf("  %s\n", resp.message);

            printf("\n── Runtime ─────────────────────────────────────────────────────────\n");
            printf("  pid      : %d\n",  pid);
            printf("  mode     : %s\n",  resp.mode == 1 ? "project" : "script");
            printf("  cycle    : %d\n",  resp.cycle_count);
            printf("  prst     : %d vars\n", resp.prst_count);
            printf("  errors   : %d\n",  resp.err_count);
            printf("  dry_run  : %s\n",  resp.dry_run ? "yes" : "no");
            printf("\n");
            done = 1;

        } else {
            fprintf(stderr, "[fluxa] explain: server error %d — %s\n",
                    resp.status, resp.message);
            ipc_client_close(&cli);
            return 1;
        }
    }

    ipc_client_close(&cli);
    return 0;
}
/* Full protocol de 5 steps:
 *   1. Executa old.flx normalmente (Runtime A)
 *   2. Parseia new.flx (programa candidato)
 *   3. Executa o handover: migration → Dry Run → switchover → cleanup
 *   4. On success: execute new.flx with the transferred pool (Runtime B live)
 *   5. Se falha: Runtime A continua; ERR_HANDOVER no err_stack
 */
static int run_handover(const char *old_path, const char *new_path) {
    fprintf(stderr, "[fluxa] handover: %s → %s\n", old_path, new_path);

    /* ── Step 0: run old.flx to populate runtime state ── */
    static ASTPool pool_a;
    ASTNode *prog_a = parse_file(old_path, &pool_a);
    if (!prog_a) return 1;

    /* Create and initialize Runtime A explicitly to retain access to state */
    Runtime *rt_a = (Runtime *)calloc(1, sizeof(Runtime));
    if (!rt_a) { pool_free(&pool_a); return 1; }

    int slots_a = resolver_run(prog_a);
    if (slots_a < 0) {
        fprintf(stderr, "[fluxa] handover: resolver error in old program\n");
        free(rt_a); pool_free(&pool_a); return 1;
    }

    FluxaConfig config = fluxa_config_find_and_load();
    rt_a->scope            = scope_new();
    rt_a->global_table     = NULL;
    rt_a->stack_size       = 0;
    rt_a->had_error        = 0;
    rt_a->call_depth       = 0;
    rt_a->ret.active       = 0;
    rt_a->ret.tco_active   = 0;
    rt_a->ret.tco_fn       = NULL;
    rt_a->ret.tco_args     = NULL;
    rt_a->ret.value        = val_nil();
    rt_a->current_instance = NULL;
    rt_a->danger_depth     = 0;
    rt_a->cycle_count      = 0;
    rt_a->dry_run          = 0;
    rt_a->cancel_flag      = NULL;
    rt_a->mode             = FLUXA_MODE_PROJECT;
    rt_a->config           = config;
    errstack_clear(&rt_a->err_stack);
    gc_init(&rt_a->gc, config.gc_cap);
    ffi_registry_init(&rt_a->ffi);
    /* Sprint 9.c-2: pre-load libs declared in [ffi] section of toml */
    ffi_load_from_config(&rt_a->ffi, &rt_a->err_stack, &config);
    prst_pool_init(&rt_a->prst_pool);
    if (config.prst_cap != PRST_POOL_INIT_CAP && config.prst_cap > 0) {
        PrstEntry *ne = (PrstEntry *)realloc(rt_a->prst_pool.entries,
                            sizeof(PrstEntry) * (size_t)config.prst_cap);
        if (ne) { rt_a->prst_pool.entries = ne; rt_a->prst_pool.cap = config.prst_cap; }
    }
    prst_graph_init_cap(&rt_a->prst_graph, config.prst_graph_cap);
    for (int i = 0; i < FLUXA_STACK_SIZE; i++) rt_a->stack[i].type = VAL_NIL;

    /* Executa Runtime A */
    runtime_exec_with_rt(rt_a, prog_a);
    if (rt_a->had_error) {
        fprintf(stderr, "[fluxa] handover: Runtime A execution failed\n");
        scope_free(&rt_a->scope);
        scope_table_free(&rt_a->global_table);
        block_registry_free();
        gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        free(rt_a);
        pool_free(&pool_a);
        return 1;
    }

    fprintf(stderr, "[fluxa] handover: Runtime A OK (prst=%d, deps=%d)\n",
            rt_a->prst_pool.count, rt_a->prst_graph.count);

    /* Clean A scope (keep only pool and graph for handover) */
    scope_free(&rt_a->scope);
    rt_a->scope = scope_new();
    scope_table_free(&rt_a->global_table);
    rt_a->global_table = NULL;
    block_registry_free();
    gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
    ffi_registry_free(&rt_a->ffi);
    gc_init(&rt_a->gc, config.gc_cap);
    ffi_registry_init(&rt_a->ffi);

    /* ── Parseia new.flx (programa candidato B) ── */
    static ASTPool pool_b;
    ASTNode *prog_b = parse_file(new_path, &pool_b);
    if (!prog_b) {
        scope_free(&rt_a->scope);
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
        free(rt_a);
        pool_free(&pool_a);
        return 1;
    }

    /* ── Executa o protocolo de handover ── */
    HandoverCtx ctx;
    handover_ctx_init(&ctx, rt_a, HANDOVER_MODE_MEMORY);

    HandoverResult r = handover_execute(&ctx, prog_b, &pool_b);

    if (r != HANDOVER_OK) {
        fprintf(stderr, "[fluxa] handover FAILED at %s: %s\n",
                handover_state_str(ctx.state), ctx.error_msg);
        /* Runtime A remains intact — continue execution of A */
        fprintf(stderr, "[fluxa] handover: Runtime A maintains control\n");
        scope_free(&rt_a->scope);
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
        free(rt_a);
        pool_free(&pool_a);
        pool_free(&pool_b);
        return 1;
    }

    fprintf(stderr, "[fluxa] handover: committed — starting Runtime B\n");

    /* ── Final step: run new.flx with transferred pool ── */
    int result = runtime_apply(prog_b, &ctx.pool_after);

    /* Cleanup */
    prst_pool_free(&ctx.pool_after);
    scope_free(&rt_a->scope);
    prst_pool_free(&rt_a->prst_pool);
    prst_graph_free(&rt_a->prst_graph);
    ffi_registry_free(&rt_a->ffi);
    gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
    free(rt_a);
    pool_free(&pool_a);
    pool_free(&pool_b);
    return result;
}

/* ── test-handover: suite interna PASS/FAIL ──────────────────────────────── */
/* Validates all 5 protocol steps with inline programs.
 * Tests: serialize→deserialize, checksum, Dry Run (ok and fail),
 * prst transfer, rollback on failure. */
static int run_test_handover(void) {
    int all_ok = 1;
    fprintf(stderr, "── Fluxa Handover Test Suite ──────────────────────────────\n");

    /* ── Teste 1: serialize → deserialize → checksum ── */
    fprintf(stderr, "  [1] serialize/deserialize/checksum... ");
    {
        /* Build a minimal Runtime A with populated pool */
        Runtime *rt_a = (Runtime *)calloc(1, sizeof(Runtime));
        prst_pool_init(&rt_a->prst_pool);
        prst_graph_init(&rt_a->prst_graph);
        errstack_clear(&rt_a->err_stack);
        FluxaConfig cfg = fluxa_config_defaults();
        gc_init(&rt_a->gc, cfg.gc_cap);
        ffi_registry_init(&rt_a->ffi);

        /* Populate pool with 3 entries */
        prst_pool_set(&rt_a->prst_pool, "score",   val_int(100),  NULL);
        prst_pool_set(&rt_a->prst_pool, "running", val_bool(1),   NULL);
        prst_pool_set(&rt_a->prst_pool, "rate",    val_float(1.5),NULL);
        prst_graph_record(&rt_a->prst_graph, "score",   "show_score");
        prst_graph_record(&rt_a->prst_graph, "running", "<global>");

        HandoverCtx ctx;
        handover_ctx_init(&ctx, rt_a, HANDOVER_MODE_MEMORY);
        ctx.rt_b = (Runtime *)calloc(1, sizeof(Runtime));

        int ok = 1;
        if (handover_serialize_state(&ctx) != HANDOVER_OK)   { ok = 0; }
        if (handover_deserialize_state(&ctx) != HANDOVER_OK) { ok = 0; }

        /* Validate that values were preserved */
        Value v;
        if (ok && prst_pool_get(&ctx.rt_b->prst_pool, "score", &v)) {
            if (v.type != VAL_INT || v.as.integer != 100) ok = 0;
        } else { ok = 0; }
        if (ok && prst_pool_get(&ctx.rt_b->prst_pool, "rate", &v)) {
            if (v.type != VAL_FLOAT) ok = 0;
        } else { ok = 0; }

        /* Checksums must match */
        uint32_t cs_a = prst_pool_checksum(&rt_a->prst_pool);
        uint32_t cs_b = prst_pool_checksum(&ctx.rt_b->prst_pool);
        if (cs_a != cs_b) ok = 0;

        if (ok) fprintf(stderr, "PASS\n");
        else  { fprintf(stderr, "FAIL\n"); all_ok = 0; }

        /* Cleanup */
        if (ctx.snapshot)  free(ctx.snapshot);
        if (ctx.rt_b) {
            prst_pool_free(&ctx.rt_b->prst_pool);
            prst_graph_free(&ctx.rt_b->prst_graph);
            free(ctx.rt_b);
        }
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        gc_collect_all(&rt_a->gc, gc_dyn_free_fn);
        free(rt_a);
    }

    /* ── Teste 2: Dry Run with valid program ── */
    fprintf(stderr, "  [2] Dry Run (valid program)... ");
    {
        static const char *src_ok =
            "prst int x = 42\nprint(x)\n";
        static ASTPool ap2;
        FILE *f2 = fopen("/tmp/_fluxa_ho2.flx", "w");
        fputs(src_ok, f2); fclose(f2);
        ASTNode *prog2 = parse_file("/tmp/_fluxa_ho2.flx", &ap2);

        Runtime *rt_a2 = (Runtime *)calloc(1, sizeof(Runtime));
        prst_pool_init(&rt_a2->prst_pool);
        prst_graph_init(&rt_a2->prst_graph);
        errstack_clear(&rt_a2->err_stack);
        FluxaConfig cfg2 = fluxa_config_defaults();
        gc_init(&rt_a2->gc, cfg2.gc_cap);
        ffi_registry_init(&rt_a2->ffi);
        prst_pool_set(&rt_a2->prst_pool, "x", val_int(42), NULL);
        rt_a2->config = cfg2;

        HandoverCtx ctx2;
        handover_ctx_init(&ctx2, rt_a2, HANDOVER_MODE_MEMORY);

        int ok2 = 1;
        if (prog2) {
            HandoverResult r1 = handover_step1_standby(&ctx2, prog2, &ap2);
            HandoverResult r2 = (r1 == HANDOVER_OK) ? handover_step2_migrate(&ctx2)  : r1;
            HandoverResult r3 = (r2 == HANDOVER_OK) ? handover_step3_dry_run(&ctx2)  : r2;
            if (r3 != HANDOVER_OK) ok2 = 0;
        } else ok2 = 0;

        if (ok2) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }

        /* Cleanup */
        if (ctx2.snapshot) free(ctx2.snapshot);
        if (ctx2.rt_b) {
            prst_pool_free(&ctx2.rt_b->prst_pool);
            prst_graph_free(&ctx2.rt_b->prst_graph);
            free(ctx2.rt_b);
        }
        prst_pool_free(&rt_a2->prst_pool);
        prst_graph_free(&rt_a2->prst_graph);
        ffi_registry_free(&rt_a2->ffi);
        gc_collect_all(&rt_a2->gc, gc_dyn_free_fn);
        free(rt_a2);
        pool_free(&ap2);
    }

    /* ── Teste 3: Dry Run detects error → rollback ── */
    fprintf(stderr, "  [3] Dry Run (program with error → rollback)... ");
    {
        static const char *src_bad =
            "prst int y = 10\nint boom = 1 / 0\nprint(y)\n";
        static ASTPool ap3;
        FILE *f3 = fopen("/tmp/_fluxa_ho3.flx", "w");
        fputs(src_bad, f3); fclose(f3);
        ASTNode *prog3 = parse_file("/tmp/_fluxa_ho3.flx", &ap3);

        Runtime *rt_a3 = (Runtime *)calloc(1, sizeof(Runtime));
        prst_pool_init(&rt_a3->prst_pool);
        prst_graph_init(&rt_a3->prst_graph);
        errstack_clear(&rt_a3->err_stack);
        FluxaConfig cfg3 = fluxa_config_defaults();
        gc_init(&rt_a3->gc, cfg3.gc_cap);
        ffi_registry_init(&rt_a3->ffi);
        prst_pool_set(&rt_a3->prst_pool, "y", val_int(10), NULL);
        rt_a3->config = cfg3;

        HandoverCtx ctx3;
        handover_ctx_init(&ctx3, rt_a3, HANDOVER_MODE_MEMORY);

        int ok3 = 0; /* esperamos FALHA no dry_run */
        if (prog3) {
            HandoverResult r1 = handover_step1_standby(&ctx3, prog3, &ap3);
            HandoverResult r2 = (r1 == HANDOVER_OK) ? handover_step2_migrate(&ctx3)  : r1;
            HandoverResult r3 = (r2 == HANDOVER_OK) ? handover_step3_dry_run(&ctx3)  : r2;
            /* r3 must be HANDOVER_ERR_DRY_RUN */
            if (r3 == HANDOVER_ERR_DRY_RUN) ok3 = 1;
            /* rt_a3 must have ERR_HANDOVER in err_stack */
            if (ok3 && rt_a3->err_stack.count == 0) ok3 = 0;
            /* pool of A must be intact */
            Value v3; prst_pool_get(&rt_a3->prst_pool, "y", &v3);
            if (ok3 && (v3.type != VAL_INT || v3.as.integer != 10)) ok3 = 0;
        }

        if (ok3) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }

        if (ctx3.snapshot) free(ctx3.snapshot);
        if (ctx3.rt_b && ctx3.state == HANDOVER_STATE_FAILED) {
            /* already aborted — rt_b was freed in ctx_abort */
        }
        prst_pool_free(&rt_a3->prst_pool);
        prst_graph_free(&rt_a3->prst_graph);
        ffi_registry_free(&rt_a3->ffi);
        gc_collect_all(&rt_a3->gc, gc_dyn_free_fn);
        free(rt_a3);
        pool_free(&ap3);
    }

    /* ── Teste 4: protocol version ── */
    fprintf(stderr, "  [4] protocol version (v1.000)... ");
    {
        int ok4 = 1;
        /* Same version: OK */
        if (handover_check_version(FLUXA_HANDOVER_VERSION) != HANDOVER_OK) ok4 = 0;
        /* Lower minor: compatible */
        if (FLUXA_HANDOVER_VERSION > 1000) {
            if (handover_check_version(FLUXA_HANDOVER_VERSION - 1) != HANDOVER_OK) ok4 = 0;
        }
        /* Different major: incompatible */
        if (handover_check_version(FLUXA_HANDOVER_VERSION + 1000u) != HANDOVER_ERR_VERSION) ok4 = 0;
        if (ok4) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }
    }

    /* ── Teste 5: prst_cap e prst_graph_cap via config ── */
    fprintf(stderr, "  [5] prst_cap / prst_graph_cap configurable... ");
    {
        int ok5 = 1;
        PrstGraph g;
        prst_graph_init_cap(&g, 4);
        if (g.cap != 4) ok5 = 0;
        /* Force growth beyond initial cap */
        for (int i = 0; i < 10; i++) {
            char name[32]; char ctx[32];
            snprintf(name, sizeof(name), "prst%d", i);
            snprintf(ctx,  sizeof(ctx),  "fn%d", i);
            prst_graph_record(&g, name, ctx);
        }
        if (g.count != 10) ok5 = 0;
        if (g.cap < 10)    ok5 = 0;
        prst_graph_free(&g);

        /* Pool com cap inicial pequeno, cresce via realloc */
        PrstPool p;
        prst_pool_init(&p);
        /* cap inicial = PRST_POOL_INIT_CAP (64), vamos simular config maior */
        PrstEntry *ne = (PrstEntry *)realloc(p.entries, sizeof(PrstEntry) * 8);
        if (ne) { p.entries = ne; p.cap = 8; }
        for (int i = 0; i < 20; i++) {
            char nm[32]; snprintf(nm, sizeof(nm), "v%d", i);
            prst_pool_set(&p, nm, val_int((long)i), NULL);
        }
        if (p.count != 20) ok5 = 0;
        prst_pool_free(&p);

        if (ok5) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }
    }

    fprintf(stderr, "──────────────────────────────────────────────────────────\n");
    fprintf(stderr, "%s\n", all_ok ? "ALL PASS" : "SOME FAILED");
    return all_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    /* Enlarge the process stack to handle deep ASTs (large Block programs,
     * heavily nested while/if). Default 8MB is insufficient for programs
     * like Dijkstra with many nested method bodies.
     * 64MB covers all practical Fluxa programs without wasting memory. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        if (rl.rlim_cur < 64 * 1024 * 1024) {
            rl.rlim_cur = 64 * 1024 * 1024;
            setrlimit(RLIMIT_STACK, &rl);
        }
    }
    if (argc >= 2 && strcmp(argv[1], "test-reload")   == 0) return run_test_reload();
    if (argc >= 2 && strcmp(argv[1], "test-handover") == 0) return run_test_handover();

    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* ── Commands with no mandatory file argument ── */

    if (strcmp(cmd, "logs")   == 0) return run_logs();
    if (strcmp(cmd, "status") == 0) return run_status();

    if (strcmp(cmd, "keygen") == 0) {
#ifdef FLUXA_SECURE
        const char *dir = ".";
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        if (sodium_init() < 0) {
            fprintf(stderr, "[fluxa] keygen: libsodium not available\n");
            return 1;
        }
        int r = 0;
        r |= fluxa_keygen_ed25519(dir);
        r |= fluxa_keygen_hmac(dir);
        return r;
#else
        fprintf(stderr,
            "[fluxa] keygen: only available in FLUXA_SECURE builds.\n"
            "  Build with: make build-secure\n");
        return 1;
#endif
    }

    if (strcmp(cmd, "dis") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: fluxa dis <file.flx> [-o output.txt]\n");
            return 1;
        }
        const char *inpath  = argv[2];
        const char *outpath = NULL;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        return fluxa_dis_file(inpath, outpath);
    }

    if (strcmp(cmd, "init") == 0) {
        const char *name = (argc >= 3) ? argv[2] : NULL;
        return run_init(name);
    }

    if (strcmp(cmd, "observe") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: fluxa observe <var>\n"); return 1; }
        return run_observe(argv[2]);
    }

    if (strcmp(cmd, "set") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: fluxa set <var> <val>\n"); return 1; }
        return run_set(argv[2], argv[3]);
    }

    /* ── Commands that require a file argument ── */
    if (argc < 3) {
        /* Special case: explain without a file → live IPC mode */
        if (strcmp(cmd, "explain") == 0) return run_explain_live();
        usage();
        return 1;
    }

    const char *file = argv[2];

    /* fluxa explain [file] — with file: execute once and print state;
     *                        without file (handled above): live IPC mode */
    if (strcmp(cmd, "explain") == 0) return run_once(file, 1);

    /* fluxa apply <file> [-p] [--force] */
    if (strcmp(cmd, "apply") == 0) {
        int preflight = 0, force = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-p")      == 0) preflight = 1;
            if (strcmp(argv[i], "--force") == 0) force     = 1;
        }
        if (force && !preflight) {
            fprintf(stderr,
                "[fluxa] --force requires -p: fluxa apply <file> -p --force\n");
            return 1;
        }
        return run_apply_flags(file, preflight, force);
    }

    /* fluxa handover <old.flx> <new.flx> */
    if (strcmp(cmd, "handover") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: fluxa handover <old.flx> <new.flx>\n");
            return 1;
        }
        return run_handover(file, argv[3]);
    }

    /* fluxa run <file> [-dev] [-dev -p] [-prod] [-proj <dir>] */
    if (strcmp(cmd, "run") == 0) {
        int dev_mode = 0, prod_mode = 0, preflight = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-dev")  == 0) dev_mode  = 1;
            if (strcmp(argv[i], "-p")    == 0) preflight = 1;
            if (strcmp(argv[i], "-prod") == 0) prod_mode = 1;
            if (strcmp(argv[i], "-proj") == 0 && i + 1 < argc) {
                /* chdir to project dir so fluxa.toml is found automatically */
                if (chdir(argv[i + 1]) != 0)
                    fprintf(stderr, "[fluxa] warning: cannot chdir to '%s'\n",
                            argv[i + 1]);
                i++;
            }
        }
        if (dev_mode) return run_dev(file);
        if (prod_mode) return run_prod(file);
        if (preflight && run_preflight(file) != 0) return 1;
        return run_once(file, 0);
    }

    /* ── fluxa ffi <subcommand> ──────────────────────────────────────────── */
    if (strcmp(cmd, "ffi") == 0) {
        /* argc >= 3, file = argv[2] = subcommand */
        const char *sub = file; /* argv[2] */
        if (strcmp(sub, "list") == 0) {
            ffi_cli_list();
            return 0;
        }
        if (strcmp(sub, "inspect") == 0) {
            if (argc < 4) {
                fprintf(stderr, "usage: fluxa ffi inspect <lib>\n");
                return 1;
            }
            ffi_cli_inspect(argv[3]);
            return 0;
        }
        fprintf(stderr, "[fluxa] unknown ffi subcommand: %s\n", sub);
        fprintf(stderr, "  fluxa ffi list\n");
        fprintf(stderr, "  fluxa ffi inspect <lib>\n");
        return 1;
    }

    /* ── fluxa update — reload [ffi] and [libs] from fluxa.toml ─────────── */
    if (strcmp(cmd, "update") == 0) {
        /* Connect to running runtime via IPC and send reload-config signal.
         * If no runtime is running: load toml locally and print what changed. */
        FluxaConfig new_cfg = fluxa_config_find_and_load();
        FFIRegistry tmp_r;
        ErrStack    tmp_err;
        ffi_registry_init(&tmp_r);
        errstack_clear(&tmp_err);
        ffi_reload_from_config(&tmp_r, &tmp_err, &new_cfg);
        if (tmp_err.count > 0) {
            for (int i = 0; i < tmp_err.count; i++) {
                const ErrEntry *e = errstack_get(&tmp_err, i);
                if (e) fprintf(stderr, "[fluxa] update error: %s\n", e->message);
            }
        } else {
            fprintf(stderr,
                "[fluxa] update: [ffi] reloaded (%d lib(s))\n", tmp_r.count);
        }
        ffi_registry_free(&tmp_r);
        /* TODO Sprint 9.c: send IPC_OP_RELOAD_CONFIG to live runtime */
        return (tmp_err.count > 0) ? 1 : 0;
    }

    /* ── fluxa runtime info ──────────────────────────────────────────────── */
    if (strcmp(cmd, "runtime") == 0 && strcmp(file, "info") == 0) {
        FluxaConfig cfg = fluxa_config_find_and_load();
        printf("Fluxa Runtime\n");
        printf("─────────────────────────────────────────\n");
        printf("[runtime]\n");
        printf("  gc_cap         : %d\n",  cfg.gc_cap);
        printf("  prst_cap       : %d\n",  cfg.prst_cap);
        printf("  prst_graph_cap : %d\n",  cfg.prst_graph_cap);
        printf("\n[ffi] declared in toml\n");
        if (cfg.ffi_count == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < cfg.ffi_count; i++) {
                printf("  %-16s  %s  (%d sig)\n",
                    cfg.ffi[i].alias,
                    cfg.ffi[i].path,
                    cfg.ffi[i].sig_count);
            }
        }
        return 0;
    }

    fprintf(stderr, "[fluxa] unknown command: %s\n", cmd);
    usage();
    return 1;
}
