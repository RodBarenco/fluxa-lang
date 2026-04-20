# std.json — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_JSON),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_JSON=1
endif
