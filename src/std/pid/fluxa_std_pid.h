/* fluxa_std_pid.h — std.pid: PID controller
 *
 * Pure C99, zero external dependencies. Embedded-friendly (RP2040, ESP32).
 * State (integral, prev_error, last_time_ms) is inside a heap-allocated
 * PidCtrl struct, wrapped in VAL_PTR inside a single-element dyn — the
 * standard Fluxa cursor pattern. User holds `prst dyn ctrl` so state
 * survives hot reloads.
 *
 * API:
 *   pid.new(float kp, float ki, float kd)       → dyn  (controller cursor)
 *   pid.set_limits(dyn, float min, float max)   → nil  (output clamp + anti-windup)
 *   pid.set_deadband(dyn, float band)            → nil  (ignore error < band)
 *   pid.compute(dyn, float setpoint, float pv)  → float (control output)
 *   pid.reset(dyn)                               → nil  (zero integral + prev_error)
 *   pid.state(dyn)                               → dyn  [kp, ki, kd, integral,
 *                                                        prev_err, out_min, out_max]
 */
#ifndef FLUXA_STD_PID_H
#define FLUXA_STD_PID_H

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Internal controller state ──────────────────────────────────────────── */
typedef struct {
    double kp, ki, kd;
    double integral;
    double prev_error;
    double out_min, out_max;   /* output clamp; out_min==out_max → no clamp */
    double deadband;           /* ignore |error| < deadband                  */
} PidCtrl;

/* ── Value constructors ──────────────────────────────────────────────────── */
static inline Value pid_float(double d) {
    Value v; v.type = VAL_FLOAT; v.as.real = d; return v;
}
static inline Value pid_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */
static inline Value pid_wrap_cursor(PidCtrl *c) {
    FluxaDyn *d   = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap         = 1; d->count = 1;
    d->items       = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = c;
    Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
    return ret;
}

static inline PidCtrl *pid_unwrap(const Value *v, ErrStack *err,
                                   int *had_error, int line,
                                   const char *fn_name) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn ||
        v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR ||
        !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "pid.%s: invalid controller — use pid.new() to create one", fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "pid", line);
        *had_error = 1;
        return NULL;
    }
    return (PidCtrl *)v->as.dyn->items[0].as.ptr;
}

/* ── Main dispatch ───────────────────────────────────────────────────────── */
static inline Value fluxa_std_pid_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "pid.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "pid", line); \
    *had_error = 1; return pid_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "pid.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "pid", line); \
        *had_error = 1; return pid_nil(); \
    } \
} while(0)

#define GET_FLOAT(idx, var) \
    double var; \
    if      (args[(idx)].type == VAL_FLOAT) var = args[(idx)].as.real; \
    else if (args[(idx)].type == VAL_INT)   var = (double)args[(idx)].as.integer; \
    else LIB_ERR("expected float argument");

    /* ── pid.new(kp, ki, kd) → dyn ────────────────────────────────── */
    if (strcmp(fn_name, "new") == 0) {
        NEED(3);
        GET_FLOAT(0, kp); GET_FLOAT(1, ki); GET_FLOAT(2, kd);
        PidCtrl *c = (PidCtrl *)calloc(1, sizeof(PidCtrl));
        if (!c) LIB_ERR("out of memory");
        c->kp = kp; c->ki = ki; c->kd = kd;
        c->out_min = c->out_max = 0.0;  /* 0==0 means no clamp */
        c->deadband = 0.0;
        return pid_wrap_cursor(c);
    }

    /* ── pid.set_limits(ctrl, min, max) → nil ──────────────────────── */
    if (strcmp(fn_name, "set_limits") == 0) {
        NEED(3);
        PidCtrl *c = pid_unwrap(&args[0], err, had_error, line, fn_name);
        if (!c) return pid_nil();
        GET_FLOAT(1, mn); GET_FLOAT(2, mx);
        if (mn >= mx) LIB_ERR("set_limits: min must be < max");
        c->out_min = mn; c->out_max = mx;
        return pid_nil();
    }

    /* ── pid.set_deadband(ctrl, band) → nil ────────────────────────── */
    if (strcmp(fn_name, "set_deadband") == 0) {
        NEED(2);
        PidCtrl *c = pid_unwrap(&args[0], err, had_error, line, fn_name);
        if (!c) return pid_nil();
        GET_FLOAT(1, band);
        if (band < 0.0) LIB_ERR("set_deadband: band must be >= 0");
        c->deadband = band;
        return pid_nil();
    }

    /* ── pid.compute(ctrl, setpoint, pv) → float ───────────────────── */
    if (strcmp(fn_name, "compute") == 0) {
        NEED(3);
        PidCtrl *c = pid_unwrap(&args[0], err, had_error, line, fn_name);
        if (!c) return pid_nil();
        GET_FLOAT(1, setpoint); GET_FLOAT(2, pv);

        double error = setpoint - pv;

        /* Deadband: treat small errors as zero */
        if (c->deadband > 0.0 && fabs(error) < c->deadband)
            error = 0.0;

        double p_term = c->kp * error;
        c->integral  += c->ki * error;
        double d_term  = c->kd * (error - c->prev_error);
        c->prev_error  = error;

        double output = p_term + c->integral + d_term;

        /* Output clamp + anti-windup */
        if (c->out_min != c->out_max) {
            if (output > c->out_max) {
                /* Back-calculate integral to prevent windup */
                c->integral -= output - c->out_max;
                output = c->out_max;
            } else if (output < c->out_min) {
                c->integral -= output - c->out_min;
                output = c->out_min;
            }
        }

        return pid_float(output);
    }

    /* ── pid.reset(ctrl) → nil ─────────────────────────────────────── */
    if (strcmp(fn_name, "reset") == 0) {
        NEED(1);
        PidCtrl *c = pid_unwrap(&args[0], err, had_error, line, fn_name);
        if (!c) return pid_nil();
        c->integral   = 0.0;
        c->prev_error = 0.0;
        return pid_nil();
    }

    /* ── pid.state(ctrl) → dyn [kp,ki,kd,integral,prev_err,min,max] ── */
    if (strcmp(fn_name, "state") == 0) {
        NEED(1);
        PidCtrl *c = pid_unwrap(&args[0], err, had_error, line, fn_name);
        if (!c) return pid_nil();
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) LIB_ERR("out of memory");
        d->cap   = 7; d->count = 7;
        d->items = (Value *)malloc(sizeof(Value) * 7);
        if (!d->items) { free(d); LIB_ERR("out of memory"); }
        d->items[0] = pid_float(c->kp);
        d->items[1] = pid_float(c->ki);
        d->items[2] = pid_float(c->kd);
        d->items[3] = pid_float(c->integral);
        d->items[4] = pid_float(c->prev_error);
        d->items[5] = pid_float(c->out_min);
        d->items[6] = pid_float(c->out_max);
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

#undef LIB_ERR
#undef NEED
#undef GET_FLOAT

    snprintf(errbuf, sizeof(errbuf), "pid.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "pid", line);
    *had_error = 1;
    return pid_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "pid",
    toml_key = "std.pid",
    owner    = "pid",
    call     = fluxa_std_pid_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_PID_H */
