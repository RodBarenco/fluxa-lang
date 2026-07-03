# std.sound — audio playback (wav/mp3/flac) + sine tone generation
#
# Two backends:
#   Default: stub (API-complete, tracks state, no audio device — for
#            testing program logic headless and for CI)
#   make FLUXA_SOUND_MINIAUDIO=1 build — miniaudio backend
#     requires: vendor/miniaudio.h (single header, public domain)
#     vendor from: https://github.com/mackron/miniaudio
#     Backends resolved by miniaudio at runtime: ALSA/PulseAudio/JACK
#     (Linux), WASAPI (Windows), CoreAudio (macOS), sndio (BSD).
#
ifeq ($(FLUXA_BUILDTIME_SOUND),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_SOUND=1
FLUXA_EXTRA_LDFLAGS += -lpthread

ifdef FLUXA_SOUND_MINIAUDIO
  ifneq ($(wildcard vendor/miniaudio.h),)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_SOUND_MINIAUDIO=1 -Ivendor
    FLUXA_EXTRA_SRCS    += src/std/sound/fluxa_std_sound_ma.c
    FLUXA_EXTRA_LDFLAGS += -lm -ldl
  else
    $(warning std.sound: FLUXA_SOUND_MINIAUDIO=1 requested but vendor/miniaudio.h not found — using stub)
  endif
endif

endif
