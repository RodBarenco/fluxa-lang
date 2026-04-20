# std.time — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_TIME),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_TIME=1
endif
