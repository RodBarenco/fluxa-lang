# std.https — HTTPS client (libcurl, TLS enforced)
ifeq ($(FLUXA_BUILDTIME_HTTPS),1)
ifeq ($(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_HTTPS=1 $(shell pkg-config --cflags libcurl)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libcurl)
endif
endif
