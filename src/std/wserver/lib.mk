# std.wserver — Resilient HTTP server via libmicrohttpd
# Dual-backend: full implementation when libmicrohttpd is available,
# stub (clear error at call time) otherwise.
#
# Default build:    auto-detects libmicrohttpd via pkg-config
# Explicit enable:  make FLUXA_WSERVER_MHD=1 build
# Explicit disable: make FLUXA_WSERVER_MHD=0 build  (forces stub)
#
# Requires: libmicrohttpd-dev
#   apt install libmicrohttpd-dev
#   brew install libmicrohttpd
ifeq ($(FLUXA_BUILDTIME_WSERVER),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_WSERVER=1

ifndef FLUXA_WSERVER_MHD
  # Auto-detect via pkg-config
  FLUXA_WSERVER_MHD := $(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo 1 || echo 0)
endif

ifeq ($(FLUXA_WSERVER_MHD),1)
  FLUXA_EXTRA_CFLAGS  += -DFLUXA_WSERVER_MHD=1 $(shell pkg-config --cflags libmicrohttpd)
  FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libmicrohttpd) -lpthread
else
  $(warning std.wserver: libmicrohttpd not found — using stub backend. Install libmicrohttpd-dev and rebuild.)
endif

endif
