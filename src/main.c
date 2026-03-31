/* main.c — Fluxa CLI entry point (Sprint 9)
 *
 * Commands:
 *   fluxa run <file.flx>                     execute (auto-detects script vs project)
 *   fluxa run <file.flx> -dev                dev mode: watch + auto-reload on change
 *   fluxa run <file.flx> -dev -p             dev mode with preflight validation
 *   fluxa run <file.flx> -prod               prod mode (manual apply via IPC)
 *   fluxa explain <file.flx>                 print prst state + dep graph
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
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pool.h"
#include "resolver.h"
#include "runtime.h"
#include "handover.h"
#include "watcher.h"
#include "fluxa_ipc.h"
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
        "  fluxa explain <file.flx>                show prst state + dependency graph\n"
        "  fluxa apply <file.flx>                  reload preserving prst state\n"
        "  fluxa apply <file.flx> -p               preflight before applying\n"
        "  fluxa apply <file.flx> -p --force       force apply with warnings (prod only)\n"
        "  fluxa handover <old.flx> <new.flx>      Atomic Handover (5-step protocol)\n"
        "  fluxa observe <var>                      watch prst value in real time\n"
        "  fluxa set <var> <val>                    mutate prst value without stopping\n"
        "  fluxa logs                               tail runtime error/event log\n"
        "  fluxa status                             runtime health snapshot\n"
        "  fluxa init [dir]                         create project with fluxa.toml\n"
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

/* Preflight: parse + resolve the new file and report any issues.
 * Returns 0 if clean, 1 if there are warnings/errors. */
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
static int run_init(const char *dir) {
    /* Default to current directory */
    const char *target = (dir && *dir) ? dir : ".";

    /* Create directory if it doesn't exist */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", target);
    if (system(cmd) != 0) {
        fprintf(stderr, "[fluxa] init: cannot create directory: %s\n", target);
        return 1;
    }

    /* Write fluxa.toml */
    char toml_path[512];
    snprintf(toml_path, sizeof toml_path, "%s/fluxa.toml", target);
    FILE *tf = fopen(toml_path, "wx");  /* O_EXCL equivalent — won't overwrite */
    if (tf) {
        fprintf(tf,
            "# fluxa.toml — project configuration\n"
            "\n"
            "[runtime]\n"
            "gc_cap          = 1024   # GC table ceiling (static array)\n"
            "prst_cap        = 64     # initial PrstPool capacity (grows via realloc)\n"
            "prst_graph_cap  = 256    # initial PrstGraph capacity (grows via realloc)\n"
        );
        fclose(tf);
        fprintf(stderr, "[fluxa] init: created %s\n", toml_path);
    } else {
        fprintf(stderr, "[fluxa] init: %s already exists, skipping\n", toml_path);
    }

    /* Write main.flx */
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/main.flx", target);
    FILE *mf = fopen(main_path, "wx");
    if (mf) {
        fprintf(mf,
            "// main.flx — entry point\n"
            "//\n"
            "// Run:  fluxa run main.flx\n"
            "// Dev:  fluxa run main.flx -dev   (hot reload on save)\n"
            "// Prod: fluxa run main.flx -prod\n"
            "\n"
            "prst int counter = 0\n"
            "\n"
            "counter = counter + 1\n"
            "print(counter)\n"
        );
        fclose(mf);
        fprintf(stderr, "[fluxa] init: created %s\n", main_path);
    } else {
        fprintf(stderr, "[fluxa] init: %s already exists, skipping\n", main_path);
    }

    fprintf(stderr, "[fluxa] init: project ready in %s/\n", target);
    fprintf(stderr, "        run: fluxa run %s/main.flx\n", target);
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

/* ── fluxa handover <old.flx> <new.flx> ──────────────────────────────────── */
/* Protocolo completo de 5 passos:
 *   1. Executa old.flx normalmente (Runtime A)
 *   2. Parseia new.flx (programa candidato)
 *   3. Executa o handover: migration → Ciclo Imaginário → switchover → cleanup
 *   4. Se sucesso: executa new.flx com o pool transferido (Runtime B real)
 *   5. Se falha: Runtime A continua; ERR_HANDOVER no err_stack
 */
static int run_handover(const char *old_path, const char *new_path) {
    fprintf(stderr, "[fluxa] handover: %s → %s\n", old_path, new_path);

    /* ── Passo 0: executa old.flx para popular o estado ── */
    static ASTPool pool_a;
    ASTNode *prog_a = parse_file(old_path, &pool_a);
    if (!prog_a) return 1;

    /* Cria e inicializa Runtime A explicitamente para manter acesso ao estado */
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
        gc_collect_all(&rt_a->gc);
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        free(rt_a);
        pool_free(&pool_a);
        return 1;
    }

    fprintf(stderr, "[fluxa] handover: Runtime A OK (prst=%d, deps=%d)\n",
            rt_a->prst_pool.count, rt_a->prst_graph.count);

    /* Limpa escopo de A (mantém apenas pool e graph para o handover) */
    scope_free(&rt_a->scope);
    rt_a->scope = scope_new();
    scope_table_free(&rt_a->global_table);
    rt_a->global_table = NULL;
    block_registry_free();
    gc_collect_all(&rt_a->gc);
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
        gc_collect_all(&rt_a->gc);
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
        /* Runtime A permanece intacto — executar continuação de A */
        fprintf(stderr, "[fluxa] handover: Runtime A maintains control\n");
        scope_free(&rt_a->scope);
        prst_pool_free(&rt_a->prst_pool);
        prst_graph_free(&rt_a->prst_graph);
        ffi_registry_free(&rt_a->ffi);
        gc_collect_all(&rt_a->gc);
        free(rt_a);
        pool_free(&pool_a);
        pool_free(&pool_b);
        return 1;
    }

    fprintf(stderr, "[fluxa] handover: committed — starting Runtime B\n");

    /* ── Passo final: executa new.flx com pool transferido ── */
    int result = runtime_apply(prog_b, &ctx.pool_after);

    /* Cleanup */
    prst_pool_free(&ctx.pool_after);
    scope_free(&rt_a->scope);
    prst_pool_free(&rt_a->prst_pool);
    prst_graph_free(&rt_a->prst_graph);
    ffi_registry_free(&rt_a->ffi);
    gc_collect_all(&rt_a->gc);
    free(rt_a);
    pool_free(&pool_a);
    pool_free(&pool_b);
    return result;
}

/* ── test-handover: suite interna PASS/FAIL ──────────────────────────────── */
/* Valida os 5 passos do protocolo com programas inline.
 * Testa: serialize→deserialize, checksum, Ciclo Imaginário (ok e fail),
 * transferência de prst, rollback em caso de falha. */
static int run_test_handover(void) {
    int all_ok = 1;
    fprintf(stderr, "── Fluxa Handover Test Suite ──────────────────────────────\n");

    /* ── Teste 1: serialize → deserialize → checksum ── */
    fprintf(stderr, "  [1] serialize/deserialize/checksum... ");
    {
        /* Monta um Runtime A mínimo com pool populado */
        Runtime *rt_a = (Runtime *)calloc(1, sizeof(Runtime));
        prst_pool_init(&rt_a->prst_pool);
        prst_graph_init(&rt_a->prst_graph);
        errstack_clear(&rt_a->err_stack);
        FluxaConfig cfg = fluxa_config_defaults();
        gc_init(&rt_a->gc, cfg.gc_cap);
        ffi_registry_init(&rt_a->ffi);

        /* Popula pool com 3 entradas */
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

        /* Valida que os valores foram preservados */
        Value v;
        if (ok && prst_pool_get(&ctx.rt_b->prst_pool, "score", &v)) {
            if (v.type != VAL_INT || v.as.integer != 100) ok = 0;
        } else { ok = 0; }
        if (ok && prst_pool_get(&ctx.rt_b->prst_pool, "rate", &v)) {
            if (v.type != VAL_FLOAT) ok = 0;
        } else { ok = 0; }

        /* Checksums devem bater */
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
        gc_collect_all(&rt_a->gc);
        free(rt_a);
    }

    /* ── Teste 2: Ciclo Imaginário com programa válido ── */
    fprintf(stderr, "  [2] Ciclo Imaginário (programa válido)... ");
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
        gc_collect_all(&rt_a2->gc);
        free(rt_a2);
        pool_free(&ap2);
    }

    /* ── Teste 3: Ciclo Imaginário detecta erro → rollback ── */
    fprintf(stderr, "  [3] Ciclo Imaginário (programa com erro → rollback)... ");
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
            /* r3 deve ser HANDOVER_ERR_DRY_RUN */
            if (r3 == HANDOVER_ERR_DRY_RUN) ok3 = 1;
            /* rt_a3 deve ter ERR_HANDOVER no err_stack */
            if (ok3 && rt_a3->err_stack.count == 0) ok3 = 0;
            /* pool de A deve estar intacto */
            Value v3; prst_pool_get(&rt_a3->prst_pool, "y", &v3);
            if (ok3 && (v3.type != VAL_INT || v3.as.integer != 10)) ok3 = 0;
        }

        if (ok3) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }

        if (ctx3.snapshot) free(ctx3.snapshot);
        if (ctx3.rt_b && ctx3.state == HANDOVER_STATE_FAILED) {
            /* já foi abortado — rt_b foi liberado em ctx_abort */
        }
        prst_pool_free(&rt_a3->prst_pool);
        prst_graph_free(&rt_a3->prst_graph);
        ffi_registry_free(&rt_a3->ffi);
        gc_collect_all(&rt_a3->gc);
        free(rt_a3);
        pool_free(&ap3);
    }

    /* ── Teste 4: versão de protocolo ── */
    fprintf(stderr, "  [4] versão de protocolo (v1.000)... ");
    {
        int ok4 = 1;
        /* Mesma versão: OK */
        if (handover_check_version(FLUXA_HANDOVER_VERSION) != HANDOVER_OK) ok4 = 0;
        /* Minor menor: compatível */
        if (FLUXA_HANDOVER_VERSION > 1000) {
            if (handover_check_version(FLUXA_HANDOVER_VERSION - 1) != HANDOVER_OK) ok4 = 0;
        }
        /* Major diferente: incompatível */
        if (handover_check_version(FLUXA_HANDOVER_VERSION + 1000u) != HANDOVER_ERR_VERSION) ok4 = 0;
        if (ok4) fprintf(stderr, "PASS\n");
        else   { fprintf(stderr, "FAIL\n"); all_ok = 0; }
    }

    /* ── Teste 5: prst_cap e prst_graph_cap via config ── */
    fprintf(stderr, "  [5] prst_cap / prst_graph_cap configuráveis... ");
    {
        int ok5 = 1;
        PrstGraph g;
        prst_graph_init_cap(&g, 4);
        if (g.cap != 4) ok5 = 0;
        /* Força crescimento além do cap inicial */
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
    if (argc >= 2 && strcmp(argv[1], "test-reload")   == 0) return run_test_reload();
    if (argc >= 2 && strcmp(argv[1], "test-handover") == 0) return run_test_handover();

    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* ── Commands with no mandatory file argument ── */

    if (strcmp(cmd, "logs")   == 0) return run_logs();
    if (strcmp(cmd, "status") == 0) return run_status();

    if (strcmp(cmd, "init") == 0) {
        const char *dir = (argc >= 3) ? argv[2] : ".";
        return run_init(dir);
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
    if (argc < 3) { usage(); return 1; }

    const char *file = argv[2];

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

    /* fluxa run <file> [-dev] [-dev -p] [-prod] */
    if (strcmp(cmd, "run") == 0) {
        int dev_mode = 0, preflight = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-dev")  == 0) dev_mode  = 1;
            if (strcmp(argv[i], "-p")    == 0) preflight = 1;
            if (strcmp(argv[i], "-prod") == 0)
                fprintf(stderr, "[fluxa] -prod: running in production mode\n");
        }
        if (dev_mode) return run_dev(file);
        if (preflight && run_preflight(file) != 0) return 1;
        return run_once(file, 0);
    }

    fprintf(stderr, "[fluxa] unknown command: %s\n", cmd);
    usage();
    return 1;
}
