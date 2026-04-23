# std.websocket — WebSocket client
#
# Two backends (selected at compile time):
#
#   Default: pure C99 native (POSIX sockets, RFC 6455, no deps, ws:// only)
#   make FLUXA_WS_LWS=1 build  — libwebsockets backend (wss:// TLS support)
#     requires: libssl-dev libwebsockets-dev
#
ifeq ($(FLUXA_BUILDTIME_WEBSOCKET),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_WEBSOCKET=1

ifdef FLUXA_WS_LWS
  ifeq ($(shell pkg-config --exists libwebsockets 2>/dev/null && echo 1 || echo 0),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_WS_LIBWEBSOCKETS=1 $(shell pkg-config --cflags libwebsockets)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libwebsockets)
  else
    $(warning std.websocket: FLUXA_WS_LWS=1 requested but libwebsockets not found — using native backend)
  endif
endif

endif
