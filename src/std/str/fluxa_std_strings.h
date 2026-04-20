/* fluxa_std_str.h — Fluxa Standard Library: str
 *
 * String manipulation functions. No danger required — all operations
 * are pure computation on str values with bounded output.
 *
 * All functions work on byte offsets (not Unicode codepoints).
 * For ASCII and UTF-8 data where you only index at character boundaries
 * this is transparent. For multi-byte codepoint indexing, use std.regex
 * when available.
 *
 * Buffer limit: output strings are capped at FLUXA_STR_MAX_OUT bytes.
 * Configure in fluxa.toml:
 *   [libs.str]
 *   max_out_bytes = 8192    # default 8192
 *
 * Usage:
 *   import std str
 *   dyn parts = str.split("a,b,c", ",")
 *   str upper = str.upper("hello")    // "HELLO"
 */
#ifndef FLUXA_STD_STRINGS_H
#define FLUXA_STD_STRINGS_H

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "../../scope.h"
#include "../../err.h"

#ifndef FLUXA_STR_MAX_OUT
#define FLUXA_STR_MAX_OUT 8192
#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline Value strlib_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v;
}
static inline Value strlib_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value strlib_bool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b; return v;
}
static inline Value strlib_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
static inline Value fluxa_std_strings_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[320];

#define STR_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "str.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "str", line); \
    *had_error = 1; return strlib_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "str.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "str", line); \
        *had_error = 1; return strlib_nil(); \
    } \
} while(0)

#define GET_S(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        STR_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_I(idx, var) \
    if (args[(idx)].type != VAL_INT) STR_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

    /* ── str.split(str s, str delim) → dyn ──────────────────────────────── */
    if (strcmp(fn_name, "split") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, delim);
        size_t dlen = strlen(delim);

        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) STR_ERR("out of memory");
        d->cap = 8; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        if (!d->items) { free(d); STR_ERR("out of memory"); }

        if (dlen == 0) {
            /* Empty delimiter — split into individual bytes */
            for (const char *p = s; *p; p++) {
                char buf[2] = {*p, '\0'};
                if (d->count >= d->cap) {
                    int nc = d->cap * 2;
                    Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                    if (!nb) break;
                    d->items = nb; d->cap = nc;
                }
                d->items[d->count++] = strlib_str(buf);
            }
        } else {
            const char *cur = s;
            const char *found;
            while ((found = strstr(cur, delim)) != NULL) {
                size_t seg_len = (size_t)(found - cur);
                char *seg = (char *)malloc(seg_len + 1);
                if (!seg) break;
                memcpy(seg, cur, seg_len); seg[seg_len] = '\0';
                if (d->count >= d->cap) {
                    int nc = d->cap * 2;
                    Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                    if (!nb) { free(seg); break; }
                    d->items = nb; d->cap = nc;
                }
                d->items[d->count].type = VAL_STRING;
                d->items[d->count].as.string = seg;
                d->count++;
                cur = found + dlen;
            }
            /* Last segment */
            if (d->count >= d->cap) {
                int nc = d->cap * 2;
                Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                if (nb) { d->items = nb; d->cap = nc; }
            }
            if (d->count < d->cap)
                d->items[d->count++] = strlib_str(cur);
        }

        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

    /* ── str.join(dyn parts, str glue) → str ────────────────────────────── */
    if (strcmp(fn_name, "join") == 0) {
        NEED(2);
        if (args[0].type != VAL_DYN || !args[0].as.dyn) STR_ERR("first arg must be dyn");
        GET_S(1, glue);
        FluxaDyn *d = args[0].as.dyn;
        char out[FLUXA_STR_MAX_OUT];
        int pos = 0;
        size_t glen = strlen(glue);
        for (int i = 0; i < d->count; i++) {
            if (i > 0 && glen > 0) {
                if (pos + (int)glen >= FLUXA_STR_MAX_OUT - 1) break;
                memcpy(out + pos, glue, glen);
                pos += (int)glen;
            }
            if (d->items[i].type == VAL_STRING && d->items[i].as.string) {
                size_t sl = strlen(d->items[i].as.string);
                if (pos + (int)sl >= FLUXA_STR_MAX_OUT - 1) sl = (size_t)(FLUXA_STR_MAX_OUT - pos - 1);
                memcpy(out + pos, d->items[i].as.string, sl);
                pos += (int)sl;
            }
        }
        out[pos] = '\0';
        return strlib_str(out);
    }

    /* ── str.slice(str s, int start, int end) → str ─────────────────────── */
    /* Byte-based. Negative indices count from end. end is exclusive. */
    if (strcmp(fn_name, "slice") == 0) {
        NEED(3); GET_S(0, s); GET_I(1, start); GET_I(2, end);
        long slen = (long)strlen(s);
        if (start < 0) start = slen + start;
        if (end   < 0) end   = slen + end;
        if (start < 0) start = 0;
        if (end > slen) end = slen;
        if (start >= end) return strlib_str("");
        size_t out_len = (size_t)(end - start);
        if (out_len >= FLUXA_STR_MAX_OUT) out_len = FLUXA_STR_MAX_OUT - 1;
        char *out = (char *)malloc(out_len + 1);
        if (!out) STR_ERR("out of memory");
        memcpy(out, s + start, out_len);
        out[out_len] = '\0';
        Value ret = strlib_str(out);
        free(out);
        return ret;
    }

    /* ── str.trim(str s) → str ───────────────────────────────────────────── */
    if (strcmp(fn_name, "trim") == 0) {
        NEED(1); GET_S(0, s);
        while (*s && isspace((unsigned char)*s)) s++;
        size_t len = strlen(s);
        while (len > 0 && isspace((unsigned char)s[len-1])) len--;
        char out[FLUXA_STR_MAX_OUT];
        if (len >= FLUXA_STR_MAX_OUT) len = FLUXA_STR_MAX_OUT - 1;
        memcpy(out, s, len);
        out[len] = '\0';
        return strlib_str(out);
    }

    /* ── str.find(str s, str sub) → int ─────────────────────────────────── */
    /* Returns byte offset of first occurrence, or -1 if not found. */
    if (strcmp(fn_name, "find") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, sub);
        const char *found = strstr(s, sub);
        return strlib_int(found ? (long)(found - s) : -1L);
    }

    /* ── str.replace(str s, str old, str new) → str ─────────────────────── */
    /* Replaces all occurrences of old with new. */
    if (strcmp(fn_name, "replace") == 0) {
        NEED(3); GET_S(0, s); GET_S(1, old_sub); GET_S(2, new_sub);
        size_t olen = strlen(old_sub);
        size_t nlen = strlen(new_sub);
        if (olen == 0) return strlib_str(s); /* nothing to replace */

        char out[FLUXA_STR_MAX_OUT];
        int pos = 0;
        const char *cur = s;
        const char *found;

        while ((found = strstr(cur, old_sub)) != NULL) {
            size_t before = (size_t)(found - cur);
            if (pos + (int)before + (int)nlen >= FLUXA_STR_MAX_OUT - 1) {
                /* Copy what fits and stop */
                size_t fits = (size_t)(FLUXA_STR_MAX_OUT - pos - 1);
                if (before < fits) { memcpy(out + pos, cur, before); pos += (int)before; }
                break;
            }
            memcpy(out + pos, cur, before); pos += (int)before;
            memcpy(out + pos, new_sub, nlen); pos += (int)nlen;
            cur = found + olen;
        }
        /* Remaining tail */
        size_t tail = strlen(cur);
        if (pos + (int)tail >= FLUXA_STR_MAX_OUT) tail = (size_t)(FLUXA_STR_MAX_OUT - pos - 1);
        memcpy(out + pos, cur, tail); pos += (int)tail;
        out[pos] = '\0';
        return strlib_str(out);
    }

    /* ── str.starts_with(str s, str prefix) → bool ───────────────────────── */
    if (strcmp(fn_name, "starts_with") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, prefix);
        size_t plen = strlen(prefix);
        return strlib_bool(strncmp(s, prefix, plen) == 0 ? 1 : 0);
    }

    /* ── str.ends_with(str s, str suffix) → bool ────────────────────────── */
    if (strcmp(fn_name, "ends_with") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, suffix);
        size_t slen  = strlen(s);
        size_t sflen = strlen(suffix);
        if (sflen > slen) return strlib_bool(0);
        return strlib_bool(strcmp(s + slen - sflen, suffix) == 0 ? 1 : 0);
    }

    /* ── str.contains(str s, str sub) → bool ────────────────────────────── */
    if (strcmp(fn_name, "contains") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, sub);
        return strlib_bool(strstr(s, sub) != NULL ? 1 : 0);
    }

    /* ── str.count(str s, str sub) → int ────────────────────────────────── */
    /* Count non-overlapping occurrences of sub in s. */
    if (strcmp(fn_name, "count") == 0) {
        NEED(2); GET_S(0, s); GET_S(1, sub);
        size_t sublen = strlen(sub);
        if (sublen == 0) return strlib_int((long)strlen(s));
        long n = 0;
        const char *cur = s;
        const char *found;
        while ((found = strstr(cur, sub)) != NULL) {
            n++;
            cur = found + sublen;
        }
        return strlib_int(n);
    }

    /* ── str.lower(str s) → str ─────────────────────────────────────────── */
    if (strcmp(fn_name, "lower") == 0) {
        NEED(1); GET_S(0, s);
        char out[FLUXA_STR_MAX_OUT];
        size_t i = 0;
        for (; s[i] && i < FLUXA_STR_MAX_OUT - 1; i++)
            out[i] = (char)tolower((unsigned char)s[i]);
        out[i] = '\0';
        return strlib_str(out);
    }

    /* ── str.upper(str s) → str ─────────────────────────────────────────── */
    if (strcmp(fn_name, "upper") == 0) {
        NEED(1); GET_S(0, s);
        char out[FLUXA_STR_MAX_OUT];
        size_t i = 0;
        for (; s[i] && i < FLUXA_STR_MAX_OUT - 1; i++)
            out[i] = (char)toupper((unsigned char)s[i]);
        out[i] = '\0';
        return strlib_str(out);
    }

    /* ── str.repeat(str s, int n) → str ─────────────────────────────────── */
    if (strcmp(fn_name, "repeat") == 0) {
        NEED(2); GET_S(0, s); GET_I(1, n);
        if (n <= 0) return strlib_str("");
        size_t slen = strlen(s);
        char out[FLUXA_STR_MAX_OUT];
        int pos = 0;
        for (long i = 0; i < n && pos + (int)slen < FLUXA_STR_MAX_OUT - 1; i++) {
            memcpy(out + pos, s, slen);
            pos += (int)slen;
        }
        out[pos] = '\0';
        return strlib_str(out);
    }

    if (strcmp(fn_name, "from_int") == 0) {
        if (argc != 1) { STR_ERR("from_int: expected 1 argument"); return strlib_nil(); }
        Value v = args[0];
        char buf[32];
        if (v.type == VAL_INT)        snprintf(buf, sizeof(buf), "%ld", v.as.integer);
        else if (v.type == VAL_FLOAT) snprintf(buf, sizeof(buf), "%g",  v.as.real);
        else { STR_ERR("from_int: expected int or float"); return strlib_nil(); }
        return strlib_str(buf);
    }

    if (strcmp(fn_name, "to_int") == 0) {
        if (argc != 1) { STR_ERR("to_int: expected 1 argument"); return strlib_nil(); }
        Value v = args[0];
        if (v.type != VAL_STRING) { STR_ERR("to_int: expected str"); return strlib_nil(); }
        return strlib_int(atol(v.as.string ? v.as.string : "0"));
    }

    if (strcmp(fn_name, "concat") == 0) {
        /* str.concat(a, b, ...) — joins any number of values as strings */
        if (argc == 0) return strlib_str("");
        char out[4096]; out[0] = '\0';
        int pos = 0;
        for (int _i = 0; _i < argc && pos < (int)sizeof(out) - 1; _i++) {
            char tmp[512]; tmp[0] = '\0';
            Value v = args[_i];
            if (v.type == VAL_INT)        snprintf(tmp, sizeof(tmp), "%ld", v.as.integer);
            else if (v.type == VAL_FLOAT) snprintf(tmp, sizeof(tmp), "%g",  v.as.real);
            else if (v.type == VAL_BOOL)  snprintf(tmp, sizeof(tmp), "%s",  v.as.boolean ? "true" : "false");
            else if (v.type == VAL_STRING && v.as.string)
                                          snprintf(tmp, sizeof(tmp), "%s",  v.as.string);
            int tlen = (int)strlen(tmp);
            if (pos + tlen < (int)sizeof(out) - 1) {
                memcpy(out + pos, tmp, (size_t)tlen);
                pos += tlen;
            }
        }
        out[pos] = '\0';
        return strlib_str(out);
    }

#undef STR_ERR
#undef NEED
#undef GET_S
#undef GET_I

    snprintf(errbuf, sizeof(errbuf),
             "str.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "str", line);
    *had_error = 1;
    return strlib_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "strings",
    toml_key = "std.strings",
    owner    = "strings",
    call     = fluxa_std_strings_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_STRINGS_H */
