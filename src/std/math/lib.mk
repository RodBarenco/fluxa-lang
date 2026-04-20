# std.math — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_MATH),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_MATH=1
endif
