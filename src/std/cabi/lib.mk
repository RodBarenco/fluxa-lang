# std.cabi — deterministic typed bridge for the stable host C ABI.
# Base bridge is pure C99. Optional secure envelope uses libsodium only when
# std.crypto is also enabled at build time.
ifeq ($(FLUXA_BUILDTIME_CABI),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_CABI=1
FLUXA_EXTRA_SRCS   += src/cabi/fluxa_cabi_context.c src/cabi/fluxa_cabi_wire.c
ifeq ($(FLUXA_BUILDTIME_CRYPTO),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_CABI_SODIUM=1
endif
endif
