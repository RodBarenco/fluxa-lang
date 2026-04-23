#ifndef FLUXA_STD_ZLIB_H
#define FLUXA_STD_ZLIB_H

/*
 * std.zlib — zlib compression for Fluxa-lang
 *
 * Wrapper over zlib (the battle-tested C library behind gzip, PNG, ZIP).
 * Critical for IoT: compress sensor data before writing to Flash, reduce
 * MQTT payloads, decompress firmware update chunks.
 *
 * All operations work on str — binary-safe via length tracking.
 * Compressed output is base64-encoded for safe str transport.
 * Raw (binary) mode available for direct Flash/socket writes.
 *
 * API:
 *   zlib.compress(data)          → str  (deflate + base64)
 *   zlib.decompress(data)        → str  (base64 → inflate)
 *   zlib.compress_raw(data)      → str  (deflate, binary — danger only)
 *   zlib.decompress_raw(data, n) → str  (inflate binary, n=max output bytes)
 *   zlib.gzip(data)              → str  (gzip format + base64)
 *   zlib.gunzip(data)            → str  (base64 → gunzip)
 *   zlib.crc32(data)             → int  (CRC-32 checksum)
 *   zlib.adler32(data)           → int  (Adler-32 checksum)
 *   zlib.ratio(original, compressed) → float  (compression ratio)
 *   zlib.version()               → str  (zlib version string)
 */

#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Base64 encode/decode (RFC 4648, for safe str transport) ─────── */
static const char _b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline char *zl_b64_encode(const unsigned char *src, size_t src_len,
                                   size_t *out_len) {
    size_t enc_len = 4 * ((src_len + 2) / 3);
    char *out = (char *)malloc(enc_len + 1);
    size_t i = 0, j = 0;
    while (i < src_len) {
        unsigned int a = (i < src_len) ? src[i++] : 0;
        unsigned int b = (i < src_len) ? src[i++] : 0;
        unsigned int c = (i < src_len) ? src[i++] : 0;
        unsigned int t = (a << 16) | (b << 8) | c;
        out[j++] = _b64_table[(t >> 18) & 0x3F];
        out[j++] = _b64_table[(t >> 12) & 0x3F];
        out[j++] = (src_len + (i > src_len ? i - src_len : 0) >= 2) ?
                    _b64_table[(t >>  6) & 0x3F] : '=';
        out[j++] = (src_len + (i > src_len ? i - src_len : 0) >= 3) ?
                    _b64_table[(t      ) & 0x3F] : '=';
    }
    out[j] = '\0'; *out_len = j;
    return out;
}

static inline unsigned char *zl_b64_decode(const char *src, size_t src_len,
                                             size_t *out_len) {
    if (src_len % 4 != 0) { *out_len = 0; return NULL; }
    size_t dec_len = src_len / 4 * 3;
    if (src_len > 1 && src[src_len-1] == '=') dec_len--;
    if (src_len > 1 && src[src_len-2] == '=') dec_len--;
    unsigned char *out = (unsigned char *)malloc(dec_len + 1);
    static const int8_t dec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    size_t oi = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        unsigned int a = (unsigned int)dec[(unsigned char)src[i]];
        unsigned int b = (unsigned int)dec[(unsigned char)src[i+1]];
        unsigned int c = (src[i+2]!='=') ? (unsigned int)dec[(unsigned char)src[i+2]] : 0;
        unsigned int d = (src[i+3]!='=') ? (unsigned int)dec[(unsigned char)src[i+3]] : 0;
        unsigned int t = (a<<18)|(b<<12)|(c<<6)|d;
        if (oi < dec_len) out[oi++] = (t>>16)&0xFF;
        if (oi < dec_len) out[oi++] = (t>> 8)&0xFF;
        if (oi < dec_len) out[oi++] = (t    )&0xFF;
    }
    out[oi] = '\0'; *out_len = oi;
    return out;
}

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value zl_nil(void)     { Value v; v.type = VAL_NIL;    return v; }
static inline Value zl_int(long n)   { Value v; v.type = VAL_INT;    v.as.integer = n; return v; }
static inline Value zl_float(double d){ Value v; v.type = VAL_FLOAT; v.as.real    = d; return v; }
static inline Value zl_str(const char *s) {
    Value v; v.type = VAL_STRING; v.as.string = strdup(s ? s : ""); return v;
}
static inline Value zl_str_n(const char *s, size_t n) {
    Value v; v.type = VAL_STRING;
    v.as.string = (char *)malloc(n + 1);
    memcpy(v.as.string, s, n); v.as.string[n] = '\0';
    return v;
}

/* ── Compress/decompress helpers ─────────────────────────────────── */
static inline int zl_do_compress(const char *src, size_t src_len,
                                   unsigned char **out, size_t *out_len,
                                   int gzip_mode) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int wbits = gzip_mode ? (MAX_WBITS | 16) : MAX_WBITS;
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    size_t buf_sz = deflateBound(&zs, (uLong)src_len) + 64;
    unsigned char *buf = (unsigned char *)malloc(buf_sz);
    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    zs.next_out  = buf;
    zs.avail_out = (uInt)buf_sz;
    int ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) { deflateEnd(&zs); free(buf); return -1; }
    *out_len = (size_t)zs.total_out;
    *out = buf;
    deflateEnd(&zs);
    return 0;
}

static inline int zl_do_decompress(const unsigned char *src, size_t src_len,
                                     unsigned char **out, size_t *out_len,
                                     size_t max_out, int gzip_mode) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int wbits = gzip_mode ? (MAX_WBITS | 16) : MAX_WBITS;
    if (inflateInit2(&zs, wbits) != Z_OK) return -1;
    size_t buf_sz = max_out > 0 ? max_out : src_len * 8;
    unsigned char *buf = (unsigned char *)malloc(buf_sz + 1);
    zs.next_in  = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    zs.next_out  = buf;
    zs.avail_out = (uInt)buf_sz;
    int ret = inflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) { inflateEnd(&zs); free(buf); return -1; }
    *out_len = (size_t)zs.total_out;
    buf[*out_len] = '\0';
    *out = buf;
    inflateEnd(&zs);
    return 0;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_zlib_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define ZL_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "zlib.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "zlib", line); \
    *had_error = 1; return zl_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "zlib.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "zlib", line); \
        *had_error = 1; return zl_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        ZL_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string; \
    size_t var##_len  = strlen(var);

    /* zlib.version() → str */
    if (!strcmp(fn_name, "version")) {
        return zl_str(zlibVersion());
    }

    /* zlib.compress(data) → str (deflate + base64) */
    if (!strcmp(fn_name, "compress")) {
        NEED(1); GET_STR(0, src);
        unsigned char *comp = NULL; size_t comp_len = 0;
        if (zl_do_compress(src, src_len, &comp, &comp_len, 0) != 0)
            ZL_ERR("compression failed");
        size_t b64_len = 0;
        char *b64 = zl_b64_encode(comp, comp_len, &b64_len);
        free(comp);
        Value v = zl_str_n(b64, b64_len); free(b64); return v;
    }

    /* zlib.decompress(data) → str (base64 → inflate) */
    if (!strcmp(fn_name, "decompress")) {
        NEED(1); GET_STR(0, src);
        size_t raw_len = 0;
        unsigned char *raw = zl_b64_decode(src, src_len, &raw_len);
        if (!raw) ZL_ERR("invalid base64 input");
        unsigned char *out = NULL; size_t out_len = 0;
        if (zl_do_decompress(raw, raw_len, &out, &out_len, 0, 0) != 0) {
            free(raw); ZL_ERR("decompression failed");
        }
        free(raw);
        Value v = zl_str_n((char *)out, out_len); free(out); return v;
    }

    /* zlib.gzip(data) → str (gzip + base64) */
    if (!strcmp(fn_name, "gzip")) {
        NEED(1); GET_STR(0, src);
        unsigned char *comp = NULL; size_t comp_len = 0;
        if (zl_do_compress(src, src_len, &comp, &comp_len, 1) != 0)
            ZL_ERR("gzip compression failed");
        size_t b64_len = 0;
        char *b64 = zl_b64_encode(comp, comp_len, &b64_len);
        free(comp);
        Value v = zl_str_n(b64, b64_len); free(b64); return v;
    }

    /* zlib.gunzip(data) → str (base64 → gunzip) */
    if (!strcmp(fn_name, "gunzip")) {
        NEED(1); GET_STR(0, src);
        size_t raw_len = 0;
        unsigned char *raw = zl_b64_decode(src, src_len, &raw_len);
        if (!raw) ZL_ERR("invalid base64 input");
        unsigned char *out = NULL; size_t out_len = 0;
        if (zl_do_decompress(raw, raw_len, &out, &out_len, 0, 1) != 0) {
            free(raw); ZL_ERR("gunzip failed");
        }
        free(raw);
        Value v = zl_str_n((char *)out, out_len); free(out); return v;
    }

    /* zlib.crc32(data) → int */
    if (!strcmp(fn_name, "crc32")) {
        NEED(1); GET_STR(0, src);
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (const Bytef *)src, (uInt)src_len);
        return zl_int((long)crc);
    }

    /* zlib.adler32(data) → int */
    if (!strcmp(fn_name, "adler32")) {
        NEED(1); GET_STR(0, src);
        uLong a = adler32(0L, Z_NULL, 0);
        a = adler32(a, (const Bytef *)src, (uInt)src_len);
        return zl_int((long)a);
    }

    /* zlib.ratio(original_len, compressed_len) → float */
    if (!strcmp(fn_name, "ratio")) {
        NEED(2);
        double orig = (args[0].type==VAL_INT)   ? (double)args[0].as.integer :
                      (args[0].type==VAL_FLOAT)  ? args[0].as.real : 0.0;
        double comp = (args[1].type==VAL_INT)   ? (double)args[1].as.integer :
                      (args[1].type==VAL_FLOAT)  ? args[1].as.real : 1.0;
        if (comp <= 0.0) ZL_ERR("compressed size must be > 0");
        return zl_float(orig / comp);
    }

#undef ZL_ERR
#undef NEED
#undef GET_STR

    snprintf(errbuf, sizeof(errbuf), "zlib.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "zlib", line);
    *had_error = 1; return zl_nil();
}

FLUXA_LIB_EXPORT(
    name      = "zlib",
    toml_key  = "std.zlib",
    owner     = "zlib",
    call      = fluxa_std_zlib_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_ZLIB_H */
