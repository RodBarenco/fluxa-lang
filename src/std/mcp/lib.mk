# std.mcp — Fluxa as MCP server via mongoose
# Depends on std.http (shares mongoose.c — only compiled once)
ifeq ($(FLUXA_BUILDTIME_MCP),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_MCP=1 -D_GNU_SOURCE -Ivendor
# mongoose.c only added if http didn't add it already
ifneq ($(FLUXA_BUILDTIME_HTTP),1)
FLUXA_EXTRA_SRCS   += vendor/mongoose.c
endif
endif
