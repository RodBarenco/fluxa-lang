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

/* Forward declarations — prst_pool_free / prst_pool_set use them, but the
 * implementations live further down because they recurse on themselves
 * via VAL_ARR walks. */
static inline Value prst_value_deep_clone(const Value *src);
static inline void  prst_value_free_clone(Value *v);

static inline void prst_pool_init(PrstPool *p) {
    p->entries = (PrstEntry *)malloc(sizeof(PrstEntry) * PRST_POOL_INIT_CAP);
    p->count   = 0;
    p->cap     = p->entries ? PRST_POOL_INIT_CAP : 0;
}

static inline void prst_pool_free(PrstPool *p) {
    if (!p->entries) return;
    for (int i = 0; i < p->count; i++) {
        /* Free only what prst_pool_set's deep_clone actually allocated —
         * never VAL_DYN (GC) or VAL_BLOCK_INST (block_registry). */
        prst_value_free_clone(&p->entries[i].value);
        prst_value_free_clone(&p->entries[i].init_value);
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

/* Recursive deep-copy of a Value. The result is fully independent from
 * the source: every VAL_STRING is strdup'd, every VAL_ARR has its own
 * malloc'd buffer, and any nested VAL_ARR is itself deep-cloned.
 *
 * VAL_DYN, VAL_BLOCK_INST, VAL_PTR, VAL_FUNC and primitives are shallow
 * copies — VAL_DYN is GC-managed, VAL_BLOCK_INST lives in the global
 * block registry, VAL_PTR is opaque, VAL_FUNC points into the AST pool,
 * and primitives carry no heap pointer. The pool does not own them. */
static inline Value prst_value_deep_clone(const Value *src) {
    Value dst = *src;
    if (src->type == VAL_STRING && src->as.string) {
        dst.as.string = strdup(src->as.string);
    } else if (src->type == VAL_ARR && src->as.arr.data && src->as.arr.size > 0) {
        int n = src->as.arr.size;
        Value *data = (Value *)malloc(sizeof(Value) * (size_t)n);
        if (data) {
            for (int i = 0; i < n; i++)
                data[i] = prst_value_deep_clone(&src->as.arr.data[i]);
            dst.as.arr.data  = data;
            dst.as.arr.owned = 1;
        } else {
            dst.as.arr.data  = NULL;
            dst.as.arr.size  = 0;
        }
    }
    return dst;
}

/* Mirror of prst_value_deep_clone — frees only what the clone allocated.
 * Crucially, this does NOT touch VAL_DYN or VAL_BLOCK_INST: the GC and
 * the block registry respectively own those, and freeing them here
 * would double-free across reload boundaries (runtime stores the same
 * dyn pointer in scope and pool; GC sweeps it after handover). */
static inline void prst_value_free_clone(Value *v) {
    if (!v) return;
    if (v->type == VAL_STRING && v->as.string) {
        free(v->as.string);
        v->as.string = NULL;
    } else if (v->type == VAL_ARR && v->as.arr.data && v->as.arr.owned) {
        for (int i = 0; i < v->as.arr.size; i++)
            prst_value_free_clone(&v->as.arr.data[i]);
        free(v->as.arr.data);
        v->as.arr.data  = NULL;
        v->as.arr.size  = 0;
        v->as.arr.owned = 0;
    }
    /* VAL_DYN, VAL_BLOCK_INST, primitives: not owned by the pool. */
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
        /* Free the old value (string + any owned array data) and replace
         * with an independent deep clone of `value`. init_value is
         * preserved — only the runtime value is being updated. */
        prst_value_free_clone(&p->entries[idx].value);
        p->entries[idx].value = prst_value_deep_clone(&value);
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
    /* Take a fully independent copy of the value so caller-side frees do
     * not corrupt the pool entry. The previous code deep-copied only
     * VAL_STRING and the top level of VAL_ARR, leaving any nested
     * VAL_ARR sharing buffers with the caller — a use-after-free / double
     * free waiting to happen when the deserializer reclaimed its
     * originals after handing them off here. */
    p->entries[p->count].value = prst_value_deep_clone(&value);
    /* init_value gets its own independent clone so it can be overwritten
     * (e.g. by the deserializer restoring the on-wire init_value) without
     * affecting `value`. */
    p->entries[p->count].init_value   = prst_value_deep_clone(&value);
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
    /* VAL_ARR: count then (etag + value_block) per element */
    int32_t arr_count = (v->type == VAL_ARR && v->as.arr.data)
                        ? (int32_t)v->as.arr.size : 0;
    memcpy(w, &arr_count, sizeof(int32_t)); w += sizeof(int32_t);
    for (int _i = 0; _i < arr_count; _i++) {
        int32_t etag = (int32_t)v->as.arr.data[_i].type;
        memcpy(w, &etag, sizeof(int32_t)); w += sizeof(int32_t);
        w = prst_ser_value(w, &v->as.arr.data[_i]);
    }
    return w;
}

/* Helper: byte count of one serialized Value */
static inline size_t prst_value_wire_size(const Value *v) {
    size_t s = sizeof(int64_t) + sizeof(double) + sizeof(int32_t)*3; /* +arr_count */
    if (v->type == VAL_STRING && v->as.string)
        s += strlen(v->as.string);
    if (v->type == VAL_ARR && v->as.arr.data) {
        for (int _i = 0; _i < v->as.arr.size; _i++)
            s += sizeof(int32_t) + prst_value_wire_size(&v->as.arr.data[_i]);
    }
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

/* Helper: read one Value from wire buffer.
 * On NULL return, any partial allocation inside *out has already been
 * freed — callers can treat *out as untouched. */
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
    r += sl;
    /* VAL_ARR: read element count then deserialize each element recursively */
    if (r + (int)sizeof(int32_t) > end) return r; /* no arr_count field (old format) */
    int32_t arr_count; memcpy(&arr_count, r, sizeof(int32_t)); r += sizeof(int32_t);
    /* Bound arr_count to a sane limit — attacker-controlled snapshots have
     * caused 50-GB calloc attempts here. 65536 matches the per-pool entry
     * cap and is far beyond any realistic Fluxa program (especially on
     * embedded targets, where total SRAM is 264–520 KB). */
    if (arr_count < 0 || arr_count > 65536) {
        if (out->type == VAL_STRING && out->as.string) {
            free(out->as.string); out->as.string = NULL;
        }
        out->type = VAL_NIL;
        return NULL;
    }
    if (arr_count > 0 && dt == VAL_ARR) {
        Value *data = (Value*)calloc((size_t)arr_count, sizeof(Value));
        if (!data) {
            if (out->type == VAL_STRING && out->as.string) {
                free(out->as.string); out->as.string = NULL;
            }
            out->type = VAL_NIL;
            return NULL;
        }
        for (int _ai = 0; _ai < arr_count; _ai++) {
            if (r + (int)sizeof(int32_t) > end) {
                for (int _j = 0; _j < _ai; _j++) value_free_data(&data[_j]);
                free(data);
                if (out->type == VAL_STRING && out->as.string) {
                    free(out->as.string); out->as.string = NULL;
                }
                out->type = VAL_NIL;
                return NULL;
            }
            int32_t etag; memcpy(&etag, r, sizeof(int32_t)); r += sizeof(int32_t);
            Value elem; memset(&elem, 0, sizeof(elem));
            r = prst_deser_value(r, end, (ValType)etag, &elem);
            if (!r) {
                for (int _j = 0; _j < _ai; _j++) value_free_data(&data[_j]);
                free(data);
                if (out->type == VAL_STRING && out->as.string) {
                    free(out->as.string); out->as.string = NULL;
                }
                out->type = VAL_NIL;
                return NULL;
            }
            data[_ai] = elem;
        }
        out->type         = VAL_ARR;
        out->as.arr.data  = data;
        out->as.arr.size  = (int)arr_count;
        out->as.arr.owned = 1;
    } else if (arr_count > 0) {
        /* non-ARR type: skip element blocks for forward compat. The skipped
         * values are not retained, so free everything they allocated.
         * On bail-out we must also free whatever the outer Value already
         * holds — typically the VAL_STRING malloc'd at line 304. */
        for (int _ai = 0; _ai < arr_count; _ai++) {
            if (r + (int)sizeof(int32_t) > end) {
                if (out->type == VAL_STRING && out->as.string) {
                    free(out->as.string); out->as.string = NULL;
                }
                out->type = VAL_NIL;
                return NULL;
            }
            int32_t etag; memcpy(&etag, r, sizeof(int32_t)); r += sizeof(int32_t);
            Value skip; memset(&skip, 0, sizeof(skip));
            r = prst_deser_value(r, end, (ValType)etag, &skip);
            if (!r) {
                if (out->type == VAL_STRING && out->as.string) {
                    free(out->as.string); out->as.string = NULL;
                }
                out->type = VAL_NIL;
                return NULL;
            }
            value_free_data(&skip);
        }
    }
    return r;
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
        if (!r) { value_free_data(&v); return 0; }

        int idx = prst_pool_set(p, name, v, NULL);
        if (idx >= 0) {
            p->entries[idx].declared_type = (ValType)dt;
            p->entries[idx].stack_offset  = (int)so;
            /* Restore init_value from wire. prst_pool_set has stored an
             * independent deep clone in init_value; free its allocations
             * and take ownership of iv_val instead — that is what the
             * snapshot encoded, and it is the key to correct
             * reload-detection (source-edit invalidation) on the next
             * reload. iv_val itself comes from prst_deser_value and
             * carries only string/arr (never VAL_DYN/VAL_BLOCK_INST), so
             * value_free_data on it later would also be safe — but use
             * the clone-specific free for consistency. */
            prst_value_free_clone(&p->entries[idx].init_value);
            p->entries[idx].init_value = iv_val;
        } else {
            /* prst_pool_set rejected v (type collision). iv_val was never
             * stored; free everything it allocated. */
            value_free_data(&iv_val);
        }
        /* prst_pool_set deep-clones the caller's value into the pool —
         * the original in `v` is an orphan now. Reclaim it. */
        value_free_data(&v);
    }
    return 1;
}
