/* fuzz_pg.c — std.pg argument-validation harness.
 *
 * Exercises the validation layer (handle bounds, param count vs arr.size,
 * per-element byte limits, connstring length) without libpq.
 * Stub backend is used — fprintf to stderr is suppressed to avoid I/O
 * saturation at libFuzzer's iteration speed.
 *
 * Input layout:
 *   [0]      — fn selector (mod PG_FN_COUNT)
 *   [1]      — conn handle  (raw byte → long)
 *   [2]      — result handle (raw byte → long)
 *   [3]      — param count byte (0..31)
 *   [4..end] — payload, split on 0xFF into up to 4 string fields
 */

/* Increase stack to 64 MB — prevents libFuzzer signal/crash handlers
 * from overflowing on deep corpora or large mutation inputs. */
#include <sys/resource.h>
__attribute__((constructor))
static void fuzz_stack_init(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        if (rl.rlim_cur < 64 * 1024 * 1024)
            rl.rlim_cur = 64 * 1024 * 1024;
        setrlimit(RLIMIT_STACK, &rl);
    }
}

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "fuzz_common.h"
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/toml_config.h"

/* Suppress stub backend's fprintf — we know it's the stub, no need for
 * stderr noise at 4M iter/s which saturates I/O and can cause ASan aborts. */
#define fprintf(stream, ...) ((void)0)
#include "../src/std/pg/fluxa_std_pg.h"
#undef fprintf

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Split data[off..size) on 0xFF into at most max null-terminated segments.
 * On malloc failure stores "" rather than NULL — callers need not NULL-check. */
static int split_segments(const uint8_t *data, size_t off, size_t size,
                           int max, char **out) {
    int n = 0;
    size_t start = off;
    for (size_t i = off; i <= size && n < max; i++) {
        if (i == size || data[i] == 0xFF) {
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            out[n] = buf ? buf : (char *)malloc(1);
            if (!out[n]) { out[n] = NULL; n++; start = i+1; continue; }
            if (buf && len > 0) memcpy(out[n], data + start, len);
            out[n][buf ? len : 0] = '\0';
            n++;
            start = i + 1;
        }
    }
    return n;
}

/* Build VAL_ARR of VAL_STRING from strs[0..count). Handles NULL strs. */
static Value make_str_arr(char **strs, int count) {
    Value v;
    v.type = VAL_ARR;
    int cap = count > 0 ? count : 1;
    v.as.arr.data = (Value *)calloc((size_t)cap, sizeof(Value));
    v.as.arr.size = count;
    if (!v.as.arr.data) { v.as.arr.size = 0; return v; }
    for (int i = 0; i < count; i++) {
        v.as.arr.data[i].type      = VAL_STRING;
        v.as.arr.data[i].as.string = strdup(strs[i] ? strs[i] : "");
    }
    return v;
}

static void free_str_arr(Value *v) {
    if (v->type != VAL_ARR || !v->as.arr.data) return;
    for (int i = 0; i < v->as.arr.size; i++)
        if (v->as.arr.data[i].type == VAL_STRING)
            free(v->as.arr.data[i].as.string);
    free(v->as.arr.data);
    v->as.arr.data = NULL;
}

/* Tight config — stresses clamping and overflow checks. */
static FluxaConfig make_tight_cfg(void) {
    FluxaConfig c;
    memset(&c, 0, sizeof(c));
    c.pg_max_conns   = 2;
    c.pg_max_results = 4;
    c.pg_max_cell    = 64;
    c.pg_max_param   = 32;
    c.pg_max_params  = 3;
    return c;
}

static const char *pg_fns[] = {
    "connect", "close", "exec", "query", "query_params",
    "rows", "cols", "col_name", "get", "get_int", "get_float",
    "get_bool", "is_null", "free_result", "last_error", "ping", "version"
};
#define PG_FN_COUNT ((int)(sizeof(pg_fns)/sizeof(pg_fns[0])))

/* ── One-time init: redirect stderr to /dev/null ─────────────────── */
__attribute__((constructor))
static void fuzz_pg_init(void) {
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) { fclose(stderr); stderr = devnull; }
}

/* ── Fuzz entry point ────────────────────────────────────────────── */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    if (size > (1u << 16)) return 0;

    int  fn_idx   = data[0] % PG_FN_COUNT;
    long conn_h   = (long)(data[1]);
    long result_h = (long)(data[2]);
    long param_n  = (long)(data[3] & 0x1f);   /* 0..31 */

    char *segs[4] = { NULL, NULL, NULL, NULL };
    int   nseg    = split_segments(data, 4, size, 4, segs);
    /* Guarantee at least one non-NULL segment */
    if (nseg == 0 || !segs[0]) {
        free(segs[0]); segs[0] = strdup(""); nseg = 1;
    }

    const char *fn  = pg_fns[fn_idx];
    FluxaConfig cfg = make_tight_cfg();

    ErrStack err;
    errstack_clear(&err);
    int had_error = 0;

    Value args[5];
    memset(args, 0, sizeof(args));
    int   argc     = 0;
    Value param_arr; param_arr.type = VAL_NIL;

    /* Build args per function */
    if (!strcmp(fn, "connect") || !strcmp(fn, "ping")) {
        args[0].type = VAL_STRING; args[0].as.string = segs[0];
        argc = 1;
    } else if (!strcmp(fn, "close")  || !strcmp(fn, "free_result") ||
               !strcmp(fn, "last_error") || !strcmp(fn, "rows")    ||
               !strcmp(fn, "cols")) {
        args[0].type = VAL_INT; args[0].as.integer = conn_h;
        argc = 1;
    } else if (!strcmp(fn, "version")) {
        args[0].type = VAL_INT; args[0].as.integer = conn_h;
        argc = (data[3] & 1) ? 1 : 0;
    } else if (!strcmp(fn, "exec") || !strcmp(fn, "query")) {
        args[0].type = VAL_INT;    args[0].as.integer = conn_h;
        args[1].type = VAL_STRING; args[1].as.string  = segs[0];
        argc = 2;
    } else if (!strcmp(fn, "query_params")) {
        int nparams = nseg > 1 ? nseg - 1 : 0;
        param_arr = make_str_arr(segs + 1, nparams);
        args[0].type = VAL_INT;    args[0].as.integer = conn_h;
        args[1].type = VAL_STRING; args[1].as.string  = segs[0];
        args[2]      = param_arr;
        args[3].type = VAL_INT;    args[3].as.integer = param_n;
        argc = 4;
    } else if (!strcmp(fn, "col_name")) {
        args[0].type = VAL_INT; args[0].as.integer = result_h;
        args[1].type = VAL_INT; args[1].as.integer = (long)(data[3]);
        argc = 2;
    } else {
        /* get, get_int, get_float, get_bool, is_null */
        args[0].type = VAL_INT; args[0].as.integer = result_h;
        args[1].type = VAL_INT; args[1].as.integer = (long)(data[3] >> 2);
        args[2].type = VAL_INT; args[2].as.integer = (long)(data[3] & 3);
        argc = 3;
    }

    /* Normal call */
    Value ret = fluxa_std_pg_call(fn, args, argc, &err, &had_error, 1, &cfg);
    value_free_data(&ret);

    /* Wrong-type for first arg — exercises every REQ_INT/REQ_STR guard */
    if (argc >= 1) {
        ValType saved = args[0].type;
        if (saved == VAL_INT) {
            args[0].type = VAL_STRING; args[0].as.string = segs[0];
        } else {
            args[0].type = VAL_INT; args[0].as.integer = 0;
        }
        errstack_clear(&err); had_error = 0;
        ret = fluxa_std_pg_call(fn, args, argc, &err, &had_error, 1, &cfg);
        value_free_data(&ret);
        /* Restore — do not leave a dangling type mismatch */
        args[0].type = saved;
        if (saved == VAL_INT) args[0].as.integer = conn_h;
        else args[0].as.string = segs[0];
    }

    /* argc=0: every function must reject gracefully */
    errstack_clear(&err); had_error = 0;
    ret = fluxa_std_pg_call(fn, args, 0, &err, &had_error, 1, &cfg);
    value_free_data(&ret);

    /* Cleanup — param_arr strings are independent copies, safe to free */
    if (param_arr.type == VAL_ARR) free_str_arr(&param_arr);
    for (int i = 0; i < 4; i++) free(segs[i]);
    return 0;
}
