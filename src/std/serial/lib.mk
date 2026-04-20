# std.serial — libserialport (optional: only if pkg available AND enabled in fluxa.libs)
ifeq ($(FLUXA_BUILDTIME_SERIAL),1)
ifeq ($(shell pkg-config --exists libserialport 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_SERIAL=1  $(shell pkg-config --cflags libserialport)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libserialport)
endif
endif
