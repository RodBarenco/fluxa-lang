# std.infer — local LLM inference
#
# Two backends:
#   Default: stub (API-complete, no-op, zero deps — for testing prompt pipelines)
#   make FLUXA_INFER_LLAMA=1 build — llama.cpp backend
#     requires: vendor/llama.h + vendor/libllama.a (C API)
#     vendor from: https://github.com/ggerganov/llama.cpp/releases
#     Note: llama.cpp requires a C++ linker even with C API
#
ifeq ($(FLUXA_BUILDTIME_INFER),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_INFER=1

ifdef FLUXA_INFER_LLAMA
  ifneq ($(wildcard vendor/llama.h),)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_INFER_LLAMA=1 -Ivendor
    FLUXA_EXTRA_LDFLAGS += vendor/libllama.a -lstdc++ -lm
  else ifeq ($(shell pkg-config --exists llama 2>/dev/null && echo 1 || echo 0),1)
    FLUXA_EXTRA_CFLAGS  += -DFLUXA_INFER_LLAMA=1 $(shell pkg-config --cflags llama)
    FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs llama) -lstdc++
  else
    $(warning std.infer: FLUXA_INFER_LLAMA=1 requested but llama.cpp not found — using stub)
  endif
endif

endif
