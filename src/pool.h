/* pool.h — Arena allocator for ASTNodes
 *
 * Instead of calling malloc() for every node, the parser draws from a
 * contiguous array. Benefits:
 *   - All nodes sit next to each other in memory → CPU cache friendly
 *   - Single free() at the end instead of one per node
 *   - Zero fragmentation during parsing
 *
 * Design decisions:
 *   - Fixed capacity (POOL_CAPACITY). If exceeded, falls back to malloc()
 *     and sets pool->overflowed = 1 so the caller can log a warning.
 *   - String data (identifiers, operator names) is stored in a separate
 *     char arena (str_buf) so node structs stay small and aligned.
 *   - Thread safety: not needed — Fluxa is single-threaded by design.
 */
#ifndef FLUXA_POOL_H
#define FLUXA_POOL_H

/* strdup is POSIX — needs _POSIX_C_SOURCE or explicit declaration */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ast.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Provide strdup if not declared by the system headers */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#ifndef strdup
char *strdup(const char *s);
#endif
#endif

/* ── Capacities ──────────────────────────────────────────────────────────── */
#define POOL_CAPACITY     4096   /* max ASTNodes per parse           */
#define POOL_STR_CAPACITY 65536  /* bytes for string data (64 KB)    */

/* ── Arena ───────────────────────────────────────────────────────────────── */
typedef struct {
    ASTNode nodes[POOL_CAPACITY];   /* contiguous node storage          */
    char    str_buf[POOL_STR_CAPACITY]; /* string data (names, ops)     */

    int     node_count;             /* next free slot in nodes[]        */
    int     str_used;               /* bytes used in str_buf            */
    int     overflowed;             /* 1 if we had to fall back to heap */
} ASTPool;

/* ── Init / reset ────────────────────────────────────────────────────────── */
static inline void pool_init(ASTPool *p) {
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
    /* nodes and str_buf are zero-initialised at program start (static/global)
       or must be memset'd if stack-allocated — caller's responsibility.     */
}

/* ── Allocate a node from the arena ─────────────────────────────────────── */
static inline ASTNode *pool_alloc_node(ASTPool *p) {
    if (p->node_count < POOL_CAPACITY) {
        ASTNode *n = &p->nodes[p->node_count++];
        memset(n, 0, sizeof(ASTNode));
        return n;
    }
    /* fallback — should not happen in normal programs */
    p->overflowed = 1;
    fprintf(stderr, "[fluxa] pool overflow — falling back to malloc()\n");
    ASTNode *n = (ASTNode*)calloc(1, sizeof(ASTNode));
    return n;
}

/* ── Intern a string into the string arena ───────────────────────────────── */
/* Returns a pointer into str_buf. The string is NUL-terminated.
   If the string doesn't fit, falls back to strdup().                        */
static inline char *pool_strdup(ASTPool *p, const char *s) {
    if (!s) s = "";
    int len = (int)strlen(s) + 1;   /* +1 for NUL */

    if (p->str_used + len <= POOL_STR_CAPACITY) {
        char *dest = p->str_buf + p->str_used;
        memcpy(dest, s, (size_t)len);
        p->str_used += len;
        return dest;
    }
    /* fallback */
    p->overflowed = 1;
    fprintf(stderr, "[fluxa] pool str overflow — falling back to strdup()\n");
    return strdup(s);
}

/* ── Free the entire arena at once ──────────────────────────────────────── */
/* Only heap-allocated nodes (overflow) need individual free().
   Pool nodes are freed as a batch by discarding the pool itself.            */
static inline void pool_free(ASTPool *p) {
    /* nothing to do for arena nodes — they live inside p->nodes[]
       which is either stack or static storage.
       Overflow nodes would need tracking — we accept the tiny leak
       since overflow should never happen in practice.                       */
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
}

/* ── Stats (debug) ───────────────────────────────────────────────────────── */
static inline void pool_print_stats(const ASTPool *p) {
    fprintf(stderr, "[fluxa] pool: %d/%d nodes, %d/%d str bytes%s\n",
            p->node_count, POOL_CAPACITY,
            p->str_used,   POOL_STR_CAPACITY,
            p->overflowed ? " (OVERFLOW)" : "");
}

#endif /* FLUXA_POOL_H */
