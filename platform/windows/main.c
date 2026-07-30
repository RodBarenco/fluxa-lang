/* Minimal Windows entrypoint for the MinGW cross-build.
 *
 * This profile intentionally supports file execution only. Native-only
 * lifecycle services (-dev, -prod, IPC, handover, update and FFI inspection)
 * remain in src/main.c and are not linked into this binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "pool.h"
#include "resolver.h"
#include "runtime.h"
#include "toml_config.h"

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    size_t nread;

    if (!f) {
        fprintf(stderr, "[fluxa] cannot open file: %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    nread = fread(buf, 1, (size_t)size, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

static ASTNode *parse_file(const char *path, ASTPool *pool,
                           const char *module_root) {
    typedef struct {
        char ns[64];
        char *src;
    } Module;

    char *main_src = load_file(path);
    Module modules[32];
    int module_count = 0;
    const char *scan;
    Parser parser;
    ASTNode *program;
    ASTNode *main_program;
    int i;

    if (!main_src) return NULL;
    pool_init(pool);

    scan = main_src;
    while (*scan && module_count < 32) {
        while (*scan == ' ' || *scan == '\t') scan++;
        if (strncmp(scan, "import", 6) == 0) {
            const char *s = scan + 6;
            int is_live;
            int is_static;

            while (*s == ' ' || *s == '\t') s++;
            is_live = strncmp(s, "live", 4) == 0 &&
                      (s[4] == ' ' || s[4] == '\t');
            is_static = strncmp(s, "static", 6) == 0 &&
                        (s[6] == ' ' || s[6] == '\t');
            if (is_live || is_static) {
                char path_buf[640];
                int ni = 0;
                const char *kind = is_live ? "live" : "static";

                s += is_live ? 4 : 6;
                while (*s == ' ' || *s == '\t') s++;
                while (*s && *s != ' ' && *s != '\t' &&
                       *s != '\n' && *s != '\r' && ni < 63)
                    modules[module_count].ns[ni++] = *s++;
                modules[module_count].ns[ni] = '\0';
                if (ni > 0) {
                    if (module_root && module_root[0])
                        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s.flx",
                                 module_root, kind,
                                 modules[module_count].ns);
                    else
                        snprintf(path_buf, sizeof(path_buf), "%s/%s.flx",
                                 kind, modules[module_count].ns);
                    modules[module_count].src = load_file(path_buf);
                    if (!modules[module_count].src) goto fail_modules;
                    module_count++;
                }
            }
        }
        while (*scan && *scan != '\n') scan++;
        if (*scan == '\n') scan++;
    }

    parser = parser_new(main_src, pool);
    program = pool_alloc_node(pool);
    if (!program) goto fail_parser;
    program->type = NODE_PROGRAM;
    program->as.list.children = NULL;
    program->as.list.count = 0;

    for (i = 0; i < module_count; i++) {
        if (parser_parse_module(&parser, program, modules[i].ns,
                                modules[i].src) != 0) {
            fprintf(stderr, "[fluxa] error in module '%s'\n", modules[i].ns);
            free(modules[i].src);
            modules[i].src = NULL;
            goto fail_parser;
        }
        free(modules[i].src);
        modules[i].src = NULL;
    }

    main_program = parser_parse(&parser);
    free(main_src);
    if (!main_program) {
        fprintf(stderr, "[fluxa] aborting due to parse errors.\n");
        parser_free(&parser);
        pool_free(pool);
        return NULL;
    }
    for (i = 0; i < main_program->as.list.count; i++)
        ast_list_push(program, main_program->as.list.children[i]);
    parser_free(&parser);
    return program;

fail_parser:
    parser_free(&parser);
fail_modules:
    for (i = 0; i < module_count; i++) free(modules[i].src);
    free(main_src);
    pool_free(pool);
    return NULL;
}

static void usage(void) {
    fprintf(stderr,
        "Fluxa Windows minimal runtime\n"
        "usage:\n"
        "  fluxa.exe run <file.flx>\n"
        "  fluxa.exe explain <file.flx>\n"
        "\n"
        "Unavailable in the minimal profile: -dev, -prod, IPC, handover,\n"
        "runtime update, native threads and C FFI.\n");
}

int main(int argc, char **argv) {
    FluxaConfig config;
    ASTPool pool;
    ASTNode *program;
    int explain;
    int result;

    if (argc != 3 ||
        (strcmp(argv[1], "run") != 0 && strcmp(argv[1], "explain") != 0)) {
        usage();
        return 1;
    }

    explain = strcmp(argv[1], "explain") == 0;
    config = fluxa_config_find_and_load();
    resolver_set_scope_cap(config.scope_cap);
    program = parse_file(argv[2], &pool,
                         config.module_root[0] ? config.module_root : NULL);
    if (!program) return 1;
    result = explain ? runtime_exec_explain(program) : runtime_exec(program);
    pool_free(&pool);
    return result;
}
