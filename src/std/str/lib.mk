# std.strings — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_STRINGS),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_STRINGS=1
endif
