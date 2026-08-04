/* pool.h — Arena allocator for ASTNodes */
#ifndef FLUXA_POOL_H
#define FLUXA_POOL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ast.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef FLUXA_HUGEPAGES
/* madvise(MADV_HUGEPAGE) hints the kernel to back large arenas with
 * 2MB transparent huge pages, reducing dTLB pressure when the parser
 * and runtime walk the AST node array in tight loops.
 * Benchmark-gated: only enabled with FLUXA_HUGEPAGES=1.
 * Linux only — no-op on other platforms. */
#  if defined(__linux__)
#    include <sys/mman.h>
#    ifndef MADV_HUGEPAGE
#      define MADV_HUGEPAGE 14  /* in case older glibc doesn't define it */
#    endif
#    define FLUXA_POOL_MADVISE(ptr, sz)          madvise((void*)(ptr), (sz), MADV_HUGEPAGE)
#  else
#    define FLUXA_POOL_MADVISE(ptr, sz) ((void)0)
#  endif
#else
#  define FLUXA_POOL_MADVISE(ptr, sz) ((void)0)
#endif /* FLUXA_HUGEPAGES */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#ifndef strdup
char *strdup(const char *s);
#endif
#endif

#define POOL_NODE_CAP_DEFAULT 4096     /* was POOL_CAPACITY */
#define POOL_STR_CAP_DEFAULT  65536    /* was POOL_STR_CAPACITY */

/* Heap tag for overflow allocations. POOL_HEAP_NODE entries also have
 * realloc'd arrays inside them; pool_free walks both their arrays and
 * frees the node. POOL_HEAP_STR entries are flat strdup'd char*. */
typedef enum {
    POOL_HEAP_NODE,
    POOL_HEAP_STR,
} PoolHeapKind;

typedef struct {
    void        *p;
    PoolHeapKind kind;
} PoolHeapEntry;

typedef struct {
    /* Fast path for the (overwhelmingly common) default-cap case: no
     * malloc/free per pool_init()/pool_free() cycle, and no heap indirection
     * while walking the AST — byte-for-byte the same layout and performance
     * as before ast_pool_cap/ast_str_pool_cap existed. Only used when
     * node_cap/str_cap equal the compiled-in default; a configured
     * non-default cap allocates nodes/str_buf on the heap instead (below). */
    ASTNode default_nodes[POOL_NODE_CAP_DEFAULT];
    char    default_str_buf[POOL_STR_CAP_DEFAULT];

    ASTNode *nodes;      /* == default_nodes, or a malloc'd buffer when a
                           * non-default ast_pool_cap is configured */
    char    *str_buf;    /* == default_str_buf, or a malloc'd buffer when a
                           * non-default ast_str_pool_cap is configured */
    int      nodes_heap;  /* 1 if `nodes` was malloc'd and must be freed */
    int      str_buf_heap; /* 1 if `str_buf` was malloc'd and must be freed */
    int      node_cap;   /* size nodes was allocated with, snapshotted at
                           * pool_init() time from g_ast_pool_cap */
    int      str_cap;    /* size str_buf was allocated with, snapshotted at
                           * pool_init() time from g_ast_str_pool_cap */
    int     node_count;
    int     str_used;
    int     overflowed_nodes;  /* 1 once the first node overflow this pool
                                 * life has already been logged */
    int     overflowed_str;    /* 1 once the first string overflow this
                                 * pool life has already been logged */
    /* Overflow allocations that pool_free must reclaim. NULL until first
     * overflow — keeps the common (no-overflow) path zero-cost. */
    PoolHeapEntry *heap;
    int            heap_count;
    int            heap_cap;
} ASTPool;

/* AST pool capacities — configurable via [runtime] ast_pool_cap /
 * ast_str_pool_cap in fluxa.toml (see toml_config.h). Defaults match the
 * historical hard-coded POOL_NODE_CAP_DEFAULT/POOL_STR_CAP_DEFAULT, so a
 * project with no fluxa.toml (or no [runtime] keys) behaves exactly like
 * before this option existed.
 *
 * pool.h is header-only (static inline throughout), so these globals have
 * internal linkage PER TRANSLATION UNIT. This is safe only because
 * pool_init() is the sole reader of them: it snapshots the current value
 * into the ASTPool instance (node_cap/str_cap above), and
 * pool_alloc_node()/pool_strdup()/pool_free() only ever look at the
 * instance fields, never the globals directly. Consequently
 * pool_set_node_cap()/pool_set_str_cap() only take effect for pool_init()
 * calls made in the SAME .c file, after the setter call — main.c satisfies
 * this today. A new .c file that adds its own pool_init() call and wants a
 * configured cap must call these setters itself first. */
static int g_ast_pool_cap     = POOL_NODE_CAP_DEFAULT;
static int g_ast_str_pool_cap = POOL_STR_CAP_DEFAULT;

/* [runtime] ast_pool_cap — floor at the historical default (4096) so a
 * misconfigured toml can never make the pool smaller than the built-in
 * default (defense in depth beyond the toml-parse-time clamp). */
static inline void pool_set_node_cap(int cap) {
    if (cap < POOL_NODE_CAP_DEFAULT) cap = POOL_NODE_CAP_DEFAULT;
    g_ast_pool_cap = cap;
}
/* [runtime] ast_str_pool_cap — floor at the historical default (65536). */
static inline void pool_set_str_cap(int cap) {
    if (cap < POOL_STR_CAP_DEFAULT) cap = POOL_STR_CAP_DEFAULT;
    g_ast_str_pool_cap = cap;
}

static inline void pool_init(ASTPool *p) {
    p->node_cap = g_ast_pool_cap;
    if (p->node_cap == POOL_NODE_CAP_DEFAULT) {
        p->nodes      = p->default_nodes;
        p->nodes_heap = 0;
    } else {
        p->nodes      = (ASTNode *)malloc((size_t)p->node_cap * sizeof(ASTNode));
        p->nodes_heap = 1;
        if (!p->nodes) p->node_cap = 0;   /* graceful degrade: pool_alloc_node
                                            * falls straight through to its
                                            * existing per-item malloc()
                                            * fallback — never crashes. Matters
                                            * on constrained embedded targets
                                            * where a large configured cap
                                            * might not fit. */
    }

    p->str_cap = g_ast_str_pool_cap;
    if (p->str_cap == POOL_STR_CAP_DEFAULT) {
        p->str_buf      = p->default_str_buf;
        p->str_buf_heap = 0;
    } else {
        p->str_buf      = (char *)malloc((size_t)p->str_cap);
        p->str_buf_heap = 1;
        if (!p->str_buf) p->str_cap = 0;
    }

    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed_nodes = 0;
    p->overflowed_str   = 0;
    p->heap       = NULL;
    p->heap_count = 0;
    p->heap_cap   = 0;
#ifdef FLUXA_HUGEPAGES
    /* Hint the kernel to back these arenas with huge pages.
     * Called once per parse cycle — the overhead is negligible vs
     * the TLB savings on programs with large ASTs. */
    if (p->nodes)
        FLUXA_POOL_MADVISE(p->nodes, (size_t)p->node_cap * sizeof(ASTNode));
    if (p->str_buf)
        FLUXA_POOL_MADVISE(p->str_buf, (size_t)p->str_cap);
#endif
}

/* Record an overflow allocation so pool_free can reclaim it. */
static inline void pool_track(ASTPool *p, void *ptr, PoolHeapKind kind) {
    if (!ptr) return;
    if (p->heap_count >= p->heap_cap) {
        int nc = p->heap_cap ? p->heap_cap * 2 : 16;
        PoolHeapEntry *ne = (PoolHeapEntry *)realloc(
            p->heap, sizeof(PoolHeapEntry) * (size_t)nc);
        if (!ne) return;  /* best-effort: leak rather than crash */
        p->heap     = ne;
        p->heap_cap = nc;
    }
    p->heap[p->heap_count].p    = ptr;
    p->heap[p->heap_count].kind = kind;
    p->heap_count++;
}

static inline ASTNode *pool_alloc_node(ASTPool *p) {
    if (p->node_count < p->node_cap) {
        ASTNode *n = &p->nodes[p->node_count++];
        memset(n, 0, sizeof(ASTNode));
        n->resolved_offset = -1;
        return n;
    }
    if (!p->overflowed_nodes) {
        p->overflowed_nodes = 1;
        fprintf(stderr,
            "[fluxa] pool: node capacity (%d) exceeded — falling back to "
            "malloc() for the rest of this run (raise ast_pool_cap in "
            "[runtime] to avoid the per-node allocation cost)\n",
            p->node_cap);
    }
    ASTNode *n = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    n->resolved_offset = -1;
    pool_track(p, n, POOL_HEAP_NODE);
    return n;
}

static inline char *pool_strdup(ASTPool *p, const char *s) {
    if (!s) s = "";
    int len = (int)strlen(s) + 1;
    if (p->str_used + len <= p->str_cap) {
        char *dest = p->str_buf + p->str_used;
        memcpy(dest, s, (size_t)len);
        p->str_used += len;
        return dest;
    }
    if (!p->overflowed_str) {
        p->overflowed_str = 1;
        fprintf(stderr,
            "[fluxa] pool: string capacity (%d) exceeded — falling back to "
            "strdup() for the rest of this run (raise ast_str_pool_cap in "
            "[runtime] to avoid the per-string allocation cost)\n",
            p->str_cap);
    }
    char *dup = strdup(s);
    pool_track(p, dup, POOL_HEAP_STR);
    return dup;
}

static inline void pool_free(ASTPool *p) {
    /* Free the realloc'd heap arrays attached to every in-pool ASTNode
     * (children, args, elements, members, params). The strings inside
     * those nodes come from pool_strdup — owned by str_buf or tracked
     * separately as POOL_HEAP_STR. */
    for (int i = 0; i < p->node_count; i++)
        ast_free_arrays(&p->nodes[i]);
    /* Reclaim overflow allocations. */
    for (int i = 0; i < p->heap_count; i++) {
        if (p->heap[i].kind == POOL_HEAP_NODE) {
            ASTNode *n = (ASTNode *)p->heap[i].p;
            ast_free_arrays(n);
            free(n);
        } else {
            free(p->heap[i].p);
        }
    }
    free(p->heap);
    if (p->nodes_heap)   free(p->nodes);
    if (p->str_buf_heap) free(p->str_buf);
    p->nodes      = NULL;
    p->str_buf    = NULL;
    p->nodes_heap   = 0;
    p->str_buf_heap = 0;
    p->node_cap   = 0;
    p->str_cap    = 0;
    p->heap       = NULL;
    p->heap_count = 0;
    p->heap_cap   = 0;
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed_nodes = 0;
    p->overflowed_str   = 0;
}

#endif /* FLUXA_POOL_H */
