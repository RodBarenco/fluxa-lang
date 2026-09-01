#ifndef FLUXA_STD_IMAGE_H
#define FLUXA_STD_IMAGE_H

/*
 * std.image — encode, decode, and transform raw images for Fluxa-lang.
 *
 * Works hand in hand with std.graph: graph.capture(win) returns a neutral RGBA
 * buffer (a dyn), and this lib saves it to PNG / JPG / BMP / TGA / QOI, resizes
 * it, reads its size, or loads an image back from disk. The buffer format is
 * the shared FluxaImageBuf (32-bit RGBA), so image never depends on graph and
 * graph never depends on image — a capture is just bytes.
 *
 * Two backends:
 *
 *   FLUXA_IMAGE_RAYLIB=1   Raylib backend — real PNG/JPG/BMP/TGA/QOI encode and
 *     decode (Raylib bundles stb_image / stb_image_write). Vendor raylib into
 *     vendor/raylib.h + vendor/libraylib.a, then build with FLUXA_IMAGE_RAYLIB=1.
 *
 *   (default) stub backend — no encode/decode, but the API is complete and the
 *     buffer transforms that don't need a codec (new, size, free, and a
 *     nearest-neighbour resize) work. save/load report a clear error so logic
 *     and tests run without the codec present.
 *
 * All IO functions (save, load) must run inside a danger {} block — they touch
 * the filesystem and fail for external reasons (bad path, unwritable dir,
 * corrupt file), exactly like sqlite/csv/fs.
 *
 * API:
 *   image.new(w, h)            → dyn   blank RGBA buffer (all zero / transparent)
 *   image.save(img, path)      → bool  encode by extension (.png/.jpg/.bmp/.tga/.qoi)
 *   image.load(path)           → dyn   decode a file into an RGBA buffer
 *   image.resize(img, w, h)    → nil   scale in place (Bicubic on raylib, NN on stub)
 *   image.update_rgba(img, px) → nil   replace all RGBA bytes from an int arr
 *   image.update_rgba_rect(img, px, x, y, w, h) → nil  replace one rectangle
 *   image.blit(dst,src,x,y[,m]) → nil   compose src onto dst; optional mask image
 *   image.fill_tris(dst,depth,tris,n,tex,tw,th,ts,alpha,flags[,rgb]) → int
 *   image.fill_rect(dst,x,y,w,h,rgb[,a]) → int   flat rectangle
 *   image.fill_tri(dst,x0,y0,x1,y1,x2,y2,rgb[,a]) → int   flat triangle
 *   image.width(img)           → int
 *   image.height(img)          → int
 *   image.set_text(path,k,t[,c])→ bool  embed PNG iTXt metadata; optional compress
 *   image.get_text(path,key)     → str   read first matching PNG iTXt metadata
 *   image.discard(img)         → nil   release the buffer (also frees graph.capture)
 *   image.version()            → str
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include "../../scope.h"
#include "../../err.h"
#include "../fluxa_image_buffer.h"

#ifdef FLUXA_IMAGE_RAYLIB
#include <raylib.h>
#endif

/* ── Triangle rasteriser ────────────────────────────────────────────
 * Per-pixel work cannot live in Fluxa. A textured, depth-tested fill runs
 * around thirty-five operations per written pixel; at the ~16 ns an iteration
 * of a compiled loop costs, a 640x480 frame is a quarter of a second. The same
 * work in C is single-digit milliseconds. The floor is the instruction count
 * per pixel, so no amount of work on the language side closes it.
 *
 * The rules below are not new. They are the ones a real Fluxa rasteriser
 * already settled on, reproduced here so this call is a drop-in replacement
 * for that loop and its output can be compared against it bit for bit:
 *
 *   - vertices arrive in screen space; projection stays with the caller
 *   - barycentric weights come from integer edge functions, and every
 *     division truncates toward zero
 *   - a texel coordinate wraps with %, and a negative result is corrected by
 *     adding the dimension once
 *   - the texel's own alpha multiplies the alpha argument
 *   - a fully opaque pixel is written straight; anything else blends as
 *     (src * a + dst * (255 - a)) / 255
 *   - the destination alpha is set to 255
 *   - depth is written only above a threshold, so a translucent texel does not
 *     hide what is behind it
 *
 * Components read out of the texture are clamped to 0..255 rather than
 * rejected: a batch draws as it goes, so failing midway would leave a half
 * drawn frame with no way back. Sizes and handles are checked up front, where
 * refusing still leaves the destination untouched. */

#define FLUXA_TRI_STRIDE 15   /* x, y, z, u, v per vertex, three vertices */

/* flags, as passed from Fluxa */
#define FLUXA_TRI_FRONT      0x01  /* draw front faces (negative signed area) */
#define FLUXA_TRI_BACK       0x02  /* draw back faces                          */
#define FLUXA_TRI_DEPTH_LESS 0x04  /* keep the smaller z; default keeps larger */
#define FLUXA_TRI_ZTHRESH(f) (((f) >> 8) & 0xFF)  /* write depth above this alpha */

static inline long fluxa_tri_i(const Value *a, long i) {
    return (a[i].type == VAL_INT) ? a[i].as.integer : 0;
}
static inline int fluxa_tri_u8(const Value *a, long i) {
    long v = fluxa_tri_i(a, i);
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

/* ── PNG iTXt metadata (for image.set_text) ─────────────────────────
 * Raylib/stb writes a bare PNG with no text chunks, so to embed metadata we
 * post-process the encoded file: read it, splice one iTXt chunk in just before
 * the trailing IEND, and write it back. The chunk format is rigid, so this is
 * done by hand with a CRC-32 over "type + data". iTXt supports an uncompressed
 * form (compression flag 0) and a zlib-deflate form (flag 1); the deflate path
 * uses the same zlib the runtime already links. */

static const unsigned int FLUXA_CRC_POLY = 0xEDB88320u;
static inline unsigned int fluxa_png_crc(const unsigned char *buf, size_t len) {
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i=0; i<len; i++) {
        c ^= buf[i];
        for (int k=0; k<8; k++) c = (c & 1) ? (FLUXA_CRC_POLY ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}
static inline void fluxa_put_be32(unsigned char *p, unsigned int v) {
    p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16);
    p[2]=(unsigned char)(v>>8);  p[3]=(unsigned char)(v);
}

#ifdef FLUXA_IMAGE_RAYLIB
#include <zlib.h>
/* deflate `src` into a freshly malloc'd buffer; returns len or -1. */
static inline long fluxa_deflate(const unsigned char *src, size_t src_len,
                                 unsigned char **out) {
    z_stream zs; memset(&zs,0,sizeof(zs));
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return -1;
    uLong bound = deflateBound(&zs, (uLong)src_len);
    unsigned char *buf = (unsigned char *)malloc(bound);
    if (!buf) { deflateEnd(&zs); return -1; }
    zs.next_in=(Bytef *)src; zs.avail_in=(uInt)src_len;
    zs.next_out=buf; zs.avail_out=(uInt)bound;
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) { deflateEnd(&zs); free(buf); return -1; }
    long n = (long)zs.total_out; deflateEnd(&zs);
    *out = buf; return n;
}

/* Defensive limits for PNG metadata parsing. Metadata is external input: keep
 * memory and CPU bounded even when the PNG was crafted maliciously. */
#define FLUXA_IMAGE_GET_TEXT_MAX_FILE_BYTES  (24u * 1024u * 1024u)
#define FLUXA_IMAGE_GET_TEXT_MAX_ITXT_BYTES  (1024u * 1024u)
#define FLUXA_IMAGE_GET_TEXT_MAX_TEXT_BYTES  (1024u * 1024u)

/* Validate UTF-8 without accepting overlong encodings, surrogate code points,
 * or values beyond U+10FFFF. NUL is rejected because Fluxa str is NUL-terminated. */
static inline int fluxa_utf8_valid_text(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c == 0) return 0;
        if (c <= 0x7F) continue;
        if (c >= 0xC2 && c <= 0xDF) {
            if (i >= n || (s[i] & 0xC0) != 0x80) return 0;
            i += 1; continue;
        }
        if (c == 0xE0) {
            if (i + 1 >= n || s[i] < 0xA0 || s[i] > 0xBF || (s[i+1] & 0xC0) != 0x80) return 0;
            i += 2; continue;
        }
        if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
            if (i + 1 >= n || (s[i] & 0xC0) != 0x80 || (s[i+1] & 0xC0) != 0x80) return 0;
            i += 2; continue;
        }
        if (c == 0xED) {
            if (i + 1 >= n || s[i] < 0x80 || s[i] > 0x9F || (s[i+1] & 0xC0) != 0x80) return 0;
            i += 2; continue;
        }
        if (c == 0xF0) {
            if (i + 2 >= n || s[i] < 0x90 || s[i] > 0xBF ||
                (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return 0;
            i += 3; continue;
        }
        if (c >= 0xF1 && c <= 0xF3) {
            if (i + 2 >= n || (s[i] & 0xC0) != 0x80 ||
                (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return 0;
            i += 3; continue;
        }
        if (c == 0xF4) {
            if (i + 2 >= n || s[i] < 0x80 || s[i] > 0x8F ||
                (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return 0;
            i += 3; continue;
        }
        return 0;
    }
    return 1;
}

/* Inflate into a bounded NUL-terminated buffer. Returns 0 on success, -1 for
 * malformed zlib data/OOM, and -2 when expansion exceeds max_out. */
static inline int fluxa_inflate_text_bounded(const unsigned char *src, size_t src_len,
                                             size_t max_out,
                                             unsigned char **out, size_t *out_len) {
    if (!out || !out_len || src_len > (size_t)UINT_MAX) return -1;
    *out = NULL; *out_len = 0;

    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) return -1;

    size_t cap = src_len ? src_len * 3u + 256u : 256u;
    if (cap < 256u) cap = 256u;
    if (cap > max_out) cap = max_out;
    if (cap == 0) cap = 1;

    unsigned char *buf = (unsigned char *)malloc(cap + 1u);
    if (!buf) { inflateEnd(&zs); return -1; }

    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    size_t used = 0;

    for (;;) {
        if (used == cap) {
            if (cap >= max_out) { free(buf); inflateEnd(&zs); return -2; }
            size_t ncap = cap > max_out / 2u ? max_out : cap * 2u;
            unsigned char *nb = (unsigned char *)realloc(buf, ncap + 1u);
            if (!nb) { free(buf); inflateEnd(&zs); return -1; }
            buf = nb; cap = ncap;
        }

        uInt step = (uInt)(cap - used);
        zs.next_out = buf + used;
        zs.avail_out = step;
        int rc = inflate(&zs, Z_NO_FLUSH);
        used += (size_t)(step - zs.avail_out);

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { free(buf); inflateEnd(&zs); return -1; }
        if (zs.avail_in == 0 && zs.avail_out != 0) { free(buf); inflateEnd(&zs); return -1; }
    }

    /* A valid stream must consume the complete compressed payload. */
    if (zs.avail_in != 0) { free(buf); inflateEnd(&zs); return -1; }
    inflateEnd(&zs);
    buf[used] = 0;
    *out = buf; *out_len = used;
    return 0;
}
#endif

/* ── Value constructors continue below ──────────────────────────── */

static inline Value image_int(long n)     { Value v; v.type=VAL_INT;  v.as.integer=n; return v; }
static inline Value image_bool(int b)     { Value v; v.type=VAL_BOOL; v.as.boolean=b; return v; }
static inline Value image_nil(void)       { Value v; v.type=VAL_NIL;                  return v; }
static inline Value image_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=fxstr_new(s?s:""); return v;
}

/* Wrap / unwrap the shared image buffer as a dyn (VAL_PTR), same shape as the
 * handle graph.capture produces — so a captured dyn flows straight into here. */
static inline Value image_wrap(FluxaImageBuf *b) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=b;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline FluxaImageBuf *image_unwrap(const Value *v) {
    if (v->type!=VAL_DYN || !v->as.dyn || v->as.dyn->count<1) return NULL;
    if (v->as.dyn->items[0].type!=VAL_PTR) return NULL;
    return (FluxaImageBuf *)v->as.dyn->items[0].as.ptr;
}

/* lowercase file extension (after the last '.'), into a small buffer */
static inline void image_ext(const char *path, char *out, size_t outsz) {
    out[0]=0;
    const char *dot=strrchr(path,'.');
    if (!dot || !dot[1]) return;
    size_t j=0;
    for (const char *p=dot+1; *p && j+1<outsz; p++) out[j++]=(char)tolower((unsigned char)*p);
    out[j]=0;
}

/* nearest-neighbour resize used by the stub backend (no codec needed) */
static inline FluxaImageBuf *image_resize_nn(const FluxaImageBuf *src, int nw, int nh) {
    FluxaImageBuf *dst = fluxa_imgbuf_new(nw, nh);
    if (!dst) return NULL;
    for (int y=0; y<nh; y++) {
        int sy = (int)((long)y * src->height / nh);
        if (sy >= src->height) sy = src->height-1;
        for (int x=0; x<nw; x++) {
            int sx = (int)((long)x * src->width / nw);
            if (sx >= src->width) sx = src->width-1;
            const unsigned char *sp = src->rgba + ((size_t)sy*src->width + sx)*4u;
            unsigned char *dp = dst->rgba + ((size_t)y*nw + x)*4u;
            dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
        }
    }
    return dst;
}

/* ── Main dispatch ──────────────────────────────────────────────── */
static inline Value fluxa_std_image_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define IMG_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"image.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"image",line); \
    *had_error=1; return image_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc<(n)) { \
        snprintf(errbuf,sizeof(errbuf),"image.%s: expected %d argument(s), got %d",fn_name,(n),argc); \
        errstack_push(err,ERR_FLUXA,errbuf,"image",line); \
        *had_error=1; return image_nil(); \
    } \
} while(0)

#define GET_INT(idx,var) \
    if (args[(idx)].type!=VAL_INT) IMG_ERR("expected int argument"); \
    long (var)=args[(idx)].as.integer;

#define GET_STR(idx,var) \
    if (args[(idx)].type!=VAL_STRING || !args[(idx)].as.string) IMG_ERR("expected str argument"); \
    const char *(var)=args[(idx)].as.string;

#define GET_IMG(idx,var) \
    FluxaImageBuf *(var)=image_unwrap(&args[(idx)]); \
    if (!fluxa_imgbuf_valid(var)) IMG_ERR("expected a live image handle");

    /* image.version() → str */
    if (strcmp(fn_name,"version")==0) {
#ifdef FLUXA_IMAGE_RAYLIB
        return image_str("std.image 1.0 (raylib codec)");
#else
        return image_str("std.image 1.0 (stub — no codec)");
#endif
    }

    /* image.new(w, h) → dyn */
    if (strcmp(fn_name,"new")==0) {
        NEED(2); GET_INT(0,w); GET_INT(1,h);
        if (w<=0 || h<=0) IMG_ERR("new: width and height must be positive");
        FluxaImageBuf *b = fluxa_imgbuf_new((int)w,(int)h);
        if (!b) IMG_ERR("new: out of memory (or size too large)");
        return image_wrap(b);
    }

    /* image.width(img) → int */
    if (strcmp(fn_name,"width")==0) {
        NEED(1); GET_IMG(0,b);
        return image_int(b->width);
    }

    /* image.height(img) → int */
    if (strcmp(fn_name,"height")==0) {
        NEED(1); GET_IMG(0,b);
        return image_int(b->height);
    }

    /* image.discard(img) → nil  (idempotent; also releases a graph.capture dyn) */
    if (strcmp(fn_name,"discard")==0) {
        NEED(1);
        FluxaImageBuf *b = image_unwrap(&args[0]);
        if (b) fluxa_imgbuf_free(b);
        if (args[0].type==VAL_DYN && args[0].as.dyn && args[0].as.dyn->count>0)
            args[0].as.dyn->items[0].as.ptr = NULL;   /* prevent double free */
        return image_nil();
    }

    /* image.resize(img, w, h) → nil  (in place) */
    if (strcmp(fn_name,"resize")==0) {
        NEED(3); GET_IMG(0,b); GET_INT(1,nw); GET_INT(2,nh);
        if (nw<=0 || nh<=0) IMG_ERR("resize: width and height must be positive");
#ifdef FLUXA_IMAGE_RAYLIB
        {
            Image img; img.data=b->rgba; img.width=b->width; img.height=b->height;
            img.mipmaps=1; img.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            Image copy = ImageCopy(img);          /* raylib owns/reallocs its own data */
            ImageResize(&copy, (int)nw, (int)nh); /* Bicubic */
            FluxaImageBuf *nb = fluxa_imgbuf_from_rgba((const unsigned char *)copy.data,
                                                       copy.width, copy.height);
            UnloadImage(copy);
            if (!nb) IMG_ERR("resize: out of memory");
            /* swap the new pixels into the existing handle so the dyn stays valid */
            free(b->rgba);
            b->rgba=nb->rgba; b->width=nb->width; b->height=nb->height;
            nb->rgba=NULL; fluxa_imgbuf_free(nb);
        }
#else
        {
            FluxaImageBuf *nb = image_resize_nn(b,(int)nw,(int)nh);
            if (!nb) IMG_ERR("resize: out of memory");
            free(b->rgba);
            b->rgba=nb->rgba; b->width=nb->width; b->height=nb->height;
            nb->rgba=NULL; fluxa_imgbuf_free(nb);
        }
#endif
        fluxa_imgbuf_touch(b);   /* pixels changed → invalidate any GPU cache */
        return image_nil();
    }

    /* image.update_rgba(img, pixels) → nil
     * Replace the complete tightly-packed RGBA buffer in row-major order.
     * Validate the entire input before changing the image, so a bad component
     * cannot leave half of the old frame mixed with half of the new one. */
    if (strcmp(fn_name,"update_rgba")==0) {
        NEED(2); GET_IMG(0,b);
        if (args[1].type!=VAL_ARR || !args[1].as.arr.data)
            IMG_ERR("update_rgba: pixels must be an int arr");
        size_t expected=(size_t)b->width*(size_t)b->height*4u;
        if ((size_t)args[1].as.arr.size!=expected) {
            snprintf(errbuf,sizeof(errbuf),
                     "image.update_rgba (line %d): expected %zu components, got %d",
                     line,expected,args[1].as.arr.size);
            errstack_push(err,ERR_FLUXA,errbuf,"image",line);
            *had_error=1;
            return image_nil();
        }
        /* Two passes on purpose: the image is left untouched when any
         * component is rejected, so a caller that catches the error in a
         * danger block still sees the image it had. Both passes read through
         * one hoisted pointer rather than copying each 24-byte Value. */
        const Value *px=args[1].as.arr.data;
        for (size_t i=0; i<expected; i++) {
            if (px[i].type!=VAL_INT)
                IMG_ERR("update_rgba: every component must be int");
            if (px[i].as.integer<0 || px[i].as.integer>255)
                IMG_ERR("update_rgba: components must be in the 0..255 range");
        }
        for (size_t i=0; i<expected; i++)
            b->rgba[i]=(unsigned char)px[i].as.integer;
        fluxa_imgbuf_touch(b);   /* stale cached texture uploads on next draw */
        return image_nil();
    }

    /* image.update_rgba_rect(img, pixels, x, y, w, h) → nil
     * Replace one rectangle of the image from a w*h*4 int arr, so a caller
     * that changes a corner pays for the corner rather than for the frame.
     * Same rules as update_rgba: components are int in 0..255, the rectangle
     * must lie inside the image, and nothing is written unless every
     * component is accepted. */
    if (strcmp(fn_name,"update_rgba_rect")==0) {
        NEED(6); GET_IMG(0,b);
        if (args[1].type!=VAL_ARR || !args[1].as.arr.data)
            IMG_ERR("update_rgba_rect: pixels must be an int arr");
        GET_INT(2,rx); GET_INT(3,ry); GET_INT(4,rw); GET_INT(5,rh);
        if (rw<=0 || rh<=0)
            IMG_ERR("update_rgba_rect: width and height must be positive");
        if (rx<0 || ry<0 || rx+rw>(long)b->width || ry+rh>(long)b->height)
            IMG_ERR("update_rgba_rect: rectangle falls outside the image");
        size_t expected=(size_t)rw*(size_t)rh*4u;
        if ((size_t)args[1].as.arr.size!=expected) {
            snprintf(errbuf,sizeof(errbuf),
                     "image.update_rgba_rect (line %d): expected %zu components, got %d",
                     line,expected,args[1].as.arr.size);
            errstack_push(err,ERR_FLUXA,errbuf,"image",line);
            *had_error=1;
            return image_nil();
        }
        const Value *px=args[1].as.arr.data;
        for (size_t i=0; i<expected; i++) {
            if (px[i].type!=VAL_INT)
                IMG_ERR("update_rgba_rect: every component must be int");
            if (px[i].as.integer<0 || px[i].as.integer>255)
                IMG_ERR("update_rgba_rect: components must be in the 0..255 range");
        }
        for (long row=0; row<rh; row++) {
            unsigned char *dst=b->rgba+(((size_t)(ry+row)*(size_t)b->width+(size_t)rx)*4u);
            const Value *sp=px+((size_t)row*(size_t)rw*4u);
            for (long i=0; i<rw*4; i++) dst[i]=(unsigned char)sp[i].as.integer;
        }
        fluxa_imgbuf_touch(b);
        return image_nil();
    }

    /* image.fill_tris(dst, depth, tris, count,
     *                 tex, tex_w, tex_h, tex_stride,
     *                 alpha, flags [, rgb]) → int   pixels written
     *
     * One call per texture, not per triangle: the whole point is to leave the
     * per-pixel loop in C, so a per-triangle call would put the cost straight
     * back into the interpreter.
     *
     *   tris   int arr, 15 ints per triangle: x, y, z, u, v per vertex, in
     *          screen space. u and v are 1/256 texel units, as the reference
     *          rasteriser used them.
     *   depth  int arr of width*height, or nil to draw with no depth test.
     *   tex    int arr of RGBA components, or nil for a flat colour.
     *   alpha  0..255, multiplied by the texel's own alpha.
     *   flags  FRONT 0x01, BACK 0x02, DEPTH_LESS 0x04, and the depth-write
     *          alpha threshold in bits 8..15.
     *   rgb    optional packed 0xRRGGBB used when tex is nil; white by default.
     */
    if (strcmp(fn_name,"fill_tris")==0) {
        NEED(10); GET_IMG(0,dst);

        const Value *dep = NULL;
        if (args[1].type == VAL_ARR && args[1].as.arr.data) {
            if ((size_t)args[1].as.arr.size !=
                (size_t)dst->width * (size_t)dst->height)
                IMG_ERR("fill_tris: depth must hold width*height entries");
            dep = args[1].as.arr.data;
        } else if (args[1].type != VAL_NIL) {
            IMG_ERR("fill_tris: depth must be an int arr or nil");
        }

        if (args[2].type != VAL_ARR || !args[2].as.arr.data)
            IMG_ERR("fill_tris: tris must be an int arr");
        GET_INT(3,tri_count);
        if (tri_count < 0) IMG_ERR("fill_tris: count must not be negative");
        if (tri_count > 0 &&
            (size_t)args[2].as.arr.size <
            (size_t)tri_count * (size_t)FLUXA_TRI_STRIDE)
            IMG_ERR("fill_tris: tris holds fewer than count*15 entries");
        const Value *tri = args[2].as.arr.data;

        GET_INT(5,tex_w); GET_INT(6,tex_h); GET_INT(7,tex_stride);
        const Value *tex = NULL;
        if (args[4].type == VAL_ARR && args[4].as.arr.data) {
            if (tex_w <= 0 || tex_h <= 0)
                IMG_ERR("fill_tris: tex_w and tex_h must be positive");
            if (tex_stride < tex_w)
                IMG_ERR("fill_tris: tex_stride must be at least tex_w");
            if ((size_t)args[4].as.arr.size <
                (size_t)tex_stride * (size_t)tex_h * 4u)
                IMG_ERR("fill_tris: tex holds fewer than tex_stride*tex_h*4 components");
            tex = args[4].as.arr.data;
        } else if (args[4].type != VAL_NIL) {
            IMG_ERR("fill_tris: tex must be an int arr or nil");
        }

        GET_INT(8,mat_alpha); GET_INT(9,flags);
        if (mat_alpha < 0 || mat_alpha > 255)
            IMG_ERR("fill_tris: alpha must be in the 0..255 range");
        long flat = 0xFFFFFF;
        if (argc >= 11) { GET_INT(10,rgbv); flat = rgbv; }
        const int flat_r = (int)((flat >> 16) & 0xFF);
        const int flat_g = (int)((flat >>  8) & 0xFF);
        const int flat_b = (int)( flat        & 0xFF);

        const int draw_front = (flags & FLUXA_TRI_FRONT) != 0;
        const int draw_back  = (flags & FLUXA_TRI_BACK)  != 0;
        const int depth_less = (flags & FLUXA_TRI_DEPTH_LESS) != 0;
        const long zthresh   = FLUXA_TRI_ZTHRESH(flags);

        const int W = dst->width, H = dst->height;
        unsigned char *fb = dst->rgba;
        long written = 0;

        for (long t = 0; t < tri_count; t++) {
            const Value *v = tri + t * FLUXA_TRI_STRIDE;
            long x0=fluxa_tri_i(v,0),  y0=fluxa_tri_i(v,1),  z0=fluxa_tri_i(v,2);
            long u0=fluxa_tri_i(v,3),  v0=fluxa_tri_i(v,4);
            long x1=fluxa_tri_i(v,5),  y1=fluxa_tri_i(v,6),  z1=fluxa_tri_i(v,7);
            long u1=fluxa_tri_i(v,8),  v1=fluxa_tri_i(v,9);
            long x2=fluxa_tri_i(v,10), y2=fluxa_tri_i(v,11), z2=fluxa_tri_i(v,12);
            long u2=fluxa_tri_i(v,13), v2=fluxa_tri_i(v,14);

            /* Screen y grows downward, so a negative signed area is the front
             * face — the same convention the reference rasteriser used. */
            long long area = (long long)(x1-x0)*(y2-y0) - (long long)(y1-y0)*(x2-x0);
            if (area == 0) continue;                       /* degenerate */
            if (area < 0 ? !draw_front : !draw_back) continue;

            long lox = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
            long hix = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
            long loy = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
            long hiy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
            if (lox < 0)   lox = 0;
            if (loy < 0)   loy = 0;
            if (hix > W-1) hix = W-1;
            if (hiy > H-1) hiy = H-1;

            for (long y = loy; y <= hiy; y++) {
                for (long x = lox; x <= hix; x++) {
                    long long w0 = (long long)(x2-x1)*(y-y1) - (long long)(y2-y1)*(x-x1);
                    long long w1 = (long long)(x0-x2)*(y-y2) - (long long)(y0-y2)*(x-x2);
                    long long w2 = (long long)(x1-x0)*(y-y0) - (long long)(y1-y0)*(x-x0);
                    if (area < 0) { if (w0 > 0 || w1 > 0 || w2 > 0) continue; }
                    else          { if (w0 < 0 || w1 < 0 || w2 < 0) continue; }

                    long pi = y * (long)W + x;
                    long zi = (long)(((long long)w0*z0 + (long long)w1*z1 +
                                      (long long)w2*z2) / area);
                    if (dep) {
                        long prev = fluxa_tri_i(dep, pi);
                        if (depth_less ? (zi >= prev) : (zi <= prev)) continue;
                    }

                    int sr, sg, sb, ta = 255;
                    if (tex) {
                        long ui = (long)(((long long)w0*u0 + (long long)w1*u1 +
                                          (long long)w2*u2) / area);
                        long vi = (long)(((long long)w0*v0 + (long long)w1*v1 +
                                          (long long)w2*v2) / area);
                        long tx = (ui / 256) % tex_w;  if (tx < 0) tx += tex_w;
                        long ty = (vi / 256) % tex_h;  if (ty < 0) ty += tex_h;
                        long src = (ty * tex_stride + tx) * 4;
                        sr = fluxa_tri_u8(tex, src);
                        sg = fluxa_tri_u8(tex, src+1);
                        sb = fluxa_tri_u8(tex, src+2);
                        ta = fluxa_tri_u8(tex, src+3);
                    } else { sr = flat_r; sg = flat_g; sb = flat_b; }

                    long a = (mat_alpha == 255) ? ta : (ta * mat_alpha / 255);
                    if (a <= 0) continue;

                    unsigned char *px = fb + pi * 4;
                    if (a >= 255) {
                        px[0]=(unsigned char)sr; px[1]=(unsigned char)sg;
                        px[2]=(unsigned char)sb;
                    } else {
                        long inv = 255 - a;
                        px[0]=(unsigned char)((sr*a + px[0]*inv) / 255);
                        px[1]=(unsigned char)((sg*a + px[1]*inv) / 255);
                        px[2]=(unsigned char)((sb*a + px[2]*inv) / 255);
                    }
                    px[3]=255;
                    written++;

                    if (dep && a > zthresh)
                        ((Value *)dep)[pi] = image_int(zi);
                }
            }
        }
        fluxa_imgbuf_touch(dst);
        return image_int(written);
    }

    /* image.fill_rect(dst, x, y, w, h, rgb [, alpha]) → int  pixels written
     * The same blend and the same clipping as fill_tris, for the case that
     * does not need a triangle. alpha defaults to opaque. */
    if (strcmp(fn_name,"fill_rect")==0) {
        NEED(6); GET_IMG(0,dst);
        GET_INT(1,rx); GET_INT(2,ry); GET_INT(3,rw); GET_INT(4,rh); GET_INT(5,rgbv);
        long a = 255;
        if (argc >= 7) { GET_INT(6,av); a = av; }
        if (a < 0 || a > 255) IMG_ERR("fill_rect: alpha must be in the 0..255 range");
        if (a == 0 || rw <= 0 || rh <= 0) return image_int(0);
        int sr=(int)((rgbv>>16)&0xFF), sg=(int)((rgbv>>8)&0xFF), sb=(int)(rgbv&0xFF);
        long x0=rx<0?0:rx, y0=ry<0?0:ry;
        long x1=rx+rw, y1=ry+rh;
        if (x1 > dst->width)  x1 = dst->width;
        if (y1 > dst->height) y1 = dst->height;
        long written = 0;
        for (long y=y0; y<y1; y++) {
            for (long x=x0; x<x1; x++) {
                unsigned char *px = dst->rgba + ((size_t)y*(size_t)dst->width+(size_t)x)*4u;
                if (a >= 255) { px[0]=(unsigned char)sr; px[1]=(unsigned char)sg; px[2]=(unsigned char)sb; }
                else {
                    long inv = 255 - a;
                    px[0]=(unsigned char)((sr*a + px[0]*inv)/255);
                    px[1]=(unsigned char)((sg*a + px[1]*inv)/255);
                    px[2]=(unsigned char)((sb*a + px[2]*inv)/255);
                }
                px[3]=255;
                written++;
            }
        }
        if (written) fluxa_imgbuf_touch(dst);
        return image_int(written);
    }

    /* image.fill_tri(dst, x0,y0, x1,y1, x2,y2, rgb [, alpha]) → int
     * One flat triangle, drawn whichever way it is wound — the winding rules
     * of fill_tris exist for depth-sorted geometry, and a lone 2D triangle
     * should not have to know about them. */
    if (strcmp(fn_name,"fill_tri")==0) {
        NEED(8); GET_IMG(0,dst);
        GET_INT(1,ax); GET_INT(2,ay); GET_INT(3,bx); GET_INT(4,by);
        GET_INT(5,cx); GET_INT(6,cy); GET_INT(7,rgbv);
        long a = 255;
        if (argc >= 9) { GET_INT(8,av); a = av; }
        if (a < 0 || a > 255) IMG_ERR("fill_tri: alpha must be in the 0..255 range");
        if (a == 0) return image_int(0);
        int sr=(int)((rgbv>>16)&0xFF), sg=(int)((rgbv>>8)&0xFF), sb=(int)(rgbv&0xFF);

        long long area = (long long)(bx-ax)*(cy-ay) - (long long)(by-ay)*(cx-ax);
        if (area == 0) return image_int(0);
        long lox = ax<bx?(ax<cx?ax:cx):(bx<cx?bx:cx);
        long hix = ax>bx?(ax>cx?ax:cx):(bx>cx?bx:cx);
        long loy = ay<by?(ay<cy?ay:cy):(by<cy?by:cy);
        long hiy = ay>by?(ay>cy?ay:cy):(by>cy?by:cy);
        if (lox < 0) lox = 0;
        if (loy < 0) loy = 0;
        if (hix > dst->width-1)  hix = dst->width-1;
        if (hiy > dst->height-1) hiy = dst->height-1;
        long written = 0;
        for (long y=loy; y<=hiy; y++) {
            for (long x=lox; x<=hix; x++) {
                long long w0 = (long long)(cx-bx)*(y-by) - (long long)(cy-by)*(x-bx);
                long long w1 = (long long)(ax-cx)*(y-cy) - (long long)(ay-cy)*(x-cx);
                long long w2 = (long long)(bx-ax)*(y-ay) - (long long)(by-ay)*(x-ax);
                if (area < 0) { if (w0 > 0 || w1 > 0 || w2 > 0) continue; }
                else          { if (w0 < 0 || w1 < 0 || w2 < 0) continue; }
                unsigned char *px = dst->rgba + ((size_t)y*(size_t)dst->width+(size_t)x)*4u;
                if (a >= 255) { px[0]=(unsigned char)sr; px[1]=(unsigned char)sg; px[2]=(unsigned char)sb; }
                else {
                    long inv = 255 - a;
                    px[0]=(unsigned char)((sr*a + px[0]*inv)/255);
                    px[1]=(unsigned char)((sg*a + px[1]*inv)/255);
                    px[2]=(unsigned char)((sb*a + px[2]*inv)/255);
                }
                px[3]=255;
                written++;
            }
        }
        if (written) fluxa_imgbuf_touch(dst);
        return image_int(written);
    }

    /* image.blit(dst, src, x, y [, mask]) → nil
     * Compose src onto dst at (x, y), alpha-blending by src's alpha channel so
     * transparent pixels don't overwrite the frame. Pixels landing outside dst
     * are clipped. The optional 5th argument is a mask image: where the mask's
     * alpha is 0 the src pixel is skipped, so a rounded/clipped frame shape
     * (the card's stepped corners) only shows through the mask. Without the mask
     * the blit is a plain rectangle. This is pure RGBA work — no codec needed,
     * so it runs on both backends and needs no danger block. */
    if (strcmp(fn_name,"blit")==0) {
        NEED(4); GET_IMG(0,dst); GET_IMG(1,src); GET_INT(2,dx); GET_INT(3,dy);

        FluxaImageBuf *mask = NULL;
        if (argc >= 5) {
            mask = image_unwrap(&args[4]);
            if (!fluxa_imgbuf_valid(mask)) IMG_ERR("blit: mask is not a live image handle");
            if (mask->width != src->width || mask->height != src->height)
                IMG_ERR("blit: mask size must match the source image");
        }

        for (int sy=0; sy<src->height; sy++) {
            int ty = (int)dy + sy;
            if (ty < 0 || ty >= dst->height) continue;
            for (int sx=0; sx<src->width; sx++) {
                int tx = (int)dx + sx;
                if (tx < 0 || tx >= dst->width) continue;

                const unsigned char *sp = src->rgba + ((size_t)sy*src->width + sx)*4u;
                int a = sp[3];

                /* mask gates the source by its own alpha, when provided */
                if (mask) {
                    const unsigned char *mp = mask->rgba + ((size_t)sy*mask->width + sx)*4u;
                    if (mp[3] == 0) continue;
                    a = (a * mp[3]) / 255;    /* combine src and mask coverage */
                }
                if (a == 0) continue;

                unsigned char *dp = dst->rgba + ((size_t)ty*dst->width + tx)*4u;
                /* standard source-over alpha blend */
                dp[0] = (unsigned char)((sp[0]*a + dp[0]*(255-a)) / 255);
                dp[1] = (unsigned char)((sp[1]*a + dp[1]*(255-a)) / 255);
                dp[2] = (unsigned char)((sp[2]*a + dp[2]*(255-a)) / 255);
                int da = dp[3] + a - (dp[3]*a)/255;   /* accumulate coverage */
                dp[3] = (unsigned char)(da > 255 ? 255 : da);
            }
        }
        fluxa_imgbuf_touch(dst);   /* pixels changed → invalidate any GPU cache */
        return image_nil();
    }

    /* image.save(img, path) → bool  (format by extension) — IO: needs danger */
    if (strcmp(fn_name,"save")==0) {
        NEED(2); GET_IMG(0,b); GET_STR(1,path);
        char ext[16]; image_ext(path,ext,sizeof(ext));
        if (!ext[0]) IMG_ERR("save: path has no file extension (use .png/.jpg/.bmp/.tga/.qoi)");
        int known = (!strcmp(ext,"png")||!strcmp(ext,"jpg")||!strcmp(ext,"jpeg")||
                     !strcmp(ext,"bmp")||!strcmp(ext,"tga")||!strcmp(ext,"qoi"));
        if (!known) IMG_ERR("save: unsupported format (use .png/.jpg/.bmp/.tga/.qoi)");
#ifdef FLUXA_IMAGE_RAYLIB
        {
            Image img; img.data=b->rgba; img.width=b->width; img.height=b->height;
            img.mipmaps=1; img.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            bool ok = ExportImage(img, path);   /* raylib picks the codec by extension */
            if (!ok) IMG_ERR("save: encode/write failed");
            return image_bool(1);
        }
#else
        (void)b; (void)path;
        IMG_ERR("save: no image codec in this build (rebuild with FLUXA_IMAGE_RAYLIB=1)");
#endif
    }

    /* image.set_text(path, key, text [, compress]) → bool
     * Embed a text field in an existing PNG as an iTXt chunk (e.g. the card's
     * cryptographic name / proof). Reads the PNG at `path`, splices the chunk in
     * before IEND, and rewrites the file. Without the 4th argument the text is
     * stored uncompressed (iTXt compression flag 0); pass a 4th argument to
     * deflate the text (flag 1) — useful for long proofs. `key` is the Latin-1
     * keyword (1–79 chars, e.g. "starfight-proof"); `text` is UTF-8.
     * PNG only. IO: needs a danger block. */
    if (strcmp(fn_name,"set_text")==0) {
        NEED(3); GET_STR(0,path); GET_STR(1,key); GET_STR(2,text);
        /* validate the key on both backends, before the codec check (like save) */
        {
            size_t klen0 = strlen(key);
            if (klen0 < 1 || klen0 > 79) IMG_ERR("set_text: key must be 1–79 characters");
        }
#ifdef FLUXA_IMAGE_RAYLIB
        {
            int want_compress = (argc >= 4) ? 1 : 0;

            size_t klen = strlen(key);

            /* read the whole PNG file */
            FILE *fp = fopen(path, "rb");
            if (!fp) IMG_ERR("set_text: cannot open PNG file");
            fseek(fp, 0, SEEK_END); long fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
            if (fsz < 57) { fclose(fp); IMG_ERR("set_text: file too small to be a PNG"); }
            unsigned char *png = (unsigned char *)malloc((size_t)fsz);
            if (!png) { fclose(fp); IMG_ERR("set_text: out of memory"); }
            if (fread(png, 1, (size_t)fsz, fp) != (size_t)fsz) { free(png); fclose(fp); IMG_ERR("set_text: read failed"); }
            fclose(fp);

            /* verify PNG signature */
            static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
            if (memcmp(png, sig, 8) != 0) { free(png); IMG_ERR("set_text: not a PNG file"); }

            /* locate the IEND chunk (last 12 bytes of a well-formed PNG) */
            if (memcmp(png + fsz - 8, "IEND", 4) != 0 &&
                memcmp(png + fsz - 12 + 4, "IEND", 4) != 0) {
                /* scan for IEND to be safe */
            }
            long iend_pos = -1;
            for (long i = 8; i + 8 <= fsz; ) {
                unsigned int clen = ((unsigned)png[i]<<24)|((unsigned)png[i+1]<<16)|
                                    ((unsigned)png[i+2]<<8)|((unsigned)png[i+3]);
                const unsigned char *ctype = png + i + 4;
                if (memcmp(ctype, "IEND", 4) == 0) { iend_pos = i; break; }
                i += 4 + 4 + (long)clen + 4;   /* len + type + data + crc */
            }
            if (iend_pos < 0) { free(png); IMG_ERR("set_text: malformed PNG (no IEND)"); }

            /* build the iTXt data payload:
             * keyword \0 comp_flag(1) comp_method(1) lang \0 trans_keyword \0 text[...] */
            unsigned char *textblob = (unsigned char *)text;
            size_t textlen = strlen(text);
            unsigned char *comp_buf = NULL;
            if (want_compress) {
                long n = fluxa_deflate((const unsigned char *)text, textlen, &comp_buf);
                if (n < 0) { free(png); IMG_ERR("set_text: text compression failed"); }
                textblob = comp_buf; textlen = (size_t)n;
            }

            /* data length: key + \0 + 1 + 1 + \0(lang) + \0(trans) + text */
            size_t dlen = klen + 1 + 1 + 1 + 1 + 1 + textlen;
            unsigned char *data = (unsigned char *)malloc(dlen);
            if (!data) { free(comp_buf); free(png); IMG_ERR("set_text: out of memory"); }
            size_t o = 0;
            memcpy(data+o, key, klen); o += klen;
            data[o++] = 0;                       /* null after keyword */
            data[o++] = (unsigned char)want_compress; /* compression flag */
            data[o++] = 0;                       /* compression method (0=zlib) */
            data[o++] = 0;                       /* empty language tag + null */
            data[o++] = 0;                       /* empty translated keyword + null */
            memcpy(data+o, textblob, textlen); o += textlen;

            /* full chunk = len(4) + "iTXt"(4) + data + crc(4) */
            size_t chunk_sz = 4 + 4 + dlen + 4;
            unsigned char *chunk = (unsigned char *)malloc(chunk_sz);
            if (!chunk) { free(data); free(comp_buf); free(png); IMG_ERR("set_text: out of memory"); }
            fluxa_put_be32(chunk, (unsigned int)dlen);
            memcpy(chunk+4, "iTXt", 4);
            memcpy(chunk+8, data, dlen);
            unsigned int crc = fluxa_png_crc(chunk+4, 4 + dlen);   /* over type+data */
            fluxa_put_be32(chunk + 8 + dlen, crc);

            /* write: [png up to IEND] + [iTXt chunk] + [IEND to end] */
            FILE *out = fopen(path, "wb");
            if (!out) { free(chunk); free(data); free(comp_buf); free(png); IMG_ERR("set_text: cannot rewrite PNG"); }
            fwrite(png, 1, (size_t)iend_pos, out);
            fwrite(chunk, 1, chunk_sz, out);
            fwrite(png + iend_pos, 1, (size_t)(fsz - iend_pos), out);
            fclose(out);

            free(chunk); free(data); free(comp_buf); free(png);
            return image_bool(1);
        }
#else
        (void)path; (void)key; (void)text;
        IMG_ERR("set_text: no image codec in this build (rebuild with FLUXA_IMAGE_RAYLIB=1)");
#endif
    }

    /* image.get_text(path, key) → str
     * Defensive PNG iTXt reader. The file is treated as hostile input: chunk
     * boundaries and CRCs are validated, iTXt/text sizes are bounded, compressed
     * text is inflated with a hard output cap, and returned text must be valid
     * UTF-8. Returns "" only when a structurally valid PNG has no matching key.
     * Does not decode pixels. PNG only. IO: needs a danger block. */
    if (strcmp(fn_name,"get_text")==0) {
        NEED(2); GET_STR(0,path); GET_STR(1,key);
        {
            size_t klen0 = strlen(key);
            if (klen0 < 1 || klen0 > 79) IMG_ERR("get_text: key must be 1–79 characters");
        }
#ifdef FLUXA_IMAGE_RAYLIB
        {
            FILE *fp = fopen(path, "rb");
            if (!fp) IMG_ERR("get_text: cannot open PNG file");

            static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
            unsigned char got_sig[8];
            if (fread(got_sig, 1, sizeof(got_sig), fp) != sizeof(got_sig)) {
                fclose(fp); IMG_ERR("get_text: file too small to be a PNG");
            }
            if (memcmp(got_sig, sig, 8) != 0) {
                fclose(fp); IMG_ERR("get_text: not a PNG file");
            }

            size_t scanned = 8;
            size_t want_klen = strlen(key);
            char *found = NULL;
            int saw_ihdr = 0, saw_iend = 0;
            unsigned int chunk_index = 0;

            while (!saw_iend) {
                unsigned char head[8];
                if (fread(head, 1, sizeof(head), fp) != sizeof(head)) {
                    free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (truncated chunk header)");
                }
                scanned += sizeof(head);
                if (scanned > FLUXA_IMAGE_GET_TEXT_MAX_FILE_BYTES) {
                    free(found); fclose(fp); IMG_ERR("get_text: PNG exceeds metadata scan limit");
                }

                unsigned int clen = ((unsigned int)head[0] << 24) |
                                    ((unsigned int)head[1] << 16) |
                                    ((unsigned int)head[2] << 8)  |
                                    (unsigned int)head[3];
                const unsigned char *ctype = head + 4;

                if (chunk_index == 0) {
                    if (memcmp(ctype, "IHDR", 4) != 0 || clen != 13u) {
                        free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (invalid IHDR)");
                    }
                    saw_ihdr = 1;
                } else if (memcmp(ctype, "IHDR", 4) == 0) {
                    free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (duplicate IHDR)");
                }

                if (scanned > FLUXA_IMAGE_GET_TEXT_MAX_FILE_BYTES - 4u ||
                    (size_t)clen > FLUXA_IMAGE_GET_TEXT_MAX_FILE_BYTES - scanned - 4u) {
                    free(found); fclose(fp); IMG_ERR("get_text: PNG exceeds metadata scan limit");
                }
                if (memcmp(ctype, "iTXt", 4) == 0 &&
                    (size_t)clen > FLUXA_IMAGE_GET_TEXT_MAX_ITXT_BYTES) {
                    free(found); fclose(fp); IMG_ERR("get_text: iTXt chunk exceeds limit");
                }

                unsigned char *idata = NULL;
                if (memcmp(ctype, "iTXt", 4) == 0) {
                    idata = (unsigned char *)malloc((size_t)clen ? (size_t)clen : 1u);
                    if (!idata) { free(found); fclose(fp); IMG_ERR("get_text: out of memory"); }
                }

                unsigned int crc = 0xFFFFFFFFu;
                for (int j=0; j<4; j++) {
                    crc ^= ctype[j];
                    for (int k=0; k<8; k++) crc = (crc & 1u) ? (FLUXA_CRC_POLY ^ (crc >> 1)) : (crc >> 1);
                }

                size_t done = 0;
                unsigned char scratch[4096];
                while (done < (size_t)clen) {
                    size_t need = (size_t)clen - done;
                    if (need > sizeof(scratch)) need = sizeof(scratch);
                    if (fread(scratch, 1, need, fp) != need) {
                        free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (truncated chunk)");
                    }
                    if (idata) memcpy(idata + done, scratch, need);
                    for (size_t j=0; j<need; j++) {
                        crc ^= scratch[j];
                        for (int k=0; k<8; k++) crc = (crc & 1u) ? (FLUXA_CRC_POLY ^ (crc >> 1)) : (crc >> 1);
                    }
                    done += need;
                }
                crc ^= 0xFFFFFFFFu;
                scanned += (size_t)clen;

                unsigned char crcbuf[4];
                if (fread(crcbuf, 1, sizeof(crcbuf), fp) != sizeof(crcbuf)) {
                    free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (missing CRC)");
                }
                scanned += sizeof(crcbuf);
                unsigned int stored_crc = ((unsigned int)crcbuf[0] << 24) |
                                          ((unsigned int)crcbuf[1] << 16) |
                                          ((unsigned int)crcbuf[2] << 8)  |
                                          (unsigned int)crcbuf[3];
                if (crc != stored_crc) {
                    free(idata); free(found); fclose(fp); IMG_ERR("get_text: PNG chunk CRC mismatch");
                }

                if (idata) {
                    size_t dlen = (size_t)clen;
                    size_t ko = 0;
                    while (ko < dlen && idata[ko] != 0) ko++;
                    if (ko == dlen || ko < 1 || ko > 79) {
                        free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed iTXt keyword");
                    }

                    size_t o = ko + 1u;
                    if (o + 2u > dlen) {
                        free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed iTXt chunk");
                    }
                    unsigned int comp_flag = idata[o++];
                    unsigned int comp_method = idata[o++];
                    if (comp_flag > 1u || comp_method != 0u) {
                        free(idata); free(found); fclose(fp); IMG_ERR("get_text: unsupported iTXt compression");
                    }

                    while (o < dlen && idata[o] != 0) o++; /* language tag */
                    if (o >= dlen) { free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed iTXt chunk"); }
                    o++;
                    while (o < dlen && idata[o] != 0) o++; /* translated keyword */
                    if (o >= dlen) { free(idata); free(found); fclose(fp); IMG_ERR("get_text: malformed iTXt chunk"); }
                    o++;

                    if (!found && ko == want_klen && memcmp(idata, key, want_klen) == 0) {
                        size_t text_len = dlen - o;
                        unsigned char *txt = NULL;
                        size_t out_len = 0;

                        if (comp_flag == 0u) {
                            if (text_len > FLUXA_IMAGE_GET_TEXT_MAX_TEXT_BYTES) {
                                free(idata); free(found); fclose(fp); IMG_ERR("get_text: iTXt text exceeds limit");
                            }
                            txt = (unsigned char *)malloc(text_len + 1u);
                            if (!txt) { free(idata); free(found); fclose(fp); IMG_ERR("get_text: out of memory"); }
                            memcpy(txt, idata + o, text_len);
                            txt[text_len] = 0;
                            out_len = text_len;
                        } else {
                            int irc = fluxa_inflate_text_bounded(idata + o, text_len,
                                                                FLUXA_IMAGE_GET_TEXT_MAX_TEXT_BYTES,
                                                                &txt, &out_len);
                            if (irc == -2) {
                                free(idata); free(found); fclose(fp); IMG_ERR("get_text: decompressed iTXt text exceeds limit");
                            }
                            if (irc != 0 || !txt) {
                                free(txt); free(idata); free(found); fclose(fp); IMG_ERR("get_text: text decompression failed");
                            }
                        }

                        if (!fluxa_utf8_valid_text(txt, out_len)) {
                            free(txt); free(idata); free(found); fclose(fp); IMG_ERR("get_text: iTXt text is not valid UTF-8");
                        }
                        found = (char *)txt;
                    }
                    free(idata);
                }

                if (memcmp(ctype, "IEND", 4) == 0) {
                    if (clen != 0u) { free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (invalid IEND)"); }
                    saw_iend = 1;
                }
                chunk_index++;
            }

            if (!saw_ihdr || !saw_iend) {
                free(found); fclose(fp); IMG_ERR("get_text: malformed PNG");
            }
            if (fgetc(fp) != EOF) {
                free(found); fclose(fp); IMG_ERR("get_text: malformed PNG (data after IEND)");
            }
            fclose(fp);

            if (!found) return image_str("");
            Value ret = image_str(found);
            free(found);
            return ret;
        }
#else
        (void)path; (void)key;
        IMG_ERR("get_text: no image codec in this build (rebuild with FLUXA_IMAGE_RAYLIB=1)");
#endif
    }

    /* image.load(path) → dyn  (decode a file) — IO: needs danger */
    if (strcmp(fn_name,"load")==0) {
        NEED(1); GET_STR(0,path);
#ifdef FLUXA_IMAGE_RAYLIB
        {
            Image img = LoadImage(path);
            if (img.data==NULL || img.width<=0 || img.height<=0) {
                if (img.data) UnloadImage(img);
                IMG_ERR("load: could not read or decode file");
            }
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            FluxaImageBuf *b = fluxa_imgbuf_from_rgba((const unsigned char *)img.data,
                                                      img.width, img.height);
            UnloadImage(img);
            if (!b) IMG_ERR("load: out of memory");
            return image_wrap(b);
        }
#else
        (void)path;
        IMG_ERR("load: no image codec in this build (rebuild with FLUXA_IMAGE_RAYLIB=1)");
#endif
    }

    /* image.sload(path[, max_bytes[, max_edge]]) → dyn   SECURE load: validates
     * the file BEFORE and around the decode, to shrink the attack surface of
     * untrusted images (e.g. a card PNG dropped into the gallery folder, or a
     * swapped capture). It is not a guarantee against every decoder bug — the
     * underlying stb_image parser still runs — but it rejects the common hostile
     * inputs (oversized files, wrong formats, absurd dimensions) before they
     * reach deep parsing. Use this for any image whose bytes are not fully under
     * the program's control.
     *
     * The optional limits let a caller tighten the bounds to what its own images
     * should never exceed — e.g. a game card is tens of KB and ~1024 px, so
     * sload(path, 262144, 1200) is far stricter than the defaults and rejects
     * more. Omit them for a safe generic default; a non-positive value falls back
     * to the default for that bound.
     *
     * The file checks (size, magic bytes) run before the codec #ifdef, mirroring
     * save's validation-first shape, so they hold in the stub build too. Only the
     * actual decode needs the codec. */
    if (strcmp(fn_name,"sload")==0) {
        NEED(1); GET_STR(0,path);

        /* Defaults: 24 MB covers any legitimate card render with room to spare
         * while blocking decompression bombs; 8192 is a generous max edge. A
         * caller who knows its images are smaller can pass tighter bounds. */
        long max_bytes = 24L * 1024L * 1024L;
        long max_edge  = 8192L;
        if (argc >= 2) {
            if (args[1].type != VAL_INT) IMG_ERR("sload: max_bytes must be int");
            if (args[1].as.integer > 0) max_bytes = args[1].as.integer;
        }
        if (argc >= 3) {
            if (args[2].type != VAL_INT) IMG_ERR("sload: max_edge must be int");
            if (args[2].as.integer > 0) max_edge = args[2].as.integer;
        }

        /* 1) File size, before opening for decode. Rejects absurd/bomb files
         *    without letting the decoder touch them. */
        FILE *fp = fopen(path, "rb");
        if (!fp) IMG_ERR("sload: could not open file");
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); IMG_ERR("sload: cannot size file"); }
        long fsize = ftell(fp);
        if (fsize < 0) { fclose(fp); IMG_ERR("sload: cannot size file"); }
        if (fsize == 0) { fclose(fp); IMG_ERR("sload: empty file"); }
        if (fsize > max_bytes) { fclose(fp); IMG_ERR("sload: file too large"); }

        /* 2) Magic bytes. Read the first bytes and accept only known-good
         *    signatures (PNG or QOI — the two formats the game produces).
         *    Anything else is rejected before decode. */
        unsigned char magic[8] = {0};
        if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); IMG_ERR("sload: cannot read header"); }
        size_t got = fread(magic, 1, sizeof(magic), fp);
        fclose(fp);
        if (got < 4) IMG_ERR("sload: file too short to identify");
        {
            int is_png = (got >= 8 && magic[0]==0x89 && magic[1]==0x50 &&
                          magic[2]==0x4E && magic[3]==0x47 && magic[4]==0x0D &&
                          magic[5]==0x0A && magic[6]==0x1A && magic[7]==0x0A);
            int is_qoi = (magic[0]==0x71 && magic[1]==0x6F &&
                          magic[2]==0x69 && magic[3]==0x66); /* 'qoif' */
            if (!is_png && !is_qoi) IMG_ERR("sload: unsupported or unexpected format");
        }
        (void)max_edge; /* used only in the codec branch below; keep stub warning-free */
#ifdef FLUXA_IMAGE_RAYLIB
        {
            /* 3) Decode, then bound the dimensions before we format/allocate. A
             *    header can claim huge dimensions; cap them so ImageFormat and the
             *    RGBA copy can't be driven to exhaust memory. */
            Image img = LoadImage(path);
            if (img.data==NULL || img.width<=0 || img.height<=0) {
                if (img.data) UnloadImage(img);
                IMG_ERR("sload: could not read or decode file");
            }
            if ((long)img.width > max_edge || (long)img.height > max_edge) {
                UnloadImage(img);
                IMG_ERR("sload: image dimensions exceed the allowed maximum");
            }
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            FluxaImageBuf *b = fluxa_imgbuf_from_rgba((const unsigned char *)img.data,
                                                      img.width, img.height);
            UnloadImage(img);
            if (!b) IMG_ERR("sload: out of memory");
            return image_wrap(b);
        }
#else
        IMG_ERR("sload: no image codec in this build (rebuild with FLUXA_IMAGE_RAYLIB=1)");
#endif
    }

#undef IMG_ERR
#undef NEED
#undef GET_INT
#undef GET_STR
#undef GET_IMG

    snprintf(errbuf,sizeof(errbuf),"image.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"image",line);
    *had_error=1;
    return image_nil();
}

FLUXA_LIB_EXPORT(
    name     = "image",
    toml_key = "std.image",
    owner    = "image",
    call     = fluxa_std_image_call,
    rt_aware = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_IMAGE_H */
