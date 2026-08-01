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
#include <limits.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value fs_nil(void)     { Value v; v.type = VAL_NIL;    return v; }
static inline Value fs_int(long n)   { Value v; v.type = VAL_INT;    v.as.integer = n; return v; }
static inline Value fs_bool(int b)   { Value v; v.type = VAL_BOOL;   v.as.boolean = b; return v; }
static inline Value fs_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = fxstr_new(s ? s : ""); return v;
}

/* Compare `len` bytes of `want` against `sig` at byte offset `off`. */
static inline int fs_sig_match(const unsigned char *sig, size_t sig_len,
                               size_t off, const unsigned char *want, size_t len) {
    if (off + len > sig_len) return 0;
    return memcmp(sig + off, want, len) == 0;
}

/* Case-insensitive check that `path` ends with `.ext`. */
static inline int fs_has_ext(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return 0;
    const char *tail = path + (pl - el);
    for (size_t i = 0; i < el; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Validate that a file's extension AND leading bytes match a known type.
 *
 * `type` is a short name ("png", "jpg", "csv", "mp4", ..., or "any"). The check
 * has three flavors, because real formats differ:
 *   - binary magic at offset 0 (png, gif, pdf, zip/xlsx/docx, flac, ogg, webm);
 *   - binary magic at an offset (RIFF containers: webp/wav/avi carry the subtype
 *     at byte 8; mp4 carries "ftyp" at byte 4);
 *   - extension-only (csv/json/txt are plain text with no reliable signature).
 * mp3 accepts either an ID3 tag or a frame-sync header.
 *
 * `sig`/`sig_len` are the first bytes already read from the file. Returns 1 if
 * OK, 0 if the type doesn't match, -1 if the type name is unknown. "any" skips
 * the type check (the caller's dir-confinement and size guards still apply). */
static inline int fs_type_ok(const char *type, const char *path,
                             const unsigned char *sig, size_t sig_len) {
    if (!strcmp(type, "any")) return 1;

    /* Text types: extension only (no dependable magic bytes). */
    if (!strcmp(type, "csv"))  return fs_has_ext(path, ".csv");
    if (!strcmp(type, "json")) return fs_has_ext(path, ".json");
    if (!strcmp(type, "txt"))  return fs_has_ext(path, ".txt");

    #define SIG_AT(off, ...) fs_sig_match(sig, sig_len, (off), \
        (const unsigned char[]){__VA_ARGS__}, sizeof((const unsigned char[]){__VA_ARGS__}))
    #define EXT(e) fs_has_ext(path, (e))

    if (!strcmp(type, "png"))
        return EXT(".png") && SIG_AT(0, 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A);
    if (!strcmp(type, "jpg"))
        return (EXT(".jpg") || EXT(".jpeg")) && SIG_AT(0, 0xFF,0xD8,0xFF);
    if (!strcmp(type, "gif"))
        return EXT(".gif") && SIG_AT(0, 0x47,0x49,0x46,0x38);
    if (!strcmp(type, "qoi"))
        return EXT(".qoi") && SIG_AT(0, 0x71,0x6F,0x69,0x66);
    if (!strcmp(type, "bmp"))
        return EXT(".bmp") && SIG_AT(0, 0x42,0x4D);
    if (!strcmp(type, "webp"))
        return EXT(".webp") && SIG_AT(0, 0x52,0x49,0x46,0x46) && SIG_AT(8, 0x57,0x45,0x42,0x50);
    if (!strcmp(type, "pdf"))
        return EXT(".pdf") && SIG_AT(0, 0x25,0x50,0x44,0x46);
    if (!strcmp(type, "zip"))
        return EXT(".zip") && SIG_AT(0, 0x50,0x4B,0x03,0x04);
    if (!strcmp(type, "xlsx"))
        return EXT(".xlsx") && SIG_AT(0, 0x50,0x4B,0x03,0x04);   /* xlsx is a zip */
    if (!strcmp(type, "docx"))
        return EXT(".docx") && SIG_AT(0, 0x50,0x4B,0x03,0x04);   /* docx is a zip */
    if (!strcmp(type, "flac"))
        return EXT(".flac") && SIG_AT(0, 0x66,0x4C,0x61,0x43);
    if (!strcmp(type, "ogg"))
        return EXT(".ogg") && SIG_AT(0, 0x4F,0x67,0x67,0x53);
    if (!strcmp(type, "wav"))
        return EXT(".wav") && SIG_AT(0, 0x52,0x49,0x46,0x46) && SIG_AT(8, 0x57,0x41,0x56,0x45);
    if (!strcmp(type, "avi"))
        return EXT(".avi") && SIG_AT(0, 0x52,0x49,0x46,0x46) && SIG_AT(8, 0x41,0x56,0x49,0x20);
    if (!strcmp(type, "mp4"))
        return (EXT(".mp4") || EXT(".m4a") || EXT(".m4v")) && SIG_AT(4, 0x66,0x74,0x79,0x70);
    if (!strcmp(type, "webm"))
        return EXT(".webm") && SIG_AT(0, 0x1A,0x45,0xDF,0xA3);
    if (!strcmp(type, "mp3"))
        return EXT(".mp3") && (SIG_AT(0, 0x49,0x44,0x33) || SIG_AT(0, 0xFF,0xFB));

    #undef SIG_AT
    #undef EXT
    return -1;  /* unknown type name */
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
        char *buf = fxstr_alloc((size_t)sz);   /* refcounted, zero extra copy */
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
            dyn->items[dyn->count].as.string = fxstr_new(ent->d_name);
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
        char *buf = fxstr_alloc(la + lb + 1);   /* refcounted */
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

    /* fs.read_base64(path, max_bytes, type) → str   SECURE read of a file's raw
     * bytes as base64. Plain fs.read returns a C string that stops at the first
     * NUL, so it truncates binary files; this reads the whole file and encodes it
     * to base64 (NUL-free) so it survives as a str and can be put in JSON and
     * sent over the network.
     *
     * Because the result can be uploaded, an unrestricted read here would be an
     * exfiltration primitive (read a key file, send it out). So it is locked down:
     *   1) the path must resolve (realpath) INSIDE the process's working dir — no
     *      '..' escape, no absolute path out, no symlink pointing outside;
     *   2) it must be a regular file (not a dir, device, or pipe);
     *   3) it must match `type` — both the extension and the file's magic bytes
     *      (see fs_type_ok). This is what stops a path aimed at a sensitive file
     *      that merely sits in the working dir (e.g. the local database): it is
     *      not the declared type. `type` is one of a known set ("png", "jpg",
     *      "pdf", "xlsx", "csv", "mp3", "mp4", "wav", ...) or "any" to skip the
     *      type check (guards 1, 2, 4 still apply — use only when you control the
     *      path and genuinely need an arbitrary file);
     *   4) it must be no larger than max_bytes (guards memory; base64 adds ~33%).
     * Only after all pass does it read and encode. Any failure returns an error,
     * never partial or wrong data. */
    if (!strcmp(fn_name, "read_base64")) {
        NEED(3); GET_STR(0, path);
        if (args[1].type != VAL_INT) FS_ERR("read_base64: max_bytes must be int");
        long max_bytes = args[1].as.integer;
        if (max_bytes <= 0) FS_ERR("read_base64: max_bytes must be positive");
        GET_STR(2, type);

        /* 1) Resolve the real, absolute path and the working directory, then
         *    require the file to live inside the working dir. realpath collapses
         *    '..' and follows symlinks, so this catches escape attempts. */
        char real_path[PATH_MAX];
        char real_cwd[PATH_MAX];
        if (!realpath(path, real_path)) FS_ERR("read_base64: file not found or unreadable");
        if (!getcwd(real_cwd, sizeof(real_cwd))) FS_ERR("read_base64: cannot resolve working directory");
        size_t cwd_len = strlen(real_cwd);
        /* The real path must start with the working dir followed by a separator
         * (so '/work' does not match '/workother'). */
        if (strncmp(real_path, real_cwd, cwd_len) != 0 ||
            (real_path[cwd_len] != '/' && real_path[cwd_len] != '\0')) {
            FS_ERR("read_base64: path is outside the allowed directory");
        }

        /* 2) Must be a regular file. */
        struct stat st;
        if (stat(real_path, &st) != 0) FS_ERR("read_base64: cannot stat file");
        if (!S_ISREG(st.st_mode)) FS_ERR("read_base64: not a regular file");

        /* 4a) Size check before reading (fail fast on oversize). */
        if ((long)st.st_size > max_bytes) FS_ERR("read_base64: file exceeds max_bytes");
        if (st.st_size <= 0) FS_ERR("read_base64: empty file");

        FILE *f = fopen(real_path, "rb");
        if (!f) FS_ERR("read_base64: cannot open file");

        /* 3) Type check: extension + magic bytes must match `type` (or "any").
         *    Read up to 16 leading bytes for the signature, then validate before
         *    reading the whole file. This blocks a path aimed at a file of the
         *    wrong type in the work dir (e.g. a .db when png was asked for). */
        unsigned char sig[16];
        size_t sig_read = fread(sig, 1, sizeof(sig), f);
        int type_result = fs_type_ok(type, path, sig, sig_read);
        if (type_result < 0) { fclose(f); FS_ERR("read_base64: unknown type name"); }
        if (type_result == 0) { fclose(f); FS_ERR("read_base64: file does not match the requested type"); }

        /* Read the whole file into a buffer (size already bounded by max_bytes). */
        long fsize = (long)st.st_size;
        unsigned char *raw = (unsigned char *)malloc((size_t)fsize);
        if (!raw) { fclose(f); FS_ERR("read_base64: out of memory"); }
        rewind(f);
        size_t got = fread(raw, 1, (size_t)fsize, f);
        fclose(f);
        if (got != (size_t)fsize) { free(raw); FS_ERR("read_base64: short read"); }

        /* Base64-encode into a refcounted Fluxa string. Output length is
         * 4*ceil(n/3), plus the NUL terminator. */
        static const char B64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t out_len = 4 * ((size_t)(fsize + 2) / 3);
        char *out = fxstr_alloc(out_len);
        if (!out) { free(raw); FS_ERR("read_base64: out of memory"); }
        size_t oi = 0;
        for (long i = 0; i < fsize; i += 3) {
            unsigned int b0 = raw[i];
            unsigned int b1 = (i + 1 < fsize) ? raw[i + 1] : 0;
            unsigned int b2 = (i + 2 < fsize) ? raw[i + 2] : 0;
            unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
            out[oi++] = B64[(triple >> 18) & 0x3F];
            out[oi++] = B64[(triple >> 12) & 0x3F];
            out[oi++] = (i + 1 < fsize) ? B64[(triple >> 6) & 0x3F] : '=';
            out[oi++] = (i + 2 < fsize) ? B64[triple & 0x3F] : '=';
        }
        out[oi] = '\0';
        free(raw);
        Value v; v.type = VAL_STRING; v.as.string = out;
        return v;
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
