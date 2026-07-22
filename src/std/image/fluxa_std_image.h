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
 *   image.blit(dst,src,x,y[,m]) → nil   compose src onto dst; optional mask image
 *   image.width(img)           → int
 *   image.height(img)          → int
 *   image.set_text(path,k,t[,c])→ bool  embed PNG iTXt metadata; optional compress
 *   image.discard(img)         → nil   release the buffer (also frees graph.capture)
 *   image.version()            → str
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "../../scope.h"
#include "../../err.h"
#include "../fluxa_image_buffer.h"

#ifdef FLUXA_IMAGE_RAYLIB
#include <raylib.h>
#endif

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
