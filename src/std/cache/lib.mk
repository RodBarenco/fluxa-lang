# std.cache — thread-safe k/v cache. pthread already linked by runtime.
ifeq ($(FLUXA_BUILDTIME_CACHE),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_CACHE=1
endif
