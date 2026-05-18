# std.pg — PostgreSQL client via libpq
# Dual-backend: full implementation when libpq is available,
# stub (clear error at call time) otherwise.
#
# Default build:    auto-detects libpq via pkg-config
# Explicit enable:  make FLUXA_PG_LIBPQ=1 build
# Explicit disable: make FLUXA_PG_LIBPQ=0 build  (forces stub)
#
# Requires: libpq-dev (PostgreSQL client library)
#   apt install libpq-dev
#   brew install libpq
ifeq ($(FLUXA_BUILDTIME_PG),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_PG=1

ifndef FLUXA_PG_LIBPQ
  # Auto-detect via pkg-config
  FLUXA_PG_LIBPQ := $(shell pkg-config --exists libpq 2>/dev/null && echo 1 || echo 0)
endif

ifeq ($(FLUXA_PG_LIBPQ),1)
  FLUXA_EXTRA_CFLAGS  += -DFLUXA_PG_LIBPQ=1 $(shell pkg-config --cflags libpq)
  FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libpq)
else
  $(warning std.pg: libpq not found — using stub backend. Install libpq-dev and rebuild.)
endif

endif
