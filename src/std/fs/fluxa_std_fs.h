#ifndef FLUXA_STD_FS_H
#define FLUXA_STD_FS_H

/*
 * std.fs — Filesystem operations for Fluxa-lang
 *
 * POSIX file and directory operations beyond CSV/JSON.
 * Designed for IoT: SD card, tmpfs, embedded Linux filesystems.
 * All paths relative to project directory or absolute.
 *
 * API:
 *   fs.read(path)              → str  (entire file contents)
 *   fs.write(path, data)       → int  (bytes written)
 *   fs.append(path, data)      → int  (bytes appended)
 *   fs.exists(path)            → bool
 *   fs.delete(path)            → bool (true if deleted)
 *   fs.rename(src, dst)        → bool
 *   fs.copy(src, dst)          → bool
 *   fs.size(path)              → int  (bytes, -1 if not found)
 *   fs.mkdir(path)             → bool (creates including parents)
 *   fs.rmdir(path)             → bool (removes empty dir)
 *   fs.listdir(path)           → dyn  (list of filenames, no path prefix)
 *   fs.isdir(path)             → bool
 *   fs.isfile(path)            → bool
 *   fs.join(a, b)              → str  (path join: "a/b")
 *   fs.basename(path)          → str  ("dir/file.txt" → "file.txt")
 *   fs.dirname(path)           → str  ("dir/file.txt" → "dir")
 *   fs.ext(path)               → str  ("file.txt" → ".txt", "" if none)
 *   fs.tempfile()              → str  (path to a new temp file)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value fs_nil(void)     { Value v; v.type = VAL_NIL;    return v; }
static inline Value fs_int(long n)   { Value v; v.type = VAL_INT;    v.as.integer = n; return v; }
static inline Value fs_bool(int b)   { Value v; v.type = VAL_BOOL;   v.as.boolean = b; return v; }
static inline Value fs_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_fs_call(const char *fn_name,
                                       const Value *args, int argc,
                                       ErrStack *err, int *had_error,
                                       int line) {
    char errbuf[280];

#define FS_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "fs.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "fs", line); \
    *had_error = 1; return fs_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "fs.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "fs", line); \
        *had_error = 1; return fs_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        FS_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* fs.read(path) → str */
    if (!strcmp(fn_name, "read")) {
        NEED(1); GET_STR(0, path);
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(errbuf, sizeof(errbuf), "fs.read: cannot open '%s': %s",
                     path, strerror(errno));
            errstack_push(err, ERR_FLUXA, errbuf, "fs", line);
            *had_error = 1; return fs_nil();
        }
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        char *buf = (char *)malloc((size_t)sz + 1);
        size_t nr = fread(buf, 1, (size_t)sz, f); buf[nr] = '\0';
        fclose(f);
        Value v; v.type = VAL_STRING; v.as.string = buf;
        return v;
    }

    /* fs.write(path, data) → int (bytes written) */
    if (!strcmp(fn_name, "write")) {
        NEED(2); GET_STR(0, path); GET_STR(1, data);
        FILE *f = fopen(path, "wb");
        if (!f) {
            snprintf(errbuf, sizeof(errbuf), "fs.write: cannot open '%s': %s",
                     path, strerror(errno));
            errstack_push(err, ERR_FLUXA, errbuf, "fs", line);
            *had_error = 1; return fs_nil();
        }
        size_t n = fwrite(data, 1, strlen(data), f);
        fclose(f); return fs_int((long)n);
    }

    /* fs.append(path, data) → int (bytes appended) */
    if (!strcmp(fn_name, "append")) {
        NEED(2); GET_STR(0, path); GET_STR(1, data);
        FILE *f = fopen(path, "ab");
        if (!f) FS_ERR("cannot open file for append");
        size_t n = fwrite(data, 1, strlen(data), f);
        fclose(f); return fs_int((long)n);
    }

    /* fs.exists(path) → bool */
    if (!strcmp(fn_name, "exists")) {
        NEED(1); GET_STR(0, path);
        return fs_bool(access(path, F_OK) == 0);
    }

    /* fs.delete(path) → bool */
    if (!strcmp(fn_name, "delete")) {
        NEED(1); GET_STR(0, path);
        return fs_bool(unlink(path) == 0);
    }

    /* fs.rename(src, dst) → bool */
    if (!strcmp(fn_name, "rename")) {
        NEED(2); GET_STR(0, src); GET_STR(1, dst);
        return fs_bool(rename(src, dst) == 0);
    }

    /* fs.copy(src, dst) → bool */
    if (!strcmp(fn_name, "copy")) {
        NEED(2); GET_STR(0, src); GET_STR(1, dst);
        FILE *in = fopen(src, "rb"); if (!in) return fs_bool(0);
        FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return fs_bool(0); }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(in); fclose(out); return fs_bool(1);
    }

    /* fs.size(path) → int (-1 if not found) */
    if (!strcmp(fn_name, "size")) {
        NEED(1); GET_STR(0, path);
        struct stat st;
        if (stat(path, &st) != 0) return fs_int(-1);
        return fs_int((long)st.st_size);
    }

    /* fs.mkdir(path) → bool (creates including parents via mkdir -p style) */
    if (!strcmp(fn_name, "mkdir")) {
        NEED(1); GET_STR(0, path);
        /* Create path components one by one */
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", path);
        size_t len = strlen(tmp);
        if (tmp[len-1] == '/') tmp[len-1] = 0;
        int ok = 1;
        for (char *p = tmp+1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { ok = 0; break; }
                *p = '/';
            }
        }
        if (ok) ok = (mkdir(tmp, 0755) == 0 || errno == EEXIST);
        return fs_bool(ok);
    }

    /* fs.rmdir(path) → bool */
    if (!strcmp(fn_name, "rmdir")) {
        NEED(1); GET_STR(0, path);
        return fs_bool(rmdir(path) == 0);
    }

    /* fs.listdir(path) → dyn of str */
    if (!strcmp(fn_name, "listdir")) {
        NEED(1); GET_STR(0, path);
        DIR *d = opendir(path);
        if (!d) {
            snprintf(errbuf, sizeof(errbuf), "fs.listdir: cannot open '%s': %s",
                     path, strerror(errno));
            errstack_push(err, ERR_FLUXA, errbuf, "fs", line);
            *had_error = 1; return fs_nil();
        }
        /* Build dyn of str entries */
        FluxaDyn *dyn = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        memset(dyn, 0, sizeof(FluxaDyn));
        dyn->cap = 16;
        dyn->items = (Value *)malloc((size_t)dyn->cap * sizeof(Value));
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (dyn->count >= dyn->cap) {
                dyn->cap *= 2;
                dyn->items = (Value *)realloc(dyn->items,
                    (size_t)dyn->cap * sizeof(Value));
            }
            dyn->items[dyn->count].type = VAL_STRING;
            dyn->items[dyn->count].as.string = strdup(ent->d_name);
            dyn->count++;
        }
        closedir(d);
        Value v; v.type = VAL_DYN; v.as.dyn = dyn;
        return v;
    }

    /* fs.isdir(path) → bool */
    if (!strcmp(fn_name, "isdir")) {
        NEED(1); GET_STR(0, path);
        struct stat st;
        if (stat(path, &st) != 0) return fs_bool(0);
        return fs_bool(S_ISDIR(st.st_mode));
    }

    /* fs.isfile(path) → bool */
    if (!strcmp(fn_name, "isfile")) {
        NEED(1); GET_STR(0, path);
        struct stat st;
        if (stat(path, &st) != 0) return fs_bool(0);
        return fs_bool(S_ISREG(st.st_mode));
    }

    /* fs.join(a, b) → str */
    if (!strcmp(fn_name, "join")) {
        NEED(2); GET_STR(0, a); GET_STR(1, b);
        size_t la = strlen(a), lb = strlen(b);
        char *buf = (char *)malloc(la + lb + 2);
        memcpy(buf, a, la);
        if (la > 0 && a[la-1] != '/' && b[0] != '/') buf[la++] = '/';
        memcpy(buf + la, b, lb); buf[la + lb] = '\0';
        Value v; v.type = VAL_STRING; v.as.string = buf;
        return v;
    }

    /* fs.basename(path) → str */
    if (!strcmp(fn_name, "basename")) {
        NEED(1); GET_STR(0, path);
        /* Use a copy since basename() may modify its argument */
        char *tmp = strdup(path);
        char *b = basename(tmp);
        Value v = fs_str(b); free(tmp); return v;
    }

    /* fs.dirname(path) → str */
    if (!strcmp(fn_name, "dirname")) {
        NEED(1); GET_STR(0, path);
        char *tmp = strdup(path);
        char *d = dirname(tmp);
        Value v = fs_str(d); free(tmp); return v;
    }

    /* fs.ext(path) → str (".txt" or "" if no extension) */
    if (!strcmp(fn_name, "ext")) {
        NEED(1); GET_STR(0, path);
        const char *dot = strrchr(path, '.');
        const char *slash = strrchr(path, '/');
        if (!dot || (slash && dot < slash)) return fs_str("");
        return fs_str(dot);
    }

    /* fs.tempfile() → str (path to a writable temp file) */
    if (!strcmp(fn_name, "tempfile")) {
        char tmpl[] = "/tmp/fluxa_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) FS_ERR("cannot create temp file");
        close(fd);
        return fs_str(tmpl);
    }

#undef FS_ERR
#undef NEED
#undef GET_STR

    snprintf(errbuf, sizeof(errbuf), "fs.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "fs", line);
    *had_error = 1; return fs_nil();
}

FLUXA_LIB_EXPORT(
    name      = "fs",
    toml_key  = "std.fs",
    owner     = "fs",
    call      = fluxa_std_fs_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_FS_H */
