/* prst_graph.h — Persistent Variable Dependency Graph (Sprint 6 stub)
 *
 * Sprint 6: structure only — no invalidation logic yet.
 *           Every read of a prst variable registers a dependency here.
 *
 * Sprint 7 (hot reload) adds:
 *   - prst_graph_invalidate(): traverse and abort dependent executions
 *   - prst_graph_remove(): remove all deps for a given prst name
 *   - Atomic cascade invalidation
 *
 * Using a flat array for now (not a hash) — Sprint 7 can upgrade if needed.
 * Cap of 256 dependencies is sufficient for Sprint 6 instrumentation.
 */
#ifndef FLUXA_PRST_GRAPH_H
#define FLUXA_PRST_GRAPH_H

#include <string.h>
#include <stdio.h>

#define PRST_GRAPH_CAP 256

typedef struct {
    char prst_name[256];    /* name of the prst variable that was read    */
    char reader_ctx[256];   /* function/Block/method that read it         */
} PrstDep;

typedef struct {
    PrstDep deps[PRST_GRAPH_CAP];
    int     count;
} PrstGraph;

static inline void prst_graph_init(PrstGraph *g) {
    g->count = 0;
}

/* Record that reader_ctx read the prst variable prst_name.
 * Silently drops if at capacity (Sprint 7 can handle overflow). */
static inline void prst_graph_record(PrstGraph *g,
                                      const char *prst_name,
                                      const char *reader_ctx) {
    if (g->count >= PRST_GRAPH_CAP) return;
    strncpy(g->deps[g->count].prst_name, prst_name, 255);
    g->deps[g->count].prst_name[255] = '\0';
    strncpy(g->deps[g->count].reader_ctx, reader_ctx ? reader_ctx : "<global>", 255);
    g->deps[g->count].reader_ctx[255] = '\0';
    g->count++;
}

/* Sprint 7 hook — stub, does nothing yet */
static inline void prst_graph_invalidate(PrstGraph *g, const char *prst_name) {
    (void)g;
    (void)prst_name;
    /* TODO Sprint 7: traverse deps, abort all executions that read prst_name */
}

static inline void prst_graph_free(PrstGraph *g) {
    g->count = 0;
}

#endif /* FLUXA_PRST_GRAPH_H */
