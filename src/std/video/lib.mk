# std.video — MP4/H.264 write and read.
#
# No external dependency: the codec is vendored under vendor/video and is plain
# C99. minimp4 and minih264e are CC0 (public domain); h264bsd is Apache-2.0,
# extracted from AOSP. See the LICENSE.* files in that directory.
#
# minimp4 and minih264e both define `bs_t` and `nal_put_esc`, so their
# implementations live in two separate .c files rather than being expanded into
# the same translation unit.
#
# The vendored sources are compiled without -pedantic: they are third-party
# code, and warnings there are not ours to fix. Everything Fluxa owns keeps the
# strict flags, so the zero-warning gate still means what it says.
ifeq ($(FLUXA_BUILDTIME_VIDEO),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_VIDEO=1 -Ivendor/video -Ivendor/video/h264bsd
FLUXA_EXTRA_SRCS   += vendor/video/fluxa_video_mux.c \
                      vendor/video/fluxa_video_enc.c \
                      vendor/video/fluxa_video_dec.c
FLUXA_EXTRA_LDFLAGS += -lm
endif
