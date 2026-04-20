# std.flxthread — pthreads (already in LDFLAGS via -lpthread)
ifeq ($(FLUXA_BUILDTIME_FLXTHREAD),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_FLXTHREAD=1
endif
