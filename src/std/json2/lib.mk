# std.json2 — full DOM JSON parser (pure C99, zero deps)
ifeq ($(FLUXA_BUILDTIME_JSON2),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_JSON2=1
endif
