#ifndef FLUXA_FXSTR_H
#define FLUXA_FXSTR_H
/* ── Refcounted immutable strings ────────────────────────────────────────
 * Layout: [ FxStrHdr | bytes... NUL ]; Value.as.string points at bytes, so
 * every reader of .as.string is untouched. Fluxa strings are IMMUTABLE
 * (mutation always produces a new buffer), so sharing the pointer on read
 * is always safe:
 *   write / produce  -> new buffer, rc = 1        (fxstr_new / _new_len / _alloc)
 *   read             -> same buffer, rc + 1       (fxstr_retain, O(1))
 *   free / reassign / frame teardown -> rc - 1    (fxstr_release)
 *   heap dies only when rc reaches 0.
 * Counters are atomic (GCC __atomic builtins) so strings crossing
 * flxthread boundaries stay correct without a copy rule.
 * BOUNDARIES: the prst pool and FFI/lib internals keep plain malloc
 * buffers; they never enter a Value directly — producers copy via
 * fxstr_new / fxstr_new_len or hand off via fxstr_adopt_raw. Never call
 * retain/release on a pointer that did not come from this allocator. */
#include <stdlib.h>
#include <string.h>

typedef struct { int rc; } FxStrHdr;
#define FXSTR_HDR(p) ((FxStrHdr *)((char *)(p) - sizeof(FxStrHdr)))

static inline char *fxstr_alloc(size_t n) {          /* zeroed, rc=1 */
    char *buf = (char *)calloc(1, sizeof(FxStrHdr) + n + 1);
    if (!buf) return NULL;
    ((FxStrHdr *)buf)->rc = 1;
    return buf + sizeof(FxStrHdr);
}
static inline char *fxstr_new_len(const char *src, size_t n) {
    char *buf = (char *)malloc(sizeof(FxStrHdr) + n + 1);
    if (!buf) return NULL;
    ((FxStrHdr *)buf)->rc = 1;
    {
        char *p = buf + sizeof(FxStrHdr);
        if (src && n) memcpy(p, src, n);
        p[n] = '\0';
        return p;
    }
}
static inline char *fxstr_new(const char *src) {
    return fxstr_new_len(src ? src : "", src ? strlen(src) : 0);
}
static inline char *fxstr_retain(char *p) {
    if (p) __atomic_fetch_add(&FXSTR_HDR(p)->rc, 1, __ATOMIC_RELAXED);
    return p;
}
static inline void fxstr_release(char *p) {
    if (!p) return;
    if (__atomic_sub_fetch(&FXSTR_HDR(p)->rc, 1, __ATOMIC_ACQ_REL) == 0)
        free((char *)p - sizeof(FxStrHdr));
}
/* Adopt a plain-malloc string from a lib/FFI boundary: copy into a
 * refcounted buffer, free the raw pointer. */
static inline char *fxstr_adopt_raw(char *raw) {
    char *p = fxstr_new(raw ? raw : "");
    free(raw);
    return p;
}
#endif /* FLUXA_FXSTR_H */
