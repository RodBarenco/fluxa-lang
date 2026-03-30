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
static int run_dev(const char *path) {
    fprintf(stderr, "[fluxa] -dev: watching %s (Ctrl-C to stop)\n", path);

    PrstPool pool;
    pool.entries = NULL;
    pool.count   = 0;
    pool.cap     = 0;
    int first_run = 1;

    while (1) {
        static ASTPool ast_pool;
        ASTNode *program = parse_file(path, &ast_pool);
        if (!program) {
            fprintf(stderr, "[fluxa] -dev: parse error — waiting for fix...\n");
        } else {
            fprintf(stderr, "[fluxa] -dev: running %s\n", path);
            int r;
            if (first_run) {
                r = runtime_exec(program);
                first_run = 0;
            } else {
                /* Apply: re-run preserving prst state */
                r = runtime_apply(program, &pool);
                fprintf(stderr, "[fluxa] -dev: reload done (exit=%d, cycle=prst preserved)\n", r);
            }
            pool_free(&ast_pool);
            (void)r;
        }

        /* Wait for file change */
        FWatcher *fw = fw_open(path);
        if (!fw) {
            fprintf(stderr, "[fluxa] -dev: cannot open watcher for %s\n", path);
            return 1;
        }
        fprintf(stderr, "[fluxa] -dev: waiting for changes...\n");
        int changed = 0;
        while (!changed) {
            int r = fw_wait(fw, 500);
            if (r == 1) changed = 1;
            else if (r == -1) { fw_close(fw); fw = fw_open(path); if (!fw) break; }
        }
        fw_close(fw);
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

int main(int argc, char **argv) {
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
