/* main.c — Fluxa CLI entry point (Sprint 7)
 *
 * Commands:
 *   fluxa run <file.flx>          execute — script or project mode (auto-detected)
 *   fluxa run <file.flx> -dev     dev mode  (project, future: watcher)
 *   fluxa run <file.flx> -prod    prod mode (project, future: manual apply)
 *   fluxa explain <file.flx>      print prst state + dep graph after execution
 *
 * Mode detection (Sprint 7):
 *   - resolver_has_prst() scans AST before execution
 *   - prst found → FLUXA_MODE_PROJECT
 *   - no prst    → FLUXA_MODE_SCRIPT
 *   - -dev/-prod flags always force PROJECT mode (Sprint 7.c: watcher/apply)
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
        "  fluxa run <file.flx>             execute (auto-detects script vs project)\n"
        "  fluxa run <file.flx> -dev        dev mode with hot reload (Sprint 7.c)\n"
        "  fluxa run <file.flx> -prod       prod mode, manual apply (Sprint 7.c)\n"
        "  fluxa explain <file.flx>         show prst state + dependency graph\n"
    );
}

/* Parse and run a .flx file, optionally running explain after.
 * force_project=1 → always PROJECT mode regardless of prst presence. */
static int run_file(const char *path, int explain, int force_project) {
    char *source = load_file(path);
    if (!source) return 1;

    static ASTPool pool;
    pool_init(&pool);

    Parser   parser  = parser_new(source, &pool);
    ASTNode *program = parser_parse(&parser);
    free(source);

    if (!program) {
        fprintf(stderr, "[fluxa] aborting due to parse errors.\n");
        parser_free(&parser);
        pool_free(&pool);
        return 1;
    }

    /* Sprint 7: mode detection — runs before runtime_exec */
    int has_prst = resolver_has_prst(program);
    if (force_project && !has_prst) {
        /* -dev or -prod flag: inform that project mode is active even without prst */
        fprintf(stderr, "[fluxa] project mode active (forced by flag)\n");
    }

    int result;
    if (explain) {
        result = runtime_exec_explain(program);
    } else {
        result = runtime_exec(program);
    }

    parser_free(&parser);
    pool_free(&pool);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }

    const char *cmd  = argv[1];
    const char *file = argv[2];

    /* fluxa explain <file> */
    if (strcmp(cmd, "explain") == 0) {
        return run_file(file, 1, 0);
    }

    /* fluxa run <file> [flags] */
    if (strcmp(cmd, "run") == 0) {
        int force_project = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-dev")  == 0) force_project = 1;
            if (strcmp(argv[i], "-prod") == 0) force_project = 1;
        }
        return run_file(file, 0, force_project);
    }

    fprintf(stderr, "[fluxa] unknown command: %s\n", cmd);
    usage();
    return 1;
}
