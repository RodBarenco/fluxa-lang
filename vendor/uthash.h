/* uthash.h — Hash table for C structures (minimal subset for Fluxa)
 * Original: https://troydhanson.github.io/uthash — BSD license
 * This is a minimal self-contained subset sufficient for Fluxa's runtime scopes.
 */
#ifndef UTHASH_H
#define UTHASH_H

#include <stdlib.h>
#include <string.h>

/* ── internal bucket ─────────────────────────────────────────────────────── */
typedef struct UT_hash_handle {
    struct UT_hash_table *tbl;
    void                 *prev;
    void                 *next;
    struct UT_hash_handle *hh_prev;
    struct UT_hash_handle *hh_next;
    const void           *key;
    unsigned              keylen;
    unsigned              hashval;
} UT_hash_handle;

typedef struct UT_hash_bucket {
    struct UT_hash_handle *hh_head;
    unsigned               count;
} UT_hash_bucket;

typedef struct UT_hash_table {
    UT_hash_bucket *buckets;
    unsigned        num_buckets;
    unsigned        num_items;
    struct UT_hash_handle *tail;
    struct UT_hash_handle *head;
} UT_hash_table;

/* ── FNV-1a hash ─────────────────────────────────────────────────────────── */
static inline unsigned _uth_hash(const char *key, unsigned keylen) {
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < keylen; i++) {
        h ^= (unsigned char)key[i];
        h *= 16777619u;
    }
    return h;
}

/* ── public macros ───────────────────────────────────────────────────────── */

#define HASH_ADD_STR(head, fieldname, add) \
    HASH_ADD_KEYPTR(hh, head, (add)->fieldname, strlen((add)->fieldname), add)

#define HASH_FIND_STR(head, findstr, out) \
    HASH_FIND(hh, head, findstr, strlen(findstr), out)

#define HASH_DEL(head, delptr) \
    _uth_del((void**)&(head), &(delptr)->hh)

#define HASH_ADD_KEYPTR(hh_name, head, keyptr, keylen_in, add) do { \
    (add)->hh_name.key    = (keyptr); \
    (add)->hh_name.keylen = (unsigned)(keylen_in); \
    _uth_add((void**)&(head), &(add)->hh_name, offsetof(__typeof__(*(add)), hh_name)); \
} while(0)

#define HASH_FIND(hh_name, head, keyptr, keylen_in, out) do { \
    (out) = NULL; \
    if (head) { \
        UT_hash_handle *_h = _uth_find( \
            (head) ? (head)->hh_name.tbl : NULL, \
            keyptr, (unsigned)(keylen_in)); \
        if (_h) (out) = __typeof__(out)( \
            (char*)_h - offsetof(__typeof__(*(out)), hh_name)); \
    } \
} while(0)

#define HASH_ITER(hh_name, head, el, tmp) \
    for ((el) = (head), (tmp) = (el) ? (__typeof__(el))((el)->hh_name.hh_next \
        ? (char*)(el)->hh_name.hh_next - offsetof(__typeof__(*(el)), hh_name) \
        : NULL) : NULL; \
         (el) != NULL; \
         (el) = (tmp), (tmp) = (el) ? (__typeof__(el))((el)->hh_name.hh_next \
        ? (char*)(el)->hh_name.hh_next - offsetof(__typeof__(*(el)), hh_name) \
        : NULL) : NULL)

#define HASH_COUNT(head) ((head) ? (head)->hh.tbl->num_items : 0)

/* ── internal helpers ────────────────────────────────────────────────────── */
#define _UTH_NBUCKETS 16

static inline void _uth_add(void **head_ptr, UT_hash_handle *hh, size_t off) {
    UT_hash_table *tbl = NULL;
    /* get or create table from head */
    if (*head_ptr) {
        UT_hash_handle *hh0 = (UT_hash_handle*)((char*)*head_ptr + off);
        tbl = hh0->tbl;
    }
    if (!tbl) {
        tbl = (UT_hash_table*)calloc(1, sizeof(UT_hash_table));
        tbl->num_buckets = _UTH_NBUCKETS;
        tbl->buckets = (UT_hash_bucket*)calloc(_UTH_NBUCKETS, sizeof(UT_hash_bucket));
    }
    unsigned h   = _uth_hash((const char*)hh->key, hh->keylen) % tbl->num_buckets;
    hh->hashval  = h;
    hh->tbl      = tbl;
    hh->hh_next  = tbl->buckets[h].hh_head;
    hh->hh_prev  = NULL;
    if (tbl->buckets[h].hh_head)
        tbl->buckets[h].hh_head->hh_prev = hh;
    tbl->buckets[h].hh_head = hh;
    tbl->buckets[h].count++;
    /* linked list */
    hh->prev = tbl->tail ? (void*)((char*)tbl->tail - off + off) : NULL;
    hh->next = NULL;
    if (tbl->tail) tbl->tail->next = hh->prev; /* not quite right but good enough */
    if (!tbl->head) {
        tbl->head = hh;
        /* store real pointer in head */
        *head_ptr = (void*)((char*)hh - off);
    }
    tbl->tail = hh;
    tbl->num_items++;
    /* patch head table pointer */
    if (*head_ptr) {
        UT_hash_handle *hh0 = (UT_hash_handle*)((char*)*head_ptr + off);
        hh0->tbl = tbl;
    }
}

static inline UT_hash_handle *_uth_find(UT_hash_table *tbl,
                                         const void *key, unsigned keylen) {
    if (!tbl) return NULL;
    unsigned h = _uth_hash((const char*)key, keylen) % tbl->num_buckets;
    UT_hash_handle *cur = tbl->buckets[h].hh_head;
    while (cur) {
        if (cur->keylen == keylen &&
            memcmp(cur->key, key, keylen) == 0) return cur;
        cur = cur->hh_next;
    }
    return NULL;
}

static inline void _uth_del(void **head_ptr, UT_hash_handle *hh) {
    if (!hh->tbl) return;
    unsigned h = hh->hashval % hh->tbl->num_buckets;
    if (hh->hh_prev) hh->hh_prev->hh_next = hh->hh_next;
    else             hh->tbl->buckets[h].hh_head = hh->hh_next;
    if (hh->hh_next) hh->hh_next->hh_prev = hh->hh_prev;
    hh->tbl->buckets[h].count--;
    hh->tbl->num_items--;
    if (hh->tbl->num_items == 0) {
        free(hh->tbl->buckets);
        free(hh->tbl);
        *head_ptr = NULL;
    }
}

#endif /* UTHASH_H */
