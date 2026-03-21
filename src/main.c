/* main.c — Fluxa CLI entry point
 * Usage: fluxa run <file.flx>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "runtime.h"

/* ── File loading ────────────────────────────────────────────────────────── */
static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[fluxa] cannot open file: %s\n", path); return NULL; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = (char*)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* ── CLI ─────────────────────────────────────────────────────────────────── */
static void usage(void) {
    fprintf(stderr, "usage: fluxa run <file.flx>\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }
    if (strcmp(argv[1], "run") != 0) {
        fprintf(stderr, "[fluxa] unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }

    const char *path = argv[2];
    char *source = load_file(path);
    if (!source) return 1;

    /* Lex → Parse → Run */
    Parser   parser  = parser_new(source);
    ASTNode *program = parser_parse(&parser);
    free(source);

    if (!program) {
        fprintf(stderr, "[fluxa] aborting due to parse errors.\n");
        parser_free(&parser);
        return 1;
    }

    int result = runtime_exec(program);

    ast_free(program);
    parser_free(&parser);
    return result;
}
