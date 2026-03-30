/* prst_graph.h — Persistent Variable Dependency Graph (Sprint 7)
 *
 * Sprint 7:   record, query, print (fluxa explain), deduplication.
 * Sprint 7.b: checksum FNV-32, serialization, real invalidation mark.
 * Sprint 7.c: cascade abort (invalidate becomes real).
 * Sprint 8:   prst_graph_serialize used in Handover Atômico.
 */
#ifndef FLUXA_PRST_GRAPH_H
#define FLUXA_PRST_GRAPH_H

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define PRST_GRAPH_CAP 256

typedef struct {
    char prst_name[256];
    char reader_ctx[256];
} PrstDep;

typedef struct {
    PrstDep deps[PRST_GRAPH_CAP];
    int     count;
} PrstGraph;

static inline void prst_graph_init(PrstGraph *g) { g->count = 0; }

static inline void prst_graph_record(PrstGraph *g,
                                      const char *prst_name,
                                      const char *reader_ctx) {
    const char *ctx = reader_ctx ? reader_ctx : "<global>";
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0 &&
            strcmp(g->deps[i].reader_ctx, ctx) == 0)
            return;
    if (g->count >= PRST_GRAPH_CAP) {
        fprintf(stderr, "[fluxa] prst_graph: capacity reached (%d) — dep dropped\n",
                PRST_GRAPH_CAP);
        return;
    }
    strncpy(g->deps[g->count].prst_name, prst_name, 255);
    g->deps[g->count].prst_name[255] = '\0';
    strncpy(g->deps[g->count].reader_ctx, ctx, 255);
    g->deps[g->count].reader_ctx[255] = '\0';
    g->count++;
}

static inline int prst_graph_has_readers(PrstGraph *g, const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0)
            return 1;
    return 0;
}

static inline void prst_graph_print_readers(PrstGraph *g, const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0)
            printf("  %s  <-  %s\n", prst_name, g->deps[i].reader_ctx);
}

/* ── Sprint 7.b: FNV-32 checksum ────────────────────────────────────────── */
static inline uint32_t prst_graph_checksum(const PrstGraph *g) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < g->count; i++) {
        const char *p = g->deps[i].prst_name;
        while (*p) { h ^= (uint8_t)*p++; h *= 16777619u; }
        h ^= 0x1f;
        const char *c = g->deps[i].reader_ctx;
        while (*c) { h ^= (uint8_t)*c++; h *= 16777619u; }
        h ^= 0x1e;
    }
    return h;
}

/* ── Sprint 7.b: serialization ───────────────────────────────────────────── */
/* Flat format: [int32 count][PrstDep × count]. Caller must free(*out_buf). */
static inline int prst_graph_serialize(const PrstGraph *g,
                                        void **out_buf, size_t *out_size) {
    size_t sz = sizeof(int32_t) + (size_t)g->count * sizeof(PrstDep);
    char *buf = (char*)malloc(sz);
    if (!buf) return 0;
    int32_t cnt = (int32_t)g->count;
    memcpy(buf, &cnt, sizeof(int32_t));
    if (g->count > 0)
        memcpy(buf + sizeof(int32_t), g->deps,
               (size_t)g->count * sizeof(PrstDep));
    *out_buf  = buf;
    *out_size = sz;
    return 1;
}

static inline int prst_graph_deserialize(PrstGraph *g,
                                          const void *buf, size_t buf_size) {
    if (buf_size < sizeof(int32_t)) return 0;
    int32_t cnt;
    memcpy(&cnt, buf, sizeof(int32_t));
    if (cnt < 0 || cnt > PRST_GRAPH_CAP) return 0;
    size_t expected = sizeof(int32_t) + (size_t)cnt * sizeof(PrstDep);
    if (buf_size < expected) return 0;
    g->count = (int)cnt;
    if (cnt > 0)
        memcpy(g->deps, (const char*)buf + sizeof(int32_t),
               (size_t)cnt * sizeof(PrstDep));
    return 1;
}

/* ── Sprint 7.b: invalidation — removes all deps for prst_name ──────────── */
/* Readers are removed so next read re-registers with updated value.
 * Sprint 7.c will additionally abort in-flight executions. */
static inline void prst_graph_invalidate(PrstGraph *g, const char *prst_name) {
    int i = 0;
    while (i < g->count) {
        if (strcmp(g->deps[i].prst_name, prst_name) == 0) {
            g->deps[i] = g->deps[g->count - 1];
            g->count--;
        } else {
            i++;
        }
    }
}

static inline void prst_graph_free(PrstGraph *g) { g->count = 0; }

#endif /* FLUXA_PRST_GRAPH_H */
