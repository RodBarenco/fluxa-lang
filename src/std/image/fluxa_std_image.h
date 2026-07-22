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
 *   image.width(img)           → int
 *   image.height(img)          → int
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

/* ── Value constructors ─────────────────────────────────────────── */
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
