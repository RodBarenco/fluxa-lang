#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../src/cabi/fluxa_cabi.h"

#define PHASE_SECONDS 5.0

static double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void die_err(const char *where, const fluxa_cabi_error *err) {
    fprintf(stderr, "%s: code=%u line=%d context=%s message=%s\n",
            where,
            err ? err->code : 0u,
            err ? err->line : 0,
            err ? err->context : "",
            err ? err->message : "");
    exit(1);
}

static uint64_t run_phase(fluxa_cabi_runtime *rt,
                          const fluxa_cabi_view *request,
                          double seconds,
                          fluxa_cabi_view *last_response,
                          fluxa_cabi_error *err,
                          double *elapsed_out) {
    uint64_t n = 0;
    double start = now_seconds();
    double end = start + seconds;
    double t;
    int rc;

    do {
        rc = fluxa_cabi_exchange(rt, request, last_response, err);
        if (rc != FLUXA_CABI_OK) die_err("exchange", err);
        n++;
        t = now_seconds();
    } while (t < end);

    *elapsed_out = t - start;
    return n;
}

static void print_result(const char *name, uint64_t n, double elapsed,
                         uint32_t request_bytes, uint32_t response_bytes) {
    double ops = elapsed > 0.0 ? (double)n / elapsed : 0.0;
    double ns = n ? (elapsed * 1e9) / (double)n : 0.0;
    double mib = elapsed > 0.0
        ? ((double)n * (double)(request_bytes + response_bytes)) /
          (1024.0 * 1024.0 * elapsed)
        : 0.0;

    printf("%-8s %12llu exchanges  %12.0f exch/s  %10.1f ns/exch  %8.2f MiB/s\n",
           name,
           (unsigned long long)n,
           ops,
           ns,
           mib);
    printf("         frame: request=%u B  response=%u B  elapsed=%.6f s\n",
           request_bytes, response_bytes, elapsed);
}

int main(int argc, char **argv) {
    fluxa_cabi_config cfg;
    fluxa_cabi_runtime *rt = NULL;
    fluxa_cabi_error err;
    fluxa_cabi_message read_req = {0};
    fluxa_cabi_message response_req = {0};
    fluxa_cabi_view read_view, response_view, resp;
    int32_t ai[3] = {10, -20, 30};
    double af[3] = {1.25, -2.5, 3.75};
    uint8_t ab[3] = {1, 0, 1};
    fluxa_cabi_str_view as[3] = {{"a",1}, {"Fluxa",5}, {"",0}};
    uint64_t read_n, response_n;
    double read_elapsed, response_elapsed;
    uint32_t read_resp_size, response_resp_size;
    int b;

    if (argc != 3) {
        fprintf(stderr, "usage: %s ENTRY CONFIG\n", argv[0]);
        return 2;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = FLUXA_CABI_ABI_VERSION;
    cfg.entry_path = argv[1];
    cfg.config_path = argv[2];
    cfg.dispatch_fn = "cabi_dispatch";

    if (fluxa_cabi_open(&cfg, &rt, &err) != FLUXA_CABI_OK)
        die_err("open", &err);

    fluxa_cabi_message_init(&read_req);
    fluxa_cabi_message_init(&response_req);

    /* Mode 1: heavy inbound frame. Build once; benchmark only exchange. */
    if (!fluxa_cabi_add_int(&read_req, 1) ||
        !fluxa_cabi_add_int(&read_req, 41) ||
        !fluxa_cabi_add_float(&read_req, 2.5) ||
        !fluxa_cabi_add_bool(&read_req, 1) ||
        !fluxa_cabi_add_str(&read_req, "hello", 5) ||
        !fluxa_cabi_add_int_arr(&read_req, ai, 3) ||
        !fluxa_cabi_add_float_arr(&read_req, af, 3) ||
        !fluxa_cabi_add_bool_arr(&read_req, ab, 3) ||
        !fluxa_cabi_add_str_arr(&read_req, as, 3)) {
        fprintf(stderr, "failed to build read request\n");
        return 1;
    }

    /* Mode 2: tiny inbound trigger; Fluxa emits the full typed frame. */
    if (!fluxa_cabi_add_int(&response_req, 2)) {
        fprintf(stderr, "failed to build response request\n");
        return 1;
    }

    read_view.data = read_req.data;
    read_view.size = read_req.size;
    response_view.data = response_req.data;
    response_view.size = response_req.size;

    /* Small untimed warm-up: parser/runtime already open, warm CPU/code paths. */
    for (int i = 0; i < 1000; i++) {
        if (fluxa_cabi_exchange(rt, &read_view, &resp, &err) != FLUXA_CABI_OK)
            die_err("warmup-read", &err);
        if (fluxa_cabi_exchange(rt, &response_view, &resp, &err) != FLUXA_CABI_OK)
            die_err("warmup-response", &err);
    }

    printf("Fluxa C ABI bridge bench — 10 measured seconds (5s READ + 5s RESPONSE)\n");
    printf("clear FXCB wire, one persistent runtime, single thread, request frames prebuilt\n\n");

    read_n = run_phase(rt, &read_view, PHASE_SECONDS, &resp, &err, &read_elapsed);
    read_resp_size = resp.size;
    if (fluxa_cabi_value_count(&resp) != 1 || !fluxa_cabi_get_bool(&resp, 0, &b) || !b) {
        fprintf(stderr, "READ phase returned invalid acknowledgement\n");
        return 1;
    }

    response_n = run_phase(rt, &response_view, PHASE_SECONDS, &resp, &err, &response_elapsed);
    response_resp_size = resp.size;
    if (fluxa_cabi_value_count(&resp) != 8) {
        fprintf(stderr, "RESPONSE phase returned invalid value count\n");
        return 1;
    }

    print_result("READ", read_n, read_elapsed, read_req.size, read_resp_size);
    print_result("RESPONSE", response_n, response_elapsed, response_req.size, response_resp_size);

    {
        uint64_t total = read_n + response_n;
        double elapsed = read_elapsed + response_elapsed;
        printf("\nTOTAL    %12llu exchanges in %.6f s  = %.0f exch/s combined average\n",
               (unsigned long long)total,
               elapsed,
               elapsed > 0.0 ? (double)total / elapsed : 0.0);
    }

    fluxa_cabi_message_free(&read_req);
    fluxa_cabi_message_free(&response_req);
    fluxa_cabi_close(rt);
    return 0;
}
