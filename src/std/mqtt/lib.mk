# std.mqtt — MQTT client via libmosquitto
ifeq ($(FLUXA_BUILDTIME_MQTT),1)
ifeq ($(shell pkg-config --exists libmosquitto 2>/dev/null && echo 1 || echo 0),1)
FLUXA_EXTRA_CFLAGS  += -DFLUXA_STD_MQTT=1 $(shell pkg-config --cflags libmosquitto)
FLUXA_EXTRA_LDFLAGS += $(shell pkg-config --libs libmosquitto)
endif
endif
