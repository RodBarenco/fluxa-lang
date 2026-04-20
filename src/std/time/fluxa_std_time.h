/* fluxa_std_time.h — Fluxa Standard Library: time
 *
 * Compiled into the binary only when FLUXA_STD_TIME is defined.
 * Declared in [libs] of fluxa.toml to enable at runtime.
 *
 * No danger required — all functions are safe wrappers over OS/hardware
 * timers with bounded behavior.
 *
 * Platform support:
 *   Linux / macOS:  clock_gettime(CLOCK_MONOTONIC) + nanosleep
 *   RP2040:         time_us_64() + sleep_ms() from pico-sdk (when
 *                   FLUXA_TARGET_RP2040 is defined)
 *   ESP32:          esp_timer_get_time() + vTaskDelay (when
 *                   FLUXA_TARGET_ESP32 is defined)
 *
 * API (8 functions):
 *   time.sleep(int ms)              Block current thread for N milliseconds
 *   time.sleep_us(int us)           Block current thread for N microseconds
 *   time.now_ms()  → int            Monotonic timestamp in milliseconds
 *   time.now_us()  → int            Monotonic timestamp in microseconds
 *   time.ticks()   → int            Raw hardware tick counter (platform-native)
 *   time.elapsed_ms(int since) → int  Milliseconds since a prior now_ms()
 *   time.timeout(int start, int max_ms) → bool  True if elapsed >= max_ms
 *   time.format(int ms) → str       Human-readable UTC datetime string
 *
 * Usage:
 *   import std time
 *   time.sleep(16)
 *   int t0 = time.now_ms()
 *   // ... work ...
 *   int dt = time.elapsed_ms(t0)
 */
#ifndef FLUXA_STD_TIME_H
#define FLUXA_STD_TIME_H

#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

#if defined(FLUXA_TARGET_RP2040)
#  include "pico/stdlib.h"
#  include "hardware/timer.h"
#elif defined(FLUXA_TARGET_ESP32)
#  include "esp_timer.h"
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#else
#  include <time.h>
#  include <stdlib.h>
#endif

/* ── Platform abstractions ───────────────────────────────────────────────── */

/* Returns monotonic time in microseconds since an arbitrary epoch. */
static inline long long time_now_us_platform(void) {
#if defined(FLUXA_TARGET_RP2040)
    return (long long)time_us_64();
#elif defined(FLUXA_TARGET_ESP32)
    return (long long)esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
#endif
}

/* Sleeps for N microseconds on the current thread. */
static inline void time_sleep_us_platform(long long us) {
    if (us <= 0) return;
#if defined(FLUXA_TARGET_RP2040)
    sleep_us((uint64_t)us);
#elif defined(FLUXA_TARGET_ESP32)
    vTaskDelay((TickType_t)(us / 1000 / portTICK_PERIOD_MS));
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000LL;
    ts.tv_nsec = (us % 1000000LL) * 1000LL;
    nanosleep(&ts, NULL);
#endif
}

/* Raw hardware tick counter — platform-native resolution.
 * On Linux/macOS this is the same as now_us for simplicity.
 * On RP2040 it uses the hardware timer directly. */
static inline long long time_ticks_platform(void) {
#if defined(FLUXA_TARGET_RP2040)
    return (long long)timer_hw->timerawl; /* lower 32 bits of hardware timer */
#elif defined(FLUXA_TARGET_ESP32)
    return (long long)esp_timer_get_time();
#else
    return time_now_us_platform();
#endif
}

/* ── Value helpers ───────────────────────────────────────────────────────── */
static inline Value time_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value time_bool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b; return v;
}
static inline Value time_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v;
}
static inline Value time_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
static inline Value fluxa_std_time_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define TIME_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "time.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "time", line); \
    *had_error = 1; return time_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "time.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "time", line); \
        *had_error = 1; return time_nil(); \
    } \
} while(0)

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT && args[(idx)].type != VAL_FLOAT) \
        TIME_ERR("expected int argument"); \
    long long (var) = args[(idx)].type == VAL_INT \
        ? (long long)args[(idx)].as.integer \
        : (long long)args[(idx)].as.real;

    /* ── time.sleep(int ms) → nil ────────────────────────────────────────── */
    if (strcmp(fn_name, "sleep") == 0) {
        NEED(1); GET_INT(0, ms);
        if (ms < 0) TIME_ERR("sleep: milliseconds cannot be negative");
        time_sleep_us_platform(ms * 1000LL);
        return time_nil();
    }

    /* ── time.sleep_us(int us) → nil ─────────────────────────────────────── */
    if (strcmp(fn_name, "sleep_us") == 0) {
        NEED(1); GET_INT(0, us);
        if (us < 0) TIME_ERR("sleep_us: microseconds cannot be negative");
        time_sleep_us_platform(us);
        return time_nil();
    }

    /* ── time.now_ms() → int ─────────────────────────────────────────────── */
    if (strcmp(fn_name, "now_ms") == 0) {
        return time_int((long)(time_now_us_platform() / 1000LL));
    }

    /* ── time.now_us() → int ─────────────────────────────────────────────── */
    if (strcmp(fn_name, "now_us") == 0) {
        return time_int((long)time_now_us_platform());
    }

    /* ── time.ticks() → int ──────────────────────────────────────────────── */
    /* Raw hardware tick counter. Resolution is platform-dependent:
     *   Linux/macOS : microseconds (same as now_us)
     *   RP2040      : lower 32 bits of hardware timer (1 µs resolution)
     *   ESP32       : microseconds from esp_timer */
    if (strcmp(fn_name, "ticks") == 0) {
        return time_int((long)time_ticks_platform());
    }

    /* ── time.elapsed_ms(int since) → int ───────────────────────────────── */
    /* Returns milliseconds elapsed since a prior time.now_ms() call.
     * Safe against the common mistake of (now - then) which can overflow
     * on platforms with 32-bit counters. */
    if (strcmp(fn_name, "elapsed_ms") == 0) {
        NEED(1); GET_INT(0, since);
        long long now_ms = time_now_us_platform() / 1000LL;
        long long diff   = now_ms - (long long)since;
        if (diff < 0) diff = 0; /* clock wraparound guard */
        return time_int((long)diff);
    }

    /* ── time.timeout(int start, int max_ms) → bool ─────────────────────── */
    /* Returns true if at least max_ms milliseconds have passed since start.
     * Typical use:
     *   int t0 = time.now_ms()
     *   while !time.timeout(t0, 5000) { ... }  // run for 5 seconds */
    if (strcmp(fn_name, "timeout") == 0) {
        NEED(2); GET_INT(0, start); GET_INT(1, max_ms);
        if (max_ms < 0) TIME_ERR("timeout: max_ms cannot be negative");
        long long now_ms = time_now_us_platform() / 1000LL;
        long long diff   = now_ms - (long long)start;
        return time_bool(diff >= (long long)max_ms ? 1 : 0);
    }

    /* ── time.format(int ms) → str ──────────────────────────────────────── */
    /* Converts a millisecond timestamp (from time.now_ms()) to a
     * human-readable UTC string: "2025-01-15 14:32:01.123"
     * On embedded targets without RTC, falls back to elapsed time format. */
    if (strcmp(fn_name, "format") == 0) {
        NEED(1); GET_INT(0, ms_ts);
        char buf[64];
#if defined(FLUXA_TARGET_RP2040) || defined(FLUXA_TARGET_ESP32)
        /* No real-time clock available — format as elapsed time */
        long long total_s = ms_ts / 1000LL;
        long long h = total_s / 3600;
        long long m = (total_s % 3600) / 60;
        long long s = total_s % 60;
        long long ms_part = ms_ts % 1000;
        snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld", h, m, s, ms_part);
#else
        time_t t = (time_t)(ms_ts / 1000LL);
        struct tm *tm_utc = gmtime(&t);
        if (!tm_utc) {
            return time_str("(invalid timestamp)");
        }
        int ms_part = (int)(ms_ts % 1000LL);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 tm_utc->tm_year + 1900,
                 tm_utc->tm_mon  + 1,
                 tm_utc->tm_mday,
                 tm_utc->tm_hour,
                 tm_utc->tm_min,
                 tm_utc->tm_sec,
                 ms_part);
#endif
        return time_str(buf);
    }

#undef TIME_ERR
#undef NEED
#undef GET_INT

    snprintf(errbuf, sizeof(errbuf), "time.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "time", line);
    *had_error = 1;
    return time_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "time",
    toml_key = "std.time",
    owner    = "time",
    call     = fluxa_std_time_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_TIME_H */
