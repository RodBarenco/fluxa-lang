# std.csv — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_CSV),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_CSV=1
endif
