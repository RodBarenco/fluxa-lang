# std.pid — pure C, no external deps
ifeq ($(FLUXA_BUILDTIME_PID),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_PID=1
endif
