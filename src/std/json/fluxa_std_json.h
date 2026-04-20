/* fluxa_std_json.h — Fluxa Standard Library: json
 *
 * Compiled into the binary only when FLUXA_STD_JSON is defined.
 * Declared in [libs] of fluxa.toml to enable at runtime.
 *
 * JSON IS ALWAYS STR — no intermediate parse tree, no dyn-in-dyn.
 * The JSON string is the data structure. Extract what you need with
 * json.get_*(). Build objects incrementally with json.set().
 *
 * THREE MODES (mirrors std.csv):
 *
 *   Mode A — cursor (large JSON array files):
 *     prst dyn cur = json.open("data.json")
 *     dyn chunk    = json.next(cur, 200)   // 200 JSON objects per chunk
 *     while len(chunk) > 0 {
 *         for item in chunk {
 *             float t = json.get_float(item, "temp")
 *         }
 *         chunk = json.next(cur, 200)
 *     }
 *     json.close(cur)
 *
 *   Mode B — chunk direct (small-medium files):
 *     dyn chunk = json.chunk("data.json", 200)
 *
 *   Mode C — load/parse:
 *     str raw  = json.load("config.json")     // whole file as str
 *     dyn arr  = json.parse_array(raw)        // JSON array → dyn of str objects
 *
 * BUILDING JSON OBJECTS:
 *     str obj = json.object()                 // "{}"
 *     obj = json.set(obj, "temp",  json.from_float(23.5))
 *     obj = json.set(obj, "unit",  json.from_str("celsius"))
 *     obj = json.set(obj, "alive", json.from_bool(true))
 *     // → {"temp":23.5,"unit":"celsius","alive":true}
 *
 * EXTRACTING FROM JSON STRINGS:
 *     float t = json.get_float(raw, "temp")
 *     str   u = json.get_str(raw,   "unit")
 *     int   n = json.get_int(raw,   "count")
 *     bool  b = json.get_bool(raw,  "active")
 *
 * MEMORY SAFETY:
 *   [libs.json]
 *   max_str_bytes = 4096   # max size of a JSON str value (default 4096)
 *
 * NOTE: This is a minimal parser — handles flat objects and flat arrays.
 * Deeply nested JSON should be flattened before passing to Fluxa, or
 * extracted field by field with json.get_*.
 */
#ifndef FLUXA_STD_JSON_H
#define FLUXA_STD_JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../scope.h"
#include "../../err.h"

#ifndef FLUXA_JSON_MAX_STR
#define FLUXA_JSON_MAX_STR  4096
#endif
#ifndef FLUXA_JSON_MAX_KEY
#define FLUXA_JSON_MAX_KEY  256
#endif

/* ── Cursor (mirrors CsvCursor) ─────────────────────────────────────────── */
typedef struct {
    FILE *fp;
    char  path[512];
    long  byte_offset;
    int   eof;
} JsonCursor;

/* ── Value helpers ───────────────────────────────────────────────────────── */
static inline Value json_str_val(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}
static inline Value json_nil(void)  { Value v; v.type = VAL_NIL; return v; }
static inline Value json_int(long n){ Value v; v.type = VAL_INT; v.as.integer = n; return v; }
static inline Value json_bool(int b){ Value v; v.type = VAL_BOOL; v.as.boolean = b; return v; }
static inline Value json_float(double d){ Value v; v.type = VAL_FLOAT; v.as.real = d; return v; }

/* ── Minimal JSON helpers ────────────────────────────────────────────────── */

/* Skip whitespace */
static inline const char *json_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Read a JSON string starting at opening quote.
 * Returns pointer past closing quote, writes into buf[buf_size]. */
static inline const char *json_read_string(const char *p, char *buf, int buf_size) {
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < buf_size - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  buf[i++] = '"';  break;
                case '\\': buf[i++] = '\\'; break;
                case '/':  buf[i++] = '/';  break;
                case 'n':  buf[i++] = '\n'; break;
                case 'r':  buf[i++] = '\r'; break;
                case 't':  buf[i++] = '\t'; break;
                default:   buf[i++] = *p;   break;
            }
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* Find key in a flat JSON object string and return pointer to value start.
 * Only handles flat objects (no nested search). */
static inline const char *json_find_key(const char *json, const char *key) {
    const char *p = json_skip_ws(json);
    if (*p != '{') return NULL;
    p++;
    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        char kbuf[FLUXA_JSON_MAX_KEY];
        p = json_read_string(p, kbuf, sizeof(kbuf));
        if (!p) break;
        p = json_skip_ws(p);
        if (*p != ':') break;
        p++;
        p = json_skip_ws(p);
        if (strcmp(kbuf, key) == 0) return p;
        /* Skip value */
        if (*p == '"') {
            char vbuf[FLUXA_JSON_MAX_STR];
            p = json_read_string(p, vbuf, sizeof(vbuf));
        } else if (*p == '{' || *p == '[') {
            int depth = 1; char open = *p; char close = open == '{' ? '}' : ']';
            p++;
            while (*p && depth > 0) {
                if (*p == open) depth++;
                else if (*p == close) depth--;
                p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
    }
    return NULL;
}

/* Read one top-level JSON object/value from fp into buf.
 * Returns 1 on success, 0 on EOF/error. */
static inline int json_read_one_object(FILE *fp, char *buf, int buf_size) {
    int c;
    /* Skip to first '{' or '[' */
    while ((c = fgetc(fp)) != EOF) {
        if (c == '{' || c == '[') break;
    }
    if (c == EOF) return 0;

    char open = (char)c, close = open == '{' ? '}' : ']';
    int depth = 1, i = 0;
    buf[i++] = open;

    while (depth > 0 && (c = fgetc(fp)) != EOF && i < buf_size - 1) {
        buf[i++] = (char)c;
        if (c == '"') {
            /* Skip string contents */
            while ((c = fgetc(fp)) != EOF && i < buf_size - 1) {
                buf[i++] = (char)c;
                if (c == '\\') {
                    c = fgetc(fp);
                    if (c != EOF && i < buf_size - 1) buf[i++] = (char)c;
                } else if (c == '"') break;
            }
        } else if (c == open)  { depth++; }
          else if (c == close) { depth--; }
    }
    buf[i] = '\0';
    return depth == 0;
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */
static inline JsonCursor *json_cursor_from_val(const Value *v,
                                                ErrStack *err, int line) {
    if (!v || v->type != VAL_DYN || !v->as.dyn || v->as.dyn->count < 1) {
        errstack_push(err, ERR_FLUXA,
            "json: invalid cursor — use json.open() to create one", "json", line);
        return NULL;
    }
    Value *slot = &v->as.dyn->items[0];
    if (slot->type != VAL_PTR || !slot->as.ptr) {
        errstack_push(err, ERR_FLUXA,
            "json: cursor is closed or invalid", "json", line);
        return NULL;
    }
    return (JsonCursor *)slot->as.ptr;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
/* FluxaConfig is defined in toml_config.h, included before this header
 * by lib_registry_gen.h. No direct include needed here. */
static inline Value fluxa_std_json_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line,
                                         const FluxaConfig *cfg) {
    char errbuf[1024];
    int max_str = (cfg && cfg->json_max_str > 0)
                  ? cfg->json_max_str : FLUXA_JSON_MAX_STR;
    if (max_str <= 0) max_str = FLUXA_JSON_MAX_STR;

#define JSON_ERR(msg) do { \
    char _m[1024]; \
    strncpy(_m, msg, sizeof(_m)-1); _m[sizeof(_m)-1] = '\0'; \
    snprintf(errbuf, sizeof(errbuf), "json.%s: %.900s", fn_name, _m); \
    errstack_push(err, ERR_FLUXA, errbuf, "json", line); \
    *had_error = 1; return json_nil(); \
} while(0)

#define REQUIRE_ARGC_MIN(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "json.%s expects at least %d argument(s), got %d", \
            fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "json", line); \
        *had_error = 1; return json_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        JSON_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT_ARG(idx, var) \
    if (args[(idx)].type != VAL_INT) JSON_ERR("expected int argument"); \
    int (var) = (int)args[(idx)].as.integer;

    /* ── json.open(str path) → dyn cursor ───────────────────────────────── */
    if (strcmp(fn_name, "open") == 0) {
        REQUIRE_ARGC_MIN(1); GET_STR(0, path);
        FILE *fp = fopen(path, "r");
        if (!fp) {
            snprintf(errbuf, sizeof(errbuf), "json.open: cannot open '%s'", path);
            JSON_ERR(errbuf);
        }
        JsonCursor *cur = (JsonCursor *)malloc(sizeof(JsonCursor));
        if (!cur) { fclose(fp); JSON_ERR("out of memory"); }
        cur->fp = fp; cur->byte_offset = 0; cur->eof = 0;
        strncpy(cur->path, path, sizeof(cur->path)-1);
        cur->path[sizeof(cur->path)-1] = '\0';

        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) { free(cur); fclose(fp); JSON_ERR("out of memory"); }
        d->cap = 1; d->count = 1;
        d->items = (Value *)malloc(sizeof(Value));
        if (!d->items) { free(d); free(cur); fclose(fp); JSON_ERR("out of memory"); }
        d->items[0].type = VAL_PTR; d->items[0].as.ptr = cur;
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

    /* ── json.next(dyn cursor, int chunk_size) → dyn ────────────────────── */
    if (strcmp(fn_name, "next") == 0) {
        REQUIRE_ARGC_MIN(2); GET_INT_ARG(1, chunk_size);
        if (chunk_size <= 0) JSON_ERR("chunk_size must be > 0");
        JsonCursor *cur = json_cursor_from_val(&args[0], err, line);
        if (!cur) { *had_error = 1; return json_nil(); }

        if (cur->eof) {
            FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
            if (!d) JSON_ERR("out of memory");
            d->cap = 0; d->count = 0; d->items = NULL;
            Value ret; ret.type = VAL_DYN; ret.as.dyn = d; return ret;
        }

        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) JSON_ERR("out of memory");
        d->cap = chunk_size; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        if (!d->items) { free(d); JSON_ERR("out of memory"); }

        char objbuf[FLUXA_JSON_MAX_STR];
        int read = 0;
        while (read < chunk_size) {
            if (!json_read_one_object(cur->fp, objbuf, sizeof(objbuf))) {
                cur->eof = 1; break;
            }
            d->items[d->count++] = json_str_val(objbuf);
            read++;
        }
        cur->byte_offset = ftell(cur->fp);
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d; return ret;
    }

    /* ── json.close(dyn cursor) → nil ───────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        REQUIRE_ARGC_MIN(1);
        JsonCursor *cur = json_cursor_from_val(&args[0], err, line);
        if (!cur) return json_nil();
        if (cur->fp) { fclose(cur->fp); cur->fp = NULL; }
        free(cur);
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return json_nil();
    }

    /* ── json.load(str path) → str ──────────────────────────────────────── */
    if (strcmp(fn_name, "load") == 0) {
        REQUIRE_ARGC_MIN(1); GET_STR(0, path);
        FILE *fp = fopen(path, "r");
        if (!fp) {
            snprintf(errbuf, sizeof(errbuf), "cannot open '%s'", path);
            JSON_ERR(errbuf);
        }
        char *buf = (char *)malloc((size_t)max_str);
        if (!buf) { fclose(fp); JSON_ERR("out of memory"); }
        size_t n = fread(buf, 1, (size_t)(max_str - 1), fp);
        /* Check if file was truncated — if there's still data left, the
         * buffer was too small. Emit a clear error instead of silently
         * loading partial JSON which would produce confusing key-not-found
         * errors later. */
        int truncated = 0;
        if ((int)n == max_str - 1) {
            int c = fgetc(fp);
            if (c != EOF) truncated = 1;
        }
        fclose(fp);
        if (truncated) {
            free(buf);
            snprintf(errbuf, sizeof(errbuf),
                "file '%s' exceeds buffer (%d bytes). "
                "Increase [libs.json] max_str_bytes in fluxa.toml and reload the runtime.",
                path, max_str);
            JSON_ERR(errbuf);
        }
        buf[n] = '\0';
        Value ret = json_str_val(buf);
        free(buf);
        return ret;
    }

    /* ── json.parse_array(str raw) → dyn ────────────────────────────────── */
    /* Parse a JSON array string into a dyn of str (one str per element). */
    if (strcmp(fn_name, "parse_array") == 0) {
        REQUIRE_ARGC_MIN(1); GET_STR(0, raw);
        const char *p = json_skip_ws(raw);
        if (*p != '[') JSON_ERR("parse_array: expected JSON array starting with '['");
        p++;

        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) JSON_ERR("out of memory");
        d->cap = 16; d->count = 0;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        if (!d->items) { free(d); JSON_ERR("out of memory"); }

        char vbuf[FLUXA_JSON_MAX_STR];
        while (1) {
            p = json_skip_ws(p);
            if (*p == ']' || *p == '\0') break;
            if (*p == ',') { p++; continue; }

            const char *start = p;
            /* Skip one value */
            if (*p == '"') {
                p = json_read_string(p, vbuf, sizeof(vbuf));
                if (!p) break;
                /* vbuf has the unquoted string — re-quote for consistency */
                char quoted[FLUXA_JSON_MAX_STR + 4];
                snprintf(quoted, sizeof(quoted), "\"%s\"", vbuf);
                if (d->count >= d->cap) {
                    int nc = d->cap * 2;
                    Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                    if (!nb) break;
                    d->items = nb; d->cap = nc;
                }
                d->items[d->count++] = json_str_val(quoted);
            } else if (*p == '{' || *p == '[') {
                char open = *p, close = open == '{' ? '}' : ']';
                int depth = 1; const char *obj_start = p; p++;
                while (*p && depth > 0) {
                    if (*p == '"') { p = json_read_string(p, vbuf, sizeof(vbuf)); if(!p) break; continue; }
                    if (*p == open) depth++;
                    else if (*p == close) depth--;
                    p++;
                }
                /* Extract the object/array as str */
                size_t olen = (size_t)(p - obj_start);
                if (olen >= FLUXA_JSON_MAX_STR) olen = FLUXA_JSON_MAX_STR - 1;
                char obuf[FLUXA_JSON_MAX_STR];
                memcpy(obuf, obj_start, olen); obuf[olen] = '\0';
                if (d->count >= d->cap) {
                    int nc = d->cap * 2;
                    Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                    if (!nb) break;
                    d->items = nb; d->cap = nc;
                }
                d->items[d->count++] = json_str_val(obuf);
            } else {
                /* number / bool / null — read until delimiter */
                while (*p && *p != ',' && *p != ']' && !isspace((unsigned char)*p)) p++;
                size_t vlen = (size_t)(p - start);
                if (vlen >= FLUXA_JSON_MAX_STR) vlen = FLUXA_JSON_MAX_STR - 1;
                memcpy(vbuf, start, vlen); vbuf[vlen] = '\0';
                if (d->count >= d->cap) {
                    int nc = d->cap * 2;
                    Value *nb = (Value *)realloc(d->items, sizeof(Value)*(size_t)nc);
                    if (!nb) break;
                    d->items = nb; d->cap = nc;
                }
                d->items[d->count++] = json_str_val(vbuf);
            }
        }
        Value ret; ret.type = VAL_DYN; ret.as.dyn = d; return ret;
    }

    /* ── json.get_str(str json, str key) → str ───────────────────────────── */
    if (strcmp(fn_name, "get_str") == 0) {
        REQUIRE_ARGC_MIN(2); GET_STR(0, jsn); GET_STR(1, key);
        const char *vp = json_find_key(jsn, key);
        if (!vp) {
            snprintf(errbuf, sizeof(errbuf), "key '%s' not found", key);
            JSON_ERR(errbuf);
        }
        vp = json_skip_ws(vp);
        if (*vp != '"') {
            snprintf(errbuf, sizeof(errbuf), "key '%s' is not a string", key);
            JSON_ERR(errbuf);
        }
        char vbuf[FLUXA_JSON_MAX_STR];
        json_read_string(vp, vbuf, sizeof(vbuf));
        return json_str_val(vbuf);
    }

    /* ── json.get_float(str json, str key) → float ───────────────────────── */
    if (strcmp(fn_name, "get_float") == 0) {
        REQUIRE_ARGC_MIN(2); GET_STR(0, jsn); GET_STR(1, key);
        const char *vp = json_find_key(jsn, key);
        if (!vp) {
            snprintf(errbuf, sizeof(errbuf), "key '%s' not found", key);
            JSON_ERR(errbuf);
        }
        vp = json_skip_ws(vp);
        char *end; double val = strtod(vp, &end);
        if (end == vp) JSON_ERR("value is not a number");
        return json_float(val);
    }

    /* ── json.get_int(str json, str key) → int ───────────────────────────── */
    if (strcmp(fn_name, "get_int") == 0) {
        REQUIRE_ARGC_MIN(2); GET_STR(0, jsn); GET_STR(1, key);
        const char *vp = json_find_key(jsn, key);
        if (!vp) {
            snprintf(errbuf, sizeof(errbuf), "key '%s' not found", key);
            JSON_ERR(errbuf);
        }
        vp = json_skip_ws(vp);
        char *end; long val = strtol(vp, &end, 10);
        if (end == vp) JSON_ERR("value is not an integer");
        return json_int(val);
    }

    /* ── json.get_bool(str json, str key) → bool ─────────────────────────── */
    if (strcmp(fn_name, "get_bool") == 0) {
        REQUIRE_ARGC_MIN(2); GET_STR(0, jsn); GET_STR(1, key);
        const char *vp = json_find_key(jsn, key);
        if (!vp) {
            snprintf(errbuf, sizeof(errbuf), "key '%s' not found", key);
            JSON_ERR(errbuf);
        }
        vp = json_skip_ws(vp);
        if (strncmp(vp, "true",  4) == 0) return json_bool(1);
        if (strncmp(vp, "false", 5) == 0) return json_bool(0);
        JSON_ERR("value is not a boolean");
    }

    /* ── json.has(str json, str key) → bool ─────────────────────────────── */
    if (strcmp(fn_name, "has") == 0) {
        REQUIRE_ARGC_MIN(2); GET_STR(0, jsn); GET_STR(1, key);
        return json_bool(json_find_key(jsn, key) != NULL ? 1 : 0);
    }

    /* ── json.object() → str ─────────────────────────────────────────────── */
    if (strcmp(fn_name, "object") == 0) {
        return json_str_val("{}");
    }

    /* ── json.array() → str ──────────────────────────────────────────────── */
    if (strcmp(fn_name, "array") == 0) {
        return json_str_val("[]");
    }

    /* ── json.set(str obj, str key, str value) → str ────────────────────── */
    /* Adds or replaces a key in a flat JSON object. Value must be a valid
     * JSON value string (use json.from_* to convert Fluxa values). */
    if (strcmp(fn_name, "set") == 0) {
        REQUIRE_ARGC_MIN(3);
        GET_STR(0, obj); GET_STR(1, key); GET_STR(2, val);

        /* Build new JSON object — simple append or replace */
        char out[FLUXA_JSON_MAX_STR * 4];
        const char *p = json_skip_ws(obj);
        if (*p != '{') JSON_ERR("json.set: first argument must be a JSON object");

        /* Check if key already exists */
        if (json_find_key(obj, key)) {
            /* Key exists — rebuild object replacing that key */
            /* Simple approach: output all pairs except the old key, then add new */
            int out_len = 0;
            out[out_len++] = '{';
            p = json_skip_ws(p + 1);  /* skip '{' */
            int first = 1;
            while (*p && *p != '}') {
                if (*p == ',') { p++; continue; }
                p = json_skip_ws(p);
                if (*p != '"') break;
                char kbuf[FLUXA_JSON_MAX_KEY];
                const char *after_k = json_read_string(p, kbuf, sizeof(kbuf));
                if (!after_k) break;
                after_k = json_skip_ws(after_k);
                if (*after_k != ':') break;
                after_k++;
                after_k = json_skip_ws(after_k);
                /* Skip value */
                const char *val_start = after_k;
                const char *val_end   = after_k;
                if (*after_k == '"') {
                    char vb[FLUXA_JSON_MAX_STR];
                    val_end = json_read_string(after_k, vb, sizeof(vb));
                } else if (*after_k == '{' || *after_k == '[') {
                    int dep = 1; char op = *after_k, cl = op=='{' ? '}' : ']';
                    val_end = after_k + 1;
                    while (*val_end && dep > 0) {
                        if (*val_end == op) dep++;
                        else if (*val_end == cl) dep--;
                        val_end++;
                    }
                } else {
                    while (*val_end && *val_end != ',' && *val_end != '}')
                        val_end++;
                }
                /* Skip this key if it matches */
                if (strcmp(kbuf, key) != 0) {
                    if (!first) out[out_len++] = ',';
                    first = 0;
                    /* Write "key": value */
                    int kl = snprintf(out + out_len,
                        sizeof(out) - (size_t)out_len - 1,
                        "\"%s\":", kbuf);
                    out_len += kl;
                    size_t vl = (size_t)(val_end - val_start);
                    if (vl >= sizeof(out) - (size_t)out_len - 2) vl = sizeof(out) - (size_t)out_len - 2;
                    memcpy(out + out_len, val_start, vl);
                    out_len += (int)vl;
                }
                p = val_end;
                p = json_skip_ws(p);
            }
            /* Append new key-value */
            if (!first) out[out_len++] = ',';
            int nl = snprintf(out + out_len, sizeof(out) - (size_t)out_len - 2,
                "\"%s\":%s}", key, val);
            out_len += nl;
            out[out_len] = '\0';
        } else {
            /* Key doesn't exist — simple append before closing brace */
            size_t obj_len = strlen(obj);
            /* Find last '}' */
            char tmp[FLUXA_JSON_MAX_STR * 2];
            strncpy(tmp, obj, sizeof(tmp)-1);
            tmp[sizeof(tmp)-1] = '\0';
            char *close = strrchr(tmp, '}');
            if (!close) JSON_ERR("json.set: malformed JSON object");
            /* Check if object is empty */
            const char *inner = json_skip_ws(tmp + 1);
            int is_empty = (*inner == '}');
            if (is_empty) {
                snprintf(out, sizeof(out), "{\"%s\":%s}", key, val);
            } else {
                *close = '\0';
                snprintf(out, sizeof(out), "%s,\"%s\":%s}", tmp, key, val);
            }
            (void)obj_len;
        }
        return json_str_val(out);
    }

    /* ── json.from_str(str val) → str ────────────────────────────────────── */
    if (strcmp(fn_name, "from_str") == 0) {
        REQUIRE_ARGC_MIN(1); GET_STR(0, s);
        char buf[FLUXA_JSON_MAX_STR];
        snprintf(buf, sizeof(buf), "\"%s\"", s);
        return json_str_val(buf);
    }

    /* ── json.from_float(float val) → str ────────────────────────────────── */
    if (strcmp(fn_name, "from_float") == 0) {
        REQUIRE_ARGC_MIN(1);
        double d = 0.0;
        if (args[0].type == VAL_FLOAT) d = args[0].as.real;
        else if (args[0].type == VAL_INT) d = (double)args[0].as.integer;
        else JSON_ERR("from_float: expected float or int");
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", d);
        return json_str_val(buf);
    }

    /* ── json.from_int(int val) → str ────────────────────────────────────── */
    if (strcmp(fn_name, "from_int") == 0) {
        REQUIRE_ARGC_MIN(1);
        long n = 0;
        if (args[0].type == VAL_INT) n = args[0].as.integer;
        else if (args[0].type == VAL_FLOAT) n = (long)args[0].as.real;
        else JSON_ERR("from_int: expected int or float");
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", n);
        return json_str_val(buf);
    }

    /* ── json.from_bool(bool val) → str ─────────────────────────────────── */
    if (strcmp(fn_name, "from_bool") == 0) {
        REQUIRE_ARGC_MIN(1);
        int b = 0;
        if (args[0].type == VAL_BOOL) b = args[0].as.boolean;
        else if (args[0].type == VAL_INT) b = args[0].as.integer != 0;
        else JSON_ERR("from_bool: expected bool or int");
        return json_str_val(b ? "true" : "false");
    }

    /* ── json.valid(str raw) → bool ──────────────────────────────────────── */
    if (strcmp(fn_name, "valid") == 0) {
        REQUIRE_ARGC_MIN(1); GET_STR(0, raw);
        const char *p = json_skip_ws(raw);
        return json_bool(*p == '{' || *p == '[' || *p == '"' ||
                         isdigit((unsigned char)*p) || *p == '-' ||
                         strncmp(p,"true",4)==0 || strncmp(p,"false",5)==0 ||
                         strncmp(p,"null",4)==0 ? 1 : 0);
    }

    /* ── json.is_eof(dyn cursor) → bool ─────────────────────────────────── */
    if (strcmp(fn_name, "is_eof") == 0) {
        REQUIRE_ARGC_MIN(1);
        JsonCursor *cur = json_cursor_from_val(&args[0], err, line);
        Value v; v.type = VAL_BOOL;
        v.as.boolean = (!cur || cur->eof) ? 1 : 0;
        return v;
    }

    /* ── json.stringify(dyn data) → str ─────────────────────────────────── */
    /* Converts a dyn of str values into a JSON array string. */
    if (strcmp(fn_name, "stringify") == 0) {
        REQUIRE_ARGC_MIN(1);
        if (args[0].type != VAL_DYN || !args[0].as.dyn)
            JSON_ERR("stringify: expected dyn");
        FluxaDyn *d = args[0].as.dyn;
        char out[FLUXA_JSON_MAX_STR * 4];
        int pos = 0;
        out[pos++] = '[';
        for (int i = 0; i < d->count; i++) {
            if (i > 0) out[pos++] = ',';
            if (d->items[i].type == VAL_STRING && d->items[i].as.string) {
                int w = snprintf(out + pos, sizeof(out) - (size_t)pos - 2,
                                 "%s", d->items[i].as.string);
                pos += w;
            } else if (d->items[i].type == VAL_INT) {
                int w = snprintf(out + pos, sizeof(out) - (size_t)pos - 2,
                                 "%ld", d->items[i].as.integer);
                pos += w;
            } else if (d->items[i].type == VAL_FLOAT) {
                int w = snprintf(out + pos, sizeof(out) - (size_t)pos - 2,
                                 "%g", d->items[i].as.real);
                pos += w;
            } else if (d->items[i].type == VAL_BOOL) {
                int w = snprintf(out + pos, sizeof(out) - (size_t)pos - 2,
                                 "%s", d->items[i].as.boolean ? "true" : "false");
                pos += w;
            }
            if (pos >= (int)sizeof(out) - 4) break;
        }
        out[pos++] = ']';
        out[pos]   = '\0';
        return json_str_val(out);
    }

#undef JSON_ERR
#undef REQUIRE_ARGC_MIN
#undef GET_STR
#undef GET_INT_ARG

    snprintf(errbuf, sizeof(errbuf),
        "json.%s: unknown function — available: "
        "open, next, close, load, parse_array, get_str, get_float, get_int, "
        "get_bool, has, object, array, set, from_str, from_float, from_int, "
        "from_bool, valid, is_eof, stringify. "
        "Make sure 'std.json = \"1.0\"' is declared under [libs] in fluxa.toml.",
        fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "json", line);
    *had_error = 1;
    return json_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── */
FLUXA_LIB_EXPORT(
    name      = "json",
    toml_key  = "std.json",
    owner     = "json",
    call      = fluxa_std_json_call,
    rt_aware  = 0,
    cfg_aware = 1
)

#endif /* FLUXA_STD_JSON_H */
