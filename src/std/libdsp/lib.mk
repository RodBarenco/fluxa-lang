# std.libdsp — DSP and radar math
# Native backend: pure C99 Cooley-Tukey FFT (always compiled)
# FFTW backend: optional, enabled when libfftw3 is present
# Set [libs.libdsp] backend = "fftw" in fluxa.toml to use FFTW
ifeq ($(FLUXA_BUILDTIME_LIBDSP),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_LIBDSP=1
FLUXA_EXTRA_LDFLAGS += -lm
# FFTW: opt-in — compiled if available, selected via fluxa.toml at runtime
ifeq ($(shell pkg-config --exists fftw3 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_LIBDSP_FFTW=1 $(shell pkg-config --cflags fftw3)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs fftw3)
endif
endif
