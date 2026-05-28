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
  # Auto-detect: try pkg-config first, then fall back to header presence
  _MHD_PC := $(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo 1 || echo 0)
  ifeq ($(_MHD_PC),1)
    FLUXA_WSERVER_MHD := 1
  else
    # pkg-config path may be incomplete on some distros — check header directly
    FLUXA_WSERVER_MHD := $(shell test -f /usr/include/microhttpd.h && echo 1 || echo 0)
  endif
endif

ifeq ($(FLUXA_WSERVER_MHD),1)
  _MHD_PC2 := $(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo 1 || echo 0)
  ifeq ($(_MHD_PC2),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_WSERVER_MHD=1 $(shell pkg-config --cflags libmicrohttpd)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libmicrohttpd) -lpthread
  else
    # Direct link — header found but no .pc file
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_WSERVER_MHD=1
    FLUXA_EXTRA_LDFLAGS += -lmicrohttpd -lpthread
  endif
else
  $(warning std.wserver: libmicrohttpd not found — using stub backend. Install libmicrohttpd-dev and rebuild.)
endif

endif
