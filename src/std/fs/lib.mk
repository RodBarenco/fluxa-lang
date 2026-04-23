# std.fs — filesystem operations (POSIX, pure C99)
ifeq ($(FLUXA_BUILDTIME_FS),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_FS=1
endif
