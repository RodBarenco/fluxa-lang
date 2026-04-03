/* ffi.c — Fluxa FFI implementation
 *
 * _GNU_SOURCE MUST be the first line before any system header.
 */
#define _GNU_SOURCE

#ifndef FLUXA_HAS_FFI
#  define FLUXA_HAS_FFI 0
#endif
#if FLUXA_HAS_FFI
#  include <ffi.h>
#endif

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "fluxa_ffi.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Registry
 * ══════════════════════════════════════════════════════════════════════════ */

void ffi_registry_init(FFIRegistry *r) {
    r->libs  = (FFILib *)malloc(sizeof(FFILib) * FFI_LIB_CAP_INIT);
    r->count = 0;
    r->cap   = r->libs ? FFI_LIB_CAP_INIT : 0;
}

void ffi_registry_free(FFIRegistry *r) {
    for (int i = 0; i < r->count; i++)
        if (r->libs[i].handle) dlclose(r->libs[i].handle);
    free(r->libs);
    r->libs  = NULL;
    r->count = 0;
    r->cap   = 0;
}

FFILib *ffi_find_lib(FFIRegistry *r, const char *name) {
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->libs[i].name, name) == 0)
            return &r->libs[i];
    return NULL;
}

FfiSig *ffi_find_sig(FFILib *lib, const char *fn_name) {
    for (int i = 0; i < lib->sig_count; i++)
        if (strcmp(lib->sigs[i].fn_name, fn_name) == 0)
            return &lib->sigs[i];
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Path resolution — Sprint 9.c-2
 *
 * Priority:
 *   1. Explicit path in toml (not "auto") → use directly
 *   2. "auto" → try platform candidates in order
 *      Linux:  lib<name>.so.N  (N=6,5,4,3,2,1,0), lib<name>.so
 *      macOS:  lib<name>.dylib, lib<name>.N.dylib
 *      Both:   plain <name> (in case user wrote the full soname)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Try dlopen on a candidate path — returns handle or NULL */
static void *try_open(const char *path) {
    return dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
}

void ffi_resolve_path(const char *path_or_auto,
                      const char *alias,
                      char *out, int out_size) {
    /* Explicit path: not "auto" and contains a '/' or ends with .so/.dylib */
    if (strcmp(path_or_auto, "auto") != 0) {
        snprintf(out, (size_t)out_size, "%s", path_or_auto);
        return;
    }

    /* Strip leading "lib" if present so alias "libm" and "m" both work */
    const char *base = alias;
    if (strncmp(base, "lib", 3) == 0) base += 3;

    /* Linux versioned candidates */
    static const int versions[] = {6,5,4,3,2,1,0,-1};
    for (int i = 0; versions[i] >= 0; i++) {
        snprintf(out, (size_t)out_size, "lib%s.so.%d", base, versions[i]);
        void *h = try_open(out);
        if (h) { dlclose(h); return; }
    }
    /* Linux unversioned */
    snprintf(out, (size_t)out_size, "lib%s.so", base);
    { void *h = try_open(out); if (h) { dlclose(h); return; } }

    /* macOS */
    snprintf(out, (size_t)out_size, "lib%s.dylib", base);
    { void *h = try_open(out); if (h) { dlclose(h); return; } }

    /* Fallback: let dlopen search LD_LIBRARY_PATH itself */
    snprintf(out, (size_t)out_size, "lib%s.so", base);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Signature → FParamKind conversion
 * ══════════════════════════════════════════════════════════════════════════ */

static FParamKind c_type_to_kind(const char *c_type) {
    if (strcmp(c_type, "int*")    == 0 ||
        strcmp(c_type, "long*")   == 0 ||
        strcmp(c_type, "int32_t*")== 0 ||
        strcmp(c_type, "int64_t*")== 0) return FPARAM_PTR_INT;

    if (strcmp(c_type, "double*") == 0 ||
        strcmp(c_type, "float*")  == 0) return FPARAM_PTR_FLT;

    if (strcmp(c_type, "bool*")   == 0) return FPARAM_PTR_BOOL;

    if (strcmp(c_type, "char*")   == 0 ||
        strcmp(c_type, "const char*") == 0) return FPARAM_STR;

    if (strcmp(c_type, "uint8_t*")== 0 ||
        strcmp(c_type, "arr")     == 0) return FPARAM_ARR;

    if (strcmp(c_type, "void*")   == 0 ||
        strcmp(c_type, "dyn")     == 0 ||
        strchr(c_type, '*') != NULL)    return FPARAM_DYN;

    return FPARAM_VALUE;
}

/* Build FfiSig from FfiSigEntry (toml parsed data) */
static void build_sig(FfiSig *sig, const FfiSigEntry *entry) {
    memset(sig, 0, sizeof(*sig));
    snprintf(sig->fn_name,    sizeof(sig->fn_name),    "%s", entry->fn_name);
    snprintf(sig->ret_c_type, sizeof(sig->ret_c_type), "%s", entry->ret_type);
    sig->param_count = entry->param_count;
    for (int i = 0; i < entry->param_count; i++)
        sig->param_kinds[i] = c_type_to_kind(entry->params[i].c_type);
}

/* ══════════════════════════════════════════════════════════════════════════
 * ffi_load_lib — low-level (alias + resolved path already known)
 * ══════════════════════════════════════════════════════════════════════════ */

int ffi_load_lib(FFIRegistry *r, ErrStack *err,
                 const char *alias, const char *path) {
    /* grow if needed */
    if (r->count >= r->cap) {
        int nc = r->cap > 0 ? r->cap * 2 : FFI_LIB_CAP_INIT;
        FFILib *nl = (FFILib *)realloc(r->libs, sizeof(FFILib) * (size_t)nc);
        if (!nl) {
            errstack_push(err, ERR_C_FFI,
                "out of memory loading FFI lib", alias, 0);
            return 0;
        }
        r->libs = nl;
        r->cap  = nc;
    }

    void *handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        char buf[384];
        snprintf(buf, sizeof(buf), "dlopen('%s'): %s", path, dlerror());
        errstack_push(err, ERR_C_FFI, buf, alias, 0);
        return 0;
    }

    FFILib *lib = &r->libs[r->count++];
    memset(lib, 0, sizeof(*lib));
    snprintf(lib->name, sizeof(lib->name), "%s", alias);
    snprintf(lib->path, sizeof(lib->path), "%s", path);
    lib->handle = handle;
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * ffi_load_from_config — Sprint 9.c-2
 *   Called at runtime boot. Loads every lib in cfg->ffi[].
 * ══════════════════════════════════════════════════════════════════════════ */

void ffi_load_from_config(FFIRegistry *r, ErrStack *err,
                           const FluxaConfig *cfg) {
    for (int i = 0; i < cfg->ffi_count; i++) {
        const TomlFfiEntry *te = &cfg->ffi[i];

        /* Skip if already loaded (import c may have loaded it earlier) */
        if (ffi_find_lib(r, te->alias)) continue;

        char path[256];
        ffi_resolve_path(te->path, te->alias, path, sizeof(path));

        int ok = ffi_load_lib(r, err, te->alias, path);
        if (!ok) {
            /* Non-fatal: print warning, continue */
            fprintf(stderr,
                "[fluxa] ffi: failed to load '%s' from path '%s'\n",
                te->alias, path);
            /* pop the error so it doesn't bleed into user code */
            if (err->count > 0) err->count--;
            continue;
        }

        /* Register signatures */
        FFILib *lib = ffi_find_lib(r, te->alias);
        if (!lib) continue;
        for (int s = 0; s < te->sig_count; s++) {
            if (lib->sig_count >= FFI_SIG_CAP) break;
            build_sig(&lib->sigs[lib->sig_count++], &te->sigs[s]);
        }

        fprintf(stderr, "[fluxa] ffi: loaded '%s' → %s\n", te->alias, path);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * ffi_reload_from_config — Sprint 9.c-2 (fluxa update)
 *   Diff old registry vs new config:
 *     - libs no longer in config → dlclose
 *     - new libs → dlopen
 *     - unchanged libs → keep handle, refresh sigs
 * ══════════════════════════════════════════════════════════════════════════ */

void ffi_reload_from_config(FFIRegistry *r, ErrStack *err,
                             const FluxaConfig *cfg) {
    /* 1. Close libs not present in new config */
    int i = 0;
    while (i < r->count) {
        int found = 0;
        for (int j = 0; j < cfg->ffi_count; j++)
            if (strcmp(r->libs[i].name, cfg->ffi[j].alias) == 0)
                { found = 1; break; }
        if (!found) {
            fprintf(stderr, "[fluxa] ffi: unloading '%s'\n", r->libs[i].name);
            if (r->libs[i].handle) dlclose(r->libs[i].handle);
            /* shift left */
            memmove(&r->libs[i], &r->libs[i+1],
                    sizeof(FFILib) * (size_t)(r->count - i - 1));
            r->count--;
        } else {
            i++;
        }
    }

    /* 2. Load new libs and refresh sigs for existing ones */
    for (int j = 0; j < cfg->ffi_count; j++) {
        const TomlFfiEntry *te = &cfg->ffi[j];
        FFILib *lib = ffi_find_lib(r, te->alias);

        if (!lib) {
            /* New lib — load it */
            char path[256];
            ffi_resolve_path(te->path, te->alias, path, sizeof(path));
            if (!ffi_load_lib(r, err, te->alias, path)) {
                fprintf(stderr,
                    "[fluxa] ffi: failed to reload '%s'\n", te->alias);
                if (err->count > 0) err->count--;
                continue;
            }
            lib = ffi_find_lib(r, te->alias);
            if (!lib) continue;
            fprintf(stderr, "[fluxa] ffi: loaded '%s' → %s\n",
                    te->alias, path);
        }

        /* Refresh signatures (clear + rebuild) */
        lib->sig_count = 0;
        for (int s = 0; s < te->sig_count && lib->sig_count < FFI_SIG_CAP; s++)
            build_sig(&lib->sigs[lib->sig_count++], &te->sigs[s]);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * CLI helpers — Sprint 9.c-4, 9.c-5
 * ══════════════════════════════════════════════════════════════════════════ */

void ffi_cli_list(void) {
    printf("Shared libraries available (via ldconfig):\n\n");
    /* ldconfig -p on Linux, otool/find on macOS */
    FILE *fp = popen("ldconfig -p 2>/dev/null | awk '{print $1, $NF}' | sort -u", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            /* strip newline */
            line[strcspn(line, "\n")] = '\0';
            /* format: "libname.so.N  /path/to/lib" */
            printf("  %s\n", line);
        }
        pclose(fp);
    } else {
        /* macOS fallback */
        fp = popen("find /usr/lib /usr/local/lib /opt/homebrew/lib"
                   " -name '*.dylib' 2>/dev/null | sort -u", "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = '\0';
                printf("  %s\n", line);
            }
            pclose(fp);
        }
    }
    printf("\nDeclare no fluxa.toml:\n");
    printf("  [ffi]\n");
    printf("  libm = \"auto\"\n");
}

/* Infer a conservative Fluxa type from a C type string from nm/readelf */
static const char *infer_fluxa_type(const char *c_type) __attribute__((unused));
static const char *infer_fluxa_type(const char *c_type) {
    if (strstr(c_type, "char*") || strstr(c_type, "char *"))
        return "str";
    if (strstr(c_type, "*"))
        return "dyn";
    if (strstr(c_type, "double") || strstr(c_type, "float"))
        return "float";
    if (strstr(c_type, "int") || strstr(c_type, "long") || strstr(c_type, "size"))
        return "int";
    if (strstr(c_type, "void"))
        return "nil";
    return "dyn"; /* conservative */
}

void ffi_cli_inspect(const char *lib_name_or_path) {
    /* Resolve path first */
    char path[256];
    ffi_resolve_path(lib_name_or_path, lib_name_or_path, path, sizeof(path));

    /* Check the lib actually opens */
    void *h = dlopen(path, RTLD_LAZY);
    if (!h) {
        fprintf(stderr, "[fluxa] ffi inspect: cannot open '%s': %s\n",
                path, dlerror());
        return;
    }
    dlclose(h);

    /* Extract exported symbols via nm -D */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "nm -D --defined-only '%s' 2>/dev/null"
        " | grep ' T ' | awk '{print $3}' | sort", path);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[fluxa] ffi inspect: nm failed\n");
        return;
    }

    /* Derive alias from lib name */
    const char *base = strrchr(lib_name_or_path, '/');
    base = base ? base + 1 : lib_name_or_path;
    /* strip "lib" prefix and ".so.*" / ".dylib" suffix */
    char alias[128];
    snprintf(alias, sizeof(alias), "%s", base);
    if (strncmp(alias, "lib", 3) == 0) memmove(alias, alias+3, strlen(alias)-2);
    char *dot = strchr(alias, '.');
    if (dot) *dot = '\0';

    printf("# Cole no seu fluxa.toml:\n");
    printf("[ffi]\n%s = \"auto\"\n\n", alias);
    printf("[ffi.%s.signatures]\n", alias);

    char sym[256];
    int  count = 0;
    while (fgets(sym, sizeof(sym), fp) && count < 128) {
        sym[strcspn(sym, "\n")] = '\0';
        if (!sym[0] || sym[0] == '_') continue; /* skip internal symbols */
        /* Conservative: all params and return → dyn unless we know better.
         * If libclang is available a future version can parse headers. */
        printf("%s = \"(dyn) -> dyn\"\n", sym);
        count++;
    }
    pclose(fp);

    if (count == 0)
        printf("# (nenhum símbolo público encontrado)\n");
    else
        printf("\n# %d função(ões) encontrada(s)."
               " Ajuste os tipos conforme a assinatura C real.\n", count);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Call dispatch
 * ══════════════════════════════════════════════════════════════════════════ */

#if FLUXA_HAS_FFI

static ffi_type *fluxa_to_ffi_type(ValType t) {
    switch (t) {
        case VAL_INT:    return &ffi_type_sint64;
        case VAL_FLOAT:  return &ffi_type_double;
        case VAL_BOOL:   return &ffi_type_sint32;
        case VAL_STRING: return &ffi_type_pointer;
        case VAL_DYN:    return &ffi_type_pointer; /* VAL_PTR inside dyn */
        default:         return &ffi_type_void;
    }
}

/* Determine ffi_type from FParamKind (reserved for future use) */
static ffi_type *kind_to_ffi_type(FParamKind k) __attribute__((unused));
static ffi_type *kind_to_ffi_type(FParamKind k) {
    switch (k) {
        case FPARAM_VALUE:    return &ffi_type_sint64;
        case FPARAM_PTR_INT:  return &ffi_type_pointer;
        case FPARAM_PTR_FLT:  return &ffi_type_pointer;
        case FPARAM_PTR_BOOL: return &ffi_type_pointer;
        case FPARAM_STR:      return &ffi_type_pointer;
        case FPARAM_ARR:      return &ffi_type_pointer;
        case FPARAM_DYN:      return &ffi_type_pointer;
    }
    return &ffi_type_pointer;
}

Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type,
                     const FfiSig *sig,
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx) {
    if (arg_count > 32) {
        errstack_push(err, ERR_C_FFI, "FFI: too many arguments (max 32)", ctx, 0);
        return val_nil();
    }

    dlerror();
    void *sym = dlsym(lib->handle, sym_name);
    char *dl_err = dlerror();
    if (dl_err) {
        char buf[320];
        snprintf(buf, sizeof(buf), "dlsym('%s'): %s", sym_name, dl_err);
        errstack_push(err, ERR_C_FFI, buf, ctx, 0);
        return val_nil();
    }

    ffi_cif   cif;
    ffi_type *arg_types[32];
    void     *arg_vals[32];

    /* Storage for by-value args */
    int64_t  ival[32];
    double   fval[32];
    int32_t  bval[32];
    char    *sval[32];
    void    *pval[32];

    /* Storage for pointer args (int*, double*, bool*) — written back after call */
    int64_t  ptr_ival[32];
    double   ptr_fval[32];
    int32_t  ptr_bval[32];
    void    *ptr_ptr[32];   /* pointer storage for pointer types */

    for (int i = 0; i < arg_count; i++) {
        FParamKind kind = FPARAM_VALUE;
        if (sig && i < sig->param_count)
            kind = sig->param_kinds[i];
        else
            /* Legacy mode: infer from value type */
            kind = FPARAM_VALUE;

        switch (kind) {
            case FPARAM_VALUE:
                switch (args[i].type) {
                    case VAL_INT:
                        ival[i] = (int64_t)args[i].as.integer;
                        arg_types[i] = &ffi_type_sint64;
                        arg_vals[i]  = &ival[i];
                        break;
                    case VAL_FLOAT:
                        fval[i] = args[i].as.real;
                        arg_types[i] = &ffi_type_double;
                        arg_vals[i]  = &fval[i];
                        break;
                    case VAL_BOOL:
                        bval[i] = (int32_t)args[i].as.boolean;
                        arg_types[i] = &ffi_type_sint32;
                        arg_vals[i]  = &bval[i];
                        break;
                    case VAL_STRING:
                        sval[i] = args[i].as.string;
                        arg_types[i] = &ffi_type_pointer;
                        arg_vals[i]  = &sval[i];
                        break;
                    default:
                        errstack_push(err, ERR_C_FFI,
                            "FFI: unsupported argument type (add signature to toml)",
                            ctx, 0);
                        return val_nil();
                }
                break;

            case FPARAM_PTR_INT:
                /* Pass &int_val; write back after call */
                ptr_ival[i] = (int64_t)args[i].as.integer;
                ptr_ptr[i]  = &ptr_ival[i];
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &ptr_ptr[i];
                break;

            case FPARAM_PTR_FLT:
                ptr_fval[i] = args[i].as.real;
                ptr_ptr[i]  = &ptr_fval[i];
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &ptr_ptr[i];
                break;

            case FPARAM_PTR_BOOL:
                ptr_bval[i] = (int32_t)args[i].as.boolean;
                ptr_ptr[i]  = &ptr_bval[i];
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &ptr_ptr[i];
                break;

            case FPARAM_STR:
                sval[i]      = args[i].as.string;
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &sval[i];
                break;

            case FPARAM_ARR:
                /* arr → raw data pointer */
                pval[i]      = args[i].type == VAL_ARR
                               ? (void*)args[i].as.arr.data
                               : NULL;
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &pval[i];
                break;

            case FPARAM_DYN:
                /* Extract void* from VAL_PTR stored in dyn[0] */
                if (args[i].type == VAL_DYN && args[i].as.dyn &&
                    args[i].as.dyn->count > 0 &&
                    args[i].as.dyn->items[0].type == VAL_PTR) {
                    pval[i] = args[i].as.dyn->items[0].as.ptr;
                } else if (args[i].type == VAL_PTR) {
                    pval[i] = args[i].as.ptr;
                } else {
                    pval[i] = NULL;
                }
                arg_types[i] = &ffi_type_pointer;
                arg_vals[i]  = &pval[i];
                break;
        }
    }

    /* Determine return ffi_type */
    ffi_type *ffi_ret;
    if (sig) {
        const char *rt_s = sig->ret_c_type;
        if (strcmp(rt_s, "void") == 0 || strcmp(rt_s, "nil") == 0)
            ffi_ret = &ffi_type_void;
        else if (strcmp(rt_s, "double") == 0 || strcmp(rt_s, "float") == 0)
            ffi_ret = &ffi_type_double;
        else if (strstr(rt_s, "*") || strcmp(rt_s, "dyn") == 0)
            ffi_ret = &ffi_type_pointer;
        else
            ffi_ret = &ffi_type_sint64;
    } else {
        ffi_ret = (ret_type == VAL_NIL)
                ? &ffi_type_void
                : fluxa_to_ffi_type(ret_type);
    }

    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)arg_count,
                     ffi_ret, arg_types) != FFI_OK) {
        errstack_push(err, ERR_C_FFI, "FFI: ffi_prep_cif failed", ctx, 0);
        return val_nil();
    }

    /* Return value storage */
    int64_t  ret_ival = 0;
    double   ret_fval = 0.0;
    void    *ret_ptr  = NULL;
    void    *ret_storage;

    if (ffi_ret == &ffi_type_double)      ret_storage = &ret_fval;
    else if (ffi_ret == &ffi_type_pointer) ret_storage = &ret_ptr;
    else                                   ret_storage = &ret_ival;

    ffi_call(&cif, FFI_FN(sym), ret_storage, arg_vals);

    /* Write back pointer args into original Fluxa Values */
    if (sig) {
        for (int i = 0; i < arg_count && i < sig->param_count; i++) {
            switch (sig->param_kinds[i]) {
                case FPARAM_PTR_INT:
                    args[i].as.integer = (long)ptr_ival[i];
                    break;
                case FPARAM_PTR_FLT:
                    args[i].as.real = ptr_fval[i];
                    break;
                case FPARAM_PTR_BOOL:
                    args[i].as.boolean = (int)ptr_bval[i];
                    break;
                default: break;
            }
        }
    }

    /* Build return Value */
    if (sig) {
        const char *rt_s = sig->ret_c_type;
        if (strcmp(rt_s, "void") == 0 || strcmp(rt_s, "nil") == 0)
            return val_nil();
        if (strcmp(rt_s, "double") == 0 || strcmp(rt_s, "float") == 0)
            return val_float(ret_fval);
        if (strstr(rt_s, "*") || strcmp(rt_s, "dyn") == 0) {
            /* Wrap pointer in a dyn containing VAL_PTR */
            if (!ret_ptr) return val_nil();
            FluxaDyn *d = (FluxaDyn*)malloc(sizeof(FluxaDyn));
            if (!d) return val_nil();
            d->items = (Value*)malloc(sizeof(Value));
            if (!d->items) { free(d); return val_nil(); }
            d->items[0] = val_ptr(ret_ptr);
            d->count = 1; d->cap = 1;
            return val_dyn(d);
        }
        return val_int((long)ret_ival);
    }

    /* Legacy mode (no sig) */
    switch (ret_type) {
        case VAL_NIL:    return val_nil();
        case VAL_INT:    return val_int((long)ret_ival);
        case VAL_FLOAT:  return val_float(ret_fval);
        case VAL_BOOL:   return val_bool((int)ret_ival);
        case VAL_STRING: return ret_ptr ? val_string((char*)ret_ptr) : val_nil();
        default:         return val_nil();
    }
}

#else /* !FLUXA_HAS_FFI */

Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type, const FfiSig *sig,
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx) {
    (void)lib; (void)sym_name; (void)ret_type; (void)sig;
    (void)args; (void)arg_count;
    errstack_push(err, ERR_C_FFI,
        "FFI not available — libffi not found at compile time", ctx, 0);
    return val_nil();
}

#endif /* FLUXA_HAS_FFI */
