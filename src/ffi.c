/* ffi.c — Fluxa FFI implementation (Sprint 6.b)
 *
 * _GNU_SOURCE MUST be the first line before any system header.
 * This isolates the libffi feature-test requirement from the rest
 * of the project which uses _POSIX_C_SOURCE 200809L.
 */
#define _GNU_SOURCE

/* libffi — FLUXA_HAS_FFI injected by Makefile via pkg-config */
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

/* ── Registry ────────────────────────────────────────────────────────────── */
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

int ffi_load_lib(FFIRegistry *r, ErrStack *err,
                 const char *alias, const char *path) {
    /* grow if needed */
    if (r->count >= r->cap) {
        int new_cap = r->cap > 0 ? r->cap * 2 : FFI_LIB_CAP_INIT;
        FFILib *nl = (FFILib *)realloc(r->libs, sizeof(FFILib) * new_cap);
        if (!nl) {
            errstack_push(err, ERR_C_FFI,
                "out of memory loading FFI lib", alias, 0);
            return 0;
        }
        r->libs = nl;
        r->cap  = new_cap;
    }

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        char buf[256];
        snprintf(buf, sizeof(buf), "dlopen failed: %s", dlerror());
        errstack_push(err, ERR_C_FFI, buf, alias, 0);
        return 0;
    }

    snprintf(r->libs[r->count].name, sizeof(r->libs[r->count].name),
             "%s", alias);
    snprintf(r->libs[r->count].path, sizeof(r->libs[r->count].path),
             "%s", path);
    r->libs[r->count].handle = handle;
    r->count++;
    return 1;
}

void ffi_resolve_path(const char *name, char *out, int out_size) {
    static const struct { const char *alias; const char *path; } known[] = {
        {"libm",       "libm.so.6"},
        {"libc",       "libc.so.6"},
        {"libz",       "libz.so.1"},
        {"libpthread", "libpthread.so.0"},
        {NULL, NULL}
    };
    for (int i = 0; known[i].alias; i++) {
        if (strcmp(name, known[i].alias) == 0) {
            snprintf(out, (size_t)out_size, "%s", known[i].path);
            return;
        }
    }
    snprintf(out, (size_t)out_size, "%s.so", name);
}

/* ── Call dispatch ───────────────────────────────────────────────────────── */
#if FLUXA_HAS_FFI

static ffi_type *fluxa_to_ffi_type(ValType t) {
    switch (t) {
        case VAL_INT:    return &ffi_type_sint64;
        case VAL_FLOAT:  return &ffi_type_double;
        case VAL_BOOL:   return &ffi_type_sint32;
        case VAL_STRING: return &ffi_type_pointer;
        default:         return &ffi_type_void;
    }
}

Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type,
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx) {
    if (arg_count > 32) {
        errstack_push(err, ERR_C_FFI,
            "FFI: too many arguments (max 32)", ctx, 0);
        return val_nil();
    }

    /* Resolve symbol */
    dlerror();
    void *sym = dlsym(lib->handle, sym_name);
    char *dl_err = dlerror();
    if (dl_err) {
        char buf[256];
        snprintf(buf, sizeof(buf), "dlsym failed: %s", dl_err);
        errstack_push(err, ERR_C_FFI, buf, ctx, 0);
        return val_nil();
    }

    ffi_cif    cif;
    ffi_type  *arg_types[32];
    void      *arg_vals[32];

    int64_t  ival[32];
    double   fval[32];
    int32_t  bval[32];
    char    *sval[32];

    for (int i = 0; i < arg_count; i++) {
        arg_types[i] = fluxa_to_ffi_type(args[i].type);
        switch (args[i].type) {
            case VAL_INT:
                ival[i] = (int64_t)args[i].as.integer;
                arg_vals[i] = &ival[i];
                break;
            case VAL_FLOAT:
                fval[i] = args[i].as.real;
                arg_vals[i] = &fval[i];
                break;
            case VAL_BOOL:
                bval[i] = (int32_t)args[i].as.boolean;
                arg_vals[i] = &bval[i];
                break;
            case VAL_STRING:
                sval[i] = args[i].as.string;
                arg_vals[i] = &sval[i];
                break;
            default:
                errstack_push(err, ERR_C_FFI,
                    "FFI: unsupported argument type", ctx, 0);
                return val_nil();
        }
    }

    ffi_type *ffi_ret = (ret_type == VAL_NIL)
                      ? &ffi_type_void
                      : fluxa_to_ffi_type(ret_type);

    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)arg_count,
                     ffi_ret, arg_types) != FFI_OK) {
        errstack_push(err, ERR_C_FFI,
            "FFI: ffi_prep_cif failed", ctx, 0);
        return val_nil();
    }

    /* Storage for return value */
    int64_t  ret_ival = 0;
    double   ret_fval = 0.0;
    int32_t  ret_bval = 0;
    void    *ret_ptr  = NULL;
    void    *ret_storage;

    switch (ret_type) {
        case VAL_INT:    ret_storage = &ret_ival; break;
        case VAL_FLOAT:  ret_storage = &ret_fval; break;
        case VAL_BOOL:   ret_storage = &ret_bval; break;
        case VAL_STRING: ret_storage = &ret_ptr;  break;
        default:         ret_storage = &ret_ival; break; /* void — discarded */
    }

    ffi_call(&cif, FFI_FN(sym), ret_storage, arg_vals);

    switch (ret_type) {
        case VAL_NIL:    return val_nil();
        case VAL_INT:    return val_int((long)ret_ival);
        case VAL_FLOAT:  return val_float(ret_fval);
        case VAL_BOOL:   return val_bool((int)ret_bval);
        case VAL_STRING: return ret_ptr ? val_string((char*)ret_ptr) : val_nil();
        default:         return val_nil();
    }
}

#else /* !FLUXA_HAS_FFI */

Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type,
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx) {
    (void)lib; (void)sym_name; (void)ret_type;
    (void)args; (void)arg_count;
    errstack_push(err, ERR_C_FFI,
        "FFI not available — libffi not found at compile time", ctx, 0);
    return val_nil();
}

#endif /* FLUXA_HAS_FFI */
