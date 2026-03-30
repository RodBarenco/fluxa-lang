/* main.c — Fluxa CLI entry point (Sprint 7.b)
 *
 * Commands:
 *   fluxa run <file.flx>          execute (auto-detects script vs project)
 *   fluxa run <file.flx> -dev     dev mode: watch + auto-reload on change
 *   fluxa run <file.flx> -prod    prod mode (manual apply via signal/IPC — Sprint 7.c)
 *   fluxa explain <file.flx>      print prst state + dep graph after execution
 *   fluxa apply <file.flx>        one-shot reload preserving prst state
 *
 * Sprint 7.b additions:
 *   -dev:        inotify/kqueue watcher loop, re-runs on save
 *   fluxa apply: runtime_apply() preserves PrstPool across reload
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pool.h"
#include "resolver.h"
#include "runtime.h"
#include "watcher.h"
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
        "  fluxa run <file.flx>             execute (auto script vs project)\n"
        "  fluxa run <file.flx> -dev        dev mode: watch + reload on save\n"
        "  fluxa run <file.flx> -prod       prod mode (Sprint 7.c: manual apply)\n"
        "  fluxa explain <file.flx>         show prst state + dependency graph\n"
        "  fluxa apply <file.flx>           reload preserving prst state\n"
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

    DevCtx ctx;
    ctx.path      = path;
    ctx.ast_pool  = &ast_pool;
    ctx.pool      = &pool;
    ctx.first_run = 1;
    ctx.cancel    = 0;
    ctx.done      = 0;
    ctx.exit_code = 0;

    while (1) {
        ctx.cancel = 0;
        ctx.done   = 0;

        /* Launch script execution in a worker thread */
        pthread_t tid;
        if (pthread_create(&tid, NULL, dev_exec_thread, &ctx) != 0) {
            fprintf(stderr, "[fluxa] -dev: pthread_create failed\n");
            return 1;
        }

        /* Main thread: watch for file changes while script runs.
         * Poll in short intervals (200ms) so we react quickly to saves. */
        FWatcher *fw = fw_open(path);
        if (!fw) {
            fprintf(stderr, "[fluxa] -dev: cannot open watcher for %s\n", path);
            ctx.cancel = 1;
            pthread_join(tid, NULL);
            return 1;
        }

        int reload = 0;
        while (!reload) {
            int wr = fw_wait(fw, 200);
            if (wr == 1) {
                reload = 1;        /* file changed → cancel + reload */
            } else if (wr == -1) {
                fw_close(fw);
                fw = fw_open(path);
                if (!fw) break;
            }
            if (ctx.done) {
                reload = 1;        /* script finished cleanly → re-watch */
            }
        }
        fw_close(fw);

        if (!ctx.done) {
            /* Script still running — cancel it and wait */
            ctx.cancel = 1;
            pthread_join(tid, NULL);
            fprintf(stderr, "[fluxa] -dev: reload triggered\n");
        } else {
            /* Script finished on its own (e.g. non-infinite loop) */
            pthread_join(tid, NULL);
            fprintf(stderr, "[fluxa] -dev: waiting for changes...\n");
        }
    }
    return 0;
}

/* Apply: one-shot reload of an existing project, preserving prst state.
 * In a real system this would IPC into a running runtime; here we
 * simulate by running with an empty pool (first apply = same as run). */
static int run_apply(const char *path) {
    fprintf(stderr, "[fluxa] apply: reloading %s with prst preservation\n", path);
    static ASTPool pool;
    ASTNode *program = parse_file(path, &pool);
    if (!program) return 1;
    /* No prior pool — this simulates first apply */
    int result = runtime_apply(program, NULL);
    pool_free(&pool);
    return result;
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

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "test-reload") == 0)
        return run_test_reload();
    if (argc < 3) { usage(); return 1; }

    const char *cmd  = argv[1];
    const char *file = argv[2];

    if (strcmp(cmd, "explain") == 0) return run_once(file, 1);
    if (strcmp(cmd, "apply")   == 0) return run_apply(file);

    if (strcmp(cmd, "run") == 0) {
        int dev_mode = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-dev")  == 0) dev_mode = 1;
            if (strcmp(argv[i], "-prod") == 0) {
                /* prod: run once, IPC channel for apply in Sprint 7.c */
                fprintf(stderr, "[fluxa] -prod: running in production mode\n");
            }
        }
        if (dev_mode) return run_dev(file);
        return run_once(file, 0);
    }

    fprintf(stderr, "[fluxa] unknown command: %s\n", cmd);
    usage();
    return 1;
}
