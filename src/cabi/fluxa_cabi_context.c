#include "fluxa_cabi_context.h"

static fluxa_cabi_exchange_ctx *g_ctx = NULL;

void fluxa_cabi_ctx_bind(fluxa_cabi_exchange_ctx *ctx) { g_ctx = ctx; }
void fluxa_cabi_ctx_unbind(void) { g_ctx = NULL; }
int fluxa_cabi_ctx_active(void) { return g_ctx != NULL; }
const fluxa_cabi_view *fluxa_cabi_ctx_request(void) { return g_ctx ? &g_ctx->request : NULL; }
fluxa_cabi_message *fluxa_cabi_ctx_response(void) { return g_ctx ? &g_ctx->response : NULL; }
uint32_t fluxa_cabi_ctx_max_frame(void) { return g_ctx ? g_ctx->max_frame : 0; }
