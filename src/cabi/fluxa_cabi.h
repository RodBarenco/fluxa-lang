#ifndef FLUXA_CABI_H
#define FLUXA_CABI_H

/*
 * Fluxa-lang C ABI v1 — deterministic typed communication bridge.
 *
 * Public contract:
 *   - transports values only: int, float, bool, str, homogeneous arr<T>
 *   - deterministic clear-wire encoding on every platform
 *   - no Fluxa runtime state crosses the boundary
 *   - no prst/dyn/Block/pointer/GC/handover/snapshot protocol
 *   - optional authenticated encryption is an envelope around the exact same
 *     deterministic clear frame; it never changes value encoding
 */

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility has three cases on Windows, not two.
 *
 *   FLUXA_CABI_BUILD   producing fluxa_cabi.dll          → dllexport
 *   (neither)          a host language consuming the DLL → dllimport
 *   FLUXA_CABI_STATIC  wire.c/context.c compiled straight
 *                      into fluxa.exe to provide std.cabi → no decoration
 *
 * The third case is what the runtime executable does: `std.cabi = true` puts
 * the wire sources in the same link as runtime.c, so nothing is imported from
 * anywhere. Marking those definitions dllimport is what produced
 * `undefined reference to __imp_fluxa_cabi_add_str_arr`: MinGW emits the
 * definition under its plain name while every caller asks for the __imp_
 * thunk of a DLL that is not being linked. POSIX never showed the fault
 * because visibility("default") does not rename the symbol.
 */
#if defined(_WIN32) && !defined(FLUXA_CABI_STATIC)
#  if defined(FLUXA_CABI_BUILD)
#    define FLUXA_CABI_API __declspec(dllexport)
#  else
#    define FLUXA_CABI_API __declspec(dllimport)
#  endif
#elif defined(_WIN32)
#  define FLUXA_CABI_API
#else
#  define FLUXA_CABI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FLUXA_CABI_ABI_MAJOR 1u
#define FLUXA_CABI_ABI_MINOR 0u
#define FLUXA_CABI_ABI_PATCH 0u
#define FLUXA_CABI_ABI_VERSION \
    ((FLUXA_CABI_ABI_MAJOR << 16) | (FLUXA_CABI_ABI_MINOR << 8) | FLUXA_CABI_ABI_PATCH)

#define FLUXA_CABI_WIRE_VERSION 1u
#define FLUXA_CABI_MAX_FRAME_DEFAULT (64u * 1024u * 1024u)
#define FLUXA_CABI_KEY_BYTES 32u

typedef struct fluxa_cabi_runtime fluxa_cabi_runtime;

typedef enum fluxa_cabi_result {
    FLUXA_CABI_OK        = 0,
    FLUXA_CABI_EINVAL    = 1,
    FLUXA_CABI_ENOMEM    = 2,
    FLUXA_CABI_EIO       = 3,
    FLUXA_CABI_EPARSE    = 4,
    FLUXA_CABI_ERESOLVE  = 5,
    FLUXA_CABI_ERUNTIME  = 6,
    FLUXA_CABI_EOVERSIZE = 7,
    FLUXA_CABI_EABI      = 8,
    FLUXA_CABI_EBUSY     = 9,
    FLUXA_CABI_EWIRE     = 10,
    FLUXA_CABI_ETYPE     = 11,
    FLUXA_CABI_ESECURITY = 12
} fluxa_cabi_result;

/* These are the ONLY semantic value tags in the v1 protocol. */
typedef enum fluxa_cabi_type {
    FLUXA_CABI_INT       = 1,
    FLUXA_CABI_FLOAT     = 2,
    FLUXA_CABI_BOOL      = 3,
    FLUXA_CABI_STR       = 4,
    FLUXA_CABI_ARR_INT   = 5,
    FLUXA_CABI_ARR_FLOAT = 6,
    FLUXA_CABI_ARR_BOOL  = 7,
    FLUXA_CABI_ARR_STR   = 8
} fluxa_cabi_type;

typedef struct fluxa_cabi_error {
    uint32_t code;
    int32_t  line;
    char     context[128];
    char     message[1024];
} fluxa_cabi_error;

/* Owned mutable message produced by the codec. */
typedef struct fluxa_cabi_message {
    void    *data;
    uint32_t size;
    uint32_t capacity;
} fluxa_cabi_message;

/* Borrowed immutable frame view. */
typedef struct fluxa_cabi_view {
    const void *data;
    uint32_t    size;
} fluxa_cabi_view;

typedef struct fluxa_cabi_str_view {
    const char *data;
    uint32_t    size;
} fluxa_cabi_str_view;

typedef struct fluxa_cabi_config {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *entry_path;
    const char *config_path;
    const char *dispatch_fn;       /* NULL => cabi_dispatch */
    uint32_t max_frame_bytes;      /* 0 => FLUXA_CABI_MAX_FRAME_DEFAULT */
    uint32_t flags;                /* reserved; must be 0 */
} fluxa_cabi_config;

/* ABI / lifecycle */
FLUXA_CABI_API uint32_t fluxa_cabi_abi_version(void);
FLUXA_CABI_API const char *fluxa_cabi_version_string(void);
FLUXA_CABI_API int fluxa_cabi_open(const fluxa_cabi_config *config,
                                    fluxa_cabi_runtime **out_runtime,
                                    fluxa_cabi_error *error);
FLUXA_CABI_API void fluxa_cabi_close(fluxa_cabi_runtime *runtime);

/*
 * Direct typed exchange.
 * `request` must be one valid deterministic FXCB frame.
 * `response` is borrowed until the next exchange on the same runtime.
 */
FLUXA_CABI_API int fluxa_cabi_exchange(fluxa_cabi_runtime *runtime,
                                        const fluxa_cabi_view *request,
                                        fluxa_cabi_view *response,
                                        fluxa_cabi_error *error);

/* Deterministic clear-wire message builder. */
FLUXA_CABI_API void fluxa_cabi_message_init(fluxa_cabi_message *message);
FLUXA_CABI_API void fluxa_cabi_message_reset(fluxa_cabi_message *message);
FLUXA_CABI_API void fluxa_cabi_message_free(fluxa_cabi_message *message);
FLUXA_CABI_API int fluxa_cabi_add_int(fluxa_cabi_message *message, int32_t value);
FLUXA_CABI_API int fluxa_cabi_add_float(fluxa_cabi_message *message, double value);
FLUXA_CABI_API int fluxa_cabi_add_bool(fluxa_cabi_message *message, int value);
FLUXA_CABI_API int fluxa_cabi_add_str(fluxa_cabi_message *message,
                                      const char *data, uint32_t size);
FLUXA_CABI_API int fluxa_cabi_add_int_arr(fluxa_cabi_message *message,
                                          const int32_t *values, uint32_t count);
FLUXA_CABI_API int fluxa_cabi_add_float_arr(fluxa_cabi_message *message,
                                            const double *values, uint32_t count);
FLUXA_CABI_API int fluxa_cabi_add_bool_arr(fluxa_cabi_message *message,
                                           const uint8_t *values, uint32_t count);
FLUXA_CABI_API int fluxa_cabi_add_str_arr(fluxa_cabi_message *message,
                                          const fluxa_cabi_str_view *values,
                                          uint32_t count);

/* Deterministic clear-wire reader. No returned pointer outlives the frame. */
FLUXA_CABI_API int fluxa_cabi_view_validate(const fluxa_cabi_view *view);
FLUXA_CABI_API uint32_t fluxa_cabi_value_count(const fluxa_cabi_view *view);
FLUXA_CABI_API int fluxa_cabi_value_type(const fluxa_cabi_view *view,
                                         uint32_t index,
                                         fluxa_cabi_type *out_type);
FLUXA_CABI_API int fluxa_cabi_get_int(const fluxa_cabi_view *view,
                                      uint32_t index, int32_t *out);
FLUXA_CABI_API int fluxa_cabi_get_float(const fluxa_cabi_view *view,
                                        uint32_t index, double *out);
FLUXA_CABI_API int fluxa_cabi_get_bool(const fluxa_cabi_view *view,
                                       uint32_t index, int *out);
FLUXA_CABI_API int fluxa_cabi_get_str(const fluxa_cabi_view *view,
                                      uint32_t index, fluxa_cabi_str_view *out);
FLUXA_CABI_API int fluxa_cabi_get_arr_count(const fluxa_cabi_view *view,
                                            uint32_t index, uint32_t *out_count);
FLUXA_CABI_API int fluxa_cabi_get_int_arr_value(const fluxa_cabi_view *view,
                                                uint32_t index, uint32_t element,
                                                int32_t *out);
FLUXA_CABI_API int fluxa_cabi_get_float_arr_value(const fluxa_cabi_view *view,
                                                  uint32_t index, uint32_t element,
                                                  double *out);
FLUXA_CABI_API int fluxa_cabi_get_bool_arr_value(const fluxa_cabi_view *view,
                                                 uint32_t index, uint32_t element,
                                                 int *out);
FLUXA_CABI_API int fluxa_cabi_get_str_arr_value(const fluxa_cabi_view *view,
                                                uint32_t index, uint32_t element,
                                                fluxa_cabi_str_view *out);

/*
 * Optional authenticated-encryption envelope.
 * Available when std.crypto/libsodium is compiled with std.cabi.
 * Clear FXCB bytes remain deterministic; sealed FXCS bytes intentionally are
 * not deterministic because every seal uses a fresh XChaCha20-Poly1305 nonce.
 */
FLUXA_CABI_API int fluxa_cabi_security_available(void);
FLUXA_CABI_API int fluxa_cabi_seal(const uint8_t key[FLUXA_CABI_KEY_BYTES],
                                   const fluxa_cabi_view *clear,
                                   fluxa_cabi_message *sealed,
                                   fluxa_cabi_error *error);
FLUXA_CABI_API int fluxa_cabi_unseal(const uint8_t key[FLUXA_CABI_KEY_BYTES],
                                     const fluxa_cabi_view *sealed,
                                     fluxa_cabi_message *clear,
                                     fluxa_cabi_error *error);

/* Convenience: authenticate/decrypt request, run the same typed exchange, then seal response. */
FLUXA_CABI_API int fluxa_cabi_exchange_sealed(fluxa_cabi_runtime *runtime,
                                               const uint8_t key[FLUXA_CABI_KEY_BYTES],
                                               const fluxa_cabi_view *sealed_request,
                                               fluxa_cabi_message *sealed_response,
                                               fluxa_cabi_error *error);

#ifdef __cplusplus
}
#endif

#endif /* FLUXA_CABI_H */
