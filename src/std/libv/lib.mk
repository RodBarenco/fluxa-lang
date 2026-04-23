# std.libv — vectors, matrices, tensors
# Native backend: pure C99 (always compiled)
# BLAS backend: optional, enabled when OpenBLAS/CBLAS is present
# Set [libs.libv] backend = "blas" in fluxa.toml to use OpenBLAS for matmul
ifeq ($(FLUXA_BUILDTIME_LIBV),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_LIBV=1
FLUXA_EXTRA_LDFLAGS += -lm
# OpenBLAS: opt-in — compiled if available, selected via fluxa.toml at runtime
ifeq ($(shell pkg-config --exists openblas 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_LIBV_BLAS=1 $(shell pkg-config --cflags openblas)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs openblas)
endif
endif
