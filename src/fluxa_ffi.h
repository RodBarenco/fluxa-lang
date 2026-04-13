/* fluxa_ffi.h — Fluxa FFI public API
 *
 * Sprint 6.b: base FFI, dlopen/dlsym, libffi call dispatch
 * Sprint 9.c-2: ffi_load_from_config() — [ffi] section in toml
 * Sprint 9.c-3: pointer marshalling via FfiSig (int*, double*, char*, void*)
 *
 * NO dependency on libffi types in this header.
 * All libffi-specific code lives in ffi.c.
 */
#ifndef FLUXA_FLUXA_FFI_H
#define FLUXA_FLUXA_FFI_H

#include "scope.h"
#include "err.h"
#include "toml_config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── FFI library registry ─────────────────────────────────────────────────── */
#define FFI_LIB_CAP_INIT 8

/* Param kind after resolving C type string */
typedef enum {
    FPARAM_VALUE,   /* pass by value — int, double, bool         */
    FPARAM_PTR_INT, /* int*  → pass &fluxa_int, write back       */
    FPARAM_PTR_FLT, /* double* / float* → pass &fluxa_float      */
    FPARAM_PTR_BOOL,/* bool* / int* (flag)                       */
    FPARAM_STR,     /* char* → sds pointer, zero copy            */
    FPARAM_ARR,     /* uint8_t* / void* buffer → arr data ptr    */
    FPARAM_DYN,     /* struct* / void* opaque → VAL_PTR from dyn */
} FParamKind;

/* Resolved signature for one C function (built from FfiSigEntry at load) */
typedef struct {
    char       fn_name[128];
    char       ret_c_type[32];  /* "int", "double", "char*", "void*", "void" */
    FParamKind param_kinds[16];
    int        param_count;
} FfiSig;

#define FFI_SIG_CAP 64

typedef struct {
    char  name[128];   /* alias used in Fluxa code, e.g. "libm"    */
    char  path[256];   /* resolved .so path                         */
    void *handle;      /* dlopen handle — opaque                    */
    FfiSig sigs[FFI_SIG_CAP];
    int    sig_count;
} FFILib;

typedef struct {
    FFILib *libs;
    int     count;
    int     cap;
} FFIRegistry;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
void ffi_registry_init(FFIRegistry *r);
void ffi_registry_free(FFIRegistry *r);

/* Find library by alias. Returns NULL if not found. */
FFILib *ffi_find_lib(FFIRegistry *r, const char *name);

/* Find signature in a lib. Returns NULL if not registered. */
FfiSig *ffi_find_sig(FFILib *lib, const char *fn_name);

/* Load a C library by alias + .so path.
 * Returns 1 on success, 0 on failure (error pushed to err). */
int ffi_load_lib(FFIRegistry *r, ErrStack *err,
                 const char *alias, const char *path);

/* Resolve .so path from alias: tries platform candidates in order.
 * "auto" → platform-specific search. Explicit path → used directly. */
void ffi_resolve_path(const char *name_or_auto_or_path,
                      const char *alias,
                      char *out, int out_size);

/* Sprint 9.c-2: Load all libs declared in FluxaConfig [ffi] section.
 * Called once at runtime boot, before any user code runs.
 * Libs already loaded (same alias) are skipped. */
void ffi_load_from_config(FFIRegistry *r, ErrStack *err,
                           const FluxaConfig *cfg);

/* Sprint 9.c-2: Reload [ffi] from a new config (fluxa update).
 * Closes handles not present in new config, opens new ones.
 * Libs still in config are NOT reloaded (dlclose+dlopen unnecessary). */
void ffi_reload_from_config(FFIRegistry *r, ErrStack *err,
                             const FluxaConfig *cfg);

/* ── Call dispatch ───────────────────────────────────────────────────────── */
/* Call a C function by symbol name inside lib.
 * ret_type: expected Fluxa return type (VAL_NIL = void function)
 * sig: optional — if non-NULL, enables pointer marshalling (9.c-3)
 * args / arg_count: Fluxa Values to pass as arguments
 * Returns the result as a Fluxa Value, or val_nil() on error. */
Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type,
                     const FfiSig *sig,       /* NULL = legacy mode */
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx,
                     int str_buf_size);       /* writable char* buf size, 0=default */

/* ── CLI helpers ─────────────────────────────────────────────────────────── */
/* fluxa ffi list — print available shared libs via ldconfig/pkg-config */
void ffi_cli_list(void);

/* fluxa ffi inspect <lib_alias_or_path> — print suggested toml signatures */
void ffi_cli_inspect(const char *lib_name_or_path);

#endif /* FLUXA_FLUXA_FFI_H */
