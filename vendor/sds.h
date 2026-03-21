/* sds.h — Simple Dynamic Strings (minimal subset for Fluxa)
 * Original: https://github.com/antirez/sds — BSD license
 * This is a minimal self-contained subset sufficient for Fluxa's lexer.
 */
#ifndef SDS_H
#define SDS_H

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef char* sds;

typedef struct {
    size_t len;
    size_t alloc;
    char   buf[];
} sdshdr;

#define SDS_HDR(s) ((sdshdr*)((s) - sizeof(sdshdr)))

static inline size_t sdslen(const sds s) { return SDS_HDR(s)->len; }

static inline sds sdsnewlen(const void *init, size_t len) {
    sdshdr *hdr = (sdshdr*)malloc(sizeof(sdshdr) + len + 1);
    if (!hdr) return NULL;
    hdr->len   = len;
    hdr->alloc = len;
    if (init) memcpy(hdr->buf, init, len);
    hdr->buf[len] = '\0';
    return hdr->buf;
}

static inline sds sdsnew(const char *s) {
    return sdsnewlen(s, s ? strlen(s) : 0);
}

static inline sds sdsempty(void) { return sdsnewlen("", 0); }

static inline void sdsfree(sds s) {
    if (s) free(SDS_HDR(s));
}

static inline sds sdscatlen(sds s, const void *t, size_t len) {
    sdshdr *hdr = SDS_HDR(s);
    size_t  cur = hdr->len;
    size_t  need = cur + len + 1;
    hdr = (sdshdr*)realloc(hdr, sizeof(sdshdr) + need);
    if (!hdr) return NULL;
    hdr->alloc = cur + len;
    memcpy(hdr->buf + cur, t, len);
    hdr->len = cur + len;
    hdr->buf[hdr->len] = '\0';
    return hdr->buf;
}

static inline sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

static inline sds sdscatchar(sds s, char c) {
    return sdscatlen(s, &c, 1);
}

#endif /* SDS_H */
