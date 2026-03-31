/* prst_graph.h — Persistent Variable Dependency Graph (Sprint 7 / Sprint 8)
 *
 * Sprint 7:   record, query, print (fluxa explain), deduplication.
 * Sprint 7.b: checksum FNV-32, serialization, real invalidation mark.
 * Sprint 8:   array dinâmico (malloc/realloc) — cap configurável via
 *             fluxa.toml [runtime] prst_cap (default PRST_GRAPH_CAP_DEFAULT).
 *             prst_graph_init_cap(g, cap) substitui prst_graph_init(g).
 *             prst_graph_init(g) mantido como alias com cap padrão.
 */
#ifndef FLUXA_PRST_GRAPH_H
#define FLUXA_PRST_GRAPH_H

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Cap padrão — usado quando fluxa.toml não especifica prst_cap */
#define PRST_GRAPH_CAP_DEFAULT 256
/* Cap máximo absoluto compilado — barreira de segurança */
#define PRST_GRAPH_CAP_MAX     65536

typedef struct {
    char prst_name[256];
    char reader_ctx[256];
} PrstDep;

typedef struct {
    PrstDep *deps;   /* heap-allocated — realloc conforme cresce */
    int      count;
    int      cap;    /* cap atual — configurável via fluxa.toml   */
} PrstGraph;

/* ── Inicialização ───────────────────────────────────────────────────────── */
static inline void prst_graph_init_cap(PrstGraph *g, int cap) {
    if (cap <= 0 || cap > PRST_GRAPH_CAP_MAX) cap = PRST_GRAPH_CAP_DEFAULT;
    g->deps  = (PrstDep *)malloc(sizeof(PrstDep) * (size_t)cap);
    g->count = 0;
    g->cap   = g->deps ? cap : 0;
}

/* Compatibilidade com código anterior — usa cap padrão */
static inline void prst_graph_init(PrstGraph *g) {
    prst_graph_init_cap(g, PRST_GRAPH_CAP_DEFAULT);
}

static inline void prst_graph_free(PrstGraph *g) {
    free(g->deps);
    g->deps  = NULL;
    g->count = 0;
    g->cap   = 0;
}

/* ── record ──────────────────────────────────────────────────────────────── */
static inline void prst_graph_record(PrstGraph *g,
                                      const char *prst_name,
                                      const char *reader_ctx) {
    const char *ctx = reader_ctx ? reader_ctx : "<global>";
    /* deduplicação */
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0 &&
            strcmp(g->deps[i].reader_ctx, ctx) == 0)
            return;
    /* grow se necessário */
    if (g->count >= g->cap) {
        int new_cap = g->cap > 0 ? g->cap * 2 : PRST_GRAPH_CAP_DEFAULT;
        if (new_cap > PRST_GRAPH_CAP_MAX) new_cap = PRST_GRAPH_CAP_MAX;
        if (g->count >= new_cap) {
            fprintf(stderr,
                "[fluxa] prst_graph: cap máximo (%d) atingido — dep descartado\n",
                PRST_GRAPH_CAP_MAX);
            return;
        }
        PrstDep *nd = (PrstDep *)realloc(g->deps,
                          sizeof(PrstDep) * (size_t)new_cap);
        if (!nd) {
            fprintf(stderr, "[fluxa] prst_graph: realloc falhou — dep descartado\n");
            return;
        }
        g->deps = nd;
        g->cap  = new_cap;
    }
    strncpy(g->deps[g->count].prst_name, prst_name,
            sizeof(g->deps[g->count].prst_name) - 1);
    g->deps[g->count].prst_name[sizeof(g->deps[g->count].prst_name)-1] = '\0';
    strncpy(g->deps[g->count].reader_ctx, ctx,
            sizeof(g->deps[g->count].reader_ctx) - 1);
    g->deps[g->count].reader_ctx[sizeof(g->deps[g->count].reader_ctx)-1] = '\0';
    g->count++;
}

/* ── query ───────────────────────────────────────────────────────────────── */
static inline int prst_graph_has_readers(PrstGraph *g, const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0) return 1;
    return 0;
}

static inline void prst_graph_print_readers(PrstGraph *g,
                                             const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0)
            printf("  %s  <-  %s\n", prst_name, g->deps[i].reader_ctx);
}

/* ── FNV-32 checksum ─────────────────────────────────────────────────────── */
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

/* ── serialization ───────────────────────────────────────────────────────── */
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
    if (cnt < 0 || cnt > PRST_GRAPH_CAP_MAX) return 0;
    size_t expected = sizeof(int32_t) + (size_t)cnt * sizeof(PrstDep);
    if (buf_size < expected) return 0;
    /* (re)inicializa com cap suficiente */
    prst_graph_free(g);
    prst_graph_init_cap(g, cnt > PRST_GRAPH_CAP_DEFAULT ? cnt : PRST_GRAPH_CAP_DEFAULT);
    if (!g->deps && cnt > 0) return 0;
    g->count = (int)cnt;
    if (cnt > 0)
        memcpy(g->deps, (const char*)buf + sizeof(int32_t),
               (size_t)cnt * sizeof(PrstDep));
    return 1;
}

/* ── invalidation ────────────────────────────────────────────────────────── */
static inline void prst_graph_invalidate(PrstGraph *g,
                                          const char *prst_name) {
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

#endif /* FLUXA_PRST_GRAPH_H */
