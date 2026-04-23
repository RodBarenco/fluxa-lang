# std.zlib — compression via zlib (deflate, gzip, crc32, adler32)
ifeq ($(FLUXA_BUILDTIME_ZLIB),1)
ifeq ($(shell pkg-config --exists zlib 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_ZLIB=1 $(shell pkg-config --cflags zlib)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs zlib)
endif
endif
