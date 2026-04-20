/* fluxa_std_csv.h — Fluxa Standard Library: csv
 *
 * Compiled into the binary only when FLUXA_STD_CSV is defined.
 * Declared in [libs] of fluxa.toml to enable at runtime.
 *
 * All file I/O must be called inside danger {} — consistent with Fluxa's
 * risk model. Errors go to err_stack inside danger, abort outside.
 *
 * THREE USAGE MODES:
 *
 *   Mode A — cursor (recommended for large files):
 *     prst dyn cur = csv.open("data.csv")
 *     dyn chunk    = csv.next(cur, 1000)
 *     while len(chunk) > 0 {
 *         for row in chunk { str f = csv.field(row, 0) }
 *         chunk = csv.next(cur, 1000)
 *     }
 *     csv.close(cur)
 *
 *   Mode B — chunk direct (simple, small files, reopens each call):
 *     dyn chunk = csv.chunk("data.csv", 1000)
 *
 *   Mode C — load all (files that fit in memory):
 *     dyn all = csv.load("data.csv")
 *
 * RETURN FORMAT:
 *   All functions return dyn of str — each element is one raw CSV line.
 *   Use csv.field(row, idx) to extract individual fields.
 *   Lines are returned as-is (no unquoting in v1.0).
 *
 * MEMORY SAFETY:
 *   Buffer sizes are configured in fluxa.toml:
 *     [libs.csv]
 *     max_line_bytes = 1024   # max bytes per line (default 1024)
 *     max_fields     = 64     # max fields per line for csv.field (default 64)
 *   Lines exceeding max_line_bytes are truncated — error reported.
 *
 * CURSOR LIFETIME:
 *   The cursor is a VAL_PTR wrapping a heap-allocated CsvCursor struct
 *   (which holds the FILE* and position). It survives hot reload in
 *   HANDOVER_MODE_MEMORY. In HANDOVER_MODE_FLASH (RP2040 reboot) the
 *   FILE* is invalid after restart — close and reopen.
 *   Always call csv.close(cur) or free(cur) when done.
 */
#ifndef FLUXA_STD_CSV_H
#define FLUXA_STD_CSV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Limits (overridable at compile time) ────────────────────────────────── */
#ifndef FLUXA_CSV_MAX_LINE
#define FLUXA_CSV_MAX_LINE   1024
#endif
#ifndef FLUXA_CSV_MAX_FIELDS
#define FLUXA_CSV_MAX_FIELDS 64
#endif
#ifndef FLUXA_CSV_DELIM
#define FLUXA_CSV_DELIM      ','
#endif

/* ── Cursor ──────────────────────────────────────────────────────────────── */
typedef struct {
    FILE *fp;
    char  path[512];
    long  byte_offset;   /* position after last csv.next() call */
    int   eof;
    char  delim;         /* field delimiter (default ',') */
} CsvCursor;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Build a val_string Value — strdup, owned by caller */
static inline Value csv_str_val(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}

static inline Value csv_nil(void) { Value v; v.type = VAL_NIL; return v; }

static inline Value csv_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}

/* Extract a VAL_PTR (CsvCursor*) from a dyn value.
 * The cursor is stored as the first element of a 1-element dyn. */
static inline CsvCursor *csv_cursor_from_val(const Value *v,
                                              ErrStack *err, int line) {
    if (!v || v->type != VAL_DYN || !v->as.dyn || v->as.dyn->count < 1) {
        errstack_push(err, ERR_FLUXA,
            "csv: invalid cursor — use csv.open() to create one",
            "csv", line);
        return NULL;
    }
    Value *slot = &v->as.dyn->items[0];
    if (slot->type != VAL_PTR || !slot->as.ptr) {
        errstack_push(err, ERR_FLUXA,
            "csv: cursor is closed or invalid",
            "csv", line);
        return NULL;
    }
    return (CsvCursor *)slot->as.ptr;
}

/* Read up to chunk_size lines from fp into a dyn. Returns dyn. */
static inline FluxaDyn *csv_read_chunk(FILE *fp, int chunk_size,
                                        ErrStack *err, int *had_error,
                                        int line) {
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    if (!d) {
        errstack_push(err, ERR_FLUXA, "csv: out of memory", "csv", line);
        *had_error = 1; return NULL;
    }
    d->cap   = chunk_size > 0 ? chunk_size : 64;
    d->count = 0;
    d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
    if (!d->items) {
        free(d);
        errstack_push(err, ERR_FLUXA, "csv: out of memory", "csv", line);
        *had_error = 1; return NULL;
    }

    char linebuf[FLUXA_CSV_MAX_LINE];
    int  lines_read = 0;

    while (lines_read < chunk_size && fgets(linebuf, sizeof(linebuf), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
            linebuf[--len] = '\0';

        /* Grow dyn if needed */
        if (d->count >= d->cap) {
            int new_cap = d->cap * 2;
            Value *nb = (Value *)realloc(d->items,
                                         sizeof(Value) * (size_t)new_cap);
            if (!nb) {
                errstack_push(err, ERR_FLUXA,
                    "csv: out of memory growing chunk", "csv", line);
                break;
            }
            d->items = nb;
            d->cap   = new_cap;
        }

        d->items[d->count++] = csv_str_val(linebuf);
        lines_read++;
    }

    return d;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static inline Value fluxa_std_csv_call(const char *fn_name,
                                        const Value *args, int argc,
                                        ErrStack *err, int *had_error,
                                        int line) {
    char errbuf[1024];

#define CSV_ERR(msg) do { \
    /* Two-step: build prefix separately to avoid restrict/truncation warnings */ \
    char _m[1024]; \
    strncpy(_m, msg, sizeof(_m)-1); _m[sizeof(_m)-1] = '\0'; \
    snprintf(errbuf, sizeof(errbuf), "csv.%s (line %d): %.900s", \
             fn_name, line, _m); \
    errstack_push(err, ERR_FLUXA, errbuf, "csv", line); \
    *had_error = 1; return csv_nil(); \
} while(0)

#define REQUIRE_ARGC_MIN(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "csv.%s expects at least %d argument(s), got %d", \
            fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "csv", line); \
        *had_error = 1; return csv_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        CSV_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) CSV_ERR("expected int argument"); \
    int (var) = (int)args[(idx)].as.integer;

    /* ── csv.open(str path) → dyn cursor ────────────────────────────────── */
    if (strcmp(fn_name, "open") == 0) {
        REQUIRE_ARGC_MIN(1);
        GET_STR(0, path);

        FILE *fp = fopen(path, "r");
        if (!fp) {
            snprintf(errbuf, sizeof(errbuf),
                "csv.open: cannot open file '%s'", path);
            CSV_ERR(errbuf);
        }

        CsvCursor *cur = (CsvCursor *)malloc(sizeof(CsvCursor));
        if (!cur) { fclose(fp); CSV_ERR("csv.open: out of memory"); }
        cur->fp          = fp;
        cur->byte_offset = 0;
        cur->eof         = 0;
        cur->delim       = FLUXA_CSV_DELIM;
        /* Optional 2nd arg: delimiter string (e.g. "\t" for TSV, ";" for European CSV) */
        if (argc >= 2 && args[1].type == VAL_STRING &&
            args[1].as.string && args[1].as.string[0])
            cur->delim = args[1].as.string[0];
        strncpy(cur->path, path, sizeof(cur->path)-1);
        cur->path[sizeof(cur->path)-1] = '\0';

        /* Wrap cursor in a 1-element dyn as VAL_PTR */
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) { free(cur); fclose(fp); CSV_ERR("out of memory"); }
        d->cap = 1; d->count = 1;
        d->items = (Value *)malloc(sizeof(Value));
        if (!d->items) { free(d); free(cur); fclose(fp); CSV_ERR("out of memory"); }
        d->items[0].type    = VAL_PTR;
        d->items[0].as.ptr  = cur;

        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

    /* ── csv.next(dyn cursor, int chunk_size) → dyn ─────────────────────── */
    if (strcmp(fn_name, "next") == 0) {
        REQUIRE_ARGC_MIN(2);
        GET_INT(1, chunk_size);
        if (chunk_size <= 0) CSV_ERR("chunk_size must be > 0");

        CsvCursor *cur = csv_cursor_from_val(&args[0], err, line);
        if (!cur) { *had_error = 1; return csv_nil(); }

        if (cur->eof) {
            /* Return empty dyn — signals EOF to the caller */
            FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
            if (!d) CSV_ERR("out of memory");
            d->cap = 0; d->count = 0; d->items = NULL;
            Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
            return ret;
        }

        FluxaDyn *chunk = csv_read_chunk(cur->fp, chunk_size,
                                          err, had_error, line);
        if (!chunk) return csv_nil();

        if (chunk->count < chunk_size) cur->eof = 1;
        cur->byte_offset = ftell(cur->fp);

        Value ret; ret.type = VAL_DYN; ret.as.dyn = chunk;
        return ret;
    }

    /* ── csv.close(dyn cursor) → nil ────────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        REQUIRE_ARGC_MIN(1);
        CsvCursor *cur = csv_cursor_from_val(&args[0], err, line);
        if (!cur) return csv_nil();  /* already closed — not an error */
        if (cur->fp) { fclose(cur->fp); cur->fp = NULL; }
        free(cur);
        /* Null the pointer in the dyn slot so double-close is safe */
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return csv_nil();
    }

    /* ── csv.chunk(str path, int chunk_size) → dyn ───────────────────────── */
    /* Simple mode: reopen file, skip to byte_offset, read chunk_size lines.
     * For small files this is fine. For large files prefer csv.open/next. */
    if (strcmp(fn_name, "chunk") == 0) {
        REQUIRE_ARGC_MIN(2);
        GET_STR(0, path);
        GET_INT(1, chunk_size);
        if (chunk_size <= 0) CSV_ERR("chunk_size must be > 0");

        /* Optional 3rd arg: byte offset (default 0) */
        long offset = 0;
        if (argc >= 3 && args[2].type == VAL_INT)
            offset = (long)args[2].as.integer;

        FILE *fp = fopen(path, "r");
        if (!fp) { CSV_ERR("cannot open file for chunk (check path)"); }
        if (offset > 0) fseek(fp, offset, SEEK_SET);

        FluxaDyn *chunk = csv_read_chunk(fp, chunk_size, err, had_error, line);
        fclose(fp);
        if (!chunk) return csv_nil();

        Value ret; ret.type = VAL_DYN; ret.as.dyn = chunk;
        return ret;
    }

    /* ── csv.load(str path) → dyn ───────────────────────────────────────── */
    if (strcmp(fn_name, "load") == 0) {
        REQUIRE_ARGC_MIN(1);
        GET_STR(0, path);

        FILE *fp = fopen(path, "r");
        if (!fp) { CSV_ERR("cannot open file for load (check path)"); }

        /* Read entire file — use a large chunk_size */
        FluxaDyn *all = csv_read_chunk(fp, 1<<30, err, had_error, line);
        fclose(fp);
        if (!all) return csv_nil();

        Value ret; ret.type = VAL_DYN; ret.as.dyn = all;
        return ret;
    }

    /* ── csv.save(dyn data, str path) → nil ─────────────────────────────── */
    if (strcmp(fn_name, "save") == 0) {
        REQUIRE_ARGC_MIN(2);
        if (args[0].type != VAL_DYN || !args[0].as.dyn)
            CSV_ERR("first argument must be dyn");
        GET_STR(1, path);

        FILE *fp = fopen(path, "w");
        if (!fp) { CSV_ERR("cannot open file for writing (check path and permissions)"); }

        FluxaDyn *d = args[0].as.dyn;
        for (int i = 0; i < d->count; i++) {
            if (d->items[i].type == VAL_STRING && d->items[i].as.string)
                fprintf(fp, "%s\n", d->items[i].as.string);
        }
        fclose(fp);
        return csv_nil();
    }

    /* ── FSM-based field parser ──────────────────────────────────────────── */
    /* States: FIELD (normal), QUOTED (inside double-quotes), POST_QUOTE      */
    /* Handles: embedded delimiters in quotes, escaped quotes (""), newlines  */

    /* ── csv.field(str row, int idx [, str delim]) → str ────────────────── */
    if (strcmp(fn_name, "field") == 0) {
        REQUIRE_ARGC_MIN(2);
        GET_STR(0, row);
        GET_INT(1, idx);
        if (idx < 0) CSV_ERR("field index must be >= 0");

        char delim = FLUXA_CSV_DELIM;
        if (argc >= 3 && args[2].type == VAL_STRING &&
            args[2].as.string && args[2].as.string[0])
            delim = args[2].as.string[0];

        char field_buf[FLUXA_CSV_MAX_LINE];
        int  field_pos   = 0;
        int  field_idx   = 0;
        int  in_quotes   = 0;
        const char *p    = row;

        for (; ; p++) {
            char c = *p;
            if (in_quotes) {
                if (c == '"') {
                    if (*(p+1) == '"') { /* escaped quote "" */
                        if (field_idx == idx && field_pos < FLUXA_CSV_MAX_LINE-1)
                            field_buf[field_pos++] = '"';
                        p++;
                    } else {
                        in_quotes = 0;
                    }
                } else if (c == '\0') {
                    break; /* unterminated quote — treat as end */
                } else {
                    if (field_idx == idx && field_pos < FLUXA_CSV_MAX_LINE-1)
                        field_buf[field_pos++] = c;
                }
            } else {
                if (c == '"' && field_pos == 0) {
                    in_quotes = 1; /* opening quote at field start */
                } else if (c == delim || c == '\0') {
                    if (field_idx == idx) {
                        field_buf[field_pos] = '\0';
                        return csv_str_val(field_buf);
                    }
                    field_idx++;
                    field_pos = 0;
                    if (c == '\0') break;
                } else {
                    if (field_idx == idx && field_pos < FLUXA_CSV_MAX_LINE-1)
                        field_buf[field_pos++] = c;
                }
            }
        }

        CSV_ERR("field index out of range");
    }

    /* ── csv.field_count(str row [, str delim]) → int ───────────────────── */
    if (strcmp(fn_name, "field_count") == 0) {
        REQUIRE_ARGC_MIN(1);
        GET_STR(0, row);
        char delim = FLUXA_CSV_DELIM;
        if (argc >= 2 && args[1].type == VAL_STRING &&
            args[1].as.string && args[1].as.string[0])
            delim = args[1].as.string[0];

        int count    = 1;
        int in_quote = 0;
        for (const char *p = row; *p; p++) {
            if (*p == '"') { in_quote = !in_quote; continue; }
            if (!in_quote && *p == delim) count++;
        }
        return csv_int((long)count);
    }

    /* ── csv.has_header — skip first line of a dyn ──────────────────────── */
    /* csv.skip(dyn chunk, int n) → dyn (returns chunk without first n rows) */
    if (strcmp(fn_name, "skip") == 0) {
        REQUIRE_ARGC_MIN(2);
        if (args[0].type != VAL_DYN || !args[0].as.dyn)
            CSV_ERR("first argument must be dyn");
        GET_INT(1, n);
        if (n <= 0) return args[0];

        FluxaDyn *src = args[0].as.dyn;
        int start_idx = n < src->count ? n : src->count;
        int new_count = src->count - start_idx;

        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) CSV_ERR("out of memory");
        d->count = new_count;
        d->cap   = new_count > 0 ? new_count : 1;
        d->items = (Value *)malloc(sizeof(Value) * (size_t)d->cap);
        if (!d->items) { free(d); CSV_ERR("out of memory"); }

        for (int i = 0; i < new_count; i++) {
            Value *s = &src->items[start_idx + i];
            d->items[i] = csv_str_val(
                s->type == VAL_STRING ? s->as.string : "");
        }

        Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
        return ret;
    }

    /* ── csv.is_eof(dyn cursor) → bool ──────────────────────────────────── */
    if (strcmp(fn_name, "is_eof") == 0) {
        REQUIRE_ARGC_MIN(1);
        CsvCursor *cur = csv_cursor_from_val(&args[0], err, line);
        Value v; v.type = VAL_BOOL;
        v.as.boolean = (!cur || cur->eof) ? 1 : 0;
        return v;
    }

#undef CSV_ERR
#undef REQUIRE_ARGC_MIN
#undef GET_STR
#undef GET_INT

    snprintf(errbuf, sizeof(errbuf),
        "csv.%s: unknown function — available: "
        "open, next, close, chunk, load, save, field, field_count, skip, is_eof. "
        "Make sure 'std.csv = \"1.0\"' is declared under [libs] in fluxa.toml.",
        fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "csv", line);
    *had_error = 1;
    return csv_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "csv",
    toml_key = "std.csv",
    owner    = "csv",
    call     = fluxa_std_csv_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_CSV_H */
