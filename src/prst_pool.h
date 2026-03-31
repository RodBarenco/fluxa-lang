/* prst_pool.h — Persistent Variable Pool (Sprint 7)
 *
 * Sprint 6.b: dynamic array, realloc ×2, basic get/set.
 * Sprint 7:   real semantics:
 *   - Type collision detection: same name + different type → error
 *   - Reload semantics: on re-declaration of existing prst, value is
 *     restored from pool instead of re-evaluated from AST initializer
 *   - Only active in FLUXA_MODE_PROJECT (prst present in source)
 *   - In FLUXA_MODE_SCRIPT: prst declarations emit a warning, pool unused
 */
#ifndef FLUXA_PRST_POOL_H
#define FLUXA_PRST_POOL_H

#include "scope.h"
#include "err.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PRST_POOL_INIT_CAP 64

typedef struct {
    char    name[256];
    Value   value;
    ValType declared_type;
    int     stack_offset;   /* resolved_offset of the VAR_DECL node, -1 if unknown */
} PrstEntry;

typedef struct {
    PrstEntry *entries;
    int        count;
    int        cap;
} PrstPool;

static inline void prst_pool_init(PrstPool *p) {
    p->entries = (PrstEntry *)malloc(sizeof(PrstEntry) * PRST_POOL_INIT_CAP);
    p->count   = 0;
    p->cap     = p->entries ? PRST_POOL_INIT_CAP : 0;
}

static inline void prst_pool_free(PrstPool *p) {
    if (!p->entries) return;
    for (int i = 0; i < p->count; i++) {
        if (p->entries[i].value.type == VAL_STRING && p->entries[i].value.as.string)
            free(p->entries[i].value.as.string);
    }
    free(p->entries);
    p->entries = NULL;
    p->count   = 0;
    p->cap     = 0;
}

static inline int prst_pool_find(PrstPool *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->entries[i].name, name) == 0)
            return i;
    return -1;
}

/* Set a prst variable.
 * First declaration: stores value + declared_type.
 * Re-declaration (reload): type must match — collision → err, returns -1.
 * Returns index on success, -1 on error. */
static inline int prst_pool_set(PrstPool *p, const char *name,
                                  Value value, ErrStack *err) {
    int idx = prst_pool_find(p, name);
    if (idx >= 0) {
        if (p->entries[idx].declared_type != value.type) {
            char buf[280];
            snprintf(buf, sizeof(buf),
                "prst collision: '%s' was type %d, reload attempts type %d — state preserved",
                name, (int)p->entries[idx].declared_type, (int)value.type);
            if (err) errstack_push(err, ERR_RELOAD, buf, "<prst>", 0);
            return -1;
        }
        if (p->entries[idx].value.type == VAL_STRING &&
            p->entries[idx].value.as.string)
            free(p->entries[idx].value.as.string);
        if (value.type == VAL_STRING && value.as.string)
            value.as.string = strdup(value.as.string);
        p->entries[idx].value = value;
        return idx;
    }
    if (p->count >= p->cap) {
        int new_cap = p->cap > 0 ? p->cap * 2 : PRST_POOL_INIT_CAP;
        PrstEntry *ne = (PrstEntry *)realloc(p->entries,
                            sizeof(PrstEntry) * new_cap);
        if (!ne) {
            if (err) errstack_push(err, ERR_FLUXA,
                "out of memory growing prst pool", "<prst>", 0);
            return -1;
        }
        p->entries = ne;
        p->cap     = new_cap;
    }
    {
        size_t _nlen = strlen(name);
        size_t _ncap = sizeof(p->entries[p->count].name) - 1;
        if (_nlen > _ncap) _nlen = _ncap;
        memcpy(p->entries[p->count].name, name, _nlen);
        p->entries[p->count].name[_nlen] = '\0';
    }
    p->entries[p->count].declared_type = value.type;
    if (value.type == VAL_STRING && value.as.string)
        value.as.string = strdup(value.as.string);
    p->entries[p->count].value        = value;
    p->entries[p->count].stack_offset = -1;   /* set by caller after decl */
    return p->count++;
}

/* Set the stack_offset for a prst entry — called once after NODE_VAR_DECL
 * resolution so the VM sync pass can read rt->stack[offset] directly. */
static inline void prst_pool_set_offset(PrstPool *p, const char *name,
                                         int offset) {
    int idx = prst_pool_find(p, name);
    if (idx >= 0) p->entries[idx].stack_offset = offset;
}

static inline int prst_pool_get(PrstPool *p, const char *name, Value *out) {
    int idx = prst_pool_find(p, name);
    if (idx < 0) return 0;
    *out = p->entries[idx].value;
    return 1;
}

static inline int prst_pool_has(PrstPool *p, const char *name) {
    return prst_pool_find(p, name) >= 0;
}

static inline void prst_pool_invalidate(PrstPool *p, const char *name) {
    int idx = prst_pool_find(p, name);
    if (idx < 0) return;
    if (p->entries[idx].value.type == VAL_STRING &&
        p->entries[idx].value.as.string)
        free(p->entries[idx].value.as.string);
    p->entries[idx] = p->entries[p->count - 1];
    p->count--;
}

#endif /* FLUXA_PRST_POOL_H */

/* ── Sprint 7.b additions (appended) ─────────────────────────────────────── */

#include <stdint.h>

/* FNV-32 checksum over all pool entries (names + types + int values).
 * String values are not checksummed (pointer instability across reloads).
 * Used by Sprint 8 Handover Atômico for bit-to-bit integrity verification. */
static inline uint32_t prst_pool_checksum(const PrstPool *p) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < p->count; i++) {
        const char *n = p->entries[i].name;
        while (*n) { h ^= (uint8_t)*n++; h *= 16777619u; }
        h ^= 0x1f;
        /* include type tag and integer value in checksum */
        h ^= (uint8_t)p->entries[i].declared_type;
        h *= 16777619u;
        if (p->entries[i].value.type == VAL_INT) {
            long v = p->entries[i].value.as.integer;
            h ^= (uint32_t)(v & 0xFFFFFFFF);
            h *= 16777619u;
            h ^= (uint32_t)((v >> 32) & 0xFFFFFFFF);
            h *= 16777619u;
        }
        h ^= 0x1e;
    }
    return h;
}

/* ── Serialization ───────────────────────────────────────────────────────── */
/* Wire format (flat, no pointers):
 *   [int32 count]
 *   [count × PrstWireEntry]
 *
 * PrstWireEntry:
 *   char name[256]
 *   int32 declared_type
 *   int32 stack_offset
 *   int64 int_val        (valid when declared_type == VAL_INT)
 *   double float_val     (valid when declared_type == VAL_FLOAT)
 *   int32 bool_val       (valid when declared_type == VAL_BOOL)
 *   int32 str_len        (0 if not VAL_STRING)
 *   char str_data[str_len]   (inline, no nul-terminator in wire)
 *
 * Strings are inlined to avoid pointer issues during transfer.
 * Caller must free(*out_buf). Returns 1 on success, 0 on failure.
 */
static inline int prst_pool_serialize(const PrstPool *p,
                                       void **out_buf, size_t *out_size) {
    /* Two-pass: first calculate size, then fill
     * Per entry: name[256] + dt(int32) + so(int32) + iv(int64) + fv(double)
     *            + bv(int32) + slen(int32) + str_data[slen]
     * = 256 + 4 + 4 + 8 + 8 + 4 + 4 + slen = 288 + slen  */
    size_t sz = sizeof(int32_t);
    for (int i = 0; i < p->count; i++) {
        sz += 256 + sizeof(int32_t)*4 + sizeof(int64_t) + sizeof(double);
        if (p->entries[i].value.type == VAL_STRING && p->entries[i].value.as.string)
            sz += (size_t)strlen(p->entries[i].value.as.string);
    }
    char *buf = (char*)malloc(sz);
    if (!buf) return 0;

    char *w = buf;
    int32_t cnt = (int32_t)p->count;
    memcpy(w, &cnt, sizeof(int32_t)); w += sizeof(int32_t);

    for (int i = 0; i < p->count; i++) {
        const PrstEntry *e = &p->entries[i];
        /* name */
        memcpy(w, e->name, 256); w += 256;
        /* declared_type */
        int32_t dt = (int32_t)e->declared_type;
        memcpy(w, &dt, sizeof(int32_t)); w += sizeof(int32_t);
        /* stack_offset */
        int32_t so = (int32_t)e->stack_offset;
        memcpy(w, &so, sizeof(int32_t)); w += sizeof(int32_t);
        /* int_val */
        int64_t iv = (e->value.type == VAL_INT) ? (int64_t)e->value.as.integer : 0;
        memcpy(w, &iv, sizeof(int64_t)); w += sizeof(int64_t);
        /* float_val */
        double fv = (e->value.type == VAL_FLOAT) ? e->value.as.real : 0.0;
        memcpy(w, &fv, sizeof(double)); w += sizeof(double);
        /* bool_val */
        int32_t bv = (e->value.type == VAL_BOOL) ? e->value.as.boolean : 0;
        memcpy(w, &bv, sizeof(int32_t)); w += sizeof(int32_t);
        /* str_len + str_data */
        int32_t slen = 0;
        if (e->value.type == VAL_STRING && e->value.as.string)
            slen = (int32_t)strlen(e->value.as.string);
        memcpy(w, &slen, sizeof(int32_t)); w += sizeof(int32_t);
        if (slen > 0) { memcpy(w, e->value.as.string, (size_t)slen); w += slen; }
    }

    *out_buf  = buf;
    *out_size = (size_t)(w - buf);
    return 1;
}

/* Deserialize into an existing PrstPool (resets it first).
 * Returns 1 on success, 0 on malformed data. */
static inline int prst_pool_deserialize(PrstPool *p,
                                         const void *buf, size_t buf_size) {
    if (buf_size < sizeof(int32_t)) return 0;
    const char *r = (const char*)buf;
    const char *end = r + buf_size;

    int32_t cnt;
    memcpy(&cnt, r, sizeof(int32_t)); r += sizeof(int32_t);
    if (cnt < 0 || cnt > 65536) return 0;

    prst_pool_free(p);
    prst_pool_init(p);

    for (int i = 0; i < cnt; i++) {
        if (r + 256 + sizeof(int32_t)*4 + sizeof(int64_t) + sizeof(double) > end)
            return 0;
        char name[256];
        memcpy(name, r, 256); name[255] = '\0'; r += 256;
        int32_t dt; memcpy(&dt, r, sizeof(int32_t)); r += sizeof(int32_t);
        int32_t so; memcpy(&so, r, sizeof(int32_t)); r += sizeof(int32_t);
        int64_t iv; memcpy(&iv, r, sizeof(int64_t)); r += sizeof(int64_t);
        double  fv; memcpy(&fv, r, sizeof(double));  r += sizeof(double);
        int32_t bv; memcpy(&bv, r, sizeof(int32_t)); r += sizeof(int32_t);
        int32_t slen; memcpy(&slen, r, sizeof(int32_t)); r += sizeof(int32_t);
        if (slen < 0 || r + slen > end) return 0;

        Value v; v.type = (ValType)dt;
        switch (v.type) {
            case VAL_INT:    v.as.integer = (long)iv; break;
            case VAL_FLOAT:  v.as.real    = fv; break;
            case VAL_BOOL:   v.as.boolean = (int)bv; break;
            case VAL_STRING:
                v.as.string = slen > 0 ? (char*)malloc((size_t)slen + 1) : strdup("");
                if (slen > 0) { memcpy(v.as.string, r, (size_t)slen); v.as.string[slen] = '\0'; }
                break;
            default:         v.type = VAL_NIL; break;
        }
        r += slen;

        int idx = prst_pool_set(p, name, v, NULL);
        if (idx >= 0) {
            p->entries[idx].declared_type = (ValType)dt;
            p->entries[idx].stack_offset  = (int)so;
        }
        /* free string we just copied — prst_pool_set will strdup again */
        if (v.type == VAL_STRING && v.as.string) free(v.as.string);
    }
    return 1;
}
