/* fluxa_std_i2c.h — std.i2c: I2C protocol via Linux i2c-dev
 *
 * Uses the Linux i2c-dev kernel interface directly (no libgpiod needed
 * for basic I2C — i2c-dev is universally available on Linux embedded).
 * For RP2040: compile with PICO_SDK and use hardware_i2c instead.
 *
 * Handles: IMU reading, sensor polling, EEPROM access, DAC/ADC via I2C.
 * Bus cursor follows the standard Fluxa VAL_PTR-in-dyn cursor pattern.
 * User holds `prst dyn bus` so the handle survives hot reloads.
 *
 * API:
 *   i2c.open(str device, int addr)      → dyn  (bus cursor, e.g. "/dev/i2c-1")
 *   i2c.close(dyn bus)                  → nil
 *   i2c.write(dyn bus, int arr data)    → int  (bytes written)
 *   i2c.read(dyn bus, int nbytes)       → int arr
 *   i2c.write_reg(dyn bus, int reg, int value) → nil
 *   i2c.read_reg(dyn bus, int reg)      → int
 *   i2c.read_reg16(dyn bus, int reg)    → int  (16-bit big-endian)
 *   i2c.scan(str device)                → dyn  (list of found addresses)
 */
#ifndef FLUXA_STD_I2C_H
#define FLUXA_STD_I2C_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#endif

#include "../../scope.h"
#include "../../err.h"

/* ── Internal bus state ──────────────────────────────────────────────────── */
typedef struct {
    int fd;
    int addr;
} I2cBus;

/* ── Value constructors ──────────────────────────────────────────────────── */
static inline Value i2c_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value i2c_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */
static inline Value i2c_wrap_cursor(I2cBus *bus) {
    FluxaDyn *d   = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap         = 1; d->count = 1;
    d->items       = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = bus;
    Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
    return ret;
}

static inline I2cBus *i2c_unwrap(const Value *v, ErrStack *err,
                                   int *had_error, int line,
                                   const char *fn_name) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn ||
        v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR ||
        !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "i2c.%s: invalid bus cursor — use i2c.open() to create one", fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "i2c", line);
        *had_error = 1;
        return NULL;
    }
    return (I2cBus *)v->as.dyn->items[0].as.ptr;
}

/* ── arr helper: build Value from byte buffer ────────────────────────────── */
static inline Value i2c_bytes_to_arr(const uint8_t *buf, int n) {
    Value *items = (Value *)malloc(sizeof(Value) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        items[i].type       = VAL_INT;
        items[i].as.integer = buf[i];
    }
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap = n > 0 ? n : 1; d->count = n; d->items = items;
    Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
    return ret;
}

/* ── Main dispatch ───────────────────────────────────────────────────────── */
static inline Value fluxa_std_i2c_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "i2c.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "i2c", line); \
    *had_error = 1; return i2c_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "i2c.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "i2c", line); \
        *had_error = 1; return i2c_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        LIB_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) LIB_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

#ifndef __linux__
    /* Non-Linux stub — every call returns an error */
    (void)args; (void)argc; (void)err; (void)had_error; (void)line;
    snprintf(errbuf, sizeof(errbuf),
        "i2c.%s: i2c-dev is Linux-only. "
        "For RP2040 use the PICO_SDK hardware_i2c API directly.", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "i2c", line);
    *had_error = 1;
    return i2c_nil();
#else

    /* ── i2c.open(device, addr) → dyn ──────────────────────────────── */
    if (strcmp(fn_name, "open") == 0) {
        NEED(2); GET_STR(0, device); GET_INT(1, addr);
        int fd = open(device, O_RDWR);
        if (fd < 0) {
            { char _m[256]; snprintf(_m, sizeof(_m), "cannot open '%s'", device); LIB_ERR(_m); }
        }
        if (ioctl(fd, I2C_SLAVE, addr) < 0) {
            close(fd);
            char _m[64]; snprintf(_m, sizeof(_m), "cannot set address 0x%02lx", addr);
            LIB_ERR(_m);
        }
        I2cBus *bus = (I2cBus *)malloc(sizeof(I2cBus));
        if (!bus) { close(fd); LIB_ERR("out of memory"); }
        bus->fd = fd; bus->addr = (int)addr;
        return i2c_wrap_cursor(bus);
    }

    /* ── i2c.close(bus) → nil ───────────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        NEED(1);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        close(bus->fd);
        free(bus);
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return i2c_nil();
    }

    /* ── i2c.write(bus, int arr data) → int ────────────────────────── */
    if (strcmp(fn_name, "write") == 0) {
        NEED(2);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        if (args[1].type != VAL_DYN || !args[1].as.dyn)
            LIB_ERR("write: expected int arr as data argument");
        FluxaDyn *d = args[1].as.dyn;
        uint8_t *buf = (uint8_t *)malloc((size_t)(d->count > 0 ? d->count : 1));
        if (!buf) LIB_ERR("out of memory");
        for (int i = 0; i < d->count; i++)
            buf[i] = (uint8_t)(d->items[i].as.integer & 0xFF);
        ssize_t n = write(bus->fd, buf, (size_t)d->count);
        free(buf);
        if (n < 0) LIB_ERR("write failed");
        return i2c_int((long)n);
    }

    /* ── i2c.read(bus, nbytes) → dyn (int arr) ─────────────────────── */
    if (strcmp(fn_name, "read") == 0) {
        NEED(2);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        GET_INT(1, nbytes);
        if (nbytes <= 0 || nbytes > 4096) LIB_ERR("nbytes out of range [1,4096]");
        uint8_t *buf = (uint8_t *)malloc((size_t)nbytes);
        if (!buf) LIB_ERR("out of memory");
        ssize_t n = read(bus->fd, buf, (size_t)nbytes);
        if (n < 0) { free(buf); LIB_ERR("read failed"); }
        Value ret = i2c_bytes_to_arr(buf, (int)n);
        free(buf);
        return ret;
    }

    /* ── i2c.write_reg(bus, reg, value) → nil ──────────────────────── */
    if (strcmp(fn_name, "write_reg") == 0) {
        NEED(3);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        GET_INT(1, reg); GET_INT(2, val);
        uint8_t buf[2] = { (uint8_t)(reg & 0xFF), (uint8_t)(val & 0xFF) };
        if (write(bus->fd, buf, 2) != 2) LIB_ERR("write_reg failed");
        return i2c_nil();
    }

    /* ── i2c.read_reg(bus, reg) → int ──────────────────────────────── */
    if (strcmp(fn_name, "read_reg") == 0) {
        NEED(2);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        GET_INT(1, reg);
        uint8_t r = (uint8_t)(reg & 0xFF);
        if (write(bus->fd, &r, 1) != 1) LIB_ERR("read_reg: register write failed");
        uint8_t val = 0;
        if (read(bus->fd, &val, 1) != 1) LIB_ERR("read_reg: read failed");
        return i2c_int((long)val);
    }

    /* ── i2c.read_reg16(bus, reg) → int (big-endian 16-bit) ────────── */
    if (strcmp(fn_name, "read_reg16") == 0) {
        NEED(2);
        I2cBus *bus = i2c_unwrap(&args[0], err, had_error, line, fn_name);
        if (!bus) return i2c_nil();
        GET_INT(1, reg);
        uint8_t r = (uint8_t)(reg & 0xFF);
        if (write(bus->fd, &r, 1) != 1) LIB_ERR("read_reg16: register write failed");
        uint8_t buf[2] = {0, 0};
        if (read(bus->fd, buf, 2) != 2) LIB_ERR("read_reg16: read failed");
        return i2c_int((long)((buf[0] << 8) | buf[1]));
    }

    /* ── i2c.scan(device) → dyn (list of addresses as int) ─────────── */
    if (strcmp(fn_name, "scan") == 0) {
        NEED(1); GET_STR(0, device);
        int fd = open(device, O_RDWR);
        if (fd < 0) LIB_ERR("scan: cannot open device");
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        d->cap = 16; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        for (int addr = 0x08; addr <= 0x77; addr++) {
            if (ioctl(fd, I2C_SLAVE, (long)addr) < 0) continue;
            uint8_t probe = 0;
            if (read(fd, &probe, 1) >= 0) {
                if (d->count >= d->cap) {
                    d->cap *= 2;
                    d->items = (Value *)realloc(d->items,
                        sizeof(Value) * (size_t)d->cap);
                }
                d->items[d->count++] = i2c_int((long)addr);
            }
        }
        close(fd);
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

#undef LIB_ERR
#undef NEED
#undef GET_STR
#undef GET_INT

#endif /* __linux__ */

    snprintf(errbuf, sizeof(errbuf), "i2c.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "i2c", line);
    *had_error = 1;
    return i2c_nil();
}

#endif /* FLUXA_STD_I2C_H */
