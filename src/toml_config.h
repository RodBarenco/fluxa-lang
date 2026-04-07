/* toml_config.h — Minimal fluxa.toml configuration loader
 *
 * Supported sections:
 *   [runtime]        gc_cap, prst_cap, prst_graph_cap
 *   [ffi]            libname = "auto" | "/path/to/lib.so"
 *   [ffi.<lib>.signatures]
 *                    fnname = "(type, type*, ...) -> type"
 *
 * Sprint 9.c-2: added [ffi] section parsing.
 * Sprint 9.c-3: added [ffi.<lib>.signatures] parsing.
 */
#ifndef FLUXA_TOML_CONFIG_H
#define FLUXA_TOML_CONFIG_H

#include "gc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef PRST_POOL_INIT_CAP
#  define PRST_POOL_INIT_CAP     64
#endif
#ifndef PRST_GRAPH_CAP_DEFAULT
#  define PRST_GRAPH_CAP_DEFAULT 256
#endif
#ifndef PRST_GRAPH_CAP_MAX
#  define PRST_GRAPH_CAP_MAX     65536
#endif

/* ── FFI entry from toml ──────────────────────────────────────────────────── */
#define TOML_FFI_MAX       32   /* max libs in [ffi]                         */
#define TOML_SIG_MAX       64   /* max signatures per lib                    */
#define TOML_SIG_PARAM_MAX 16   /* max params per signature                  */

/* Single param descriptor — carries C type string, e.g. "int*", "char*" */
typedef struct {
    char c_type[32];   /* "int", "int*", "double*", "char*", "void*", etc. */
} FfiParamDesc;

/* One function signature from [ffi.<lib>.signatures] */
typedef struct {
    char         fn_name[128];
    char         ret_type[32];
    FfiParamDesc params[TOML_SIG_PARAM_MAX];
    int          param_count;
} FfiSigEntry;

/* One lib declared in [ffi] */
typedef struct {
    char        alias[128];          /* key in toml, e.g. "libm"          */
    char        path[256];           /* "auto" or explicit path            */
    FfiSigEntry sigs[TOML_SIG_MAX];
    int         sig_count;
} TomlFfiEntry;

/* ── Stdlib lib flags — one per opt-in lib ───────────────────────────────── */
/* Set to 1 when the lib is declared in [libs] of fluxa.toml.
 * The runtime uses these flags to register lib functions as builtins.
 * Each lib is only compiled into the binary when its FLUXA_STD_* macro
 * is defined at build time (set by the Makefile reading fluxa.toml). */
typedef struct {
    int has_math;   /* std.math — math functions (sqrt, pow, sin, ...) */
    int has_csv;    /* std.csv  — CSV parser, returns dyn               */
    int has_json;   /* std.json — JSON stringify/extract, uses str      */
} FluxaStdLibs;

/* ── Main config ──────────────────────────────────────────────────────────── */
typedef struct {
    int          gc_cap;
    int          prst_cap;
    int          prst_graph_cap;
    TomlFfiEntry ffi[TOML_FFI_MAX];
    int          ffi_count;
    FluxaStdLibs std_libs;  /* opt-in stdlib — declared in [libs] toml */
} FluxaConfig;

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline FluxaConfig fluxa_config_defaults(void) {
    FluxaConfig c;
    memset(&c, 0, sizeof(c));
    /* std_libs: all disabled by default — must be declared in [libs] */
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

/* Strip surrounding quotes from a toml string value: "auto" → auto */
static inline void cfg_unquote(const char *src, char *dst, int dst_sz) {
    int len = (int)strlen(src);
    if (len >= 2 && src[0] == '"' && src[len-1] == '"') {
        int n = len - 2;
        if (n >= dst_sz) n = dst_sz - 1;
        memcpy(dst, src + 1, (size_t)n);
        dst[n] = '\0';
    } else {
        snprintf(dst, (size_t)dst_sz, "%s", src);
    }
}

/* Parse signature string "(int, int*, char*) -> float"
 * into FfiSigEntry params + ret_type.                                       */
static inline void cfg_parse_sig(const char *sig_str, FfiSigEntry *out) {
    /* ret type: everything after " -> " */
    const char *arrow = strstr(sig_str, "->");
    if (arrow) {
        char ret[32];
        snprintf(ret, sizeof(ret), "%s", cfg_trim((char*)(arrow + 2)));
        snprintf(out->ret_type, sizeof(out->ret_type), "%s", ret);
    } else {
        snprintf(out->ret_type, sizeof(out->ret_type), "nil");
    }

    /* params: between ( and ) */
    const char *open  = strchr(sig_str, '(');
    const char *close = arrow ? arrow : strchr(sig_str, ')');
    if (!open || !close || close <= open) return;

    char inner[512];
    int inner_len = (int)(close - open - 1);
    if (inner_len <= 0 || inner_len >= (int)sizeof(inner)) return;
    memcpy(inner, open + 1, (size_t)inner_len);
    inner[inner_len] = '\0';

    /* tokenize by comma */
    char *saveptr = NULL;
    char *tok = strtok_r(inner, ",", &saveptr);
    while (tok && out->param_count < TOML_SIG_PARAM_MAX) {
        char *t = cfg_trim(tok);
        snprintf(out->params[out->param_count].c_type,
                 sizeof(out->params[out->param_count].c_type), "%s", t);
        out->param_count++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

/* Find or create a TomlFfiEntry by alias */
static inline TomlFfiEntry *cfg_ffi_find_or_create(
        FluxaConfig *cfg, const char *alias) {
    for (int i = 0; i < cfg->ffi_count; i++)
        if (strcmp(cfg->ffi[i].alias, alias) == 0)
            return &cfg->ffi[i];
    if (cfg->ffi_count >= TOML_FFI_MAX) return NULL;
    TomlFfiEntry *e = &cfg->ffi[cfg->ffi_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->alias, sizeof(e->alias), "%s", alias);
    snprintf(e->path,  sizeof(e->path),  "auto");
    return e;
}

/* ── Main parser ──────────────────────────────────────────────────────────── */
static inline FluxaConfig fluxa_config_load(const char *path) {
    FluxaConfig cfg = fluxa_config_defaults();
    if (!path) return cfg;

    FILE *f = fopen(path, "r");
    if (!f) return cfg;

    char line[512];
    /* section state */
    int  in_runtime  = 0;
    int  in_ffi_root = 0;   /* [ffi] */
    char sig_lib[128] = ""; /* "libm" when in [ffi.libm.signatures] */

    while (fgets(line, sizeof(line), f)) {
        char *l = cfg_trim(line);
        if (!*l || *l == '#') continue;

        /* ── Section header ── */
        if (*l == '[') {
            in_runtime  = 0;
            in_ffi_root = 0;
            sig_lib[0]  = '\0';

            if (strcmp(l, "[runtime]") == 0) {
                in_runtime = 1;
            } else if (strcmp(l, "[ffi]") == 0) {
                in_ffi_root = 1;
            } else if (strncmp(l, "[ffi.", 5) == 0) {
                /* [ffi.<lib>.signatures] */
                char inner[256];
                int inner_len = (int)strlen(l) - 2; /* strip [ ] */
                if (inner_len > 0 && inner_len < (int)sizeof(inner)) {
                    memcpy(inner, l + 1, (size_t)inner_len);
                    inner[inner_len] = '\0';
                    /* inner = "ffi.libm.signatures" */
                    char *first_dot = strchr(inner, '.');
                    if (first_dot) {
                        char *lib_part  = first_dot + 1; /* "libm.signatures" */
                        char *second_dot = strchr(lib_part, '.');
                        if (second_dot && strcmp(second_dot, ".signatures") == 0) {
                            *second_dot = '\0';
                            snprintf(sig_lib, sizeof(sig_lib), "%s", lib_part);
                            /* ensure entry exists */
                            cfg_ffi_find_or_create(&cfg, sig_lib);
                        }
                    }
                }
            }
            continue;
        }

        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = cfg_trim(l);
        char *val = cfg_trim(eq + 1);
        /* strip inline comment */
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; cfg_trim(val); }

        /* ── [runtime] keys ── */
        if (in_runtime) {
            int v = atoi(val);
            if (strcmp(key, "gc_cap") == 0) {
                if (v > 0 && v <= GC_TABLE_CAP) cfg.gc_cap = v;
                else if (v > GC_TABLE_CAP)
                    fprintf(stderr, "[fluxa] toml: gc_cap %d > max %d\n",
                            v, GC_TABLE_CAP);
            } else if (strcmp(key, "prst_cap") == 0) {
                if (v > 0) cfg.prst_cap = v;
            } else if (strcmp(key, "prst_graph_cap") == 0) {
                if (v > 0 && v <= PRST_GRAPH_CAP_MAX) cfg.prst_graph_cap = v;
            }
            continue;
        }

        /* ── [ffi] root: alias = "auto" | "path" ── */
        if (in_ffi_root) {
            char resolved[256];
            cfg_unquote(val, resolved, sizeof(resolved));
            TomlFfiEntry *e = cfg_ffi_find_or_create(&cfg, key);
            if (e) snprintf(e->path, sizeof(e->path), "%s", resolved);
            continue;
        }

        /* ── [ffi.<lib>.signatures]: fnname = "(types) -> type" ── */
        if (sig_lib[0]) {
            TomlFfiEntry *e = cfg_ffi_find_or_create(&cfg, sig_lib);
            if (e && e->sig_count < TOML_SIG_MAX) {
                FfiSigEntry *sig = &e->sigs[e->sig_count++];
                memset(sig, 0, sizeof(*sig));
                snprintf(sig->fn_name, sizeof(sig->fn_name), "%s", key);
                char sig_str[256];
                cfg_unquote(val, sig_str, sizeof(sig_str));
                cfg_parse_sig(sig_str, sig);
            }
            continue;
        }
    }

    fclose(f);
    return cfg;
}

/* ── [libs] section parser ────────────────────────────────────────────────── */
/* Call after fluxa_config_load to parse [libs] section.
 * Sets FluxaStdLibs flags based on which libs are declared.
 * Example toml:
 *   [libs]
 *   std.math = "1.0"
 *   std.csv  = "1.0"
 */
static inline void fluxa_config_load_libs(FluxaConfig *cfg, const char *toml_path) {
    FILE *f = fopen(toml_path, "r");
    if (!f) return;
    char line[512];
    int in_libs = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* Section header */
        if (*p == '[') {
            in_libs = (strncmp(p, "[libs]", 6) == 0);
            continue;
        }
        if (!in_libs) continue;
        /* Parse key = value */
        if (strncmp(p, "std.math", 8) == 0) cfg->std_libs.has_math = 1;
        if (strncmp(p, "std.csv",  7) == 0) cfg->std_libs.has_csv  = 1;
        if (strncmp(p, "std.json", 8) == 0) cfg->std_libs.has_json = 1;
    }
    fclose(f);
}

static inline FluxaConfig fluxa_config_find_and_load(void) {
    FluxaConfig cfg = fluxa_config_load("fluxa.toml");
    fluxa_config_load_libs(&cfg, "fluxa.toml");
    return cfg;
}

#endif /* FLUXA_TOML_CONFIG_H */
