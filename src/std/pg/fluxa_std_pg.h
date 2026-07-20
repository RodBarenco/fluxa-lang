#ifndef FLUXA_STD_PG_H
#define FLUXA_STD_PG_H

/*
 * std.pg — PostgreSQL client for Fluxa-lang
 *
 * Design: opaque int handles. No dyn cursors exposed to Fluxa code.
 * Connections and results live in module-level tables sized by fluxa.toml.
 * All table access is protected by pg_mu — safe for std.flxthread.
 *
 * Return value convention:
 *   pg.connect / pg.query / pg.query_params → int handle > 0 on success, 0 on error.
 *   0 is never a valid handle. Callers must check inside danger {}.
 *
 * API:
 *   pg.connect(str connstr)                              → int  conn handle
 *   pg.close(int conn)                                   → nil
 *   pg.exec(int conn, str sql)                           → nil
 *   pg.query(int conn, str sql)                          → int  result handle
 *   pg.query_params(int conn, str sql, str arr p, int n) → int  result handle
 *   pg.rows(int result)                                  → int
 *   pg.cols(int result)                                  → int
 *   pg.col_name(int result, int col)                     → str
 *   pg.get(int result, int row, int col)                 → str
 *   pg.get_int(int result, int row, int col)             → int
 *   pg.get_float(int result, int row, int col)           → float
 *   pg.get_bool(int result, int row, int col)            → bool
 *   pg.is_null(int result, int row, int col)             → bool
 *   pg.free_result(int result)                           → nil
 *   pg.last_error(int conn)                              → str
 *   pg.ping(str connstr)                                 → bool
 *   pg.version(int conn)                                 → str  server version
 *   pg.version()                                         → str  libpq version
 *
 * Configuration (fluxa.toml):
 *   [libs.pg]
 *   max_connections = 16     # max simultaneous open connections  (hard cap 256)
 *   max_results     = 64     # max simultaneous live result sets  (hard cap 1024)
 *   max_cell_bytes  = 4096   # max bytes returned by pg.get       (hard cap 65536)
 *   max_param_bytes = 1024   # max bytes per query_params element (hard cap 65536)
 *   max_params      = 16     # max elements in query_params arr   (hard cap 64)
 *
 * Correct multi-thread pattern:
 *
 *   import std pg
 *   import std flxthread as ft
 *
 *   fn fetch(int conn, str sql) str {
 *       danger {
 *           int res = pg.query(conn, sql)
 *           str val = ""
 *           if pg.rows(res) > 0 { val = pg.get(res, 0, 0) }
 *           pg.free_result(res)
 *           return val
 *       }
 *       return ""
 *   }
 *
 *   Block Worker {
 *       prst int count = 0
 *       prst float sum = 0.0
 *       fn record(float v) nil {
 *           sum   = sum + v
 *           count = count + 1
 *       }
 *       fn avg() float { return sum / count }
 *   }
 *
 *   int c1 = 0
 *   int c2 = 0
 *   danger {
 *       c1 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
 *       c2 = pg.connect("host=localhost dbname=sensors user=fluxa password=s3cr3t")
 *   }
 *
 *   Block w1 typeof Worker
 *   Block w2 typeof Worker
 *   ft.new("t1", w1, "record")
 *   ft.new("t2", w2, "record")
 *
 *   int tick = 0
 *   while tick < 1000 {
 *       danger {
 *           str v1 = fetch(c1, "SELECT val FROM readings ORDER BY ts DESC LIMIT 1")
 *           str v2 = fetch(c2, "SELECT val FROM readings ORDER BY ts DESC LIMIT 1")
 *           ft.message("t1", "record", v1)
 *           ft.message("t2", "record", v2)
 *       }
 *       tick = tick + 1
 *   }
 *   ft.resolve_all()
 *   pg.close(c1)
 *   pg.close(c2)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Defaults and hard caps ──────────────────────────────────────── */
#define PG_DEFAULT_MAX_CONNS       16
#define PG_DEFAULT_MAX_RESULTS     64
#define PG_DEFAULT_MAX_CELL_BYTES  4096
#define PG_DEFAULT_MAX_PARAM_BYTES 1024
#define PG_DEFAULT_MAX_PARAMS      16

#define PG_HARD_MAX_CONNS       256
#define PG_HARD_MAX_RESULTS     1024
#define PG_HARD_MAX_CELL_BYTES  65536
#define PG_HARD_MAX_PARAM_BYTES 65536
#define PG_HARD_MAX_PARAMS      64

/* ── Value constructors ──────────────────────────────────────────── */
static inline Value pg_nil(void)          { Value v; v.type=VAL_NIL;            return v; }
static inline Value pg_int(long n)        { Value v; v.type=VAL_INT;   v.as.integer=n; return v; }
static inline Value pg_float(double d)    { Value v; v.type=VAL_FLOAT; v.as.real=d;    return v; }
static inline Value pg_bool(int b)        { Value v; v.type=VAL_BOOL;  v.as.boolean=b; return v; }
static inline Value pg_str(const char *s) {
    Value v; v.type=VAL_STRING;
    v.as.string = fxstr_new(s ? s : "");
    return v;
}

#ifdef FLUXA_PG_LIBPQ
/* ══════════════════════════════════════════════════════════════════
 * Real backend — libpq
 * ══════════════════════════════════════════════════════════════════ */

#include <libpq-fe.h>
#include <pthread.h>

/* ── libpq one-time initialization ────────────────────────────────
 * Called once via pthread_once before the first PQconnectdb. Without this,
 * libpq lazily initializes OpenSSL, libcrypto, and libkrb5 on the first
 * connection — and if multiple threads do that concurrently, the inits
 * race and leave libkrb5's internal mutexes corrupted. Symptoms: assertion
 * "k5_mutex_lock: Invalid argument" and glibc fortify "%n in writable
 * segment detected".
 *
 * PQinitOpenSSL(0, 0) tells libpq we (the application) are responsible for
 * OpenSSL/libcrypto setup. Since the SUT does not use SSL connections
 * (sslmode=disable in the connstr), this is safe and avoids the races. */
static void pg_libpq_init(void) {
    PQinitOpenSSL(0, 0);
}

/* ── Module-level state ──────────────────────────────────────────── */
static pthread_mutex_t pg_mu          = PTHREAD_MUTEX_INITIALIZER;
/* Serializes PQconnectdb calls. See pg.connect for rationale. */
static pthread_mutex_t pg_connect_mu  = PTHREAD_MUTEX_INITIALIZER;
static PGconn        **pg_conns       = NULL;
static PGresult      **pg_results     = NULL;
static int             pg_max_conns   = 0;
static int             pg_max_results = 0;
static int             pg_max_cell    = 0;
static int             pg_max_param   = 0;
static int             pg_max_params  = 0;
static int             pg_initialized = 0;

/* Called once, while holding pg_mu. Clamps all limits to hard caps. */
static void pg_ensure_init(int mc, int mr, int mce, int mpa, int mpn) {
    if (pg_initialized) return;

#define PG_CLAMP(v, def, hi) ((v) > 0 && (v) <= (hi) ? (v) : (def))
    mc  = PG_CLAMP(mc,  PG_DEFAULT_MAX_CONNS,       PG_HARD_MAX_CONNS);
    mr  = PG_CLAMP(mr,  PG_DEFAULT_MAX_RESULTS,     PG_HARD_MAX_RESULTS);
    mce = PG_CLAMP(mce, PG_DEFAULT_MAX_CELL_BYTES,  PG_HARD_MAX_CELL_BYTES);
    mpa = PG_CLAMP(mpa, PG_DEFAULT_MAX_PARAM_BYTES, PG_HARD_MAX_PARAM_BYTES);
    mpn = PG_CLAMP(mpn, PG_DEFAULT_MAX_PARAMS,      PG_HARD_MAX_PARAMS);
#undef PG_CLAMP

    pg_conns   = (PGconn   **)calloc((size_t)mc, sizeof(PGconn *));
    pg_results = (PGresult **)calloc((size_t)mr, sizeof(PGresult *));
    if (!pg_conns || !pg_results) { free(pg_conns); free(pg_results); return; }

    pg_max_conns   = mc;
    pg_max_results = mr;
    pg_max_cell    = mce;
    pg_max_param   = mpa;
    pg_max_params  = mpn;
    pg_initialized = 1;
}

/* 1-based slot allocators — call while holding pg_mu. Return 0 if full. */
static int pg_alloc_conn(PGconn *c) {
    for (int i = 0; i < pg_max_conns; i++)
        if (!pg_conns[i]) { pg_conns[i] = c; return i + 1; }
    return 0;
}
static int pg_alloc_result(PGresult *r) {
    for (int i = 0; i < pg_max_results; i++)
        if (!pg_results[i]) { pg_results[i] = r; return i + 1; }
    return 0;
}

/* Safe deref — returns NULL and pushes error on invalid handle. */
static PGconn *pg_get_conn(long h, ErrStack *e, int *he, int line, const char *fn) {
    char eb[280];
    pthread_mutex_lock(&pg_mu);
    int ok = pg_initialized && h >= 1 && h <= pg_max_conns && pg_conns[h-1];
    PGconn *c = ok ? pg_conns[h-1] : NULL;
    pthread_mutex_unlock(&pg_mu);
    if (!c) {
        snprintf(eb, sizeof(eb), "pg.%s: invalid or closed connection handle %ld", fn, h);
        errstack_push(e, ERR_FLUXA, eb, "pg", line); *he = 1;
    }
    return c;
}
static PGresult *pg_get_result(long h, ErrStack *e, int *he, int line, const char *fn) {
    char eb[280];
    pthread_mutex_lock(&pg_mu);
    int ok = pg_initialized && h >= 1 && h <= pg_max_results && pg_results[h-1];
    PGresult *r = ok ? pg_results[h-1] : NULL;
    pthread_mutex_unlock(&pg_mu);
    if (!r) {
        snprintf(eb, sizeof(eb), "pg.%s: invalid or freed result handle %ld", fn, h);
        errstack_push(e, ERR_FLUXA, eb, "pg", line); *he = 1;
    }
    return r;
}

/* Validate PGresult status. Clears and returns 0 on failure. */
static int pg_check(PGresult *res, PGconn *conn,
                     const char *fn, ErrStack *e, int *he, int line) {
    if (!res) {
        char eb[512];
        snprintf(eb, sizeof(eb), "pg.%s: null result (%.400s)",
                 fn, PQerrorMessage(conn));
        errstack_push(e, ERR_FLUXA, eb, "pg", line); *he = 1; return 0;
    }
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        char eb[512];
        const char *m = PQresultErrorMessage(res);
        snprintf(eb, sizeof(eb), "pg.%s: %.400s", fn, m ? m : "query failed");
        errstack_push(e, ERR_FLUXA, eb, "pg", line);
        PQclear(res); *he = 1; return 0;
    }
    return 1;
}

/* Row/col bounds check. */
static int pg_bounds(PGresult *res, int row, int col,
                      const char *fn, ErrStack *e, int *he, int line) {
    int nr = PQntuples(res), nc = PQnfields(res);
    if (row < 0 || row >= nr || col < 0 || col >= nc) {
        char eb[280];
        snprintf(eb, sizeof(eb),
                 "pg.%s: out of bounds (row=%d col=%d nrows=%d ncols=%d)",
                 fn, row, col, nr, nc);
        errstack_push(e, ERR_FLUXA, eb, "pg", line); *he = 1; return 0;
    }
    return 1;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_pg_call(const char *fn_name,
                                       const Value *args, int argc,
                                       ErrStack *err, int *had_error,
                                       int line,
                                       const FluxaConfig *cfg) {
    char errbuf[512];

    /* Lazy init under lock */
    pthread_mutex_lock(&pg_mu);
    if (!pg_initialized) {
        int mc  = (cfg && cfg->pg_max_conns   > 0) ? cfg->pg_max_conns   : PG_DEFAULT_MAX_CONNS;
        int mr  = (cfg && cfg->pg_max_results > 0) ? cfg->pg_max_results : PG_DEFAULT_MAX_RESULTS;
        int mce = (cfg && cfg->pg_max_cell    > 0) ? cfg->pg_max_cell    : PG_DEFAULT_MAX_CELL_BYTES;
        int mpa = (cfg && cfg->pg_max_param   > 0) ? cfg->pg_max_param   : PG_DEFAULT_MAX_PARAM_BYTES;
        int mpn = (cfg && cfg->pg_max_params  > 0) ? cfg->pg_max_params  : PG_DEFAULT_MAX_PARAMS;
        pg_ensure_init(mc, mr, mce, mpa, mpn);
    }
    pthread_mutex_unlock(&pg_mu);

    if (!pg_initialized || !pg_conns || !pg_results) {
        snprintf(errbuf, sizeof(errbuf),
                 "pg.%s: handle tables not initialized (out of memory)", fn_name);
        errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
        *had_error = 1; return pg_nil();
    }

#define PG_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "pg.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "pg", line); \
    *had_error = 1; return pg_nil(); } while(0)

#define NEED(n) do { if (argc < (n)) { \
    snprintf(errbuf, sizeof(errbuf), "pg.%s: need %d arg(s), got %d", fn_name, (n), argc); \
    errstack_push(err, ERR_FLUXA, errbuf, "pg", line); \
    *had_error = 1; return pg_nil(); } } while(0)

    /* Safe typed accessors — all bounds-checked before use */
#define REQ_STR(idx, var) \
    if ((idx) >= argc || args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        PG_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define REQ_INT(idx, var) \
    if ((idx) >= argc || args[(idx)].type != VAL_INT) PG_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

    /* ── pg.connect(connstr) → int ──────────────────────────────── */
    if (!strcmp(fn_name, "connect")) {
        NEED(1); REQ_STR(0, connstr);
        if (strlen(connstr) > 1024) PG_ERR("connection string too long (max 1024 bytes)");

        /* Thread-safe libpq initialization. libpq lazily inits OpenSSL,
         * libcrypto, libgssapi, and libkrb5 on the first PQconnectdb. None
         * of those inits are individually thread-safe, and PQconnectdb makes
         * no attempt to serialize them. With many threads doing concurrent
         * connect attempts at startup (24 workers × retry-every-second), the
         * inits race and corrupt libkrb5's internal mutex state.
         *
         * Symptoms without this:
         *   "k5_mutex_lock: Received error 22 (Invalid argument)"
         *   "%n in writable segment detected" (glibc fortify)
         *
         * Fix: PQinitOpenSSL(0, 0) once (we don't use SSL), then serialize
         * ALL PQconnectdb calls with pg_connect_mu. After the first successful
         * connect, all lazy init paths are warm and subsequent connects are
         * safe — but serializing them costs almost nothing (a few ms each)
         * since it only matters at startup. */
        static pthread_once_t pg_init_once = PTHREAD_ONCE_INIT;
        pthread_once(&pg_init_once, pg_libpq_init);

        pthread_mutex_lock(&pg_connect_mu);
        PGconn *conn = PQconnectdb(connstr);
        pthread_mutex_unlock(&pg_connect_mu);
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            char msg[512];
            snprintf(msg, sizeof(msg), "pg.connect: %.400s",
                     conn ? PQerrorMessage(conn) : "out of memory");
            if (conn) PQfinish(conn);
            errstack_push(err, ERR_FLUXA, msg, "pg", line);
            *had_error = 1; return pg_int(0);
        }
        pthread_mutex_lock(&pg_mu);
        int slot = pg_alloc_conn(conn);
        pthread_mutex_unlock(&pg_mu);
        if (!slot) { PQfinish(conn); PG_ERR("connection table full — increase [libs.pg] max_connections"); }
        return pg_int(slot);
    }

    /* ── pg.close(conn) → nil ───────────────────────────────────── */
    if (!strcmp(fn_name, "close")) {
        NEED(1); REQ_INT(0, h);
        if (h < 1 || h > pg_max_conns) return pg_nil();
        pthread_mutex_lock(&pg_mu);
        PGconn *c = pg_conns[h-1]; pg_conns[h-1] = NULL;
        pthread_mutex_unlock(&pg_mu);
        if (c) PQfinish(c);
        return pg_nil();
    }

    /* ── pg.exec(conn, sql) → nil ───────────────────────────────── */
    if (!strcmp(fn_name, "exec")) {
        NEED(2); REQ_INT(0, h); REQ_STR(1, sql);
        PGconn *c = pg_get_conn(h, err, had_error, line, fn_name);
        if (!c) return pg_nil();
        PGresult *res = PQexec(c, sql);
        if (!pg_check(res, c, fn_name, err, had_error, line)) return pg_nil();
        PQclear(res);
        return pg_nil();
    }

    /* ── pg.query(conn, sql) → int ──────────────────────────────── */
    if (!strcmp(fn_name, "query")) {
        NEED(2); REQ_INT(0, h); REQ_STR(1, sql);
        PGconn *c = pg_get_conn(h, err, had_error, line, fn_name);
        if (!c) return pg_int(0);
        PGresult *res = PQexec(c, sql);
        if (!pg_check(res, c, fn_name, err, had_error, line)) return pg_int(0);
        pthread_mutex_lock(&pg_mu);
        int slot = pg_alloc_result(res);
        pthread_mutex_unlock(&pg_mu);
        if (!slot) { PQclear(res); PG_ERR("result table full — increase [libs.pg] max_results"); }
        return pg_int(slot);
    }

    /* ── pg.query_params(conn, sql, str arr params, int n) → int ── */
    if (!strcmp(fn_name, "query_params")) {
        NEED(4); REQ_INT(0, h); REQ_STR(1, sql);
        PGconn *c = pg_get_conn(h, err, had_error, line, fn_name);
        if (!c) return pg_int(0);

        if (args[2].type != VAL_ARR || !args[2].as.arr.data)
            PG_ERR("query_params: arg 3 must be str arr");
        if (args[3].type != VAL_INT)
            PG_ERR("query_params: arg 4 must be int (param count)");

        long n = args[3].as.integer;
        if (n < 0) PG_ERR("query_params: param count cannot be negative");
        if (n > pg_max_params) {
            snprintf(errbuf, sizeof(errbuf),
                     "pg.query_params: count %ld > max_params %d"
                     " — increase [libs.pg] max_params", n, pg_max_params);
            errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
            *had_error = 1; return pg_int(0);
        }
        if (n > args[2].as.arr.size) {
            snprintf(errbuf, sizeof(errbuf),
                     "pg.query_params: count %ld > arr size %d", n, args[2].as.arr.size);
            errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
            *had_error = 1; return pg_int(0);
        }

        /* Build param array — validate every element before touching libpq */
        const char **pv = NULL;
        if (n > 0) {
            pv = (const char **)malloc((size_t)n * sizeof(char *));
            if (!pv) PG_ERR("query_params: out of memory");
            for (long i = 0; i < n; i++) {
                Value *el = &args[2].as.arr.data[i];
                if (el->type != VAL_STRING || !el->as.string) {
                    free(pv); PG_ERR("query_params: every arr element must be str");
                }
                if ((int)strlen(el->as.string) > pg_max_param) {
                    free(pv);
                    snprintf(errbuf, sizeof(errbuf),
                             "pg.query_params: param[%ld] exceeds max_param_bytes %d", i, pg_max_param);
                    errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
                    *had_error = 1; return pg_int(0);
                }
                pv[i] = el->as.string;
            }
        }

        PGresult *res = PQexecParams(c, sql, (int)n, NULL, pv, NULL, NULL, 0);
        free(pv);
        if (!pg_check(res, c, fn_name, err, had_error, line)) return pg_int(0);

        pthread_mutex_lock(&pg_mu);
        int slot = pg_alloc_result(res);
        pthread_mutex_unlock(&pg_mu);
        if (!slot) { PQclear(res); PG_ERR("result table full — increase [libs.pg] max_results"); }
        return pg_int(slot);
    }

    /* ── pg.rows(result) → int ──────────────────────────────────── */
    if (!strcmp(fn_name, "rows")) {
        NEED(1); REQ_INT(0, h);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        return pg_int(PQntuples(r));
    }

    /* ── pg.cols(result) → int ──────────────────────────────────── */
    if (!strcmp(fn_name, "cols")) {
        NEED(1); REQ_INT(0, h);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        return pg_int(PQnfields(r));
    }

    /* ── pg.col_name(result, col) → str ─────────────────────────── */
    if (!strcmp(fn_name, "col_name")) {
        NEED(2); REQ_INT(0, h); REQ_INT(1, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (col < 0 || col >= PQnfields(r)) PG_ERR("col_name: column index out of bounds");
        const char *name = PQfname(r, (int)col);
        return pg_str(name ? name : "");
    }

    /* ── pg.get(result, row, col) → str ─────────────────────────── */
    if (!strcmp(fn_name, "get")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, row); REQ_INT(2, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (!pg_bounds(r, (int)row, (int)col, fn_name, err, had_error, line)) return pg_nil();
        if (PQgetisnull(r, (int)row, (int)col)) return pg_str("");
        const char *raw = PQgetvalue(r, (int)row, (int)col);
        if (!raw) return pg_str("");
        if ((int)strlen(raw) > pg_max_cell) {
            snprintf(errbuf, sizeof(errbuf),
                     "pg.get: cell exceeds max_cell_bytes %d"
                     " — increase [libs.pg] max_cell_bytes", pg_max_cell);
            errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
            *had_error = 1; return pg_nil();
        }
        return pg_str(raw);
    }

    /* ── pg.get_int(result, row, col) → int ─────────────────────── */
    if (!strcmp(fn_name, "get_int")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, row); REQ_INT(2, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (!pg_bounds(r, (int)row, (int)col, fn_name, err, had_error, line)) return pg_nil();
        if (PQgetisnull(r, (int)row, (int)col)) return pg_int(0);
        const char *v = PQgetvalue(r, (int)row, (int)col);
        return pg_int(v ? atol(v) : 0);
    }

    /* ── pg.get_float(result, row, col) → float ─────────────────── */
    if (!strcmp(fn_name, "get_float")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, row); REQ_INT(2, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (!pg_bounds(r, (int)row, (int)col, fn_name, err, had_error, line)) return pg_nil();
        if (PQgetisnull(r, (int)row, (int)col)) return pg_float(0.0);
        const char *v = PQgetvalue(r, (int)row, (int)col);
        return pg_float(v ? atof(v) : 0.0);
    }

    /* ── pg.get_bool(result, row, col) → bool ───────────────────── */
    if (!strcmp(fn_name, "get_bool")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, row); REQ_INT(2, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (!pg_bounds(r, (int)row, (int)col, fn_name, err, had_error, line)) return pg_nil();
        if (PQgetisnull(r, (int)row, (int)col)) return pg_bool(0);
        const char *v = PQgetvalue(r, (int)row, (int)col);
        return pg_bool(v && (v[0]=='t' || v[0]=='T' || v[0]=='1'));
    }

    /* ── pg.is_null(result, row, col) → bool ────────────────────── */
    if (!strcmp(fn_name, "is_null")) {
        NEED(3); REQ_INT(0, h); REQ_INT(1, row); REQ_INT(2, col);
        PGresult *r = pg_get_result(h, err, had_error, line, fn_name);
        if (!r) return pg_nil();
        if (!pg_bounds(r, (int)row, (int)col, fn_name, err, had_error, line)) return pg_nil();
        return pg_bool(PQgetisnull(r, (int)row, (int)col));
    }

    /* ── pg.free_result(result) → nil ───────────────────────────── */
    if (!strcmp(fn_name, "free_result")) {
        NEED(1); REQ_INT(0, h);
        /* Silent on bad handle — double-free is a no-op */
        if (h < 1 || h > pg_max_results) return pg_nil();
        pthread_mutex_lock(&pg_mu);
        PGresult *r = pg_results[h-1]; pg_results[h-1] = NULL;
        pthread_mutex_unlock(&pg_mu);
        if (r) PQclear(r);
        return pg_nil();
    }

    /* ── pg.last_error(conn) → str ──────────────────────────────── */
    if (!strcmp(fn_name, "last_error")) {
        NEED(1); REQ_INT(0, h);
        PGconn *c = pg_get_conn(h, err, had_error, line, fn_name);
        if (!c) return pg_nil();
        const char *msg = PQerrorMessage(c);
        char buf[4096];
        /* Clamp to max_cell to avoid returning unbounded data */
        int lim = (pg_max_cell > 0 && pg_max_cell < 4096) ? pg_max_cell : 4095;
        snprintf(buf, sizeof(buf), "%.*s", lim, msg ? msg : "");
        return pg_str(buf);
    }

    /* ── pg.ping(connstr) → bool ────────────────────────────────── */
    if (!strcmp(fn_name, "ping")) {
        NEED(1); REQ_STR(0, connstr);
        if (strlen(connstr) > 1024) PG_ERR("connection string too long (max 1024 bytes)");
        return pg_bool(PQping(connstr) == PQPING_OK);
    }

    /* ── pg.version(conn?) → str ────────────────────────────────── */
    if (!strcmp(fn_name, "version")) {
        if (argc >= 1 && args[0].type == VAL_INT && args[0].as.integer > 0) {
            PGconn *c = pg_get_conn(args[0].as.integer, err, had_error, line, fn_name);
            if (!c) return pg_nil();
            int ver = PQserverVersion(c);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d.%d.%d",
                     ver/10000, (ver%10000)/100, ver%100);
            return pg_str(buf);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "libpq/%d", PQlibVersion());
        return pg_str(buf);
    }

#undef PG_ERR
#undef NEED
#undef REQ_STR
#undef REQ_INT

    snprintf(errbuf, sizeof(errbuf), "pg.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
    *had_error = 1;
    return pg_nil();
}

#else /* !FLUXA_PG_LIBPQ — stub backend */
/* ══════════════════════════════════════════════════════════════════
 * Stub backend
 * ══════════════════════════════════════════════════════════════════ */
static inline Value fluxa_std_pg_call(const char *fn_name,
                                       const Value *args, int argc,
                                       ErrStack *err, int *had_error,
                                       int line,
                                       const FluxaConfig *cfg) {
    char errbuf[280];
    (void)args; (void)argc; (void)cfg;
    fprintf(stderr, "[fluxa] std.pg: stub backend — "
            "install libpq-dev and rebuild with make build\n");
    snprintf(errbuf, sizeof(errbuf),
             "pg.%s: backend not available (libpq not found at build time)", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "pg", line);
    *had_error = 1;
    Value v; v.type = VAL_NIL; return v;
}
#endif /* FLUXA_PG_LIBPQ */

FLUXA_LIB_EXPORT(
    name      = "pg",
    toml_key  = "std.pg",
    owner     = "pg",
    call      = fluxa_std_pg_call,
    rt_aware  = 0,
    cfg_aware = 1
)

#endif /* FLUXA_STD_PG_H */
