# std.mcps — MCP client over HTTPS (TLS enforced)
ifeq ($(FLUXA_BUILDTIME_MCPS),1)
ifeq ($(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_MCPS=1 $(shell pkg-config --cflags libcurl)
ifneq ($(FLUXA_BUILDTIME_HTTPC)$(FLUXA_BUILDTIME_HTTPS)$(FLUXA_BUILDTIME_MCPC),111)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libcurl)
endif
endif
endif
