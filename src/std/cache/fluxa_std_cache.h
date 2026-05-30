#ifndef FLUXA_STD_CACHE_H
#define FLUXA_STD_CACHE_H

/* std.cache — thread-safe cache + arena allocator for HTTP-server workloads.
 *
 * Two cooperating facilities, both thread-safe via internal mutexes:
 *
 * 1. SHARDED CACHE — process-wide string→string store backed by 16 shards
 *    of 64 slots each (1024 total slots, 8-probe linear within shard).
 *    hash(key) picks the shard; only writes to the same shard contend on
 *    a mutex. Inspired by folly::ConcurrentHashMap / tbb::concurrent_hash_map.
 *
 *      cache.put(key, val)  → nil
 *      cache.get(key)       → str  ("" on miss; OWNED copy on hit)
 *      cache.del(key)       → nil
 *      cache.clear()        → nil
 *      cache.size()         → int
 *
 *    Ownership: the cache strdup's the caller's strings on put, and
 *    strdup's its stored value on get. The caller may free() inputs and
 *    outputs safely without touching the cache.
 *
 * 2. ARENA ALLOCATOR — per-handle bump-pointer arena with linked-list slabs.
 *    Inspired by Apache Arrow Arena / bumpalo / Nginx ngx_pool_t.
 *
 *      cache.arena_new()                 → int handle
 *      cache.arena_str(h, src)           → str  (src copied into arena)
 *      cache.arena_concat(h, a, b)       → str  (concat in arena)
 *      cache.arena_concat3(h, a, b, c)   → str
 *      cache.arena_concat5(h, a, b, c, d, e) → str
 *      cache.arena_reset(h)              → nil  (frees all slabs in O(slab count))
 *      cache.arena_drop(h)               → nil  (frees the arena entirely)
 *
 *    CRITICAL: arena-allocated strings live in the arena's slabs. Their
 *    char* pointers are NOT individually freeable. NEVER call free(x) on a
 *    string returned by an arena_* function — that would try to free a
 *    pointer mid-slab and crash. The arena releases all its strings at once
 *    via arena_reset (reuses slabs) or arena_drop (frees the arena).
 *
 *    Typical usage in a worker loop:
 *
 *      fn worker(int srv) nil {
 *          int a = cache.arena_new()
 *          while !ft.should_stop() {
 *              // all per-request strings go to the arena
 *              str body = cache.arena_str(a, wserver.req_body(req))
 *              str row  = cache.arena_concat3(a, "{\"id\":\"", id, "\"}")
 *              wserver.reply_json(req, 200, row)
 *              // no free() — arena_reset releases the lot in one O(1)-ish call
 *              cache.arena_reset(a)
 *          }
 *          cache.arena_drop(a)
 *      }
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Sharded cache ──────────────────────────────────────────────────────── */

#define CACHE_SHARDS    16          /* must be power of 2 */
#define CACHE_PER_SHARD 64          /* slots per shard */
#define CACHE_PROBE     8

typedef struct {
    char *key;     /* NULL = empty slot */
    char *val;     /* heap-owned by the cache */
} CacheEntry;

typedef struct {
    pthread_mutex_t mu;
    CacheEntry      entries[CACHE_PER_SHARD];
    int             populated;
} CacheShard;

/* Zero-initialised by the C standard. Mutexes are constructed at first use
 * via pthread_once to keep this header strictly ISO C (no GCC range-init). */
static CacheShard      g_cache_shards[CACHE_SHARDS];
static pthread_once_t  g_cache_shards_once = PTHREAD_ONCE_INIT;

static void cache_shards_init(void) {
    for (int i = 0; i < CACHE_SHARDS; i++)
        pthread_mutex_init(&g_cache_shards[i].mu, NULL);
}
static inline void cache_shards_ensure(void) {
    pthread_once(&g_cache_shards_once, cache_shards_init);
}

/* FNV-1a 32-bit — matches strings.hash */
static inline uint32_t cache_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* ── Arena allocator ────────────────────────────────────────────────────── */

#define ARENA_SLAB_SIZE  (64 * 1024)   /* 64 KB per slab */
#define ARENA_MAX        64            /* max concurrent arenas */

typedef struct ArenaSlab {
    struct ArenaSlab *next;
    size_t            used;
    size_t            cap;
    char              data[];   /* flexible array, sized by cap */
} ArenaSlab;

typedef struct {
    pthread_mutex_t mu;
    ArenaSlab      *head;          /* current slab for new allocs */
    ArenaSlab      *first;         /* first slab — kept across reset */
    int             active;        /* 1 if handle is in use */
} Arena;

static Arena           g_arenas[ARENA_MAX];
static pthread_mutex_t g_arena_registry_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_arenas_once = PTHREAD_ONCE_INIT;

static void arenas_init(void) {
    for (int i = 0; i < ARENA_MAX; i++)
        pthread_mutex_init(&g_arenas[i].mu, NULL);
}
static inline void arenas_ensure(void) {
    pthread_once(&g_arenas_once, arenas_init);
}

static ArenaSlab *arena_slab_new(size_t cap) {
    ArenaSlab *s = (ArenaSlab *)malloc(sizeof(ArenaSlab) + cap);
    if (!s) return NULL;
    s->next = NULL;
    s->used = 0;
    s->cap  = cap;
    return s;
}

static void arena_slab_free_chain(ArenaSlab *s) {
    while (s) {
        ArenaSlab *next = s->next;
        free(s);
        s = next;
    }
}

/* Allocate `n` bytes (with 8-byte alignment) inside the arena. Caller holds
 * arena->mu. Returns a pointer into a slab, or NULL on OOM. */
static char *arena_alloc(Arena *a, size_t n) {
    n = (n + 7) & ~((size_t)7);     /* align to 8 bytes */
    if (a->head && a->head->cap - a->head->used >= n) {
        char *p = a->head->data + a->head->used;
        a->head->used += n;
        return p;
    }
    /* current slab full — allocate a new slab. Slab cap = max(default, n). */
    size_t cap = ARENA_SLAB_SIZE;
    if (n > cap) cap = n;
    ArenaSlab *s = arena_slab_new(cap);
    if (!s) return NULL;
    s->used = n;
    s->next = a->head;
    a->head = s;
    if (!a->first) a->first = s;
    char *p = s->data;
    return p;
}

/* Reset: free all slabs except the first, reset first slab's used to 0.
 * This keeps one slab hot in cache for next iteration. */
static void arena_reset_locked(Arena *a) {
    if (!a->first) { a->head = NULL; return; }
    /* free all slabs EXCEPT first */
    ArenaSlab *s = a->head;
    while (s && s != a->first) {
        ArenaSlab *next = s->next;
        free(s);
        s = next;
    }
    a->first->used = 0;
    a->first->next = NULL;
    a->head = a->first;
}

/* ── Value helpers ──────────────────────────────────────────────────────── */
static inline Value cache_nil(void)    { Value v; v.type = VAL_NIL; return v; }
static inline Value cache_int(long n)  { Value v; v.type = VAL_INT; v.as.integer = n; return v; }
static inline Value cache_str_heap(const char *s) {
    /* For cache.get and friends: caller owns and may free(). */
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}
static inline Value cache_str_arena(char *s) {
    /* For arena_* returns: arena-owned, caller MUST NOT free(). */
    Value v; v.type = VAL_STRING;
    v.as.string = s;
    return v;
}

/* ── Shard probe: returns slot index within shard, or -1 ────────────────── */
static int shard_locate(CacheShard *sh, const char *key, uint32_t h, int create_if_missing) {
    int idx = (int)((h >> 4) & (CACHE_PER_SHARD - 1));   /* >>4 to decorrelate from shard pick */
    int first_empty = -1;
    for (int probe = 0; probe < CACHE_PROBE; probe++) {
        CacheEntry *e = &sh->entries[idx];
        if (e->key == NULL) {
            if (first_empty < 0) first_empty = idx;
            if (!create_if_missing) return -1;
        } else if (strcmp(e->key, key) == 0) {
            return idx;
        }
        idx = (idx + 1) & (CACHE_PER_SHARD - 1);
    }
    return create_if_missing ? first_empty : -1;
}

/* ── Main dispatch ──────────────────────────────────────────────────────── */
static inline Value fluxa_std_cache_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    /* Lazy-init mutexes on first call from any thread. pthread_once is
     * cheap (atomic load on the fast path after first run). */
    cache_shards_ensure();
    arenas_ensure();
    char errbuf[280];

    #define CACHE_ERR(msg) do { \
        snprintf(errbuf, sizeof(errbuf), "cache.%s (line %d): %s", \
                 fn_name, line, (msg)); \
        errstack_push(err, ERR_FLUXA, errbuf, "cache", line); \
        *had_error = 1; return cache_nil(); \
    } while(0)

    #define NEED(n) do { \
        if (argc < (n)) { \
            snprintf(errbuf, sizeof(errbuf), \
                "cache.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
            errstack_push(err, ERR_FLUXA, errbuf, "cache", line); \
            *had_error = 1; return cache_nil(); \
        } \
    } while(0)

    #define GET_STR(i, var) \
        if (args[(i)].type != VAL_STRING || !args[(i)].as.string) { \
            snprintf(errbuf, sizeof(errbuf), \
                "cache.%s: argument %d must be str", fn_name, (i)+1); \
            errstack_push(err, ERR_FLUXA, errbuf, "cache", line); \
            *had_error = 1; return cache_nil(); \
        } \
        const char *var = args[(i)].as.string

    #define GET_INT(i, var) \
        if (args[(i)].type != VAL_INT) { \
            snprintf(errbuf, sizeof(errbuf), \
                "cache.%s: argument %d must be int", fn_name, (i)+1); \
            errstack_push(err, ERR_FLUXA, errbuf, "cache", line); \
            *had_error = 1; return cache_nil(); \
        } \
        long var = args[(i)].as.integer

    /* ─────────────────────────── KEY-VALUE CACHE ──────────────────────── */

    if (strcmp(fn_name, "put") == 0) {
        NEED(2);
        GET_STR(0, key);
        GET_STR(1, val);
        uint32_t h = cache_hash(key);
        CacheShard *sh = &g_cache_shards[h & (CACHE_SHARDS - 1)];

        pthread_mutex_lock(&sh->mu);
        int idx = shard_locate(sh, key, h, 1);
        if (idx >= 0) {
            CacheEntry *e = &sh->entries[idx];
            if (e->key == NULL) {
                e->key = strdup(key);
                sh->populated++;
            }
            if (e->val) free(e->val);
            e->val = strdup(val);
        }
        pthread_mutex_unlock(&sh->mu);
        return cache_nil();
    }

    if (strcmp(fn_name, "get") == 0) {
        NEED(1);
        GET_STR(0, key);
        uint32_t h = cache_hash(key);
        CacheShard *sh = &g_cache_shards[h & (CACHE_SHARDS - 1)];

        pthread_mutex_lock(&sh->mu);
        int idx = shard_locate(sh, key, h, 0);
        if (idx < 0) {
            pthread_mutex_unlock(&sh->mu);
            return cache_str_heap("");
        }
        Value out = cache_str_heap(sh->entries[idx].val ? sh->entries[idx].val : "");
        pthread_mutex_unlock(&sh->mu);
        return out;
    }

    if (strcmp(fn_name, "del") == 0) {
        NEED(1);
        GET_STR(0, key);
        uint32_t h = cache_hash(key);
        CacheShard *sh = &g_cache_shards[h & (CACHE_SHARDS - 1)];

        pthread_mutex_lock(&sh->mu);
        int idx = shard_locate(sh, key, h, 0);
        if (idx >= 0) {
            CacheEntry *e = &sh->entries[idx];
            if (e->key) { free(e->key); e->key = NULL; }
            if (e->val) { free(e->val); e->val = NULL; }
            sh->populated--;
        }
        pthread_mutex_unlock(&sh->mu);
        return cache_nil();
    }

    if (strcmp(fn_name, "clear") == 0) {
        for (int s = 0; s < CACHE_SHARDS; s++) {
            CacheShard *sh = &g_cache_shards[s];
            pthread_mutex_lock(&sh->mu);
            for (int i = 0; i < CACHE_PER_SHARD; i++) {
                CacheEntry *e = &sh->entries[i];
                if (e->key) { free(e->key); e->key = NULL; }
                if (e->val) { free(e->val); e->val = NULL; }
            }
            sh->populated = 0;
            pthread_mutex_unlock(&sh->mu);
        }
        return cache_nil();
    }

    if (strcmp(fn_name, "size") == 0) {
        long n = 0;
        for (int s = 0; s < CACHE_SHARDS; s++) {
            CacheShard *sh = &g_cache_shards[s];
            pthread_mutex_lock(&sh->mu);
            n += sh->populated;
            pthread_mutex_unlock(&sh->mu);
        }
        return cache_int(n);
    }

    /* ─────────────────────────── ARENA ALLOCATOR ──────────────────────── */

    if (strcmp(fn_name, "arena_new") == 0) {
        pthread_mutex_lock(&g_arena_registry_mu);
        int handle = -1;
        for (int i = 0; i < ARENA_MAX; i++) {
            if (!g_arenas[i].active) {
                Arena *a = &g_arenas[i];
                a->active = 1;
                a->head = NULL;
                a->first = NULL;
                handle = i;
                break;
            }
        }
        pthread_mutex_unlock(&g_arena_registry_mu);
        if (handle < 0) CACHE_ERR("arena_new: registry full (max 64 arenas)");
        return cache_int(handle);
    }

    if (strcmp(fn_name, "arena_str") == 0) {
        NEED(2);
        GET_INT(0, h);
        GET_STR(1, src);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_str: invalid handle");
        Arena *a = &g_arenas[h];
        size_t n = strlen(src) + 1;
        pthread_mutex_lock(&a->mu);
        char *dst = arena_alloc(a, n);
        if (dst) memcpy(dst, src, n);
        pthread_mutex_unlock(&a->mu);
        if (!dst) CACHE_ERR("arena_str: out of memory");
        return cache_str_arena(dst);
    }

    if (strcmp(fn_name, "arena_concat") == 0) {
        NEED(3);
        GET_INT(0, h);
        GET_STR(1, a_s);
        GET_STR(2, b_s);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_concat: invalid handle");
        Arena *ar = &g_arenas[h];
        size_t la = strlen(a_s), lb = strlen(b_s);
        size_t total = la + lb + 1;
        pthread_mutex_lock(&ar->mu);
        char *dst = arena_alloc(ar, total);
        if (dst) { memcpy(dst, a_s, la); memcpy(dst + la, b_s, lb); dst[la+lb] = 0; }
        pthread_mutex_unlock(&ar->mu);
        if (!dst) CACHE_ERR("arena_concat: out of memory");
        return cache_str_arena(dst);
    }

    if (strcmp(fn_name, "arena_concat3") == 0) {
        NEED(4);
        GET_INT(0, h);
        GET_STR(1, a_s);
        GET_STR(2, b_s);
        GET_STR(3, c_s);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_concat3: invalid handle");
        Arena *ar = &g_arenas[h];
        size_t la = strlen(a_s), lb = strlen(b_s), lc = strlen(c_s);
        size_t total = la + lb + lc + 1;
        pthread_mutex_lock(&ar->mu);
        char *dst = arena_alloc(ar, total);
        if (dst) {
            memcpy(dst, a_s, la);
            memcpy(dst + la, b_s, lb);
            memcpy(dst + la + lb, c_s, lc);
            dst[la+lb+lc] = 0;
        }
        pthread_mutex_unlock(&ar->mu);
        if (!dst) CACHE_ERR("arena_concat3: out of memory");
        return cache_str_arena(dst);
    }

    if (strcmp(fn_name, "arena_concat5") == 0) {
        NEED(6);
        GET_INT(0, h);
        GET_STR(1, s1);
        GET_STR(2, s2);
        GET_STR(3, s3);
        GET_STR(4, s4);
        GET_STR(5, s5);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_concat5: invalid handle");
        Arena *ar = &g_arenas[h];
        size_t l1=strlen(s1), l2=strlen(s2), l3=strlen(s3), l4=strlen(s4), l5=strlen(s5);
        size_t total = l1+l2+l3+l4+l5+1;
        pthread_mutex_lock(&ar->mu);
        char *dst = arena_alloc(ar, total);
        if (dst) {
            char *p = dst;
            memcpy(p, s1, l1); p += l1;
            memcpy(p, s2, l2); p += l2;
            memcpy(p, s3, l3); p += l3;
            memcpy(p, s4, l4); p += l4;
            memcpy(p, s5, l5); p += l5;
            *p = 0;
        }
        pthread_mutex_unlock(&ar->mu);
        if (!dst) CACHE_ERR("arena_concat5: out of memory");
        return cache_str_arena(dst);
    }

    if (strcmp(fn_name, "arena_reset") == 0) {
        NEED(1);
        GET_INT(0, h);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_reset: invalid handle");
        Arena *a = &g_arenas[h];
        pthread_mutex_lock(&a->mu);
        arena_reset_locked(a);
        pthread_mutex_unlock(&a->mu);
        return cache_nil();
    }

    if (strcmp(fn_name, "arena_drop") == 0) {
        NEED(1);
        GET_INT(0, h);
        if (h < 0 || h >= ARENA_MAX || !g_arenas[h].active) CACHE_ERR("arena_drop: invalid handle");
        Arena *a = &g_arenas[h];
        pthread_mutex_lock(&a->mu);
        arena_slab_free_chain(a->head);
        a->head = NULL;
        a->first = NULL;
        a->active = 0;
        pthread_mutex_unlock(&a->mu);
        return cache_nil();
    }

    snprintf(errbuf, sizeof(errbuf), "cache.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "cache", line);
    *had_error = 1;
    return cache_nil();

    #undef CACHE_ERR
    #undef NEED
    #undef GET_STR
    #undef GET_INT
}

FLUXA_LIB_EXPORT(
    name     = "cache",
    toml_key = "std.cache",
    owner    = "cache",
    call     = fluxa_std_cache_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_CACHE_H */
