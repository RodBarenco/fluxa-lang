/* snapshot_seedgen.c — generate a few seed snapshots.
 *
 * Builds tiny PrstPools/PrstGraphs in memory, serializes them with the
 * same code paths the runtime uses, and writes them out as fuzz seeds
 * prefixed with the mode byte the harness consumes.
 *
 * Compile: clang -std=c99 -Isrc -D_POSIX_C_SOURCE=200809L \
 *                fuzz/snapshot_seedgen.c src/scope.c -o /tmp/seedgen
 * Run:     /tmp/seedgen fuzz/corpus/snapshot
 *
 * This is run by hand once; the corpus files are checked in. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/prst_pool.h"
#include "../src/prst_graph.h"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pool_checksum;
    uint32_t graph_checksum;
    uint32_t pool_size;
    uint32_t graph_size;
    int32_t  pool_count;
    int32_t  graph_count;
    int32_t  cycle_count_a;
    uint8_t  _pad[4];
} HandoverSnapshotHeader;

#define FLUXA_HANDOVER_MAGIC   0xF10A8888u
#define FLUXA_HANDOVER_VERSION 1001u

static void write_seed(const char *dir, const char *name,
                       uint8_t mode, const void *body, size_t blen) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(&mode, 1, 1, f);
    if (blen) fwrite(body, 1, blen, f);
    fclose(f);
    fprintf(stderr, "wrote %s (mode=%u, %zu bytes)\n", path, mode, blen);
}

static void build_full_snapshot(uint8_t **out, size_t *outsz,
                                 const void *pool_buf, size_t pool_sz,
                                 const void *graph_buf, size_t graph_sz,
                                 uint32_t pool_cs, uint32_t graph_cs,
                                 int32_t pool_cnt, int32_t graph_cnt) {
    size_t total = sizeof(HandoverSnapshotHeader) + pool_sz + graph_sz;
    uint8_t *buf = (uint8_t *)malloc(total);
    HandoverSnapshotHeader hdr = {
        .magic = FLUXA_HANDOVER_MAGIC,
        .version = FLUXA_HANDOVER_VERSION,
        .pool_checksum = pool_cs,
        .graph_checksum = graph_cs,
        .pool_size = (uint32_t)pool_sz,
        .graph_size = (uint32_t)graph_sz,
        .pool_count = pool_cnt,
        .graph_count = graph_cnt,
        .cycle_count_a = 0,
        ._pad = {0},
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), pool_buf, pool_sz);
    memcpy(buf + sizeof(hdr) + pool_sz, graph_buf, graph_sz);
    *out   = buf;
    *outsz = total;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    const char *outdir = argv[1];

    /* ── Seed A: a tiny PrstPool with int + string ────────────────────── */
    {
        PrstPool p; prst_pool_init(&p);
        Value vi = { .type = VAL_INT };  vi.as.integer = 42;
        prst_pool_set(&p, "counter", vi, NULL);
        Value vs = { .type = VAL_STRING }; vs.as.string = strdup("hello");
        prst_pool_set(&p, "label", vs, NULL);
        free(vs.as.string);
        p.entries[0].declared_type = VAL_INT;
        p.entries[1].declared_type = VAL_STRING;

        void *buf = NULL; size_t sz = 0;
        prst_pool_serialize(&p, &buf, &sz);
        write_seed(outdir, "seed_pool_basic", 0, buf, sz);
        free(buf);
        prst_pool_free(&p);
    }

    /* ── Seed B: a PrstPool with VAL_ARR(int) ──────────────────────────── */
    {
        PrstPool p; prst_pool_init(&p);
        Value *elems = (Value *)calloc(3, sizeof(Value));
        for (int i = 0; i < 3; i++) {
            elems[i].type = VAL_INT;
            elems[i].as.integer = (long)i * 10;
        }
        Value va = val_arr(elems, 3);
        prst_pool_set(&p, "readings", va, NULL);
        p.entries[0].declared_type = VAL_ARR;

        void *buf = NULL; size_t sz = 0;
        prst_pool_serialize(&p, &buf, &sz);
        write_seed(outdir, "seed_pool_arr", 0, buf, sz);
        free(buf);
        /* Note: prst_pool_set shallow-copies the Value, so the entry's
         * arr.data points to our `elems`. Free it explicitly. */
        free(elems);
        prst_pool_free(&p);
    }

    /* ── Seed C: a PrstGraph with a couple of edges ────────────────────── */
    {
        PrstGraph g; prst_graph_init(&g);
        prst_graph_record(&g, "counter", "main");
        prst_graph_record(&g, "counter", "tick");
        prst_graph_record(&g, "label",   "log_block");

        void *buf = NULL; size_t sz = 0;
        prst_graph_serialize(&g, &buf, &sz);
        write_seed(outdir, "seed_graph_basic", 1, buf, sz);
        free(buf);
        prst_graph_free(&g);
    }

    /* ── Seed D: full snapshot (header + pool + graph) ─────────────────── */
    {
        PrstPool p; prst_pool_init(&p);
        Value vi = { .type = VAL_INT };  vi.as.integer = 7;
        prst_pool_set(&p, "x", vi, NULL);
        p.entries[0].declared_type = VAL_INT;

        PrstGraph g; prst_graph_init(&g);
        prst_graph_record(&g, "x", "main");

        void *pb = NULL; size_t ps = 0;
        void *gb = NULL; size_t gs = 0;
        prst_pool_serialize(&p, &pb, &ps);
        prst_graph_serialize(&g, &gb, &gs);

        uint32_t pcs = prst_pool_checksum(&p);
        uint32_t gcs = prst_graph_checksum(&g);

        uint8_t *snap = NULL; size_t snapsz = 0;
        build_full_snapshot(&snap, &snapsz, pb, ps, gb, gs,
                            pcs, gcs, (int32_t)p.count, (int32_t)g.count);
        write_seed(outdir, "seed_full_snapshot", 2, snap, snapsz);

        free(snap); free(pb); free(gb);
        prst_pool_free(&p);
        prst_graph_free(&g);
    }

    return 0;
}
