/* fluxa_std_serial.h — std.serial: UART/serial via libserialport
 *
 * Requires: libserialport-dev  (stable since 2013, Sigrok project)
 * Fundamental for RP2040/ESP32 UART communication.
 * Port cursor follows the standard Fluxa VAL_PTR-in-dyn pattern.
 * User holds `prst dyn port` so the connection survives hot reloads.
 *
 * API:
 *   serial.list()                       → dyn  (list of port name strings)
 *   serial.open(str port, int baud)     → dyn  (port cursor)
 *   serial.close(dyn port)              → nil
 *   serial.write(dyn port, str data)    → int  (bytes written)
 *   serial.read(dyn port, int max_bytes, int timeout_ms) → str
 *   serial.readline(dyn port, int timeout_ms) → str
 *   serial.flush(dyn port)              → nil
 *   serial.bytes_available(dyn port)    → int
 */
#ifndef FLUXA_STD_SERIAL_H
#define FLUXA_STD_SERIAL_H

#include <stdlib.h>
#include <string.h>
#include <libserialport.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Value constructors ──────────────────────────────────────────────────── */
static inline Value serial_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value serial_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}
static inline Value serial_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */
static inline Value serial_wrap_cursor(struct sp_port *port) {
    FluxaDyn *d   = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap         = 1; d->count = 1;
    d->items       = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = port;
    Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
    return ret;
}

static inline struct sp_port *serial_unwrap(const Value *v, ErrStack *err,
                                             int *had_error, int line,
                                             const char *fn_name) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn ||
        v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR ||
        !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "serial.%s: invalid port cursor — use serial.open() to create one",
            fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "serial", line);
        *had_error = 1;
        return NULL;
    }
    return (struct sp_port *)v->as.dyn->items[0].as.ptr;
}

/* ── Main dispatch ───────────────────────────────────────────────────────── */
static inline Value fluxa_std_serial_call(const char *fn_name,
                                           const Value *args, int argc,
                                           ErrStack *err, int *had_error,
                                           int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "serial.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "serial", line); \
    *had_error = 1; return serial_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "serial.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "serial", line); \
        *had_error = 1; return serial_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        LIB_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) LIB_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

    /* ── serial.list() → dyn ────────────────────────────────────────── */
    if (strcmp(fn_name, "list") == 0) {
        struct sp_port **ports = NULL;
        enum sp_return rc = sp_list_ports(&ports);
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        d->cap = 8; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        if (rc == SP_OK && ports) {
            for (int i = 0; ports[i]; i++) {
                const char *name = sp_get_port_name(ports[i]);
                if (!name) continue;
                if (d->count >= d->cap) {
                    d->cap *= 2;
                    d->items = (Value *)realloc(d->items,
                        sizeof(Value) * (size_t)d->cap);
                }
                d->items[d->count++] = serial_str(name);
            }
            sp_free_port_list(ports);
        }
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

    /* ── serial.open(port_name, baud) → dyn ────────────────────────── */
    if (strcmp(fn_name, "open") == 0) {
        NEED(2); GET_STR(0, port_name); GET_INT(1, baud);
        struct sp_port *port = NULL;
        if (sp_get_port_by_name(port_name, &port) != SP_OK)
            LIB_ERR("port not found");
        if (sp_open(port, SP_MODE_READ_WRITE) != SP_OK) {
            sp_free_port(port);
            LIB_ERR("cannot open port");
        }
        sp_set_baudrate(port, (int)baud);
        sp_set_bits(port, 8);
        sp_set_parity(port, SP_PARITY_NONE);
        sp_set_stopbits(port, 1);
        sp_set_flowcontrol(port, SP_FLOWCONTROL_NONE);
        return serial_wrap_cursor(port);
    }

    /* ── serial.close(port) → nil ───────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        NEED(1);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        sp_close(port);
        sp_free_port(port);
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return serial_nil();
    }

    /* ── serial.write(port, data) → int ────────────────────────────── */
    if (strcmp(fn_name, "write") == 0) {
        NEED(2);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        GET_STR(1, data);
        int n = sp_nonblocking_write(port, data, strlen(data));
        if (n < 0) LIB_ERR("write failed");
        return serial_int((long)n);
    }

    /* ── serial.read(port, max_bytes, timeout_ms) → str ────────────── */
    if (strcmp(fn_name, "read") == 0) {
        NEED(3);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        GET_INT(1, max_bytes); GET_INT(2, timeout_ms);
        if (max_bytes <= 0 || max_bytes > 65536) LIB_ERR("max_bytes out of range [1,65536]");
        char *buf = (char *)malloc((size_t)max_bytes + 1);
        if (!buf) LIB_ERR("out of memory");
        int n = sp_blocking_read(port, buf, (size_t)max_bytes, (unsigned int)timeout_ms);
        if (n < 0) { free(buf); LIB_ERR("read failed"); }
        buf[n] = '\0';
        Value ret = serial_str(buf);
        free(buf);
        return ret;
    }

    /* ── serial.readline(port, timeout_ms) → str ───────────────────── */
    if (strcmp(fn_name, "readline") == 0) {
        NEED(2);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        GET_INT(1, timeout_ms);
        char buf[4096];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            char c;
            int n = sp_blocking_read(port, &c, 1, (unsigned int)timeout_ms);
            if (n <= 0) break;
            buf[pos++] = c;
            if (c == '\n') break;
        }
        buf[pos] = '\0';
        return serial_str(buf);
    }

    /* ── serial.flush(port) → nil ───────────────────────────────────── */
    if (strcmp(fn_name, "flush") == 0) {
        NEED(1);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        sp_flush(port, SP_BUF_BOTH);
        return serial_nil();
    }

    /* ── serial.bytes_available(port) → int ────────────────────────── */
    if (strcmp(fn_name, "bytes_available") == 0) {
        NEED(1);
        struct sp_port *port = serial_unwrap(&args[0], err, had_error, line, fn_name);
        if (!port) return serial_nil();
        int n = sp_input_waiting(port);
        return serial_int(n < 0 ? 0 : (long)n);
    }

#undef LIB_ERR
#undef NEED
#undef GET_STR
#undef GET_INT

    snprintf(errbuf, sizeof(errbuf), "serial.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "serial", line);
    *had_error = 1;
    return serial_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "serial",
    toml_key = "std.serial",
    owner    = "serial",
    call     = fluxa_std_serial_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_SERIAL_H */
