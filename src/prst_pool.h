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
    Value   value;          /* current runtime value (mutated at runtime)       */
    Value   init_value;     /* declared initializer — stored once, never mutated */
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
    /* Store the declared initializer value — used to detect source edits on reload.
     * A separate strdup because init_value and value can diverge at runtime. */
    p->entries[p->count].init_value   = p->entries[p->count].value;
    if (p->entries[p->count].init_value.type == VAL_STRING &&
        p->entries[p->count].init_value.as.string)
        p->entries[p->count].init_value.as.string =
            strdup(p->entries[p->count].init_value.as.string);
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
 * Used by Sprint 8 Atomic Handover for bit-to-bit integrity verification. */
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
 *   --- runtime value (current, possibly mutated) ---
 *   int64 int_val
 *   double float_val
 *   int32 bool_val
 *   int32 str_len
 *   char str_data[str_len]
 *   --- init_value (declared default at first run) ---
 *   int64 init_int_val
 *   double init_float_val
 *   int32 init_bool_val
 *   int32 init_str_len
 *   char init_str_data[init_str_len]
 *
 * Strings are inlined to avoid pointer issues during transfer.
 * Caller must free(*out_buf). Returns 1 on success, 0 on failure.
 */
/* Helper: serialize one Value block into wire buffer */
static inline char *prst_ser_value(char *w, const Value *v) {
    int64_t iv  = (v->type == VAL_INT)   ? (int64_t)v->as.integer : 0;
    double  fv  = (v->type == VAL_FLOAT) ? v->as.real             : 0.0;
    int32_t bv  = (v->type == VAL_BOOL)  ? v->as.boolean          : 0;
    int32_t slen = 0;
    if (v->type == VAL_STRING && v->as.string)
        slen = (int32_t)strlen(v->as.string);
    memcpy(w, &iv,   sizeof(int64_t)); w += sizeof(int64_t);
    memcpy(w, &fv,   sizeof(double));  w += sizeof(double);
    memcpy(w, &bv,   sizeof(int32_t)); w += sizeof(int32_t);
    memcpy(w, &slen, sizeof(int32_t)); w += sizeof(int32_t);
    if (slen > 0) { memcpy(w, v->as.string, (size_t)slen); w += slen; }
    return w;
}

/* Helper: byte count of one serialized Value */
static inline size_t prst_value_wire_size(const Value *v) {
    size_t s = sizeof(int64_t) + sizeof(double) + sizeof(int32_t)*2;
    if (v->type == VAL_STRING && v->as.string)
        s += strlen(v->as.string);
    return s;
}

static inline int prst_pool_serialize(const PrstPool *p,
                                       void **out_buf, size_t *out_size) {
    /* Per entry: name[256] + dt(i32) + so(i32) + value_block + init_value_block
     * value_block = i64 + f64 + i32 + i32 + str_data */
    size_t sz = sizeof(int32_t); /* count */
    for (int i = 0; i < p->count; i++) {
        sz += 256 + sizeof(int32_t)*2;   /* name + declared_type + stack_offset */
        sz += prst_value_wire_size(&p->entries[i].value);
        sz += prst_value_wire_size(&p->entries[i].init_value);
    }
    char *buf = (char*)malloc(sz);
    if (!buf) return 0;

    char *w = buf;
    int32_t cnt = (int32_t)p->count;
    memcpy(w, &cnt, sizeof(int32_t)); w += sizeof(int32_t);

    for (int i = 0; i < p->count; i++) {
        const PrstEntry *e = &p->entries[i];
        memcpy(w, e->name, 256); w += 256;
        int32_t dt = (int32_t)e->declared_type;
        memcpy(w, &dt, sizeof(int32_t)); w += sizeof(int32_t);
        int32_t so = (int32_t)e->stack_offset;
        memcpy(w, &so, sizeof(int32_t)); w += sizeof(int32_t);
        w = prst_ser_value(w, &e->value);
        w = prst_ser_value(w, &e->init_value);
    }

    *out_buf  = buf;
    *out_size = (size_t)(w - buf);
    return 1;
}

/* Helper: read one Value from wire buffer */
static inline const char *prst_deser_value(const char *r, const char *end,
                                            ValType dt, Value *out) {
    if (r + sizeof(int64_t) + sizeof(double) + sizeof(int32_t)*2 > end) return NULL;
    int64_t iv; memcpy(&iv, r, sizeof(int64_t)); r += sizeof(int64_t);
    double  fv; memcpy(&fv, r, sizeof(double));  r += sizeof(double);
    int32_t bv; memcpy(&bv, r, sizeof(int32_t)); r += sizeof(int32_t);
    int32_t sl; memcpy(&sl, r, sizeof(int32_t)); r += sizeof(int32_t);
    if (sl < 0 || r + sl > end) return NULL;
    out->type = dt;
    switch (dt) {
        case VAL_INT:    out->as.integer = (long)iv; break;
        case VAL_FLOAT:  out->as.real    = fv; break;
        case VAL_BOOL:   out->as.boolean = (int)bv; break;
        case VAL_STRING:
            out->as.string = sl > 0 ? (char*)malloc((size_t)sl+1) : strdup("");
            if (sl > 0) { memcpy(out->as.string, r, (size_t)sl); out->as.string[sl]='\0'; }
            break;
        default: out->type = VAL_NIL; break;
    }
    return r + sl;
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
        if (r + 256 + sizeof(int32_t)*2 > end) return 0;
        char name[256];
        memcpy(name, r, 256); name[255] = '\0'; r += 256;
        int32_t dt; memcpy(&dt, r, sizeof(int32_t)); r += sizeof(int32_t);
        int32_t so; memcpy(&so, r, sizeof(int32_t)); r += sizeof(int32_t);

        /* runtime value */
        Value v = {0};
        r = prst_deser_value(r, end, (ValType)dt, &v);
        if (!r) return 0;

        /* init_value (declared default at first run) */
        Value iv_val = {0};
        r = prst_deser_value(r, end, (ValType)dt, &iv_val);
        if (!r) return 0;

        int idx = prst_pool_set(p, name, v, NULL);
        if (idx >= 0) {
            p->entries[idx].declared_type = (ValType)dt;
            p->entries[idx].stack_offset  = (int)so;
            /* Restore init_value from wire — this is the key to correct
             * reload detection: the source default that was declared at first run. */
            if (p->entries[idx].init_value.type == VAL_STRING &&
                p->entries[idx].init_value.as.string)
                free(p->entries[idx].init_value.as.string);
            p->entries[idx].init_value = iv_val;
        } else {
            /* Free strings if not stored */
            if (iv_val.type == VAL_STRING && iv_val.as.string)
                free(iv_val.as.string);
        }
        if (v.type == VAL_STRING && v.as.string) free(v.as.string);
    }
    return 1;
}
