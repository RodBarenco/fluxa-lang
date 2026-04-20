# std.libv — N-dimensional vectors, matrices, tensors (pure C99, zero deps)
ifeq ($(FLUXA_BUILDTIME_LIBV),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_LIBV=1
FLUXA_EXTRA_LDFLAGS += -lm
endif
