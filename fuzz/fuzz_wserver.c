/* fuzz_wserver.c — std.wserver argument-validation harness.
 *
 * Exercises the validation layer without libmicrohttpd.
 * Stub backend fprintf suppressed to avoid I/O saturation.
 *
 * Input layout:
 *   [0]      — fn selector (mod WS_FN_COUNT)
 *   [1]      — server handle byte
 *   [2]      — request handle byte
 *   [3]      — pair count byte (0..31) / status low byte
 *   [4..end] — payload split on 0xFF into up to 6 string fields
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

#define fprintf(stream, ...) ((void)0)
#include "../src/std/wserver/fluxa_std_wserver.h"
#undef fprintf

/* ── Helpers ─────────────────────────────────────────────────────── */

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

/* Flat key/value str arr from segs[0..count). */
static Value make_hdr_arr(char **segs, int count) {
    int cap = count > 0 ? count : 1;
    Value v;
    v.type = VAL_ARR;
    v.as.arr.data = (Value *)calloc((size_t)cap, sizeof(Value));
    v.as.arr.size = count;
    if (!v.as.arr.data) { v.as.arr.size = 0; return v; }
    for (int i = 0; i < count; i++) {
        v.as.arr.data[i].type      = VAL_STRING;
        v.as.arr.data[i].as.string = strdup(segs[i] ? segs[i] : "");
    }
    return v;
}

static void free_arr(Value *v) {
    if (v->type != VAL_ARR || !v->as.arr.data) return;
    for (int i = 0; i < v->as.arr.size; i++)
        if (v->as.arr.data[i].type == VAL_STRING)
            free(v->as.arr.data[i].as.string);
    free(v->as.arr.data);
    v->as.arr.data = NULL;
}

static FluxaConfig make_tight_cfg(void) {
    FluxaConfig c;
    memset(&c, 0, sizeof(c));
    c.wserver_max_servers      = 2;
    c.wserver_max_requests     = 4;
    c.wserver_max_body_bytes   = 256;
    c.wserver_max_header_pairs = 2;
    c.wserver_max_header_bytes = 64;
    c.wserver_queue_depth      = 8;
    return c;
}

static const char *ws_fns[] = {
    "serve", "accept", "req_method", "req_path", "req_body",
    "req_header", "reply", "reply_json", "reply_headers",
    "connections", "stop", "version"
};
#define WS_FN_COUNT ((int)(sizeof(ws_fns)/sizeof(ws_fns[0])))

__attribute__((constructor))
static void fuzz_wserver_init(void) {
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) { fclose(stderr); stderr = devnull; }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    if (size > (1u << 16)) return 0;

    int  fn_idx = data[0] % WS_FN_COUNT;
    long srv_h  = (long)(data[1]);
    long req_h  = (long)(data[2]);
    long npairs = (long)(data[3] & 0x1f);

    char *segs[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    int   nseg    = split_segments(data, 4, size, 6, segs);
    if (nseg == 0 || !segs[0]) {
        free(segs[0]); segs[0] = strdup(""); nseg = 1;
    }

    const char *fn  = ws_fns[fn_idx];
    FluxaConfig cfg = make_tight_cfg();

    ErrStack err;
    errstack_clear(&err);
    int had_error = 0;

    Value args[5];
    memset(args, 0, sizeof(args));
    int   argc    = 0;
    Value hdr_arr; hdr_arr.type = VAL_NIL;

    if (!strcmp(fn, "serve")) {
        args[0].type = VAL_INT; args[0].as.integer = srv_h;
        argc = 1;
    } else if (!strcmp(fn, "version")) {
        argc = 0;
    } else if (!strcmp(fn, "accept")) {
        args[0].type = VAL_INT; args[0].as.integer = srv_h;
        args[1].type = VAL_INT; args[1].as.integer = (long)(data[3]);
        argc = 2;
    } else if (!strcmp(fn, "req_method") || !strcmp(fn, "req_path")  ||
               !strcmp(fn, "req_body")   || !strcmp(fn, "connections") ||
               !strcmp(fn, "stop")) {
        args[0].type = VAL_INT; args[0].as.integer = req_h;
        argc = 1;
    } else if (!strcmp(fn, "req_header")) {
        args[0].type = VAL_INT;    args[0].as.integer = req_h;
        args[1].type = VAL_STRING; args[1].as.string  = segs[0];
        argc = 2;
    } else if (!strcmp(fn, "reply")) {
        /* Full byte range for status — exercises 100-599 boundary check */
        long status = (long)(data[3]);
        args[0].type = VAL_INT;    args[0].as.integer = req_h;
        args[1].type = VAL_INT;    args[1].as.integer = status;
        args[2].type = VAL_STRING; args[2].as.string  = segs[0];
        argc = 3;
    } else if (!strcmp(fn, "reply_json")) {
        long status = (long)(data[3]);
        args[0].type = VAL_INT;    args[0].as.integer = req_h;
        args[1].type = VAL_INT;    args[1].as.integer = status;
        args[2].type = VAL_STRING; args[2].as.string  = segs[0];
        argc = 3;
    } else if (!strcmp(fn, "reply_headers")) {
        long status = 200 + (long)(data[3] & 0x3);
        int nhdr = nseg > 1 ? nseg - 1 : 0;
        hdr_arr = make_hdr_arr(segs + 1, nhdr);
        args[0].type = VAL_INT;    args[0].as.integer = req_h;
        args[1].type = VAL_INT;    args[1].as.integer = status;
        args[2].type = VAL_STRING; args[2].as.string  = segs[0];
        args[3]      = hdr_arr;
        args[4].type = VAL_INT;    args[4].as.integer = npairs;
        argc = 5;
    }

    /* Normal call */
    Value ret = fluxa_std_wserver_call(fn, args, argc, &err, &had_error, 1, &cfg);
    value_free_data(&ret);

    /* Wrong-type for first arg */
    if (argc >= 1) {
        ValType saved = args[0].type;
        if (saved == VAL_INT) {
            args[0].type = VAL_STRING; args[0].as.string = segs[0];
        } else {
            args[0].type = VAL_INT; args[0].as.integer = 0;
        }
        errstack_clear(&err); had_error = 0;
        ret = fluxa_std_wserver_call(fn, args, argc, &err, &had_error, 1, &cfg);
        value_free_data(&ret);
        /* Restore */
        args[0].type = saved;
        if (saved == VAL_INT) args[0].as.integer = srv_h;
        else args[0].as.string = segs[0];
    }

    /* argc=0 */
    errstack_clear(&err); had_error = 0;
    ret = fluxa_std_wserver_call(fn, args, 0, &err, &had_error, 1, &cfg);
    value_free_data(&ret);

    if (hdr_arr.type == VAL_ARR) free_arr(&hdr_arr);
    for (int i = 0; i < 6; i++) free(segs[i]);
    return 0;
}
