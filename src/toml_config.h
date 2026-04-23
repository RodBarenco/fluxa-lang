/* toml_config.h — Minimal fluxa.toml configuration loader
 *
 * Supported sections:
 *   [runtime]        gc_cap, prst_cap, prst_graph_cap, warm_func_cap
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
/* Libs declared in [libs] of fluxa.toml are stored by name.
 * The runtime checks against this list via fluxa_std_lib_enabled().
 * Adding a new lib requires NO changes here — just create the lib
 * header with FLUXA_LIB_EXPORT and a lib.mk file. */
#define FLUXA_STD_LIBS_MAX 32

typedef struct {
    char names[FLUXA_STD_LIBS_MAX][32]; /* e.g. "pid", "sqlite", "math" */
    int  count;
} FluxaStdLibs;

static inline int fluxa_std_lib_enabled(const FluxaStdLibs *libs,
                                         const char *name) {
    for (int i = 0; i < libs->count; i++)
        if (strcmp(libs->names[i], name) == 0) return 1;
    return 0;
}

/* ── Main config ──────────────────────────────────────────────────────────── */
/* ── Security configuration ([security] section) ─────────────────────────── *
 * Only meaningful with FLUXA_SECURE=1 builds, but parsed always so the      *
 * toml is valid regardless of build flags.                                   *
 * Keys are NEVER stored inline in fluxa.toml — only file paths.             */
typedef enum {
    FLUXA_SEC_MODE_OFF    = 0,  /* no validation — dev default              */
    FLUXA_SEC_MODE_WARN   = 1,  /* accept but log unsigned commands         */
    FLUXA_SEC_MODE_STRICT = 2   /* reject apply/update without valid sig    */
} FluxaSecMode;

typedef struct {
    char         signing_key_path[256]; /* path to Ed25519 private key file  *
                                         * 0400, generated with fluxa keygen */
    char         ipc_hmac_key_path[256];/* path to HMAC-SHA512 secret        *
                                         * 0400, optional — auto-generated   *
                                         * in memory if absent               */
    FluxaSecMode mode;                  /* off / warn / strict               */
    int          handshake_timeout_ms;  /* IPC recv timeout, default 50ms    *
                                         * Increase for slow/embedded links  */
    int          ipc_max_conns;         /* max simultaneous IPC connections  *
                                         * default 16, increase if needed    */
} FluxaSecurityConfig;

typedef struct {
    int          gc_cap;
    int          prst_cap;
    int          prst_graph_cap;
    int          warm_func_cap;   /* WarmProfile hash table size: default 32,
                                   * range 4..256, must be power of 2.
                                   * Larger = more functions profiled, more RAM.
                                   * Each slot = 276 bytes. 32 = 8.8KB, 256 = 70KB */
    TomlFfiEntry ffi[TOML_FFI_MAX];
    int          ffi_count;
    FluxaStdLibs std_libs;  /* opt-in stdlib — declared in [libs] toml */
    int          json_max_str;    /* [libs.json] max_str_bytes, default 4096  */
    int          ffi_str_buf_size; /* [ffi] str_buf_size — writable char* buffer
                                   * allocated per pointer arg, default 1024   */
    char         libdsp_backend[16]; /* [libs.libdsp] backend = "native"|"fftw"  */
    char         libv_backend[16];   /* [libs.libv]   backend = "native"|"blas"  */
    FluxaSecurityConfig security; /* [security] — key paths + enforcement mode */
} FluxaConfig;

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline FluxaConfig fluxa_config_defaults(void) {
    FluxaConfig c;
    memset(&c, 0, sizeof(c));
    /* std_libs: all disabled by default — must be declared in [libs] */
    c.gc_cap         = GC_TABLE_CAP;
    c.prst_cap       = PRST_POOL_INIT_CAP;
    c.prst_graph_cap = PRST_GRAPH_CAP_DEFAULT;
    c.warm_func_cap  = 32;  /* WARM_FUNC_CAP_DEFAULT */
    c.json_max_str      = 4096;
    c.ffi_str_buf_size  = 1024;
    strncpy(c.libdsp_backend, "native", sizeof(c.libdsp_backend)-1);
    strncpy(c.libv_backend,   "native", sizeof(c.libv_backend)-1);
    /* security: off by default — must be explicitly enabled in [security] */
    c.security.mode = FLUXA_SEC_MODE_OFF;
    c.security.signing_key_path[0]  = '\0';
    c.security.ipc_hmac_key_path[0] = '\0';
    c.security.handshake_timeout_ms = 50;   /* IPC_TIMEOUT_MS default */
    c.security.ipc_max_conns        = 16;   /* IPC_MAX_CONNS_DEFAULT  */
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

    /* params: between ( and last ) before -> */
    const char *open  = strchr(sig_str, '(');
    /* find the ) that closes the param list — search backwards from arrow or end */
    const char *close = NULL;
    {
        const char *search_end = arrow ? arrow : (sig_str + strlen(sig_str));
        for (const char *p = search_end - 1; p > open; p--) {
            if (*p == ')') { close = p; break; }
        }
    }
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
    int  in_security = 0;   /* [security] */
    char sig_lib[128] = ""; /* "libm" when in [ffi.libm.signatures] */

    while (fgets(line, sizeof(line), f)) {
        char *l = cfg_trim(line);
        if (!*l || *l == '#') continue;

        /* ── Section header ── */
        if (*l == '[') {
            in_runtime  = 0;
            in_ffi_root = 0;
            in_security = 0;
            sig_lib[0]  = '\0';

            if (strcmp(l, "[runtime]") == 0) {
                in_runtime = 1;
            } else if (strcmp(l, "[security]") == 0) {
                in_security = 1;
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
            } else if (strcmp(key, "warm_func_cap") == 0) {
                /* Must be power of 2 in [4, 256]. Round down to nearest pow2. */
                if (v >= 4 && v <= 256) {
                    int p = 4;
                    while (p * 2 <= v) p *= 2;
                    cfg.warm_func_cap = p;
                } else {
                    fprintf(stderr,
                        "[fluxa] toml: warm_func_cap %d out of range [4,256]"
                        " — using default 32\n", v);
                }
            }
            continue;
        }

        /* ── [security] ─────────────────────────────────────────────────── *
         * Keys reference FILE PATHS only — never inline key material.      *
         * signing_key   = "/path/to/signing.key"   (Ed25519 private, 0400) *
         * ipc_hmac_key  = "/path/to/ipc_hmac.key"  (HMAC secret, 0400)    *
         * mode          = "off" | "warn" | "strict"                        */
        if (in_security) {
            /* key and val are already parsed and trimmed by the outer block */
            /* Strip surrounding quotes from val (paths may be quoted)       */
            char clean_val[256];
            strncpy(clean_val, val, sizeof(clean_val)-1);
            clean_val[sizeof(clean_val)-1] = '\0';
            /* strip leading/trailing quotes */
            char *cv = clean_val;
            if (*cv == '"') cv++;
            char *cvend = cv + strlen(cv);
            while (cvend > cv && (*(cvend-1) == '"' ||
                                   isspace((unsigned char)*(cvend-1))))
                *--cvend = '\0';

            if (strcmp(key, "signing_key") == 0) {
                snprintf(cfg.security.signing_key_path,
                         sizeof(cfg.security.signing_key_path), "%s", cv);
            } else if (strcmp(key, "ipc_hmac_key") == 0) {
                snprintf(cfg.security.ipc_hmac_key_path,
                         sizeof(cfg.security.ipc_hmac_key_path), "%s", cv);
            } else if (strcmp(key, "handshake_timeout_ms") == 0) {
                int v = atoi(cv);
                if (v >= 10 && v <= 5000)
                    cfg.security.handshake_timeout_ms = v;
                else
                    fprintf(stderr,
                        "[fluxa] toml: security.handshake_timeout_ms %d "
                        "out of range [10, 5000] — using default 50\n", v);
            } else if (strcmp(key, "ipc_max_conns") == 0) {
                int v = atoi(cv);
                if (v >= 1 && v <= 256)
                    cfg.security.ipc_max_conns = v;
                else
                    fprintf(stderr,
                        "[fluxa] toml: security.ipc_max_conns %d "
                        "out of range [1, 256] — using default 16\n", v);
            } else if (strcmp(key, "mode") == 0) {
                if (strcmp(cv, "strict") == 0)
                    cfg.security.mode = FLUXA_SEC_MODE_STRICT;
                else if (strcmp(cv, "warn") == 0)
                    cfg.security.mode = FLUXA_SEC_MODE_WARN;
                else if (strcmp(cv, "off") == 0)
                    cfg.security.mode = FLUXA_SEC_MODE_OFF;
                else
                    fprintf(stderr,
                        "[fluxa] toml: security.mode '%s' unknown "
                        "(use: off / warn / strict)\n", cv);
            }
            continue;
        }

        /* ── [ffi] root: alias = "auto" | "path" | str_buf_size = N ── */
        if (in_ffi_root) {
            /* str_buf_size is a special scalar key, not a lib alias */
            if (strcmp(key, "str_buf_size") == 0) {
                int v = atoi(val);
                if (v >= 64 && v <= 65536) cfg.ffi_str_buf_size = v;
                else fprintf(stderr,
                    "[fluxa] toml: ffi.str_buf_size %d out of range [64..65536],"
                    " keeping default %d\n", v, cfg.ffi_str_buf_size);
                continue;
            }
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
    int in_libs      = 0;
    int in_libs_json = 0;
    int in_libs_libdsp = 0;
    int in_libs_libv   = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* Section header */
        if (*p == '[') {
            in_libs        = (strncmp(p, "[libs]",        6) == 0);
            in_libs_json   = (strncmp(p, "[libs.json]",  11) == 0);
            in_libs_libdsp = (strncmp(p, "[libs.libdsp]",13) == 0);
            in_libs_libv   = (strncmp(p, "[libs.libv]",  11) == 0);
            continue;
        }
        if (in_libs) {
            /* Generic: any "std.<name> = ..." line registers the lib name.
             * The registry (lib_registry_gen.h) validates at runtime whether
             * the lib is actually compiled in. No hardcoded names here. */
            if (strncmp(p, "std.", 4) == 0) {
                /* Extract lib name: "std.pid = ..." → "pid" */
                char lib_name[32] = "";
                const char *start = p + 4;
                int len = 0;
                while (start[len] && start[len] != ' ' &&
                       start[len] != '=' && start[len] != '\n' &&
                       len < 31) len++;
                if (len > 0 && cfg->std_libs.count < FLUXA_STD_LIBS_MAX) {
                    memcpy(lib_name, start, (size_t)len);
                    lib_name[len] = '\0';
                    /* Deduplicate */
                    int found = 0;
                    for (int i = 0; i < cfg->std_libs.count; i++)
                        if (strcmp(cfg->std_libs.names[i], lib_name) == 0)
                            { found = 1; break; }
                    if (!found)
                        snprintf(cfg->std_libs.names[cfg->std_libs.count++],
                                 32, "%s", lib_name);
                }
            }
        }
        if (in_libs_json) {
            if (strncmp(p, "max_str_bytes", 13) == 0) {
                char *eq = strchr(p, '=');
                if (eq) {
                    int v = (int)strtol(eq + 1, NULL, 10);
                    if (v > 0) cfg->json_max_str = v;
                }
            }
        }
        if (in_libs_libdsp) {
            if (strncmp(p, "backend", 7) == 0) {
                char *eq = strchr(p, '=');
                if (eq) {
                    eq++;
                    while (*eq == ' ' || *eq == '"') eq++;
                    char *end = eq;
                    while (*end && *end != '"' && *end != '\n' && *end != ' ') end++;
                    int len = (int)(end - eq);
                    if (len > 0 && len < 15) {
                        memcpy(cfg->libdsp_backend, eq, (size_t)len);
                        cfg->libdsp_backend[len] = '\0';
                    }
                }
            }
        }
        if (in_libs_libv) {
            if (strncmp(p, "backend", 7) == 0) {
                char *eq = strchr(p, '=');
                if (eq) {
                    eq++;
                    while (*eq == ' ' || *eq == '"') eq++;
                    char *end = eq;
                    while (*end && *end != '"' && *end != '\n' && *end != ' ') end++;
                    int len = (int)(end - eq);
                    if (len > 0 && len < 15) {
                        memcpy(cfg->libv_backend, eq, (size_t)len);
                        cfg->libv_backend[len] = '\0';
                    }
                }
            }
        }
    }
    fclose(f);
}

static inline FluxaConfig fluxa_config_find_and_load(void) {
    FluxaConfig cfg = fluxa_config_load("fluxa.toml");
    fluxa_config_load_libs(&cfg, "fluxa.toml");
    return cfg;
}

#endif /* FLUXA_TOML_CONFIG_H */
