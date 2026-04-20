/* fluxa_std_sqlite.h — std.sqlite: embedded SQL via libsqlite3
 *
 * Requires: libsqlite3-dev
 * Embedded-friendly: SQLite runs on RP2040 (with filesystem) and ESP32.
 * Connection cursor follows the standard Fluxa VAL_PTR-in-dyn pattern.
 * User holds `prst dyn db` so the connection survives hot reloads.
 *
 * API:
 *   sqlite.open(str path)               → dyn  (db cursor)
 *   sqlite.close(dyn db)                → nil
 *   sqlite.exec(dyn db, str sql)        → nil  (DDL/DML, no result)
 *   sqlite.query(dyn db, str sql)       → dyn  (rows: dyn of dyn rows)
 *   sqlite.last_insert_id(dyn db)       → int
 *   sqlite.changes(dyn db)              → int  (rows affected by last DML)
 *   sqlite.version()                    → str
 */
#ifndef FLUXA_STD_SQLITE_H
#define FLUXA_STD_SQLITE_H

#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Value constructors ──────────────────────────────────────────────────── */
static inline Value sqlite_int(long n) {
    Value v; v.type = VAL_INT; v.as.integer = n; return v;
}
static inline Value sqlite_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}
static inline Value sqlite_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}
static inline Value sqlite_float(double d) {
    Value v; v.type = VAL_FLOAT; v.as.real = d; return v;
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */
static inline Value sqlite_wrap_cursor(sqlite3 *db) {
    FluxaDyn *d   = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap         = 1; d->count = 1;
    d->items       = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = db;
    Value ret; ret.type = VAL_DYN; ret.as.dyn = d;
    return ret;
}

static inline sqlite3 *sqlite_unwrap(const Value *v, ErrStack *err,
                                      int *had_error, int line,
                                      const char *fn_name) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn ||
        v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR ||
        !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "sqlite.%s: invalid db cursor — use sqlite.open() to create one",
            fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line);
        *had_error = 1;
        return NULL;
    }
    return (sqlite3 *)v->as.dyn->items[0].as.ptr;
}

/* ── Main dispatch ───────────────────────────────────────────────────────── */
static inline Value fluxa_std_sqlite_call(const char *fn_name,
                                           const Value *args, int argc,
                                           ErrStack *err, int *had_error,
                                           int line) {
    char errbuf[280];

#define LIB_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "sqlite.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line); \
    *had_error = 1; return sqlite_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "sqlite.%s: expected %d argument(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line); \
        *had_error = 1; return sqlite_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        LIB_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* ── sqlite.open(path) → dyn ───────────────────────────────────── */
    if (strcmp(fn_name, "open") == 0) {
        NEED(1); GET_STR(0, path);
        sqlite3 *db = NULL;
        int rc = sqlite3_open(path, &db);
        if (rc != SQLITE_OK) {
            snprintf(errbuf, sizeof(errbuf),
                "sqlite.open: cannot open '%s': %s", path, sqlite3_errmsg(db));
            if (db) sqlite3_close(db);
            errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line);
            *had_error = 1;
            return sqlite_nil();
        }
        return sqlite_wrap_cursor(db);
    }

    /* ── sqlite.close(db) → nil ────────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        NEED(1);
        sqlite3 *db = sqlite_unwrap(&args[0], err, had_error, line, fn_name);
        if (!db) return sqlite_nil();
        sqlite3_close(db);
        /* Zero the ptr so double-close is a no-op */
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return sqlite_nil();
    }

    /* ── sqlite.exec(db, sql) → nil ────────────────────────────────── */
    if (strcmp(fn_name, "exec") == 0) {
        NEED(2);
        sqlite3 *db = sqlite_unwrap(&args[0], err, had_error, line, fn_name);
        if (!db) return sqlite_nil();
        GET_STR(1, sql);
        char *errmsg = NULL;
        int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            snprintf(errbuf, sizeof(errbuf),
                "sqlite.exec: %s", errmsg ? errmsg : "unknown error");
            sqlite3_free(errmsg);
            errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line);
            *had_error = 1;
            return sqlite_nil();
        }
        return sqlite_nil();
    }

    /* ── sqlite.query(db, sql) → dyn (rows) ────────────────────────── */
    if (strcmp(fn_name, "query") == 0) {
        NEED(2);
        sqlite3 *db = sqlite_unwrap(&args[0], err, had_error, line, fn_name);
        if (!db) return sqlite_nil();
        GET_STR(1, sql);

        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            snprintf(errbuf, sizeof(errbuf),
                "sqlite.query: %s", sqlite3_errmsg(db));
            errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line);
            *had_error = 1;
            return sqlite_nil();
        }

        /* Build outer dyn (list of rows) */
        FluxaDyn *rows = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        rows->cap   = 8; rows->count = 0;
        rows->items = (Value *)malloc(sizeof(Value) * (size_t)rows->cap);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int ncols = sqlite3_column_count(stmt);
            FluxaDyn *row = (FluxaDyn *)malloc(sizeof(FluxaDyn));
            row->cap   = ncols > 0 ? ncols : 1;
            row->count = ncols;
            row->items = (Value *)malloc(sizeof(Value) * (size_t)row->cap);

            for (int c = 0; c < ncols; c++) {
                int ctype = sqlite3_column_type(stmt, c);
                if (ctype == SQLITE_INTEGER)
                    row->items[c] = sqlite_int(sqlite3_column_int64(stmt, c));
                else if (ctype == SQLITE_FLOAT)
                    row->items[c] = sqlite_float(sqlite3_column_double(stmt, c));
                else if (ctype == SQLITE_TEXT)
                    row->items[c] = sqlite_str(
                        (const char *)sqlite3_column_text(stmt, c));
                else if (ctype == SQLITE_NULL)
                    row->items[c] = sqlite_nil();
                else
                    row->items[c] = sqlite_str("[blob]");
            }

            /* Append row to rows dyn */
            if (rows->count >= rows->cap) {
                rows->cap *= 2;
                rows->items = (Value *)realloc(rows->items,
                    sizeof(Value) * (size_t)rows->cap);
            }
            Value rv; rv.type = VAL_DYN; rv.as.dyn = row;
            rows->items[rows->count++] = rv;
        }

        sqlite3_finalize(stmt);
        Value ret; ret.type = VAL_DYN; ret.as.dyn = rows;
        return ret;
    }

    /* ── sqlite.last_insert_id(db) → int ───────────────────────────── */
    if (strcmp(fn_name, "last_insert_id") == 0) {
        NEED(1);
        sqlite3 *db = sqlite_unwrap(&args[0], err, had_error, line, fn_name);
        if (!db) return sqlite_nil();
        return sqlite_int((long)sqlite3_last_insert_rowid(db));
    }

    /* ── sqlite.changes(db) → int ──────────────────────────────────── */
    if (strcmp(fn_name, "changes") == 0) {
        NEED(1);
        sqlite3 *db = sqlite_unwrap(&args[0], err, had_error, line, fn_name);
        if (!db) return sqlite_nil();
        return sqlite_int((long)sqlite3_changes(db));
    }

    /* ── sqlite.version() → str ────────────────────────────────────── */
    if (strcmp(fn_name, "version") == 0) {
        return sqlite_str(sqlite3_libversion());
    }

#undef LIB_ERR
#undef NEED
#undef GET_STR

    snprintf(errbuf, sizeof(errbuf), "sqlite.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "sqlite", line);
    *had_error = 1;
    return sqlite_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "sqlite",
    toml_key = "std.sqlite",
    owner    = "sqlite",
    call     = fluxa_std_sqlite_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_SQLITE_H */
