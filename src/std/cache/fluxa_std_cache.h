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

#define CACHE_SHARDS     32         /* must be power of 2 — doubled in v0.19.2
                                       to reduce per-shard contention with more
                                       worker threads */
#define CACHE_PER_SHARD  256        /* slots per shard — was 64 in v0.19; bumped
                                       to keep cache useful under high-cardinality
                                       workloads (k6 generates ~100k distinct
                                       UUIDs over a benchmark run). Random
                                       eviction now retires old entries when
                                       probes fill, so capacity directly
                                       controls hit-ratio under load. */
#define CACHE_PROBE       8         /* probe distance unchanged — eviction
                                       handles the case where probes fill */

typedef struct {
    char *key;     /* NULL = empty slot */
    char *val;     /* heap-owned by the cache */
} CacheEntry;

typedef struct {
    pthread_mutex_t mu;
    CacheEntry      entries[CACHE_PER_SHARD];
    int             populated;
    uint32_t        rand_state;        /* xorshift32 for random eviction */
} CacheShard;

/* Zero-initialised by the C standard. Mutexes are constructed at first use
 * via pthread_once to keep this header strictly ISO C (no GCC range-init). */
static CacheShard      g_cache_shards[CACHE_SHARDS];
static pthread_once_t  g_cache_shards_once = PTHREAD_ONCE_INIT;

/* Diagnostic counters (atomic via __sync) — exposed via cache.stats() */
static volatile unsigned long g_cache_put_calls    = 0;
static volatile unsigned long g_cache_put_inserts  = 0;  /* new entries */
static volatile unsigned long g_cache_put_updates  = 0;  /* overwrites */
static volatile unsigned long g_cache_put_evicts   = 0;  /* random eviction */
static volatile unsigned long g_cache_put_failures = 0;  /* silent drops (legacy) */
static volatile unsigned long g_cache_get_calls    = 0;
static volatile unsigned long g_cache_get_hits     = 0;
static volatile unsigned long g_cache_get_misses   = 0;

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
    v.as.string = fxstr_new(s);
    return v;
}
static inline Value cache_str_arena(char *s) {
    /* Arena memory has no refcount header — copy out so the Value follows
     * the uniform ownership rules (the old borrow convention was unsound:
     * any release path consuming this Value corrupted the arena). */
    Value v; v.type = VAL_STRING;
    v.as.string = fxstr_new(s);
    return v;
}

/* ── Shard probe: returns slot index within shard, or -1 ────────────────── */
/* Cheap xorshift-based PRNG for eviction slot selection. Each shard maintains
 * its own state (no cross-shard contention) since rand_state lives on
 * CacheShard. Output is used only to pick which slot to evict — quality is
 * not critical. */
static inline uint32_t shard_rand(CacheShard *sh) {
    uint32_t x = sh->rand_state;
    if (x == 0) x = 0x9E3779B9u;          /* golden-ratio seed */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sh->rand_state = x;
    return x;
}

/* shard_locate — find or place an entry within CACHE_PROBE slots starting
 * from h's natural position. Modes:
 *   create_if_missing == 0  → lookup only. Returns idx if key found, else -1.
 *   create_if_missing == 1  → return idx for insert/update:
 *       - existing key       → returns its idx, *was_evict = 0
 *       - empty probe slot   → returns it, *was_evict = 0
 *       - all 8 probes taken → returns a random probe slot, *was_evict = 1
 *                              (caller must free the old key+val before reuse)
 * Random eviction lets the cache stay useful when distinct-key load exceeds
 * CACHE_PROBE per bucket (the common case under sustained traffic with high
 * key cardinality). Previously such inserts were silent drops, defeating the
 * cache after a few minutes of real load. */
static int shard_locate(CacheShard *sh, const char *key, uint32_t h,
                         int create_if_missing, int *was_evict) {
    int base = (int)((h >> 4) & (CACHE_PER_SHARD - 1));
    int first_empty = -1;
    int idx = base;
    if (was_evict) *was_evict = 0;
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
    if (!create_if_missing) return -1;
    if (first_empty >= 0) return first_empty;
    /* All probes occupied — evict a random probe slot. Picking uniformly from
     * the 8 probed positions gives a 1/8 chance per slot, which is close to
     * random replacement (RR) and keeps the working set roughly proportional
     * to recent traffic. */
    if (was_evict) *was_evict = 1;
    int evict_slot = (int)(shard_rand(sh) & (CACHE_PROBE - 1));
    int evict_idx  = (base + evict_slot) & (CACHE_PER_SHARD - 1);
    return evict_idx;
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

        __sync_fetch_and_add(&g_cache_put_calls, 1);
        pthread_mutex_lock(&sh->mu);
        int was_evict = 0;
        int idx = shard_locate(sh, key, h, 1, &was_evict);
        if (idx < 0) {
            /* Shouldn't happen — shard_locate with create=1 always returns
             * a slot (eviction is the fallback). Defensive. */
            __sync_fetch_and_add(&g_cache_put_failures, 1);
            pthread_mutex_unlock(&sh->mu);
            return cache_nil();
        }
        CacheEntry *e = &sh->entries[idx];
        if (was_evict) {
            /* Random eviction — replace the existing key + value entirely. */
            __sync_fetch_and_add(&g_cache_put_evicts, 1);
            free(e->key); free(e->val);
            e->key = strdup(key);
            e->val = strdup(val);
        } else if (e->key == NULL) {
            /* Fresh insert into empty slot */
            __sync_fetch_and_add(&g_cache_put_inserts, 1);
            e->key = strdup(key);
            e->val = strdup(val);
            sh->populated++;
        } else {
            /* Update existing key — reuse key slot, replace val */
            __sync_fetch_and_add(&g_cache_put_updates, 1);
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

        __sync_fetch_and_add(&g_cache_get_calls, 1);
        pthread_mutex_lock(&sh->mu);
        int idx = shard_locate(sh, key, h, 0, NULL);
        if (idx < 0) {
            pthread_mutex_unlock(&sh->mu);
            __sync_fetch_and_add(&g_cache_get_misses, 1);
            return cache_str_heap("");
        }
        Value out = cache_str_heap(sh->entries[idx].val ? sh->entries[idx].val : "");
        pthread_mutex_unlock(&sh->mu);
        __sync_fetch_and_add(&g_cache_get_hits, 1);
        return out;
    }

    if (strcmp(fn_name, "del") == 0) {
        NEED(1);
        GET_STR(0, key);
        uint32_t h = cache_hash(key);
        CacheShard *sh = &g_cache_shards[h & (CACHE_SHARDS - 1)];

        pthread_mutex_lock(&sh->mu);
        int idx = shard_locate(sh, key, h, 0, NULL);
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

    /* cache.stats() → str — diagnostic snapshot of put/get/evict counters.
     * Format is one key=value pair per line, easy to grep. Useful for
     * confirming whether a workload is saturating the cache before
     * deciding on capacity changes. */
    if (strcmp(fn_name, "stats") == 0) {
        long size = 0;
        for (int s = 0; s < CACHE_SHARDS; s++) {
            CacheShard *sh = &g_cache_shards[s];
            pthread_mutex_lock(&sh->mu);
            size += sh->populated;
            pthread_mutex_unlock(&sh->mu);
        }
        unsigned long put_calls    = g_cache_put_calls;
        unsigned long put_inserts  = g_cache_put_inserts;
        unsigned long put_updates  = g_cache_put_updates;
        unsigned long put_evicts   = g_cache_put_evicts;
        unsigned long put_failures = g_cache_put_failures;
        unsigned long get_calls    = g_cache_get_calls;
        unsigned long get_hits     = g_cache_get_hits;
        unsigned long get_misses   = g_cache_get_misses;
        double hit_ratio = get_calls ? (double)get_hits / (double)get_calls : 0.0;
        char buf[512];
        snprintf(buf, sizeof(buf),
            "size=%ld capacity=%d shards=%d probe=%d "
            "puts=%lu inserts=%lu updates=%lu evicts=%lu failures=%lu "
            "gets=%lu hits=%lu misses=%lu hit_ratio=%.4f",
            size, CACHE_SHARDS * CACHE_PER_SHARD, CACHE_SHARDS, CACHE_PROBE,
            put_calls, put_inserts, put_updates, put_evicts, put_failures,
            get_calls, get_hits, get_misses, hit_ratio);
        Value v; v.type = VAL_STRING; v.as.string = fxstr_new(buf);
        return v;
    }

    /* cache.stats_reset() → nil — zero out diagnostic counters. Useful for
     * isolating measurements from warm-up traffic. */
    if (strcmp(fn_name, "stats_reset") == 0) {
        g_cache_put_calls    = 0;
        g_cache_put_inserts  = 0;
        g_cache_put_updates  = 0;
        g_cache_put_evicts   = 0;
        g_cache_put_failures = 0;
        g_cache_get_calls    = 0;
        g_cache_get_hits     = 0;
        g_cache_get_misses   = 0;
        return cache_nil();
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
