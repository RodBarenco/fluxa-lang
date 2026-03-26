/* err.h — Fluxa Error Stack (Sprint 6)
 *
 * Design principles:
 *   - Static allocation only — no malloc, no dynamic growth
 *   - 32 entries: enough for any real dangerous sequence, ~16KB per Runtime
 *   - Overflow: ring buffer — keeps most recent, discards oldest
 *   - err is ONLY relevant inside a danger block
 *   - Outside danger, err always reads as nil (no state leaks between blocks)
 *   - errstack_clear() is called at the START of every danger block
 *
 * ErrKind separates Fluxa runtime errors from future FFI errors (6.b)
 * and hot-reload invalidations (Sprint 7) — all three share this struct.
 */
#ifndef FLUXA_ERR_H
#define FLUXA_ERR_H

#include <string.h>
#include <stdio.h>

/* ── Error kinds ─────────────────────────────────────────────────────────── */
typedef enum {
    ERR_FLUXA,    /* runtime error from Fluxa itself (div/0, OOB, etc.)     */
    ERR_C_FFI,    /* error from import c — Sprint 6.b                       */
    ERR_RELOAD,   /* prst invalidation during hot reload — Sprint 7         */
} ErrKind;

/* ── Single error entry ──────────────────────────────────────────────────── */
typedef struct {
    ErrKind kind;
    char    message[256];  /* human-readable description                    */
    char    context[128];  /* function name, Block name, or "<global>"      */
    int     line;          /* source line — 0 if unknown                    */
} ErrEntry;

/* ── Error stack ─────────────────────────────────────────────────────────── */
#define ERR_STACK_CAP 32

typedef struct ErrStack {
    ErrEntry entries[ERR_STACK_CAP];
    int      base;    /* index of oldest entry (ring buffer)                */
    int      count;   /* number of live entries (0 = nil / empty)           */
} ErrStack;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Zero the stack — called at the start of every danger block */
static inline void errstack_clear(ErrStack *s) {
    s->base  = 0;
    s->count = 0;
}

/* Push a new error. If full, the oldest entry is overwritten (ring). */
static inline void errstack_push(ErrStack *s, ErrKind kind,
                                  const char *message, const char *context,
                                  int line) {
    int slot;
    if (s->count < ERR_STACK_CAP) {
        /* not full yet — write at (base + count) % CAP */
        slot = (s->base + s->count) % ERR_STACK_CAP;
        s->count++;
    } else {
        /* full — overwrite oldest, advance base */
        slot   = s->base;
        s->base = (s->base + 1) % ERR_STACK_CAP;
    }
    s->entries[slot].kind = kind;
    snprintf(s->entries[slot].message, sizeof(s->entries[slot].message),
             "%s", message ? message : "");
    snprintf(s->entries[slot].context, sizeof(s->entries[slot].context),
             "%s", context ? context : "<global>");
    s->entries[slot].line = line;
}

/* Access by logical index: 0 = most recent, 1 = one before, etc.
 * Returns NULL if index is out of range. */
static inline const ErrEntry *errstack_get(const ErrStack *s, int i) {
    if (i < 0 || i >= s->count) return NULL;
    /* most recent = (base + count - 1) % CAP, going backwards */
    int slot = (s->base + s->count - 1 - i + ERR_STACK_CAP * 2) % ERR_STACK_CAP;
    return &s->entries[slot];
}

/* Print all entries, most recent first */
static inline void errstack_print(const ErrStack *s) {
    if (s->count == 0) {
        printf("nil\n");
        return;
    }
    for (int i = 0; i < s->count; i++) {
        const ErrEntry *e = errstack_get(s, i);
        if (!e) break;
        const char *kind_str =
            (e->kind == ERR_C_FFI)   ? "c_ffi"  :
            (e->kind == ERR_RELOAD)  ? "reload" : "fluxa";
        if (e->line > 0)
            printf("[%s] %s (in %s, line %d)\n",
                   kind_str, e->message, e->context, e->line);
        else
            printf("[%s] %s (in %s)\n",
                   kind_str, e->message, e->context);
    }
}

#endif /* FLUXA_ERR_H */
