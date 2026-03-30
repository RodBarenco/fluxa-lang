/* toml_config.h — Minimal fluxa.toml configuration loader (Sprint 7)
 *
 * Parses only the [runtime] section of fluxa.toml.
 * No external toml library — hand-written line parser sufficient
 * for the small number of keys we need.
 *
 * Supported keys (fluxa.toml):
 *   [runtime]
 *   gc_cap = 1024     # max GC-tracked objects (default: GC_TABLE_CAP)
 *
 * Format rules (strict subset of TOML):
 *   - Lines starting with '#' are comments
 *   - Section headers: [section_name]
 *   - Key-value: key = value  (integer values only for now)
 *   - Whitespace around '=' is ignored
 *   - Unknown keys silently ignored
 *
 * Usage:
 *   FluxaConfig cfg = fluxa_config_load("fluxa.toml");
 *   gc_init(&rt.gc, cfg.gc_cap);
 */
#ifndef FLUXA_TOML_CONFIG_H
#define FLUXA_TOML_CONFIG_H

#include "gc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct {
    int gc_cap;     /* [runtime] gc_cap — default GC_TABLE_CAP */
} FluxaConfig;

static inline FluxaConfig fluxa_config_defaults(void) {
    FluxaConfig c;
    c.gc_cap = GC_TABLE_CAP;
    return c;
}

/* Trim leading and trailing whitespace in-place.
 * Returns pointer into s (no allocation). */
static inline char *cfg_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/* Load fluxa.toml from path. If file not found or unreadable,
 * returns defaults silently — fluxa.toml is optional. */
static inline FluxaConfig fluxa_config_load(const char *path) {
    FluxaConfig cfg = fluxa_config_defaults();
    if (!path) return cfg;

    FILE *f = fopen(path, "r");
    if (!f) return cfg;   /* no toml = all defaults */

    char   line[512];
    int    in_runtime = 0;

    while (fgets(line, sizeof(line), f)) {
        char *l = cfg_trim(line);
        if (!*l || *l == '#') continue;

        /* Section header */
        if (*l == '[') {
            in_runtime = (strncmp(l, "[runtime]", 9) == 0);
            continue;
        }

        if (!in_runtime) continue;

        /* key = value */
        char *eq = strchr(l, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = cfg_trim(l);
        char *val = cfg_trim(eq + 1);

        /* Strip inline comment */
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; cfg_trim(val); }

        if (strcmp(key, "gc_cap") == 0) {
            int v = atoi(val);
            if (v > 0 && v <= GC_TABLE_CAP)
                cfg.gc_cap = v;
            else if (v > GC_TABLE_CAP)
                fprintf(stderr,
                    "[fluxa] fluxa.toml: gc_cap %d exceeds compiled max %d — using %d\n",
                    v, GC_TABLE_CAP, GC_TABLE_CAP);
        }
        /* Future keys: thread_cap, prst_cap, etc. */
    }

    fclose(f);
    return cfg;
}

/* Search for fluxa.toml in current directory (simple heuristic for Sprint 7).
 * Sprint 8 (CLI) will do proper project root discovery. */
static inline FluxaConfig fluxa_config_find_and_load(void) {
    return fluxa_config_load("fluxa.toml");
}

#endif /* FLUXA_TOML_CONFIG_H */
