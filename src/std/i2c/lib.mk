# std.i2c — Linux i2c-dev (always enabled; stub on non-Linux)
ifeq ($(FLUXA_BUILDTIME_I2C),1)
FLUXA_EXTRA_CFLAGS += -DFLUXA_STD_I2C=1
endif
