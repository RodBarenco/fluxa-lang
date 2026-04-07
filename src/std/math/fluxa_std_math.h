/* fluxa_std_math.h — Fluxa Standard Library: math
 *
 * Provides mathematical functions callable without a danger block.
 * Compiled into the binary only when FLUXA_STD_MATH is defined (i.e.
 * std.math is declared in [libs] of fluxa.toml).
 *
 * All functions operate on Fluxa Value types and return Value.
 * Integer arguments are promoted to float where the C function requires
 * double. Results are always VAL_FLOAT unless noted otherwise.
 *
 * Error handling:
 *   - Domain errors (sqrt(-1), log(-1), etc.) return val_float(0.0) and
 *     set had_error so the caller gets a clear runtime error with line number.
 *   - Outside danger: aborts execution.
 *   - Inside danger: captured in err_stack, execution continues.
 *
 * Constants exposed as functions (no arguments):
 *   math.pi()   → 3.14159265358979323846
 *   math.e()    → 2.71828182845904523536
 *   math.inf()  → INFINITY
 *
 * Usage in Fluxa:
 *   import std math
 *   float r = math.sqrt(16.0)      // 4.0
 *   float s = math.sin(math.pi())  // ~0.0
 *   int   n = math.abs_int(-5)     // 5
 */
#ifndef FLUXA_STD_MATH_H
#define FLUXA_STD_MATH_H

#include <math.h>
#include <string.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Value helpers (inline to avoid dependency on runtime.h) ─────────────── */
static inline Value std_math_float(double d) {
    Value v; v.type = VAL_FLOAT; v.as.real = d; return v;
}
static inline Value std_math_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value std_math_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* Coerce a Value to double — accepts int or float */
static inline int std_math_to_double(const Value *v, double *out,
                                      char *errbuf, int errbuf_len) {
    if (v->type == VAL_FLOAT) { *out = v->as.real;              return 1; }
    if (v->type == VAL_INT)   { *out = (double)v->as.integer;   return 1; }
    snprintf(errbuf, (size_t)errbuf_len,
             "math: expected int or float argument, got %s",
             v->type == VAL_STRING ? "str" :
             v->type == VAL_BOOL   ? "bool" : "nil");
    return 0;
}

/* Coerce a Value to long (for int-returning functions) */
static inline int std_math_to_long(const Value *v, long *out,
                                    char *errbuf, int errbuf_len) {
    if (v->type == VAL_INT)   { *out = v->as.integer;           return 1; }
    if (v->type == VAL_FLOAT) { *out = (long)v->as.real;        return 1; }
    snprintf(errbuf, (size_t)errbuf_len,
             "math: expected int or float argument, got %s",
             v->type == VAL_STRING ? "str" :
             v->type == VAL_BOOL   ? "bool" : "nil");
    return 0;
}

/* ── std.math function dispatch ──────────────────────────────────────────── */
/*
 * Called by the runtime when it encounters a call to math.<fn>.
 * fn_name: the function name after the dot (e.g. "sqrt")
 * args:    array of argument Values
 * argc:    argument count
 * err:     ErrStack for domain errors
 * had_error: set to 1 on error
 *
 * Returns the result Value, or val_nil() on error.
 */
static inline Value fluxa_std_math_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int current_line) {
    char errbuf[280];
    double a = 0.0, b = 0.0;

#define MATH_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "math.%s (line %d): %s", \
             fn_name, current_line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "math", current_line); \
    *had_error = 1; \
    return std_math_nil(); \
} while(0)

#define REQUIRE_ARGC(n) do { \
    if (argc != (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
                 "math.%s expects %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "math", current_line); \
        *had_error = 1; return std_math_nil(); \
    } \
} while(0)

#define GET_A() do { \
    char _eb[200]; \
    if (!std_math_to_double(&args[0], &a, _eb, sizeof(_eb))) MATH_ERR(_eb); \
} while(0)

#define GET_B() do { \
    char _eb[200]; \
    if (!std_math_to_double(&args[1], &b, _eb, sizeof(_eb))) MATH_ERR(_eb); \
} while(0)

    /* ── Constants (0 args) ──────────────────────────────────────────────── */
    if (strcmp(fn_name, "pi")  == 0) { REQUIRE_ARGC(0); return std_math_float(3.14159265358979323846); }
    if (strcmp(fn_name, "e")   == 0) { REQUIRE_ARGC(0); return std_math_float(2.71828182845904523536); }
    if (strcmp(fn_name, "inf") == 0) { REQUIRE_ARGC(0); return std_math_float(HUGE_VAL); }
    if (strcmp(fn_name, "nan") == 0) { REQUIRE_ARGC(0); return std_math_float(0.0 / 0.0); }

    /* ── 1-argument float functions ──────────────────────────────────────── */
    if (strcmp(fn_name, "sqrt") == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a < 0.0) MATH_ERR("sqrt of negative number");
        return std_math_float(sqrt(a));
    }
    if (strcmp(fn_name, "cbrt")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(cbrt(a)); }
    if (strcmp(fn_name, "floor") == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(floor(a)); }
    if (strcmp(fn_name, "ceil")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(ceil(a)); }
    if (strcmp(fn_name, "round") == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(round(a)); }
    if (strcmp(fn_name, "trunc") == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(trunc(a)); }
    if (strcmp(fn_name, "exp")   == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(exp(a)); }
    if (strcmp(fn_name, "exp2")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(exp2(a)); }

    if (strcmp(fn_name, "log") == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a <= 0.0) MATH_ERR("log of non-positive number");
        return std_math_float(log(a));
    }
    if (strcmp(fn_name, "log2") == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a <= 0.0) MATH_ERR("log2 of non-positive number");
        return std_math_float(log2(a));
    }
    if (strcmp(fn_name, "log10") == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a <= 0.0) MATH_ERR("log10 of non-positive number");
        return std_math_float(log10(a));
    }

    /* ── Trigonometry (radians) ───────────────────────────────────────────── */
    if (strcmp(fn_name, "sin")   == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(sin(a)); }
    if (strcmp(fn_name, "cos")   == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(cos(a)); }
    if (strcmp(fn_name, "tan")   == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(tan(a)); }
    if (strcmp(fn_name, "asin")  == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a < -1.0 || a > 1.0) MATH_ERR("asin: argument out of range [-1, 1]");
        return std_math_float(asin(a));
    }
    if (strcmp(fn_name, "acos")  == 0) {
        REQUIRE_ARGC(1); GET_A();
        if (a < -1.0 || a > 1.0) MATH_ERR("acos: argument out of range [-1, 1]");
        return std_math_float(acos(a));
    }
    if (strcmp(fn_name, "atan")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(atan(a)); }
    if (strcmp(fn_name, "sinh")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(sinh(a)); }
    if (strcmp(fn_name, "cosh")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(cosh(a)); }
    if (strcmp(fn_name, "tanh")  == 0) { REQUIRE_ARGC(1); GET_A(); return std_math_float(tanh(a)); }

    /* ── 2-argument functions ─────────────────────────────────────────────── */
    if (strcmp(fn_name, "pow") == 0) {
        REQUIRE_ARGC(2); GET_A(); GET_B();
        if (a < 0.0 && b != (double)(long)b)
            MATH_ERR("pow: negative base with non-integer exponent");
        return std_math_float(pow(a, b));
    }
    if (strcmp(fn_name, "atan2") == 0) { REQUIRE_ARGC(2); GET_A(); GET_B(); return std_math_float(atan2(a, b)); }
    if (strcmp(fn_name, "fmod")  == 0) {
        REQUIRE_ARGC(2); GET_A(); GET_B();
        if (b == 0.0) MATH_ERR("fmod: division by zero");
        return std_math_float(fmod(a, b));
    }
    if (strcmp(fn_name, "hypot") == 0) { REQUIRE_ARGC(2); GET_A(); GET_B(); return std_math_float(hypot(a, b)); }

    /* ── Absolute value (type-preserving) ────────────────────────────────── */
    if (strcmp(fn_name, "abs") == 0) {
        REQUIRE_ARGC(1);
        if (args[0].type == VAL_INT)   return std_math_int((long)llabs((long long)args[0].as.integer));
        if (args[0].type == VAL_FLOAT) return std_math_float(fabs(args[0].as.real));
        MATH_ERR("abs: expected int or float");
    }

    /* ── Min / max (type-preserving) ─────────────────────────────────────── */
    if (strcmp(fn_name, "min") == 0) {
        REQUIRE_ARGC(2); GET_A(); GET_B();
        /* Preserve int type if both args are int */
        if (args[0].type == VAL_INT && args[1].type == VAL_INT)
            return std_math_int(args[0].as.integer < args[1].as.integer
                                ? args[0].as.integer : args[1].as.integer);
        return std_math_float(a < b ? a : b);
    }
    if (strcmp(fn_name, "max") == 0) {
        REQUIRE_ARGC(2); GET_A(); GET_B();
        if (args[0].type == VAL_INT && args[1].type == VAL_INT)
            return std_math_int(args[0].as.integer > args[1].as.integer
                                ? args[0].as.integer : args[1].as.integer);
        return std_math_float(a > b ? a : b);
    }

    /* ── Clamp ────────────────────────────────────────────────────────────── */
    if (strcmp(fn_name, "clamp") == 0) {
        /* clamp(value, min, max) */
        if (argc != 3) MATH_ERR("clamp expects 3 arguments: clamp(value, min, max)");
        double v2, lo, hi;
        char eb[200];
        if (!std_math_to_double(&args[0], &v2, eb, sizeof(eb))) MATH_ERR(eb);
        if (!std_math_to_double(&args[1], &lo, eb, sizeof(eb))) MATH_ERR(eb);
        if (!std_math_to_double(&args[2], &hi, eb, sizeof(eb))) MATH_ERR(eb);
        if (lo > hi) MATH_ERR("clamp: min > max");
        double clamped = v2 < lo ? lo : (v2 > hi ? hi : v2);
        if (args[0].type == VAL_INT && args[1].type == VAL_INT && args[2].type == VAL_INT)
            return std_math_int((long)clamped);
        return std_math_float(clamped);
    }

    /* ── Conversion helpers ───────────────────────────────────────────────── */
    if (strcmp(fn_name, "to_int")   == 0) {
        REQUIRE_ARGC(1);
        long n = 0; char eb[200];
        if (!std_math_to_long(&args[0], &n, eb, sizeof(eb))) MATH_ERR(eb);
        return std_math_int(n);
    }
    if (strcmp(fn_name, "to_float") == 0) {
        REQUIRE_ARGC(1); GET_A(); return std_math_float(a);
    }
    if (strcmp(fn_name, "deg_to_rad") == 0) {
        REQUIRE_ARGC(1); GET_A();
        return std_math_float(a * 3.14159265358979323846 / 180.0);
    }
    if (strcmp(fn_name, "rad_to_deg") == 0) {
        REQUIRE_ARGC(1); GET_A();
        return std_math_float(a * 180.0 / 3.14159265358979323846);
    }

    /* ── Sign ─────────────────────────────────────────────────────────────── */
    if (strcmp(fn_name, "sign") == 0) {
        REQUIRE_ARGC(1); GET_A();
        return std_math_int(a > 0.0 ? 1 : (a < 0.0 ? -1 : 0));
    }

    /* ── Predicates ───────────────────────────────────────────────────────── */
    if (strcmp(fn_name, "is_nan") == 0) {
        REQUIRE_ARGC(1); GET_A();
        Value r; r.type = VAL_BOOL; r.as.boolean = isnan(a) ? 1 : 0; return r;
    }
    if (strcmp(fn_name, "is_inf") == 0) {
        REQUIRE_ARGC(1); GET_A();
        Value r; r.type = VAL_BOOL; r.as.boolean = isinf(a) ? 1 : 0; return r;
    }

#undef MATH_ERR
#undef REQUIRE_ARGC
#undef GET_A
#undef GET_B

    /* Unknown function */
    snprintf(errbuf, sizeof(errbuf),
             "math.%s: unknown function — see import std math documentation",
             fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "math", current_line);
    *had_error = 1;
    return std_math_nil();
}

#endif /* FLUXA_STD_MATH_H */
