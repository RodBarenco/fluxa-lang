#ifndef FLUXA_STD_VIDEO_H
#define FLUXA_STD_VIDEO_H

/* fluxa_std_video.h — MP4/H.264 writing and reading.
 *
 *   video.open(path, w, h, fps)          → dyn   start writing
 *   video.frame(v, img)                  → nil   append one RGBA frame
 *   video.audio(v, path)                 → nil   remux an .aac/.mp3 track
 *   video.subtitle(v, start, end, text)  → nil   queue a subtitle cue
 *   video.close(v)                       → nil   finalise the file
 *
 *   video.play_open(path)                → dyn   start reading
 *   video.play_frame(v)                  → dyn   next frame, or nil at the end
 *   video.info(v)                        → dyn   [w, h, fps, frames]
 *   video.play_close(v)                  → nil
 *   video.version()                      → str
 *
 * Frames are the same FluxaImageBuf that graph.capture produces and std.image
 * consumes, so a render loop can go straight to disk with no conversion in the
 * script. RGBA↔I420 happens here.
 *
 * Backends
 * --------
 * Default: minimp4 (mux/demux, CC0) + minih264e (encode, CC0) + h264bsd
 * (decode, Apache-2.0, from AOSP). All plain C99, no external dependency, and
 * they cross-compile to MinGW and bare-metal ARM. That is the whole point of
 * this choice: the common formats without dragging in ffmpeg.
 *
 * Audio is remuxed, never re-encoded. There is no lightweight AAC encoder with
 * a clean licence, and the real use — "I have a soundtrack, put it in the
 * video" — does not need one: the frames are copied across verbatim, which is
 * both faster and lossless.
 *
 * Subtitles are written as a .srt file beside the video. minimp4 has no timed
 * text track, and a sidecar is what every player, browser and editor already
 * reads.
 *
 * Safety
 * ------
 * Decoding untrusted video is a classic attack surface, so nothing here sizes
 * an allocation from an unvalidated field. Dimensions, frame counts and NAL
 * sizes are checked against caps before any malloc, and h264bsd is
 * baseline-only, which is a far smaller surface than a full decoder. A
 * malformed file raises an ordinary Fluxa error inside danger {}.
 *
 * Release contract
 * ----------------
 * play_frame allocates a full RGBA frame per call and the caller must release
 * it with image.discard. This is not a nicety: the collector runs at a safe
 * point (call_depth == 0 && danger_depth == 0) and a playback loop never
 * reaches one, so a frame that is not discarded stays resident until the loop
 * ends. At 1920x1080 that is 8 MB per iteration.
 *
 * The same contract every stateful lib here follows — pg.free_result,
 * json2.discard, sqlite.close, image.discard — applies: whatever asked for the
 * payload releases it, and the release nulls the pointer inside the dyn so a
 * second call is a no-op rather than a double free. Only the thin FluxaDyn
 * wrapper is left to the collector.
 *
 * info() likewise returns an allocated dyn; call it once outside the loop, or
 * free() the result.
 *
 * All IO functions must run inside a danger {} block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../scope.h"
#include "../../err.h"
#include "../fluxa_image_buffer.h"

#ifdef FLUXA_STD_VIDEO

#include "minimp4.h"
#define H264E_MAX_THREADS 0
#include "minih264e.h"
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

/* ── Caps ────────────────────────────────────────────────────────────
 * Every one of these bounds a decision made from file contents. They are
 * generous for real work and still keep a hostile file from asking for an
 * enormous allocation. */
#define VID_MAX_DIM        7680        /* 8K wide/tall                        */
#define VID_MAX_PIXELS     (7680*4320) /* guards w*h overflow before malloc   */
#define VID_MAX_FRAMES     216000      /* 1 hour at 60 fps                    */
#define VID_MAX_NAL        (16u*1024u*1024u)  /* one compressed frame         */
#define VID_MAX_AUDIO      (256u*1024u*1024u) /* audio track file size        */
#define VID_MAX_SUBS       10000       /* subtitle cues                       */
#define VID_MAX_SUB_TEXT   1024        /* bytes per cue                       */
#define VID_MAGIC          0x46565844u /* 'FVXD' — live cursor tag            */

typedef struct {
    double start, end;
    char  *text;
} VidSub;

typedef struct {
    unsigned int magic;
    /* writer */
    FILE            *fp;
    MP4E_mux_t      *mux;
    mp4_h26x_writer_t wtr;
    H264E_persist_t *enc;
    H264E_scratch_t *scr;
    int   width, height, fps;
    int   frames;
    int   wtr_ready, enc_ready;
    unsigned char *yuv;         /* reused I420 scratch — one alloc per file */
    char *path;
    VidSub *subs; int sub_count, sub_cap;
    /* reader */
    MP4D_demux_t demux;
    int   demux_ready;
    storage_t *dec;
    int   dec_ready, hdrs_done;
    int   vtrack;        /* which demux track carries the video */
    unsigned cur_sample; /* next sample to hand to the decoder    */
} VidCtx;

/* ── RGBA → I420 ─────────────────────────────────────────────────────
 * BT.601 studio range, the coefficients H.264 expects. Chroma is sampled at
 * the even pixel of each 2x2 block rather than averaged: it is what the
 * encoder's own test harness does, it is measurably cheaper per frame, and the
 * difference is invisible at video bitrates. */
static void vid_rgba_to_i420(const unsigned char *rgba, int w, int h,
                              unsigned char *yuv) {
    unsigned char *Y = yuv;
    unsigned char *U = yuv + (size_t)w * (size_t)h;
    unsigned char *V = U + ((size_t)w * (size_t)h) / 4u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            int r = p[0], g = p[1], b = p[2];
            Y[(size_t)y*(size_t)w + (size_t)x] =
                (unsigned char)(((66*r + 129*g + 25*b + 128) >> 8) + 16);
            if (!(y & 1) && !(x & 1)) {
                size_t ci = (size_t)(y/2) * (size_t)(w/2) + (size_t)(x/2);
                U[ci] = (unsigned char)(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
                V[ci] = (unsigned char)(((112*r - 94*g - 18*b + 128) >> 8) + 128);
            }
        }
    }
}

/* ── I420 → RGBA ─────────────────────────────────────────────────────
 * The inverse, for playback. Clamped because the conversion can overshoot on
 * saturated colours. */
static void vid_i420_to_rgba(const unsigned char *Y, const unsigned char *U,
                              const unsigned char *V, int sy, int suv,
                              int w, int h, unsigned char *rgba) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int c = (int)Y[(size_t)y*(size_t)sy + (size_t)x] - 16;
            int d = (int)U[(size_t)(y/2)*(size_t)suv + (size_t)(x/2)] - 128;
            int e = (int)V[(size_t)(y/2)*(size_t)suv + (size_t)(x/2)] - 128;
            int r = (298*c + 409*e + 128) >> 8;
            int g = (298*c - 100*d - 208*e + 128) >> 8;
            int b = (298*c + 516*d + 128) >> 8;
            unsigned char *p = rgba + ((size_t)y*(size_t)w + (size_t)x) * 4u;
            p[0] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p[1] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p[2] = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
            p[3] = 255;
        }
    }
}

static int vid_write_cb(int64_t offset, const void *buf, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 1;
    return fwrite(buf, 1, size, f) != size;
}
static int vid_read_cb(int64_t offset, void *buf, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 1;
    return fread(buf, 1, size, f) != size;
}

/* Write the queued cues as SubRip beside the video: same path with .srt. */
static void vid_write_srt(VidCtx *v) {
    if (!v->subs || v->sub_count <= 0 || !v->path) return;
    size_t n = strlen(v->path);
    char *sp = (char *)malloc(n + 8);
    if (!sp) return;
    memcpy(sp, v->path, n);
    /* replace the extension, or append when there is none */
    size_t cut = n;
    for (size_t i = n; i > 0; i--) {
        if (v->path[i-1] == '/' || v->path[i-1] == '\\') break;
        if (v->path[i-1] == '.') { cut = i - 1; break; }
    }
    memcpy(sp + cut, ".srt", 5);
    FILE *f = fopen(sp, "wb");
    free(sp);
    if (!f) return;
    for (int i = 0; i < v->sub_count; i++) {
        double s = v->subs[i].start, e = v->subs[i].end;
        int sh = (int)(s/3600), sm = (int)(s/60) % 60, ss = (int)s % 60;
        int sms = (int)((s - (double)(long)s) * 1000.0 + 0.5);
        int eh = (int)(e/3600), em = (int)(e/60) % 60, es = (int)e % 60;
        int ems = (int)((e - (double)(long)e) * 1000.0 + 0.5);
        fprintf(f, "%d\r\n%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\r\n%s\r\n\r\n",
                i + 1, sh, sm, ss, sms, eh, em, es, ems,
                v->subs[i].text ? v->subs[i].text : "");
    }
    fclose(f);
}

static void vid_free(VidCtx *v) {
    if (!v) return;
    if (v->subs) {
        for (int i = 0; i < v->sub_count; i++) free(v->subs[i].text);
        free(v->subs);
        v->subs = NULL;
    }
    free(v->yuv);   v->yuv = NULL;
    free(v->enc);   v->enc = NULL;
    free(v->scr);   v->scr = NULL;
    free(v->path);  v->path = NULL;
    if (v->dec_ready)   { h264bsdShutdown(v->dec); h264bsdFree(v->dec); v->dec_ready = 0; }
    if (v->demux_ready) { MP4D_close(&v->demux); v->demux_ready = 0; }
    if (v->fp) { fclose(v->fp); v->fp = NULL; }
    v->magic = 0;
    free(v);
}

/* ── Value helpers ───────────────────────────────────────────────────── */
static inline Value video_nil(void)   { Value v; v.type=VAL_NIL;  return v; }
static inline Value video_int(long n) { Value v; v.type=VAL_INT;  v.as.integer=n; return v; }
static inline Value video_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=fxstr_new(s?s:""); return v; }

static inline Value video_wrap(VidCtx *v) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=v;
    d->count=1; d->cap=1;
    Value r; r.type=VAL_DYN; r.as.dyn=d; return r;
}
static inline Value video_wrap_img(FluxaImageBuf *b) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=b;
    d->count=1; d->cap=1;
    Value r; r.type=VAL_DYN; r.as.dyn=d; return r;
}

#endif /* FLUXA_STD_VIDEO */

/* ── Dispatch ────────────────────────────────────────────────────────── */
static inline Value fluxa_std_video_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define VID_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"video.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"video",line); \
    *had_error=1; return video_nil(); } while(0)

#define VID_NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"video.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"video",line); \
    *had_error=1; return video_nil(); } } while(0)

#define VID_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) VID_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

#define VID_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) VID_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

#define VID_NUM(idx,var) \
    if(args[(idx)].type!=VAL_INT && args[(idx)].type!=VAL_FLOAT) \
        VID_ERR("expected int or float"); \
    double (var)=(args[(idx)].type==VAL_INT)?(double)args[(idx)].as.integer \
                                            :args[(idx)].as.real;

#ifndef FLUXA_STD_VIDEO
    (void)args; (void)argc;
    if (!strcmp(fn_name,"version")) {
        Value v; v.type=VAL_STRING; v.as.string=fxstr_new("stub (no codec)"); return v;
    }
    snprintf(errbuf,sizeof(errbuf),
        "video.%s: std.video was not compiled into this binary "
        "(set std.video = true in fluxa.libs and rebuild)", fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"video",line);
    *had_error=1;
    { Value v; v.type=VAL_NIL; return v; }
#else

#define VID_CTX(idx,var) \
    if(args[(idx)].type!=VAL_DYN||!args[(idx)].as.dyn||args[(idx)].as.dyn->count<1|| \
       args[(idx)].as.dyn->items[0].type!=VAL_PTR||!args[(idx)].as.dyn->items[0].as.ptr) \
        VID_ERR("invalid video cursor — use video.open() or video.play_open()"); \
    VidCtx *(var)=(VidCtx *)args[(idx)].as.dyn->items[0].as.ptr; \
    if((var)->magic!=VID_MAGIC) VID_ERR("video cursor is already closed");

    if (!strcmp(fn_name,"version")) {
        return video_str("minimp4 + minih264e (encode) + h264bsd (decode)");
    }

    /* video.open(path, w, h, fps) → dyn */
    if (!strcmp(fn_name,"open")) {
        VID_NEED(4);
        VID_STR(0,path); VID_INT(1,w); VID_INT(2,h); VID_INT(3,fps);
        if (w <= 0 || h <= 0)        VID_ERR("open: width and height must be positive");
        if (w > VID_MAX_DIM || h > VID_MAX_DIM) VID_ERR("open: dimensions exceed 7680 px");
        if ((long)w * (long)h > VID_MAX_PIXELS) VID_ERR("open: frame is too large");
        /* H.264 chroma is subsampled 2x2 — odd dimensions have no valid encoding. */
        if ((w & 1) || (h & 1))      VID_ERR("open: width and height must be even");
        if (fps <= 0 || fps > 240)   VID_ERR("open: fps must be between 1 and 240");

        VidCtx *v = (VidCtx *)calloc(1, sizeof(VidCtx));
        if (!v) VID_ERR("open: out of memory");
        v->magic = VID_MAGIC;
        v->width = (int)w; v->height = (int)h; v->fps = (int)fps;
        v->path = (char *)malloc(strlen(path) + 1);
        if (!v->path) { vid_free(v); VID_ERR("open: out of memory"); }
        strcpy(v->path, path);

        v->fp = fopen(path, "wb");
        if (!v->fp) { vid_free(v); VID_ERR("open: cannot create the output file"); }

        v->mux = MP4E_open(0, 0, v->fp, vid_write_cb);
        if (!v->mux) { vid_free(v); VID_ERR("open: could not start the MP4 container"); }
        if (mp4_h26x_write_init(&v->wtr, v->mux, (int)w, (int)h, 0)) {
            vid_free(v); VID_ERR("open: could not start the H.264 track");
        }
        v->wtr_ready = 1;

        H264E_create_param_t cp; memset(&cp, 0, sizeof(cp));
        cp.width  = (int)w;
        cp.height = (int)h;
        cp.gop    = (int)fps * 2;      /* a keyframe every two seconds */
        cp.max_long_term_reference_frames = 0;
        cp.fine_rate_control_flag = 0;
        cp.const_input_flag       = 1; /* we own the yuv buffer between calls */
        cp.vbv_size_bytes         = 100000;
        int np = 0, ns = 0;
        if (H264E_sizeof(&cp, &np, &ns)) {
            vid_free(v); VID_ERR("open: encoder rejected these parameters");
        }
        v->enc = (H264E_persist_t *)malloc((size_t)np);
        v->scr = (H264E_scratch_t *)malloc((size_t)ns);
        v->yuv = (unsigned char *)malloc((size_t)w * (size_t)h * 3u / 2u);
        if (!v->enc || !v->scr || !v->yuv) { vid_free(v); VID_ERR("open: out of memory"); }
        if (H264E_init(v->enc, &cp)) {
            vid_free(v); VID_ERR("open: could not initialise the encoder");
        }
        v->enc_ready = 1;
        return video_wrap(v);
    }

    /* video.frame(v, img) → nil */
    if (!strcmp(fn_name,"frame")) {
        VID_NEED(2); VID_CTX(0,v);
        if (!v->enc_ready) VID_ERR("frame: this cursor is open for reading");
        if (v->frames >= VID_MAX_FRAMES) VID_ERR("frame: too many frames");
        FluxaImageBuf *b = (args[1].type==VAL_DYN && args[1].as.dyn &&
                            args[1].as.dyn->count>0 &&
                            args[1].as.dyn->items[0].type==VAL_PTR)
                           ? (FluxaImageBuf *)args[1].as.dyn->items[0].as.ptr : NULL;
        if (!fluxa_imgbuf_valid(b)) VID_ERR("frame: expected a live image handle");
        if (b->width != v->width || b->height != v->height)
            VID_ERR("frame: image size does not match the video size");

        vid_rgba_to_i420(b->rgba, v->width, v->height, v->yuv);
        H264E_io_yuv_t io;
        io.yuv[0]    = v->yuv;                                    io.stride[0] = v->width;
        io.yuv[1]    = v->yuv + (size_t)v->width*(size_t)v->height; io.stride[1] = v->width/2;
        io.yuv[2]    = io.yuv[1] + ((size_t)v->width*(size_t)v->height)/4u;
        io.stride[2] = v->width/2;

        H264E_run_param_t rp; memset(&rp, 0, sizeof(rp));
        rp.frame_type   = 0;      /* let the encoder choose I or P */
        rp.encode_speed = 0;
        rp.qp_min = 20; rp.qp_max = 40;

        unsigned char *nal = NULL; int nal_size = 0;
        if (H264E_encode(v->enc, v->scr, &rp, &io, &nal, &nal_size))
            VID_ERR("frame: encoder failed");
        if (nal_size < 0 || (unsigned)nal_size > VID_MAX_NAL)
            VID_ERR("frame: encoded frame is implausibly large");
        if (mp4_h26x_write_nal(&v->wtr, nal, nal_size, 90000 / v->fps))
            VID_ERR("frame: could not write the frame");
        v->frames++;
        return video_nil();
    }

    /* video.audio(v, path) → nil — remux, never re-encode. */
    if (!strcmp(fn_name,"audio")) {
        VID_NEED(2); VID_CTX(0,v); VID_STR(1,apath);
        if (!v->enc_ready) VID_ERR("audio: this cursor is open for reading");

        FILE *af = fopen(apath, "rb");
        if (!af) VID_ERR("audio: cannot open the audio file");
        if (fseek(af, 0, SEEK_END) != 0) { fclose(af); VID_ERR("audio: cannot size the file"); }
        long asz = ftell(af);
        if (asz <= 0) { fclose(af); VID_ERR("audio: the audio file is empty"); }
        if ((unsigned long)asz > VID_MAX_AUDIO) {
            fclose(af); VID_ERR("audio: the audio file exceeds the size limit");
        }
        if (fseek(af, 0, SEEK_SET) != 0) { fclose(af); VID_ERR("audio: cannot rewind"); }
        unsigned char *abuf = (unsigned char *)malloc((size_t)asz);
        if (!abuf) { fclose(af); VID_ERR("audio: out of memory"); }
        if (fread(abuf, 1, (size_t)asz, af) != (size_t)asz) {
            free(abuf); fclose(af); VID_ERR("audio: short read");
        }
        fclose(af);

        /* Only the two container-friendly forms, identified by their own
         * signature rather than by the file extension. */
        int is_adts = (asz > 2 && abuf[0] == 0xFF && (abuf[1] & 0xF6) == 0xF0);
        int is_mp3  = (asz > 2 && ((abuf[0] == 0xFF && (abuf[1] & 0xE0) == 0xE0) ||
                                   (asz > 3 && !memcmp(abuf, "ID3", 3))));
        if (!is_adts && !is_mp3) {
            free(abuf);
            VID_ERR("audio: expected an ADTS .aac or an .mp3 file");
        }

        MP4E_track_t tr; memset(&tr, 0, sizeof(tr));
        tr.track_media_kind        = e_audio;
        tr.language[0]='u';tr.language[1]='n';tr.language[2]='d';tr.language[3]=0;
        tr.object_type_indication  = is_adts
            ? MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE
            : 0x69;   /* MPEG-1 Audio Layer III */
        tr.time_scale              = 90000;
        tr.default_duration        = 0;
        tr.u.a.channelcount        = 2;
        int atrack = MP4E_add_track(v->mux, &tr);
        if (atrack < 0) { free(abuf); VID_ERR("audio: could not add the audio track"); }

        /* The whole track goes in as one sample spanning the video's duration.
         * Players read the elementary stream's own frame headers for timing,
         * which is why a verbatim copy stays in sync. */
        int dur = v->frames > 0 ? (90000 / v->fps) * v->frames : 90000;
        if (MP4E_put_sample(v->mux, atrack, abuf, (int)asz, dur, MP4E_SAMPLE_RANDOM_ACCESS)) {
            free(abuf); VID_ERR("audio: could not write the audio track");
        }
        free(abuf);
        return video_nil();
    }

    /* video.subtitle(v, start_sec, end_sec, text) → nil */
    if (!strcmp(fn_name,"subtitle")) {
        VID_NEED(4); VID_CTX(0,v);
        VID_NUM(1,start); VID_NUM(2,end); VID_STR(3,text);
        if (start < 0.0)  VID_ERR("subtitle: start must not be negative");
        if (end <= start) VID_ERR("subtitle: end must be after start");
        if (strlen(text) > VID_MAX_SUB_TEXT) VID_ERR("subtitle: text is too long");
        if (v->sub_count >= VID_MAX_SUBS)    VID_ERR("subtitle: too many cues");
        if (v->sub_count >= v->sub_cap) {
            int nc = v->sub_cap ? v->sub_cap * 2 : 16;
            VidSub *ns = (VidSub *)realloc(v->subs, sizeof(VidSub) * (size_t)nc);
            if (!ns) VID_ERR("subtitle: out of memory");
            v->subs = ns; v->sub_cap = nc;
        }
        size_t tl = strlen(text);
        char *copy = (char *)malloc(tl + 1);
        if (!copy) VID_ERR("subtitle: out of memory");
        memcpy(copy, text, tl + 1);
        v->subs[v->sub_count].start = start;
        v->subs[v->sub_count].end   = end;
        v->subs[v->sub_count].text  = copy;
        v->sub_count++;
        return video_nil();
    }

    /* video.close(v) → nil
     *
     * Releasing is silent on a cursor that is already released, matching
     * pg.free_result, json2.discard and image.discard: a second close is a
     * no-op, never an error and never a double free. Only the functions that
     * DO something with the cursor report an invalid one. */
    if (!strcmp(fn_name,"close") || !strcmp(fn_name,"play_close")) {
        VID_NEED(1);
        if (args[0].type!=VAL_DYN || !args[0].as.dyn || args[0].as.dyn->count<1 ||
            args[0].as.dyn->items[0].type!=VAL_PTR || !args[0].as.dyn->items[0].as.ptr)
            return video_nil();
        VidCtx *v = (VidCtx *)args[0].as.dyn->items[0].as.ptr;
        if (v->magic != VID_MAGIC) return video_nil();
        if (v->wtr_ready) { mp4_h26x_write_close(&v->wtr); v->wtr_ready = 0; }
        if (v->mux)       { MP4E_close(v->mux); v->mux = NULL; }
        vid_write_srt(v);
        vid_free(v);
        /* Null the cursor so a second close is a no-op, not a double free. */
        if (args[0].type==VAL_DYN && args[0].as.dyn && args[0].as.dyn->count>=1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return video_nil();
    }

    /* video.play_open(path) → dyn */
    if (!strcmp(fn_name,"play_open")) {
        VID_NEED(1); VID_STR(0,path);
        VidCtx *v = (VidCtx *)calloc(1, sizeof(VidCtx));
        if (!v) VID_ERR("play_open: out of memory");
        v->magic = VID_MAGIC;
        v->fp = fopen(path, "rb");
        if (!v->fp) { vid_free(v); VID_ERR("play_open: cannot open the file"); }
        if (fseek(v->fp, 0, SEEK_END) != 0) { vid_free(v); VID_ERR("play_open: cannot size the file"); }
        long fsz = ftell(v->fp);
        if (fsz <= 0) { vid_free(v); VID_ERR("play_open: the file is empty"); }
        if (fseek(v->fp, 0, SEEK_SET) != 0) { vid_free(v); VID_ERR("play_open: cannot rewind"); }

        memset(&v->demux, 0, sizeof(v->demux));
        if (!MP4D_open(&v->demux, vid_read_cb, v->fp, fsz)) {
            vid_free(v); VID_ERR("play_open: not a readable MP4 file");
        }
        v->demux_ready = 1;
        if (v->demux.track_count < 1) { vid_free(v); VID_ERR("play_open: the file has no tracks"); }

        /* Find a video track rather than assuming track 0 — a file with audio
         * first is perfectly ordinary. */
        int vt = -1;
        for (unsigned t = 0; t < v->demux.track_count; t++) {
            if (v->demux.track[t].handler_type == MP4D_HANDLER_TYPE_VIDE) { vt = (int)t; break; }
        }
        if (vt < 0) { vid_free(v); VID_ERR("play_open: the file has no video track"); }
        v->vtrack = vt;
        v->cur_sample = 0;
        v->width  = (int)v->demux.track[vt].SampleDescription.video.width;
        v->height = (int)v->demux.track[vt].SampleDescription.video.height;
        v->frames = (int)v->demux.track[vt].sample_count;
        {   /* timescale/duration → fps, guarding a zero duration */
            unsigned ts = v->demux.track[vt].timescale;
            unsigned du = (unsigned)v->demux.track[vt].duration_lo;
            v->fps = (du > 0 && ts > 0 && v->frames > 0)
                     ? (int)(((double)v->frames * (double)ts) / (double)du + 0.5) : 0;
            if (v->fps <= 0 || v->fps > 240) v->fps = 30;
        }
        v->enc_ready = 0;
        /* Validate what the header claims before anything sizes a buffer from it. */
        if (v->width <= 0 || v->height <= 0 ||
            v->width > VID_MAX_DIM || v->height > VID_MAX_DIM ||
            (long)v->width * (long)v->height > VID_MAX_PIXELS) {
            vid_free(v); VID_ERR("play_open: implausible frame dimensions");
        }
        if (v->frames < 0 || v->frames > VID_MAX_FRAMES) {
            vid_free(v); VID_ERR("play_open: implausible frame count");
        }

        v->dec = h264bsdAlloc();
        if (!v->dec) { vid_free(v); VID_ERR("play_open: out of memory"); }
        if (h264bsdInit(v->dec, 0) != HANTRO_OK) {
            h264bsdFree(v->dec); v->dec = NULL;
            vid_free(v); VID_ERR("play_open: could not start the decoder");
        }
        v->dec_ready = 1;
        v->hdrs_done = 0;
        return video_wrap(v);
    }

    /* video.play_frame(v) → dyn | nil
     *
     * Decodes the next frame and returns it as the same image handle std.image
     * and graph.draw_image take. Returns nil once the stream is exhausted, so
     * the caller's loop is `while img != nil`.
     *
     * Two things about h264bsd shape this. First, it wants one NAL at a time,
     * while MP4 stores them length-prefixed and concatenated — so each sample
     * is split and converted to Annex-B. Second, on the first IDR it returns
     * HDRS_RDY, having built its headers and consumed NOTHING; the same NAL has
     * to be fed again to actually decode. Miss that and every frame silently
     * decodes to nothing, with no error to explain it. */
    if (!strcmp(fn_name,"play_frame")) {
        VID_NEED(1); VID_CTX(0,v);
        if (!v->dec_ready) VID_ERR("play_frame: this cursor is open for writing");

        /* SPS/PPS from the sample description, once. */
        if (!v->hdrs_done) {
            for (int kind = 0; kind < 2; kind++) {
                int psz = 0;
                const void *ps = kind == 0
                    ? MP4D_read_sps(&v->demux, (unsigned)v->vtrack, 0, &psz)
                    : MP4D_read_pps(&v->demux, (unsigned)v->vtrack, 0, &psz);
                if (!ps || psz <= 0 || (unsigned)psz > VID_MAX_NAL)
                    VID_ERR("play_frame: missing or implausible H.264 parameter set");
                unsigned char *pb = (unsigned char *)malloc((size_t)psz + 4u);
                if (!pb) VID_ERR("play_frame: out of memory");
                pb[0]=0; pb[1]=0; pb[2]=0; pb[3]=1;
                memcpy(pb + 4, ps, (size_t)psz);
                u32 consumed = 0;
                h264bsdDecode(v->dec, pb, (u32)psz + 4u, 0, &consumed);
                free(pb);
            }
            v->hdrs_done = 1;
        }

        while (v->cur_sample < v->demux.track[v->vtrack].sample_count) {
            unsigned fsize = 0, ts = 0, dur = 0;
            MP4D_file_offset_t off = MP4D_frame_offset(
                &v->demux, (unsigned)v->vtrack, v->cur_sample, &fsize, &ts, &dur);
            v->cur_sample++;
            if (fsize == 0 || fsize > VID_MAX_NAL) continue;

            unsigned char *fr = (unsigned char *)malloc(fsize);
            if (!fr) VID_ERR("play_frame: out of memory");
            if (fseek(v->fp, (long)off, SEEK_SET) != 0 ||
                fread(fr, 1, fsize, v->fp) != fsize) {
                free(fr); VID_ERR("play_frame: short read");
            }

            int produced = 0;
            unsigned p = 0;
            while (p + 4u <= fsize) {
                unsigned nl = ((unsigned)fr[p]<<24)|((unsigned)fr[p+1]<<16)|
                              ((unsigned)fr[p+2]<<8)|(unsigned)fr[p+3];
                /* Length must fit inside what is left — a crafted file cannot
                 * push the read past the buffer. */
                if (nl == 0 || nl > fsize - p - 4u) break;
                unsigned char *nb = (unsigned char *)malloc((size_t)nl + 4u);
                if (!nb) { free(fr); VID_ERR("play_frame: out of memory"); }
                nb[0]=0; nb[1]=0; nb[2]=0; nb[3]=1;
                memcpy(nb + 4, fr + p + 4u, nl);

                u32 consumed = 0;
                u32 res = h264bsdDecode(v->dec, nb, nl + 4u, 0, &consumed);
                if (res == H264BSD_HDRS_RDY)      /* headers built, nothing eaten */
                    res = h264bsdDecode(v->dec, nb, nl + 4u, 0, &consumed);
                free(nb);

                if (res == H264BSD_PIC_RDY) {
                    u32 pic_id = 0, pw = 0, ph = 0;
                    u8 *pic = (u8 *)h264bsdNextOutputPicture(v->dec, &pic_id, &pw, &ph);
                    if (pic) {
                        /* H.264 codes whole 16x16 macroblocks, so the decoded
                         * surface is rounded up: a 160x120 video decodes as
                         * 160x128. Crop back to the size the container
                         * declares, otherwise every frame comes out eight rows
                         * taller than the video actually is. The stride stays
                         * the padded width — that is how the rows are laid
                         * out — while only width x height is converted. */
                        int PW = (int)h264bsdPicWidth(v->dec) * 16;
                        int PH = (int)h264bsdPicHeight(v->dec) * 16;
                        if (PW <= 0 || PH <= 0 || PW > VID_MAX_DIM || PH > VID_MAX_DIM ||
                            (long)PW * (long)PH > VID_MAX_PIXELS) {
                            free(fr); VID_ERR("play_frame: implausible decoded size");
                        }
                        int W = v->width  > 0 && v->width  <= PW ? v->width  : PW;
                        int H = v->height > 0 && v->height <= PH ? v->height : PH;
                        FluxaImageBuf *b = fluxa_imgbuf_new(W, H);
                        if (!b) { free(fr); VID_ERR("play_frame: out of memory"); }
                        const u8 *Y = pic;
                        const u8 *U = pic + (size_t)PW * (size_t)PH;
                        const u8 *V = U + ((size_t)PW * (size_t)PH) / 4u;
                        vid_i420_to_rgba(Y, U, V, PW, PW/2, W, H, b->rgba);
                        b->version++;
                        free(fr);
                        return video_wrap_img(b);
                    }
                }
                p += 4u + nl;
                produced++;
            }
            (void)produced;
            free(fr);
        }
        return video_nil();   /* end of stream */
    }

    /* video.play_eof(v) → bool
     *
     * True once every sample has been handed to the decoder. This is the shape
     * a playback loop should use:
     *
     *     while !video.play_eof(p) {
     *         dyn fr = video.play_frame(p)
     *         graph.draw_image(win, fr, 0, 0)
     *         image.discard(fr)
     *     }
     *
     * Testing the returned frame against nil instead does not work: `dyn x =`
     * a call that returned nil leaves x undeclared, so the next line fails with
     * "undefined variable". Deciding from a return value is the same rule the
     * language uses for accept() and friends. */
    if (!strcmp(fn_name,"play_eof")) {
        VID_NEED(1); VID_CTX(0,v);
        if (!v->dec_ready) VID_ERR("play_eof: this cursor is open for writing");
        Value r; r.type=VAL_BOOL;
        r.as.boolean = (v->cur_sample >= v->demux.track[v->vtrack].sample_count);
        return r;
    }

    /* video.info(v) → dyn [w, h, fps, frames] */
    if (!strcmp(fn_name,"info")) {
        VID_NEED(1); VID_CTX(0,v);
        FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) VID_ERR("info: out of memory");
        memset(d,0,sizeof(*d));
        d->items=(Value *)malloc(sizeof(Value)*4);
        if (!d->items) { free(d); VID_ERR("info: out of memory"); }
        d->items[0]=video_int(v->width);
        d->items[1]=video_int(v->height);
        d->items[2]=video_int(v->fps);
        d->items[3]=video_int(v->frames);
        d->count=4; d->cap=4;
        Value r; r.type=VAL_DYN; r.as.dyn=d; return r;
    }

    snprintf(errbuf,sizeof(errbuf),"video.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"video",line);
    *had_error=1;
    return video_nil();
#endif /* FLUXA_STD_VIDEO */
}

#undef VID_ERR
#undef VID_NEED
#undef VID_STR
#undef VID_INT
#undef VID_NUM

FLUXA_LIB_EXPORT(
    name     = "video",
    toml_key = "std.video",
    owner    = "video",
    call     = fluxa_std_video_call,
    rt_aware = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_VIDEO_H */
