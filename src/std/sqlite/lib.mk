# std.sqlite — sqlite3 (optional: only if pkg available AND enabled in fluxa.libs)
ifeq ($(FLUXA_BUILDTIME_SQLITE),1)
ifeq ($(shell pkg-config --exists sqlite3 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_SQLITE=1  $(shell pkg-config --cflags sqlite3)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs sqlite3)
endif
endif
