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

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#ifndef strdup
char *strdup(const char *s);
#endif
#endif

#define POOL_CAPACITY     4096
#define POOL_STR_CAPACITY 65536

typedef struct {
    ASTNode nodes[POOL_CAPACITY];
    char    str_buf[POOL_STR_CAPACITY];
    int     node_count;
    int     str_used;
    int     overflowed;
} ASTPool;

static inline void pool_init(ASTPool *p) {
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
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
    n->resolved_offset = -1;
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
    return strdup(s);
}

static inline void pool_free(ASTPool *p) {
    p->node_count = 0;
    p->str_used   = 0;
    p->overflowed = 0;
}

#endif /* FLUXA_POOL_H */
