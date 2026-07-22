#ifndef FLUXA_IMAGE_BUFFER_H
#define FLUXA_IMAGE_BUFFER_H

/* fluxa_image_buffer.h — a backend-neutral raw image, shared by std.graph
 * (which produces one via capture) and std.image (which resizes/exports it).
 *
 * Neither lib depends on the other, and neither type here is a Raylib type:
 * pixels are always 32-bit RGBA, tightly packed, row-major, top-left origin.
 * std.graph fills this from a RenderTexture; std.image reads it to encode PNG,
 * JPG, BMP, TGA, or QOI. Keeping the wire format neutral is what lets the two
 * libs stay decoupled (a capture from graph is just a byte buffer to image).
 *
 * The struct is wrapped in a Fluxa dyn as a VAL_PTR, so the script side holds
 * an opaque `dyn` handle and never sees these bytes directly. Ownership: the
 * lib that creates the buffer owns it until the script calls the matching
 * release (image.free); freeing sets data = NULL so double free is a no-op.
 */

#include <stdlib.h>
#include <string.h>

#define FLUXA_IMG_MAGIC 0x464C5849u   /* 'FLXI' — sanity tag on the handle */

typedef struct {
    unsigned int   magic;   /* FLUXA_IMG_MAGIC while the buffer is live */
    int            width;   /* pixels across */
    int            height;  /* pixels down   */
    unsigned char *rgba;    /* width*height*4 bytes, or NULL once freed */
} FluxaImageBuf;

/* Allocate a zeroed RGBA buffer of w x h. Returns NULL on bad size or OOM. */
static inline FluxaImageBuf *fluxa_imgbuf_new(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    /* guard against overflow in w*h*4 */
    long px = (long)w * (long)h;
    if (px <= 0 || px > (long)(1 << 28)) return NULL;   /* cap ~268M px */
    FluxaImageBuf *b = (FluxaImageBuf *)calloc(1, sizeof(FluxaImageBuf));
    if (!b) return NULL;
    b->rgba = (unsigned char *)calloc((size_t)px * 4u, 1);
    if (!b->rgba) { free(b); return NULL; }
    b->magic  = FLUXA_IMG_MAGIC;
    b->width  = w;
    b->height = h;
    return b;
}

/* Wrap existing RGBA bytes (copied in). Returns NULL on bad args or OOM. */
static inline FluxaImageBuf *fluxa_imgbuf_from_rgba(const unsigned char *src,
                                                    int w, int h) {
    FluxaImageBuf *b = fluxa_imgbuf_new(w, h);
    if (!b) return NULL;
    if (src) memcpy(b->rgba, src, (size_t)w * (size_t)h * 4u);
    return b;
}

/* True if the handle points at a live buffer. */
static inline int fluxa_imgbuf_valid(const FluxaImageBuf *b) {
    return b && b->magic == FLUXA_IMG_MAGIC && b->rgba && b->width > 0 && b->height > 0;
}

/* Release the buffer. Safe to call twice (idempotent). */
static inline void fluxa_imgbuf_free(FluxaImageBuf *b) {
    if (!b) return;
    if (b->rgba) { free(b->rgba); b->rgba = NULL; }
    b->magic = 0;
    b->width = 0;
    b->height = 0;
    free(b);
}

#endif /* FLUXA_IMAGE_BUFFER_H */
