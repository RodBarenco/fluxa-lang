# std.mcp — MCP client (depends on libcurl via std.http)
ifeq ($(FLUXA_BUILDTIME_MCP),1)
ifeq ($(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_MCP=1 $(shell pkg-config --cflags libcurl)
# libcurl already added by std.http/lib.mk if both enabled — no dup link needed
# but we guard in case http is disabled and mcp is enabled standalone
ifneq ($(FLUXA_BUILDTIME_HTTP),1)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libcurl)
endif
endif
endif
