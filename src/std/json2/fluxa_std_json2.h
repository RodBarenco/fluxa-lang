#ifndef FLUXA_STD_JSON2_H
#define FLUXA_STD_JSON2_H

/*
 * std.json2 — Full DOM JSON parser for Fluxa-lang
 *
 * Unlike std.json (streaming, no DOM, fields as strings), json2 parses
 * the entire document into an in-memory tree. Navigate by path, index
 * arrays, read typed values, modify nodes, stringify back.
 *
 * Design:
 *   - Documents stored as opaque cursors (VAL_PTR → Json2Doc*)
 *   - prst dyn doc survives hot reloads (pointer preserved)
 *   - Path syntax: "a.b.c" for objects, "arr[0]" for arrays
 *   - Types: null, bool, int, float, str, array, object
 *   - Pure C99, zero external deps
 *
 * API:
 *   json2.parse(str)           → dyn cursor
 *   json2.load(path)           → dyn cursor (from file)
 *   json2.stringify(doc)       → str
 *   json2.get(doc, path)       → str  (any value as string)
 *   json2.get_int(doc, path)   → int
 *   json2.get_float(doc, path) → float
 *   json2.get_bool(doc, path)  → bool
 *   json2.has(doc, path)       → bool
 *   json2.type(doc, path)      → str  ("null"|"bool"|"int"|"float"|"str"|"array"|"object")
 *   json2.length(doc, path)    → int  (array or object key count)
 *   json2.key(doc, path, i)    → str  (i-th key of object at path)
 *   json2.set(doc, path, val)  → nil  (set string value)
 *   json2.set_int(doc, path, n)   → nil
 *   json2.set_float(doc, path, f) → nil
 *   json2.set_bool(doc, path, b)  → nil
 *   json2.delete(doc, path)    → nil
 *   json2.free(doc)            → nil
 *   json2.valid(doc)           → bool (cursor is valid, non-null)
 *   json2.error(doc)           → str  (last parse error, or "")
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Node types ──────────────────────────────────────────────────── */
typedef enum {
    J2_NULL = 0,
    J2_BOOL,
    J2_INT,
    J2_FLOAT,
    J2_STRING,
    J2_ARRAY,
    J2_OBJECT
} J2Type;

typedef struct J2Node J2Node;
struct J2Node {
    J2Type type;
    char  *key;        /* for object children — allocated */
    union {
        int    boolean;
        long   integer;
        double real;
        char  *string;
        struct { J2Node **items; int count; int cap; } arr; /* array/object */
    } v;
};

typedef struct {
    J2Node *root;
    char    error[256];
} Json2Doc;

/* ── Node allocation ─────────────────────────────────────────────── */
static inline J2Node *j2_node(J2Type t) {
    J2Node *n = (J2Node *)calloc(1, sizeof(J2Node));
    n->type = t;
    return n;
}

static inline void j2_free_node(J2Node *n) {
    if (!n) return;
    free(n->key);
    if (n->type == J2_STRING) { free(n->v.string); }
    else if (n->type == J2_ARRAY || n->type == J2_OBJECT) {
        for (int i = 0; i < n->v.arr.count; i++)
            j2_free_node(n->v.arr.items[i]);
        free(n->v.arr.items);
    }
    free(n);
}

static inline void j2_arr_push(J2Node *parent, J2Node *child) {
    if (parent->v.arr.count >= parent->v.arr.cap) {
        parent->v.arr.cap = parent->v.arr.cap ? parent->v.arr.cap * 2 : 4;
        parent->v.arr.items = (J2Node **)realloc(parent->v.arr.items,
            (size_t)parent->v.arr.cap * sizeof(J2Node *));
    }
    parent->v.arr.items[parent->v.arr.count++] = child;
}

/* ── Parser ──────────────────────────────────────────────────────── */
typedef struct { const char *s; int pos; int len; } J2Lex;

static inline void j2_skip_ws(J2Lex *l) {
    while (l->pos < l->len && isspace((unsigned char)l->s[l->pos])) l->pos++;
}
static inline char j2_peek(J2Lex *l) {
    j2_skip_ws(l); return (l->pos < l->len) ? l->s[l->pos] : 0;
}
static inline char j2_next(J2Lex *l) {
    j2_skip_ws(l); return (l->pos < l->len) ? l->s[l->pos++] : 0;
}

static J2Node *j2_parse_value(J2Lex *l, char *errbuf, int errsz);

static inline char *j2_parse_str_raw(J2Lex *l, char *errbuf, int errsz) {
    if (j2_next(l) != '"') { snprintf(errbuf,errsz,"expected '\"'"); return NULL; }
    int start = l->pos;
    /* Count length first */
    char *buf = (char *)malloc(4096);
    int bi = 0;
    while (l->pos < l->len && l->s[l->pos] != '"') {
        if (l->s[l->pos] == '\\') {
            l->pos++;
            if (l->pos >= l->len) break;
            char esc = l->s[l->pos++];
            char ch = (esc=='n') ? '\n' : (esc=='t') ? '\t' :
                      (esc=='r') ? '\r' : (esc=='\\') ? '\\' :
                      (esc=='"') ? '"'  : (esc=='/') ? '/'  : esc;
            buf[bi++] = ch;
        } else {
            buf[bi++] = l->s[l->pos++];
        }
        if (bi >= 4095) break;
    }
    buf[bi] = '\0';
    if (l->pos < l->len) l->pos++; /* closing " */
    (void)start;
    return buf;
}

static J2Node *j2_parse_value(J2Lex *l, char *errbuf, int errsz) {
    char c = j2_peek(l);
    if (c == '"') {
        char *s = j2_parse_str_raw(l, errbuf, errsz);
        if (!s) return NULL;
        J2Node *n = j2_node(J2_STRING); n->v.string = s; return n;
    }
    if (c == '{') {
        l->pos++; j2_skip_ws(l);
        J2Node *obj = j2_node(J2_OBJECT);
        if (j2_peek(l) == '}') { l->pos++; return obj; }
        while (1) {
            j2_skip_ws(l);
            char *key = j2_parse_str_raw(l, errbuf, errsz);
            if (!key) { j2_free_node(obj); return NULL; }
            j2_skip_ws(l);
            if (j2_next(l) != ':') {
                free(key); j2_free_node(obj);
                snprintf(errbuf, errsz, "expected ':'"); return NULL;
            }
            J2Node *val = j2_parse_value(l, errbuf, errsz);
            if (!val) { free(key); j2_free_node(obj); return NULL; }
            val->key = key;
            j2_arr_push(obj, val);
            j2_skip_ws(l);
            char sep = j2_next(l);
            if (sep == '}') break;
            if (sep != ',') {
                j2_free_node(obj);
                snprintf(errbuf, errsz, "expected ',' or '}'"); return NULL;
            }
        }
        return obj;
    }
    if (c == '[') {
        l->pos++; j2_skip_ws(l);
        J2Node *arr = j2_node(J2_ARRAY);
        if (j2_peek(l) == ']') { l->pos++; return arr; }
        while (1) {
            J2Node *val = j2_parse_value(l, errbuf, errsz);
            if (!val) { j2_free_node(arr); return NULL; }
            j2_arr_push(arr, val);
            j2_skip_ws(l);
            char sep = j2_next(l);
            if (sep == ']') break;
            if (sep != ',') {
                j2_free_node(arr);
                snprintf(errbuf, errsz, "expected ',' or ']'"); return NULL;
            }
        }
        return arr;
    }
    if (c == 't' && strncmp(l->s+l->pos, "true", 4) == 0)  { l->pos+=4; J2Node *n=j2_node(J2_BOOL); n->v.boolean=1; return n; }
    if (c == 'f' && strncmp(l->s+l->pos, "false", 5) == 0) { l->pos+=5; J2Node *n=j2_node(J2_BOOL); n->v.boolean=0; return n; }
    if (c == 'n' && strncmp(l->s+l->pos, "null",  4) == 0) { l->pos+=4; return j2_node(J2_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char nbuf[64]; int ni = 0;
        if (l->s[l->pos] == '-') nbuf[ni++] = l->s[l->pos++];
        while (l->pos < l->len && isdigit((unsigned char)l->s[l->pos]) && ni < 62)
            nbuf[ni++] = l->s[l->pos++];
        int is_float = 0;
        if (l->pos < l->len && (l->s[l->pos] == '.' || l->s[l->pos] == 'e' || l->s[l->pos] == 'E')) {
            is_float = 1;
            while (l->pos < l->len && (isdigit((unsigned char)l->s[l->pos]) ||
                   l->s[l->pos]=='.'||l->s[l->pos]=='e'||l->s[l->pos]=='E'||
                   l->s[l->pos]=='+'||l->s[l->pos]=='-') && ni < 62)
                nbuf[ni++] = l->s[l->pos++];
        }
        nbuf[ni] = '\0';
        J2Node *n;
        if (is_float) { n = j2_node(J2_FLOAT); n->v.real    = strtod(nbuf, NULL); }
        else          { n = j2_node(J2_INT);   n->v.integer = strtol(nbuf, NULL, 10); }
        return n;
    }
    snprintf(errbuf, errsz, "unexpected char '%c' at pos %d", c, l->pos);
    return NULL;
}

static inline Json2Doc *j2_parse(const char *src) {
    Json2Doc *doc = (Json2Doc *)calloc(1, sizeof(Json2Doc));
    J2Lex l; l.s = src; l.pos = 0; l.len = (int)strlen(src);
    doc->root = j2_parse_value(&l, doc->error, sizeof(doc->error));
    return doc;
}

/* ── Path navigation ─────────────────────────────────────────────── */
/* Path: "a.b.c" → object keys, "arr[2]" → array index, ".items[0].name" */
static inline J2Node *j2_navigate(J2Node *root, const char *path) {
    if (!root || !path || path[0] == '\0') return root;
    J2Node *cur = root;
    const char *p = path;
    while (*p && cur) {
        if (*p == '.') { p++; continue; }
        if (*p == '[') {
            /* array index */
            p++;
            int idx = 0;
            while (*p && isdigit((unsigned char)*p)) { idx = idx*10 + (*p-'0'); p++; }
            if (*p == ']') p++;
            if (cur->type != J2_ARRAY || idx >= cur->v.arr.count) return NULL;
            cur = cur->v.arr.items[idx];
        } else {
            /* object key — read until '.', '[', or end */
            char key[256]; int ki = 0;
            while (*p && *p != '.' && *p != '[' && ki < 255) key[ki++] = *p++;
            key[ki] = '\0';
            if (cur->type != J2_OBJECT) return NULL;
            J2Node *found = NULL;
            for (int i = 0; i < cur->v.arr.count; i++)
                if (cur->v.arr.items[i]->key && strcmp(cur->v.arr.items[i]->key, key) == 0)
                    { found = cur->v.arr.items[i]; break; }
            cur = found;
        }
    }
    return cur;
}

/* ── Stringify ───────────────────────────────────────────────────── */
typedef struct { char *buf; int len; int cap; } J2SB;
static inline void j2_sb_append(J2SB *sb, const char *s, int n) {
    if (sb->len + n + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 256;
        while (sb->cap < sb->len + n + 2) sb->cap *= 2;
        sb->buf = (char *)realloc(sb->buf, (size_t)sb->cap);
    }
    memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n; sb->buf[sb->len] = '\0';
}
static inline void j2_sb_str(J2SB *sb, const char *s) { j2_sb_append(sb, s, (int)strlen(s)); }

static void j2_stringify_node(J2Node *n, J2SB *sb) {
    if (!n) { j2_sb_str(sb, "null"); return; }
    char tmp[64];
    switch (n->type) {
        case J2_NULL:   j2_sb_str(sb, "null"); break;
        case J2_BOOL:   j2_sb_str(sb, n->v.boolean ? "true" : "false"); break;
        case J2_INT:    snprintf(tmp, sizeof(tmp), "%ld", n->v.integer); j2_sb_str(sb, tmp); break;
        case J2_FLOAT:  snprintf(tmp, sizeof(tmp), "%g",  n->v.real);    j2_sb_str(sb, tmp); break;
        case J2_STRING:
            j2_sb_str(sb, "\"");
            /* Escape special chars */
            for (const char *c = n->v.string; *c; c++) {
                if (*c == '"')       j2_sb_str(sb, "\\\"");
                else if (*c == '\\') j2_sb_str(sb, "\\\\");
                else if (*c == '\n') j2_sb_str(sb, "\\n");
                else if (*c == '\t') j2_sb_str(sb, "\\t");
                else { char ch[2] = {*c, 0}; j2_sb_str(sb, ch); }
            }
            j2_sb_str(sb, "\"");
            break;
        case J2_ARRAY:
            j2_sb_str(sb, "[");
            for (int i = 0; i < n->v.arr.count; i++) {
                if (i) j2_sb_str(sb, ",");
                j2_stringify_node(n->v.arr.items[i], sb);
            }
            j2_sb_str(sb, "]");
            break;
        case J2_OBJECT:
            j2_sb_str(sb, "{");
            for (int i = 0; i < n->v.arr.count; i++) {
                if (i) j2_sb_str(sb, ",");
                j2_sb_str(sb, "\"");
                j2_sb_str(sb, n->v.arr.items[i]->key ? n->v.arr.items[i]->key : "");
                j2_sb_str(sb, "\":");
                j2_stringify_node(n->v.arr.items[i], sb);
            }
            j2_sb_str(sb, "}");
            break;
    }
}

/* ── Cursor helpers ──────────────────────────────────────────────── */
static inline Value j2_nil(void)     { Value v; v.type = VAL_NIL;    return v; }
static inline Value j2_int(long n)   { Value v; v.type = VAL_INT;    v.as.integer = n; return v; }
static inline Value j2_float(double d){ Value v; v.type = VAL_FLOAT; v.as.real    = d; return v; }
static inline Value j2_bool(int b)   { Value v; v.type = VAL_BOOL;   v.as.boolean = b; return v; }
static inline Value j2_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v;
}
static inline Value j2_wrap(Json2Doc *doc) {
    Value v; v.type = VAL_DYN;
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    memset(d, 0, sizeof(FluxaDyn));
    Value ptr; ptr.type = VAL_PTR; ptr.as.ptr = doc;
    d->items = (Value *)malloc(sizeof(Value));
    d->items[0] = ptr; d->count = 1; d->cap = 1;
    v.as.dyn = d; return v;
}
static inline Json2Doc *j2_unwrap(const Value *v, ErrStack *err,
                                    int *had_error, int line, const char *fn) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn || v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR || !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf), "json2.%s: invalid document cursor", fn);
        errstack_push(err, ERR_FLUXA, errbuf, "json2", line);
        *had_error = 1; return NULL;
    }
    return (Json2Doc *)v->as.dyn->items[0].as.ptr;
}

/* ── Node value → Fluxa Value ────────────────────────────────────── */
static inline Value j2_node_to_str(J2Node *n) {
    if (!n) return j2_str("null");
    char tmp[64];
    switch (n->type) {
        case J2_NULL:   return j2_str("null");
        case J2_BOOL:   return j2_str(n->v.boolean ? "true" : "false");
        case J2_INT:    snprintf(tmp, sizeof(tmp), "%ld", n->v.integer); return j2_str(tmp);
        case J2_FLOAT:  snprintf(tmp, sizeof(tmp), "%g",  n->v.real);    return j2_str(tmp);
        case J2_STRING: return j2_str(n->v.string ? n->v.string : "");
        case J2_ARRAY:  return j2_str("[array]");
        case J2_OBJECT: return j2_str("[object]");
    }
    return j2_str("");
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_json2_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define J2_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "json2.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "json2", line); \
    *had_error = 1; return j2_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), "json2.%s: expected %d arg(s), got %d", \
                 fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "json2", line); \
        *had_error = 1; return j2_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        J2_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_DOC(idx, var) \
    Json2Doc *(var) = j2_unwrap(&args[(idx)], err, had_error, line, fn_name); \
    if (!(var)) return j2_nil();

#define GET_PATH(idx, var) GET_STR(idx, var)

    /* json2.parse(str) → dyn */
    if (!strcmp(fn_name, "parse")) {
        NEED(1); GET_STR(0, src);
        Json2Doc *doc = j2_parse(src);
        if (!doc->root && doc->error[0]) {
            snprintf(errbuf, sizeof(errbuf), "json2.parse: %s", doc->error);
            errstack_push(err, ERR_FLUXA, errbuf, "json2", line);
            *had_error = 1; free(doc); return j2_nil();
        }
        return j2_wrap(doc);
    }

    /* json2.load(path) → dyn */
    if (!strcmp(fn_name, "load")) {
        NEED(1); GET_STR(0, path);
        FILE *f = fopen(path, "r");
        if (!f) J2_ERR("cannot open file");
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        char *buf = (char *)malloc((size_t)sz + 1);
        size_t _nr = fread(buf, 1, (size_t)sz, f); buf[_nr] = '\0'; fclose(f);
        Json2Doc *doc = j2_parse(buf); free(buf);
        if (!doc->root && doc->error[0]) {
            snprintf(errbuf, sizeof(errbuf), "json2.load: %s", doc->error);
            errstack_push(err, ERR_FLUXA, errbuf, "json2", line);
            *had_error = 1; free(doc); return j2_nil();
        }
        return j2_wrap(doc);
    }

    /* json2.stringify(doc) → str */
    if (!strcmp(fn_name, "stringify")) {
        NEED(1); GET_DOC(0, doc);
        J2SB sb; memset(&sb, 0, sizeof(sb));
        j2_stringify_node(doc->root, &sb);
        Value v = j2_str(sb.buf ? sb.buf : "null");
        free(sb.buf); return v;
    }

    /* json2.valid(doc) → bool */
    if (!strcmp(fn_name, "valid")) {
        NEED(1);
        if (args[0].type != VAL_DYN || !args[0].as.dyn ||
            args[0].as.dyn->count < 1 ||
            args[0].as.dyn->items[0].type != VAL_PTR ||
            !args[0].as.dyn->items[0].as.ptr) return j2_bool(0);
        Json2Doc *doc = (Json2Doc *)args[0].as.dyn->items[0].as.ptr;
        return j2_bool(doc->root != NULL);
    }

    /* json2.error(doc) → str */
    if (!strcmp(fn_name, "error")) {
        NEED(1); GET_DOC(0, doc);
        return j2_str(doc->error);
    }

    /* json2.get(doc, path) → str */
    if (!strcmp(fn_name, "get")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) return j2_str("null");
        return j2_node_to_str(n);
    }

    /* json2.get_int(doc, path) → int */
    if (!strcmp(fn_name, "get_int")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        if (n->type == J2_INT)   return j2_int(n->v.integer);
        if (n->type == J2_FLOAT) return j2_int((long)n->v.real);
        if (n->type == J2_BOOL)  return j2_int(n->v.boolean);
        J2_ERR("value is not numeric");
    }

    /* json2.get_float(doc, path) → float */
    if (!strcmp(fn_name, "get_float")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        if (n->type == J2_FLOAT) return j2_float(n->v.real);
        if (n->type == J2_INT)   return j2_float((double)n->v.integer);
        J2_ERR("value is not numeric");
    }

    /* json2.get_bool(doc, path) → bool */
    if (!strcmp(fn_name, "get_bool")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        if (n->type == J2_BOOL) return j2_bool(n->v.boolean);
        if (n->type == J2_INT)  return j2_bool(n->v.integer != 0);
        J2_ERR("value is not bool");
    }

    /* json2.has(doc, path) → bool */
    if (!strcmp(fn_name, "has")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        return j2_bool(j2_navigate(doc->root, path) != NULL);
    }

    /* json2.type(doc, path) → str */
    if (!strcmp(fn_name, "type")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) return j2_str("null");
        switch (n->type) {
            case J2_NULL:   return j2_str("null");
            case J2_BOOL:   return j2_str("bool");
            case J2_INT:    return j2_str("int");
            case J2_FLOAT:  return j2_str("float");
            case J2_STRING: return j2_str("str");
            case J2_ARRAY:  return j2_str("array");
            case J2_OBJECT: return j2_str("object");
        }
        return j2_str("null");
    }

    /* json2.length(doc, path) → int */
    if (!strcmp(fn_name, "length")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) return j2_int(0);
        if (n->type == J2_ARRAY || n->type == J2_OBJECT) return j2_int(n->v.arr.count);
        if (n->type == J2_STRING) return j2_int((long)strlen(n->v.string ? n->v.string : ""));
        return j2_int(0);
    }

    /* json2.key(doc, path, i) → str (i-th key of object at path) */
    if (!strcmp(fn_name, "key")) {
        NEED(3); GET_DOC(0, doc); GET_PATH(1, path);
        if (args[2].type != VAL_INT) J2_ERR("index must be int");
        int idx = (int)args[2].as.integer;
        J2Node *n = j2_navigate(doc->root, path);
        if (!n || n->type != J2_OBJECT) J2_ERR("path is not an object");
        if (idx < 0 || idx >= n->v.arr.count) J2_ERR("index out of range");
        return j2_str(n->v.arr.items[idx]->key ? n->v.arr.items[idx]->key : "");
    }

    /* json2.set(doc, path, val) → nil */
    if (!strcmp(fn_name, "set")) {
        NEED(3); GET_DOC(0, doc); GET_PATH(1, path);
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found — use json2.set on existing paths");
        if (args[2].type == VAL_STRING) {
            if (n->type == J2_STRING) { free(n->v.string); }
            n->type = J2_STRING;
            n->v.string = strdup(args[2].as.string ? args[2].as.string : "");
        } else if (args[2].type == VAL_INT) {
            n->type = J2_INT; n->v.integer = args[2].as.integer;
        } else if (args[2].type == VAL_FLOAT) {
            n->type = J2_FLOAT; n->v.real = args[2].as.real;
        } else if (args[2].type == VAL_BOOL) {
            n->type = J2_BOOL; n->v.boolean = args[2].as.boolean;
        } else { J2_ERR("unsupported value type for set"); }
        return j2_nil();
    }

    /* json2.set_int(doc, path, n) → nil */
    if (!strcmp(fn_name, "set_int")) {
        NEED(3); GET_DOC(0, doc); GET_PATH(1, path);
        if (args[2].type != VAL_INT) J2_ERR("expected int");
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        n->type = J2_INT; n->v.integer = args[2].as.integer;
        return j2_nil();
    }

    /* json2.set_float(doc, path, f) → nil */
    if (!strcmp(fn_name, "set_float")) {
        NEED(3); GET_DOC(0, doc); GET_PATH(1, path);
        double fval = (args[2].type == VAL_FLOAT) ? args[2].as.real :
                      (args[2].type == VAL_INT)   ? (double)args[2].as.integer : 0.0;
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        n->type = J2_FLOAT; n->v.real = fval;
        return j2_nil();
    }

    /* json2.set_bool(doc, path, b) → nil */
    if (!strcmp(fn_name, "set_bool")) {
        NEED(3); GET_DOC(0, doc); GET_PATH(1, path);
        if (args[2].type != VAL_BOOL) J2_ERR("expected bool");
        J2Node *n = j2_navigate(doc->root, path);
        if (!n) J2_ERR("path not found");
        n->type = J2_BOOL; n->v.boolean = args[2].as.boolean;
        return j2_nil();
    }

    /* json2.delete(doc, path) → nil */
    if (!strcmp(fn_name, "delete")) {
        NEED(2); GET_DOC(0, doc); GET_PATH(1, path);
        /* Find the parent and remove the child */
        /* Simple case: path ends with ".key" or "[idx]" */
        const char *last_dot = strrchr(path, '.');
        const char *last_brk = strrchr(path, '[');
        const char *split = NULL;
        int is_index = 0; int idx = 0;
        if (last_brk && (!last_dot || last_brk > last_dot)) {
            split = last_brk; is_index = 1;
            idx = atoi(last_brk + 1);
        } else if (last_dot) {
            split = last_dot;
        }
        if (!split) J2_ERR("cannot delete root node");
        char parent_path[512];
        int plen = (int)(split - path);
        if (plen >= 512) J2_ERR("path too long");
        memcpy(parent_path, path, (size_t)plen); parent_path[plen] = '\0';
        J2Node *parent = j2_navigate(doc->root, parent_path[0] ? parent_path : NULL);
        if (!parent) J2_ERR("parent path not found");
        if (is_index) {
            if (parent->type != J2_ARRAY || idx < 0 || idx >= parent->v.arr.count)
                J2_ERR("index out of range");
            j2_free_node(parent->v.arr.items[idx]);
            memmove(&parent->v.arr.items[idx], &parent->v.arr.items[idx+1],
                    (size_t)(parent->v.arr.count - idx - 1) * sizeof(J2Node*));
            parent->v.arr.count--;
        } else {
            const char *key = split + 1;
            if (parent->type != J2_OBJECT) J2_ERR("parent is not an object");
            for (int i = 0; i < parent->v.arr.count; i++) {
                if (parent->v.arr.items[i]->key && strcmp(parent->v.arr.items[i]->key, key) == 0) {
                    j2_free_node(parent->v.arr.items[i]);
                    memmove(&parent->v.arr.items[i], &parent->v.arr.items[i+1],
                            (size_t)(parent->v.arr.count - i - 1) * sizeof(J2Node*));
                    parent->v.arr.count--;
                    return j2_nil();
                }
            }
            J2_ERR("key not found");
        }
        return j2_nil();
    }

    /* json2.free(doc) → nil */
    if (!strcmp(fn_name, "free")) {
        NEED(1);
        if (args[0].type != VAL_DYN || !args[0].as.dyn ||
            args[0].as.dyn->count < 1 ||
            args[0].as.dyn->items[0].type != VAL_PTR) return j2_nil();
        Json2Doc *doc = (Json2Doc *)args[0].as.dyn->items[0].as.ptr;
        if (doc) { j2_free_node(doc->root); free(doc); }
        args[0].as.dyn->items[0].as.ptr = NULL;
        return j2_nil();
    }

#undef J2_ERR
#undef NEED
#undef GET_STR
#undef GET_DOC
#undef GET_PATH

    snprintf(errbuf, sizeof(errbuf), "json2.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "json2", line);
    *had_error = 1;
    return j2_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "json2",
    toml_key  = "std.json2",
    owner     = "json2",
    call      = fluxa_std_json2_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_JSON2_H */
