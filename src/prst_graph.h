/* prst_graph.h — Persistent Variable Dependency Graph (Sprint 7)
 *
 * Tracks which functions/methods READ each prst variable.
 * Used by fluxa explain and future invalidation cascade.
 *
 * Structure: flat array of (prst_name, reader_ctx) pairs.
 * Deduplication: same pair is not recorded twice.
 * Cap: PRST_GRAPH_CAP (256) — sufficient for real programs.
 *      Silent drop on overflow (logs to stderr).
 *
 * Sprint 7 delivers: record, query, print (for fluxa explain).
 * Sprint 7.c delivers: invalidate (cascade abort on reload).
 */
#ifndef FLUXA_PRST_GRAPH_H
#define FLUXA_PRST_GRAPH_H

#include <string.h>
#include <stdio.h>

#define PRST_GRAPH_CAP 256

typedef struct {
    char prst_name[256];   /* qualified: "inst.field" or "global_name" */
    char reader_ctx[256];  /* fn/method/Block that read the prst var   */
} PrstDep;

typedef struct {
    PrstDep deps[PRST_GRAPH_CAP];
    int     count;
} PrstGraph;

static inline void prst_graph_init(PrstGraph *g) {
    g->count = 0;
}

/* Record a dependency. Deduplicates — same pair recorded only once. */
static inline void prst_graph_record(PrstGraph *g,
                                      const char *prst_name,
                                      const char *reader_ctx) {
    const char *ctx = reader_ctx ? reader_ctx : "<global>";
    /* dedup check */
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->deps[i].prst_name, prst_name) == 0 &&
            strcmp(g->deps[i].reader_ctx, ctx) == 0)
            return;
    }
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

/* Returns 1 if prst_name has any registered reader. */
static inline int prst_graph_has_readers(PrstGraph *g, const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0)
            return 1;
    return 0;
}

/* Print all deps for a given prst — used by fluxa explain. */
static inline void prst_graph_print_readers(PrstGraph *g, const char *prst_name) {
    for (int i = 0; i < g->count; i++)
        if (strcmp(g->deps[i].prst_name, prst_name) == 0)
            printf("  %s  <-  %s\n", prst_name, g->deps[i].reader_ctx);
}

/* Sprint 7.c hook — cascade invalidation on reload */
static inline void prst_graph_invalidate(PrstGraph *g, const char *prst_name) {
    (void)g; (void)prst_name;
    /* TODO Sprint 7.c: abort all executions that read prst_name */
}

static inline void prst_graph_free(PrstGraph *g) {
    g->count = 0;
}

#endif /* FLUXA_PRST_GRAPH_H */
