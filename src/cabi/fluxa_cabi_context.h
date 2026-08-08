#ifndef FLUXA_CABI_CONTEXT_H
#define FLUXA_CABI_CONTEXT_H

#include "fluxa_cabi.h"

typedef struct fluxa_cabi_exchange_ctx {
    fluxa_cabi_view request;
    fluxa_cabi_message response;
    uint32_t max_frame;
} fluxa_cabi_exchange_ctx;

void fluxa_cabi_ctx_bind(fluxa_cabi_exchange_ctx *ctx);
void fluxa_cabi_ctx_unbind(void);
int  fluxa_cabi_ctx_active(void);
const fluxa_cabi_view *fluxa_cabi_ctx_request(void);
fluxa_cabi_message *fluxa_cabi_ctx_response(void);
uint32_t fluxa_cabi_ctx_max_frame(void);

#endif
