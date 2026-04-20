# std.crypto — libsodium (optional: only if pkg available AND enabled in fluxa.libs)
ifeq ($(FLUXA_BUILDTIME_CRYPTO),1)
ifeq ($(shell pkg-config --exists libsodium 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_CRYPTO=1
FLUXA_EXTRA_LDFLAGS += -lsodium
endif
endif
