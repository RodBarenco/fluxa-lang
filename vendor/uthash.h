/* uthash.h — Hash table for C structures (Fluxa subset, C99 compatible)
 * Based on uthash by Troy D. Hanson — BSD license
 * Strictly C99 — no __typeof__, no GCC extensions.
 */
#ifndef UTHASH_H
#define UTHASH_H

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define UTHASH_NBUCKETS 32

typedef struct UT_hash_handle {
    struct UT_hash_table  *tbl;
    struct UT_hash_handle *hh_prev;
    struct UT_hash_handle *hh_next;
    struct UT_hash_handle *next_item;
    struct UT_hash_handle *prev_item;
    const void            *key;
    unsigned               keylen;
    unsigned               hashval;
} UT_hash_handle;

typedef struct UT_hash_bucket {
    UT_hash_handle *head;
    unsigned        count;
} UT_hash_bucket;

typedef struct UT_hash_table {
    UT_hash_bucket   buckets[UTHASH_NBUCKETS];
    UT_hash_handle  *head;
    UT_hash_handle  *tail;
    unsigned         num_items;
} UT_hash_table;

static unsigned _uth_fnv(const char *key, unsigned len) {
    unsigned h = 2166136261u, i;
    for (i = 0; i < len; i++) { h ^= (unsigned char)key[i]; h *= 16777619u; }
    return h;
}

static inline void _uth_add(void **head_vp, UT_hash_handle *hh, size_t hh_off) {
    UT_hash_table *tbl;
    unsigned bucket;
    if (*head_vp == NULL) {
        tbl = (UT_hash_table*)calloc(1, sizeof(UT_hash_table));
    } else {
        tbl = ((UT_hash_handle*)((char*)*head_vp + hh_off))->tbl;
    }
    bucket       = _uth_fnv((const char*)hh->key, hh->keylen) % UTHASH_NBUCKETS;
    hh->hashval  = bucket;
    hh->tbl      = tbl;
    hh->hh_prev  = NULL;
    hh->hh_next  = tbl->buckets[bucket].head;
    if (tbl->buckets[bucket].head) tbl->buckets[bucket].head->hh_prev = hh;
    tbl->buckets[bucket].head = hh;
    tbl->buckets[bucket].count++;
    hh->prev_item = tbl->tail;
    hh->next_item = NULL;
    if (tbl->tail) tbl->tail->next_item = hh;
    else           tbl->head = hh;
    tbl->tail = hh;
    tbl->num_items++;
    if (*head_vp == NULL) *head_vp = (char*)hh - hh_off;
    ((UT_hash_handle*)((char*)*head_vp + hh_off))->tbl = tbl;
}

static inline UT_hash_handle *_uth_find(UT_hash_table *tbl,
                                  const char *key, unsigned keylen) {
    UT_hash_handle *cur;
    unsigned bucket;
    if (!tbl) return NULL;
    bucket = _uth_fnv(key, keylen) % UTHASH_NBUCKETS;
    cur = tbl->buckets[bucket].head;
    while (cur) {
        if (cur->keylen == keylen && memcmp(cur->key, key, keylen) == 0)
            return cur;
        cur = cur->hh_next;
    }
    return NULL;
}

static inline void _uth_del(void **head_vp, UT_hash_handle *hh, size_t hh_off) {
    UT_hash_table *tbl = hh->tbl;
    unsigned bucket;
    if (!tbl) return;
    bucket = hh->hashval % UTHASH_NBUCKETS;
    if (hh->hh_prev) hh->hh_prev->hh_next = hh->hh_next;
    else             tbl->buckets[bucket].head = hh->hh_next;
    if (hh->hh_next) hh->hh_next->hh_prev = hh->hh_prev;
    tbl->buckets[bucket].count--;
    if (hh->prev_item) hh->prev_item->next_item = hh->next_item;
    else               tbl->head = hh->next_item;
    if (hh->next_item) hh->next_item->prev_item = hh->prev_item;
    else               tbl->tail = hh->prev_item;
    tbl->num_items--;
    if (tbl->num_items == 0) { free(tbl); *head_vp = NULL; }
    else *head_vp = tbl->head ? (char*)tbl->head - hh_off : NULL;
}

/* ── Public macros (C99, no __typeof__) ─────────────────────────────────── */

#define HASH_ADD_STR(head, fieldname, add)                              \
    do {                                                                \
        (add)->hh.key    = (void*)((add)->fieldname);                  \
        (add)->hh.keylen = (unsigned)strlen((add)->fieldname);         \
        _uth_add((void**)&(head), &(add)->hh,                          \
                 (size_t)((char*)&(add)->hh - (char*)(add)));          \
    } while(0)

#define HASH_FIND_STR(head, findstr, out)                               \
    do {                                                                \
        (out) = NULL;                                                   \
        if (head) {                                                     \
            size_t _off = (size_t)((char*)&(head)->hh-(char*)(head));  \
            UT_hash_handle *_h = _uth_find((head)->hh.tbl,             \
                (findstr), (unsigned)strlen(findstr));                  \
            if (_h) (out) = (void*)((char*)_h - _off);                 \
        }                                                               \
    } while(0)

#define HASH_DEL(head, delptr)                                          \
    do {                                                                \
        size_t _off=(size_t)((char*)&(delptr)->hh-(char*)(delptr));    \
        _uth_del((void**)&(head), &(delptr)->hh, _off);                \
    } while(0)

#define HASH_ITER(hh_field, head, el, tmp)                              \
    for ((el)  = (head),                                                \
         (tmp) = (el) ? (void*)((el)->hh_field.next_item               \
             ? (char*)(el)->hh_field.next_item                         \
               - (size_t)((char*)&(el)->hh_field-(char*)(el)) : NULL)  \
             : NULL;                                                    \
         (el) != NULL;                                                  \
         (el)  = (tmp),                                                 \
         (tmp) = (el) ? (void*)((el)->hh_field.next_item               \
             ? (char*)(el)->hh_field.next_item                         \
               - (size_t)((char*)&(el)->hh_field-(char*)(el)) : NULL)  \
             : NULL)

#define HASH_COUNT(head) ((head) ? (head)->hh.tbl->num_items : 0u)

#endif /* UTHASH_H */
