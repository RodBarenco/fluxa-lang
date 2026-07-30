/* Stubs for services deliberately excluded from the Windows minimal profile. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "fluxa_ffi.h"

struct IpcRtView;

void ipc_apply_pending_set(Runtime *rt) {
    (void)rt;
}

void ipc_rtview_update(struct IpcRtView *view, Runtime *rt) {
    (void)view;
    (void)rt;
}

void ipc_rtview_clear_live(struct IpcRtView *view) {
    (void)view;
}

void ffi_registry_init(FFIRegistry *registry) {
    registry->libs = NULL;
    registry->count = 0;
    registry->cap = 0;
}

void ffi_registry_free(FFIRegistry *registry) {
    free(registry->libs);
    registry->libs = NULL;
    registry->count = 0;
    registry->cap = 0;
}

FFILib *ffi_find_lib(FFIRegistry *registry, const char *name) {
    (void)registry;
    (void)name;
    return NULL;
}

FfiSig *ffi_find_sig(FFILib *lib, const char *name) {
    (void)lib;
    (void)name;
    return NULL;
}

void ffi_resolve_path(const char *path, const char *alias,
                      char *out, int out_size) {
    (void)path;
    (void)alias;
    if (out && out_size > 0) out[0] = '\0';
}

int ffi_load_lib(FFIRegistry *registry, ErrStack *err,
                 const char *alias, const char *path) {
    (void)registry;
    (void)err;
    (void)alias;
    (void)path;
    return 0;
}

void ffi_load_from_config(FFIRegistry *registry, ErrStack *err,
                          const FluxaConfig *config) {
    (void)registry;
    (void)err;
    (void)config;
}

void ffi_reload_from_config(FFIRegistry *registry, ErrStack *err,
                            const FluxaConfig *config) {
    (void)registry;
    (void)err;
    (void)config;
}

Value fluxa_ffi_call(FFILib *lib, const char *symbol, ValType return_type,
                     const FfiSig *signature, Value *args, int arg_count,
                     ErrStack *err, const char *context, int string_buf_size) {
    (void)lib;
    (void)symbol;
    (void)return_type;
    (void)signature;
    (void)args;
    (void)arg_count;
    (void)err;
    (void)context;
    (void)string_buf_size;
    return val_nil();
}

void ffi_cli_list(void) {
    fprintf(stderr, "[fluxa] C FFI is unavailable in Windows minimal.\n");
}

void ffi_cli_inspect(const char *name) {
    (void)name;
    fprintf(stderr, "[fluxa] C FFI is unavailable in Windows minimal.\n");
}
