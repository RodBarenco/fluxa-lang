# std.compute — general-purpose GPU computation.
#
# Dual backend, as every stdlib here: a real Vulkan path when the loader and
# headers are present, and a stub that keeps the whole lifecycle working in
# host memory otherwise. The stub is not a placeholder — buffers, uploads,
# downloads, copies and every handle rule behave identically on it, which is
# what makes the library testable on a machine with no GPU (a CI runner, a
# container, a laptop with no ICD installed).
#
# The one thing the stub cannot do is run a kernel: executing SPIR-V without a
# device is not something a stub can honestly pretend at, so dispatch is a
# no-op and compute.version() says which backend is in use.
#
# Vulkan is opt-in rather than automatic. Linking libvulkan on a machine with
# no driver installed produces a binary that fails at compute.init() instead of
# falling back, so the default stays the stub and the real backend is asked for
# explicitly:
#
#     make FLUXA_COMPUTE_VULKAN=1 build
ifeq ($(FLUXA_BUILDTIME_COMPUTE),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_COMPUTE=1

ifeq ($(FLUXA_COMPUTE_VULKAN),1)
ifeq ($(shell pkg-config --exists vulkan 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_COMPUTE_VULKAN=1 $(shell pkg-config --cflags vulkan)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs vulkan)
else
$(warning std.compute: FLUXA_COMPUTE_VULKAN=1 but Vulkan was not found via pkg-config — using the stub backend. Install libvulkan-dev.)
endif
endif

endif
