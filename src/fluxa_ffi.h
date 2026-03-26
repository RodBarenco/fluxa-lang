/* ffi.h — Fluxa FFI public API (Sprint 6.b)
 *
 * This header is include-safe and has NO dependency on libffi types.
 * All libffi-specific code lives in ffi.c which sets _GNU_SOURCE before
 * any system headers, avoiding feature-test macro conflicts.
 *
 * Rules enforced at runtime:
 *   - import c: library loaded via dlopen
 *   - FFI calls must be inside danger block (runtime check)
 *   - Errors accumulate in ErrStack with kind = ERR_C_FFI
 */
#ifndef FLUXA_FLUXA_FFI_H
#define FLUXA_FLUXA_FFI_H

#include "scope.h"
#include "err.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── FFI library registry ─────────────────────────────────────────────────── */
#define FFI_LIB_CAP_INIT 8

typedef struct {
    char  name[128];   /* alias used in Fluxa code */
    char  path[256];   /* resolved .so path        */
    void *handle;      /* dlopen handle — opaque   */
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

/* Load a C library by alias + .so path.
 * Returns 1 on success, 0 on failure (error pushed to err). */
int ffi_load_lib(FFIRegistry *r, ErrStack *err,
                 const char *alias, const char *path);

/* Resolve .so path from well-known alias names.
 * out receives the path string. */
void ffi_resolve_path(const char *name, char *out, int out_size);

/* ── Call dispatch ───────────────────────────────────────────────────────── */
/* Call a C function by symbol name inside lib.
 * ret_type: expected Fluxa return type (VAL_NIL = void function)
 * args / arg_count: Fluxa Values to pass as arguments
 * err: ErrStack for ERR_C_FFI accumulation
 * Returns the result as a Fluxa Value, or val_nil() on error. */
Value fluxa_ffi_call(FFILib *lib, const char *sym_name,
                     ValType ret_type,
                     Value *args, int arg_count,
                     ErrStack *err, const char *ctx);

#endif /* FLUXA_FLUXA_FFI_H */
