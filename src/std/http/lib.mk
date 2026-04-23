# std.http — HTTP server + client via mongoose (vendored in vendor/)
ifeq ($(FLUXA_BUILDTIME_HTTP),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_HTTP=1 -D_GNU_SOURCE -Ivendor
FLUXA_MG_GNU_SOURCE := 1
# mongoose.c compiled as part of the build
FLUXA_EXTRA_SRCS    += vendor/mongoose.c
endif
