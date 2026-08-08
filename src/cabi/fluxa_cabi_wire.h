#ifndef FLUXA_CABI_WIRE_H
#define FLUXA_CABI_WIRE_H
#include "fluxa_cabi.h"

/* Internal helpers shared by the host ABI and std.cabi. */
int fxcabi_wire_add_tagged_raw(fluxa_cabi_message *m, uint8_t tag,
                               const void *payload, uint32_t payload_size);
int fxcabi_wire_locate(const fluxa_cabi_view *view, uint32_t index,
                       fluxa_cabi_type *type,
                       const unsigned char **payload,
                       uint32_t *payload_size,
                       uint32_t *array_count);
#endif
