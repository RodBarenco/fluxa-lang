# std.graph — 2D/3D graphics
#
# Two backends:
#   Default: stub (API-complete, no-op, zero deps — for testing logic without display)
#   make FLUXA_GRAPH_RAYLIB=1 build — Raylib backend
#     requires: vendor/raylib.h + vendor/libraylib.a (or system raylib)
#     vendor from: https://github.com/raysan5/raylib/releases
#
ifeq ($(FLUXA_BUILDTIME_GRAPH),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_GRAPH=1

ifdef FLUXA_GRAPH_RAYLIB
  # Check vendor first, then system
  ifneq ($(wildcard vendor/raylib.h),)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_GRAPH_RAYLIB=1 -Ivendor
    FLUXA_EXTRA_LDFLAGS += vendor/libraylib.a -lm -lpthread -ldl
  else ifeq ($(shell pkg-config --exists raylib 2>/dev/null && echo 1 || echo 0),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_GRAPH_RAYLIB=1 $(shell pkg-config --cflags raylib)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs raylib)
  else
    $(warning std.graph: FLUXA_GRAPH_RAYLIB=1 requested but raylib not found — using stub)
  endif
endif

endif
