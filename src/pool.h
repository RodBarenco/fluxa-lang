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

#define POOL_CAPACITY     4096
#define POOL_STR_CAPACITY 65536

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
    ASTNode nodes[POOL_CAPACITY];
    char    str_buf[POOL_STR_CAPACITY];
    int     node_count;
    int     str_used;
    int     overflowed;
    /* Overflow allocations that pool_free must reclaim. NULL until first
     * overflow — keeps the common (no-overflow) path zero-cost. */
    PoolHeapEntry *heap;
    int            heap_count;
    int            heap_cap;
} ASTPool;

static inline void pool_init(ASTPool *p) {
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
    p->heap       = NULL;
    p->heap_count = 0;
    p->heap_cap   = 0;
#ifdef FLUXA_HUGEPAGES
    /* Hint the kernel to back these arenas with huge pages.
     * Called once per parse cycle — the overhead is negligible vs
     * the TLB savings on programs with large ASTs. */
    FLUXA_POOL_MADVISE(p->nodes,   sizeof(p->nodes));
    FLUXA_POOL_MADVISE(p->str_buf, sizeof(p->str_buf));
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
    if (p->node_count < POOL_CAPACITY) {
        ASTNode *n = &p->nodes[p->node_count++];
        memset(n, 0, sizeof(ASTNode));
        n->resolved_offset = -1;
        return n;
    }
    p->overflowed = 1;
    fprintf(stderr, "[fluxa] pool overflow — falling back to malloc()\n");
    ASTNode *n = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    n->resolved_offset = -1;
    pool_track(p, n, POOL_HEAP_NODE);
    return n;
}

static inline char *pool_strdup(ASTPool *p, const char *s) {
    if (!s) s = "";
    int len = (int)strlen(s) + 1;
    if (p->str_used + len <= POOL_STR_CAPACITY) {
        char *dest = p->str_buf + p->str_used;
        memcpy(dest, s, (size_t)len);
        p->str_used += len;
        return dest;
    }
    p->overflowed = 1;
    fprintf(stderr, "[fluxa] pool str overflow — falling back to strdup()\n");
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
    p->heap       = NULL;
    p->heap_count = 0;
    p->heap_cap   = 0;
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
}

#endif /* FLUXA_POOL_H */
