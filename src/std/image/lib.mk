# std.image — image encode/decode/transform (PNG/JPG/BMP/TGA/QOI)
#
# Two backends:
#   Default: stub (API-complete; new/size/free/resize work, save/load report a
#            clear "no codec" error — for testing logic without the encoder)
#   make FLUXA_IMAGE_RAYLIB=1 build — Raylib codec backend
#     Raylib bundles stb_image / stb_image_write, so PNG/JPG/BMP/TGA/QOI encode
#     and decode need no extra dependency beyond raylib itself.
#     requires: vendor/raylib.h + vendor/libraylib.a (or system raylib)
#     vendor from: https://github.com/raysan5/raylib/releases
#
# Note: when std.graph is already built with FLUXA_GRAPH_RAYLIB=1, raylib is
# linked once and shared — building std.image with FLUXA_IMAGE_RAYLIB=1 adds no
# new library, only enables the codec calls in this lib.
#
ifeq ($(FLUXA_BUILDTIME_IMAGE),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_IMAGE=1

ifdef FLUXA_IMAGE_RAYLIB
  # Check vendor first, then system
  ifneq ($(wildcard vendor/raylib.h),)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_IMAGE_RAYLIB=1 -Ivendor
    FLUXA_EXTRA_LDFLAGS += vendor/libraylib.a -lm -lpthread -ldl
  else ifeq ($(shell pkg-config --exists raylib 2>/dev/null && echo 1 || echo 0),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_IMAGE_RAYLIB=1 $(shell pkg-config --cflags raylib)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs raylib)
  else
    $(warning std.image: FLUXA_IMAGE_RAYLIB=1 requested but raylib not found — using stub)
  endif
endif

endif
