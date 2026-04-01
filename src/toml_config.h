/* toml_config.h — Minimal fluxa.toml configuration loader (Sprint 7 / Sprint 8)
 *
 * Supported keys (fluxa.toml):
 *   [runtime]
 *   gc_cap        = 1024   # max objetos rastreados pelo GC (default GC_TABLE_CAP)
 *   prst_cap      = 64     # cap inicial do PrstPool (default PRST_POOL_INIT_CAP)
 *   prst_graph_cap= 256    # cap inicial do PrstGraph (default PRST_GRAPH_CAP_DEFAULT)
 *
 * All caps are dynamic — grow via realloc up to the compiled maximum.
 * The toml value is the initial allocation size, not an absolute ceiling.
 *
 * Exception: gc_cap IS an absolute ceiling (GCTable uses a static array).
 */
#ifndef FLUXA_TOML_CONFIG_H
#define FLUXA_TOML_CONFIG_H

#include "gc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Defaults mirrored here to avoid double-inclusion of headers
 * com static inline (prst_pool.h / prst_graph.h).
 * The real values are defined in those headers and must stay in sync. */
#ifndef PRST_POOL_INIT_CAP
#  define PRST_POOL_INIT_CAP     64
#endif
#ifndef PRST_GRAPH_CAP_DEFAULT
#  define PRST_GRAPH_CAP_DEFAULT 256
#endif
#ifndef PRST_GRAPH_CAP_MAX
#  define PRST_GRAPH_CAP_MAX     65536
#endif

typedef struct {
    int gc_cap;          /* absolute ceiling for GC (static array)       */
    int prst_cap;        /* initial cap for PrstPool (dynamic)         */
    int prst_graph_cap;  /* initial cap for PrstGraph (dynamic)        */
} FluxaConfig;

static inline FluxaConfig fluxa_config_defaults(void) {
    FluxaConfig c;
    c.gc_cap         = GC_TABLE_CAP;
    c.prst_cap       = PRST_POOL_INIT_CAP;
    c.prst_graph_cap = PRST_GRAPH_CAP_DEFAULT;
    return c;
}

static inline char *cfg_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static inline FluxaConfig fluxa_config_load(const char *path) {
    FluxaConfig cfg = fluxa_config_defaults();
    if (!path) return cfg;

    FILE *f = fopen(path, "r");
    if (!f) return cfg;

    char line[512];
    int  in_runtime = 0;

    while (fgets(line, sizeof(line), f)) {
        char *l = cfg_trim(line);
        if (!*l || *l == '#') continue;

        if (*l == '[') {
            in_runtime = (strncmp(l, "[runtime]", 9) == 0);
            continue;
        }
        if (!in_runtime) continue;

        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = cfg_trim(l);
        char *val = cfg_trim(eq + 1);
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; cfg_trim(val); }

        int v = atoi(val);

        if (strcmp(key, "gc_cap") == 0) {
            /* gc_cap: absolute ceiling — cannot exceed compiled maximum */
            if (v > 0 && v <= GC_TABLE_CAP)
                cfg.gc_cap = v;
            else if (v > GC_TABLE_CAP)
                fprintf(stderr,
                    "[fluxa] fluxa.toml: gc_cap %d exceeds compiled maximum %d"
                    " — usando %d\n", v, GC_TABLE_CAP, GC_TABLE_CAP);
        }
        else if (strcmp(key, "prst_cap") == 0) {
            /* prst_cap: initial cap for PrstPool; dynamic, no hard ceiling */
            if (v > 0)
                cfg.prst_cap = v;
            else
                fprintf(stderr,
                    "[fluxa] fluxa.toml: prst_cap invalid (%d) — using %d\n",
                    v, PRST_POOL_INIT_CAP);
        }
        else if (strcmp(key, "prst_graph_cap") == 0) {
            /* prst_graph_cap: initial cap for PrstGraph; dynamic */
            if (v > 0 && v <= PRST_GRAPH_CAP_MAX)
                cfg.prst_graph_cap = v;
            else if (v > PRST_GRAPH_CAP_MAX)
                fprintf(stderr,
                    "[fluxa] fluxa.toml: prst_graph_cap %d exceeds maximum %d"
                    " — usando %d\n", v, PRST_GRAPH_CAP_MAX, PRST_GRAPH_CAP_MAX);
            else
                fprintf(stderr,
                    "[fluxa] fluxa.toml: prst_graph_cap invalid (%d)"
                    " — usando %d\n", v, PRST_GRAPH_CAP_DEFAULT);
        }
        /* Future keys: thread_cap, etc. */
    }

    fclose(f);
    return cfg;
}

static inline FluxaConfig fluxa_config_find_and_load(void) {
    return fluxa_config_load("fluxa.toml");
}

#endif /* FLUXA_TOML_CONFIG_H */
