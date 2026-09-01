#ifndef FLUXA_STD_GRAPH_H
#define FLUXA_STD_GRAPH_H

/*
 * std.graph — 2D/3D graphics for Fluxa-lang
 *
 * Two backends:
 *
 *   FLUXA_GRAPH_RAYLIB=1   Raylib backend (requires raylib.h + libraylib)
 *     Full hardware-accelerated 2D/3D. Works on Linux, macOS, Windows,
 *     Raspberry Pi. Vendor raylib into vendor/raylib.h + vendor/libraylib.a,
 *     then: make FLUXA_GRAPH_RAYLIB=1 build
 *
 *   (default) stub backend
 *     API-complete, no-op rendering. Useful for testing game logic,
 *     state machines, and prst patterns without a display.
 *     Returns sensible values (ok=true, window=dyn cursor, etc.).
 *
 * API:
 *   graph.init(width, height, title)  → dyn window cursor
 *   graph.close(win)                  → nil
 *   graph.should_close(win)           → bool
 *   graph.begin_frame(win)            → nil
 *   graph.end_frame(win)              → nil
 *   graph.capture(win)                → dyn   (RGBA snapshot; free via image.discard)
 *   graph.draw_image(win,img,x,y[,s]) → nil   (draw an image buffer; optional scale)
 *   graph.draw_image_tint(win,img,x,y,r,g,b,a,scale) → nil
 *   graph.clear(win, r, g, b)         → nil   (RGB 0-255)
 *   graph.fps(win)                    → int
 *   graph.set_fps(win, fps)           → nil
 *   graph.fullscreen(win)             → bool  (alterna tela cheia; retorna o estado novo)
 *   graph.draw_rect(win, x, y, w, h, r, g, b)          → nil
 *   graph.draw_circle(win, x, y, radius, r, g, b)       → nil
 *   graph.draw_line(win, x1, y1, x2, y2, r, g, b)      → nil
 *   graph.draw_text(win, text, x, y, size, r, g, b)     → nil
 *   graph.load_font(win, path, size)  → dyn font cursor  (TTF/OTF, incl. Latin-1 accents)
 *   graph.draw_text_font(win, font, text, x, y, size, r, g, b) → nil
 *   graph.text_width(win, font, text, size)             → int  (pixels, for layout)
 *   graph.unload_font(win, font)      → nil
 *   graph.key_pressed(win, key)       → bool  (key: "SPACE", "A"-"Z", etc.)
 *   graph.key_down(win, key)          → bool
 *   graph.mouse_x(win)                → int
 *   graph.mouse_y(win)                → int
 *   graph.mouse_pressed(win)          → bool  (left button)
 *   graph.dt(win)                     → float (delta time seconds)
 *   graph.key_pressed/key_down also accept: SHIFT, CTRL, ALT, PLUS, MINUS
 *   graph.is_fullscreen(win)          → bool  (query only; fullscreen() toggles)
 *   graph.open_url(url)               → bool  (http/https/mailto → system browser)
 *   graph.version()                   → str
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../../scope.h"
#include "../../err.h"
#include "../fluxa_image_buffer.h"   /* neutral RGBA buffer shared with std.image */

/* for graph.open_url — launching the system browser (no display required) */
#if defined(_WIN32)
#  include <windows.h>          /* ShellExecuteA — link with -lshell32 on Windows */
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

/* ── Opening a URL in the system's default browser ─────────────────
 * Deliberately NOT raylib's OpenURL: that one shells out through system(), so a
 * crafted URL can carry a command along with it. Here the URL is handed to exec
 * as a single argv element with no shell in between, which makes injection
 * impossible by construction, and the scheme is checked first. This lives
 * outside the backend #ifdefs because opening a browser needs no display —
 * it works on the stub build too. */

/* Allow only http://, https:// and mailto:, and no control characters. */
static inline int graph_url_ok(const char *u) {
    if (!u) return 0;
    size_t n = strlen(u);
    if (n < 8 || n > 2048) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c < 0x20 || c == 0x7F) return 0;   /* newlines, NUL-ish, controls */
    }
    if (strncmp(u, "http://",  7) == 0) return 1;
    if (strncmp(u, "https://", 8) == 0) return 1;
    if (strncmp(u, "mailto:",  7) == 0) return 1;
    return 0;
}

/* Hand the URL to the platform's default handler. Returns 1 if the launch
 * started (what the browser does afterwards is out of our hands). */
static inline int graph_launch_url(const char *url) {
#if defined(_WIN32)
    HINSTANCE r = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32) ? 1 : 0;
#else
    /* Double fork: the intermediate child exits immediately and the browser is
     * reparented to init. That leaves no zombie for the game to reap and never
     * blocks the frame loop, without touching the global SIGCHLD handler. */
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        pid_t inner = fork();
        if (inner == 0) {
            setsid();                      /* detach from our session */
#  if defined(__APPLE__)
            execlp("open", "open", url, (char *)NULL);
#  else
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
#  endif
            _exit(127);                    /* exec failed — nothing else to do */
        }
        _exit(0);
    }
    {
        int st = 0;
        waitpid(pid, &st, 0);              /* reap the intermediate child */
    }
    return 1;
#endif
}

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: Raylib (when FLUXA_GRAPH_RAYLIB=1)
 * ════════════════════════════════════════════════════════════════════ */
#ifdef FLUXA_GRAPH_RAYLIB

#include <raylib.h>
#include <rlgl.h>   /* the triangle batch draw_tris submits into */

/* The GPU cache stored in FluxaImageBuf.gpu_cache is a heap Texture2D. This hook
 * lets the neutral image buffer unload it on free without knowing the type;
 * graph_new_win installs it at window creation. Defined here so it precedes
 * every use and sees the Raylib Texture2D type. */
static inline void graph_img_gpu_free(void *gpu_cache) {
    if (!gpu_cache) return;
    Texture2D *tex = (Texture2D *)gpu_cache;
    UnloadTexture(*tex);
    free(tex);
}
static inline void graph_install_gpu_hook(void) {
    fluxa_imgbuf_set_gpu_free_hook(graph_img_gpu_free);
}

typedef struct {
    int width, height;       /* logical (design) resolution — game draws here */
    int fps_target;
    RenderTexture2D target;  /* fixed-size offscreen buffer for scaled output */
    int has_target;          /* 1 once the render texture is created */
    /* Active 2D camera, mirrored here so screen_to_world / world_to_screen can
     * do the maths themselves rather than asking the backend. Keeping one
     * implementation means the headless build computes the same answer as the
     * rendered one, and the round trip is testable without a display. */
    int    cam_on;
    double cam_x, cam_y, cam_rot, cam_zoom;
} GraphWin;

/* An extra off-screen surface. Wrapped in a dyn as an opaque cursor, same
 * ownership shape as GraphFont: create, pass as an argument, release. */
typedef struct {
    int             width, height;
    RenderTexture2D target;
    int             has_target;
} GraphRT;

static GraphRT *graph_new_rt(int w, int h) {
    GraphRT *rt = (GraphRT *)calloc(1, sizeof(GraphRT));
    if (!rt) return NULL;
    rt->width = w; rt->height = h;
    rt->target = LoadRenderTexture(w, h);
    rt->has_target = 1;
    return rt;
}

/* Upload (or refresh) the GPU texture cached on an image buffer. Shared by
 * draw_image_rot and draw_sprite; draw_image keeps its own inline copy so its
 * behaviour cannot shift. Returns NULL only on allocation failure. */
static Texture2D *graph_img_texture(FluxaImageBuf *b) {
    Texture2D *tex = (Texture2D *)b->gpu_cache;
    if (tex == NULL || b->gpu_version != b->version) {
        if (tex != NULL) { UnloadTexture(*tex); free(tex); b->gpu_cache = NULL; }
        Image img;
        img.data = b->rgba; img.width = b->width; img.height = b->height;
        img.mipmaps = 1; img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        tex = (Texture2D *)malloc(sizeof(Texture2D));
        if (!tex) return NULL;
        *tex = LoadTextureFromImage(img);
        b->gpu_cache = tex;
        b->gpu_version = b->version;
    }
    return tex;
}

/* Returns NULL when no usable OpenGL driver was found. InitWindow() fails
 * silently by design (it logs a WARNING and returns) rather than aborting, so
 * without this check the caller would keep going with no GL context at all:
 * every draw call would quietly no-op or warn, and CloseWindow() would
 * eventually segfault in rlglClose() -> rlUnloadRenderBatch(), which
 * dereferences a render batch that rlglInit() never got to allocate. Caught
 * here instead: the caller gets a clean NULL and never touches raylib again
 * for this window, including never calling CloseWindow() on it — that call is
 * exactly what crashes, so a failed window must never reach it. */
static GraphWin *graph_new_win(int w, int h, const char *title) {
    InitWindow(w, h, title);
    if (!IsWindowReady()) return NULL;

    GraphWin *win = (GraphWin *)calloc(1, sizeof(GraphWin));
    win->width = w; win->height = h; win->fps_target = 60;
    graph_install_gpu_hook();   /* so image buffers can free their cached textures */
    SetExitKey(KEY_NULL);   /* ESC must reach the program (quit-confirm UIs),
                             * not silently close the window (raylib default) */
    SetTargetFPS(60);
    /* offscreen buffer at the logical resolution; end_frame blits it scaled
     * and letterboxed to the real window, so fullscreen enlarges the game
     * proportionally instead of leaving it in the top-left corner. */
    win->target = LoadRenderTexture(w, h);
    SetTextureFilter(win->target.texture, TEXTURE_FILTER_BILINEAR);
    win->has_target = 1;
    return win;
}

typedef struct {
    Font font;
    int  base_size;   /* size the glyph atlas was rasterized at */
    int  loaded;      /* 1 = owns GPU texture; 0 = fell back to default font */
} GraphFont;

/* Load a TTF/OTF at base_size with ASCII 32-126 + Latin-1 160-255
 * (covers Portuguese and Western European accented characters).
 * Returns NULL if the file cannot be read. loaded=0 means raylib
 * fell back to its default font (unsupported/corrupt file). */
static GraphFont *graph_new_font(const char *path, int base_size) {
    FILE *probe = fopen(path, "rb");
    if (!probe) return NULL;
    fclose(probe);

    int codepoints[95 + 96 + 6];
    int n = 0;
    for (int c = 32;  c <= 126; c++) codepoints[n++] = c;   /* ASCII    */
    for (int c = 160; c <= 255; c++) codepoints[n++] = c;   /* Latin-1  */
    codepoints[n++] = 0x2014;                               /* — em dash */
    codepoints[n++] = 0x2018;                               /* '         */
    codepoints[n++] = 0x2190;                               /* ←         */
    codepoints[n++] = 0x2191;                               /* ↑         */
    codepoints[n++] = 0x2192;                               /* →         */
    codepoints[n++] = 0x2193;                               /* ↓         */

    GraphFont *f = (GraphFont *)calloc(1, sizeof(GraphFont));
    f->base_size = base_size;
    f->font      = LoadFontEx(path, base_size, codepoints, n);
    /* On failure LoadFontEx returns the default font — never unload that. */
    f->loaded    = (f->font.texture.id != GetFontDefault().texture.id);
    SetTextureFilter(f->font.texture, TEXTURE_FILTER_BILINEAR);
    return f;
}

static int graph_key_code(const char *key) {
    if (!key) return 0;
    if (!strcmp(key,"SPACE"))  return KEY_SPACE;
    if (!strcmp(key,"ENTER"))  return KEY_ENTER;
    if (!strcmp(key,"ESCAPE")) return KEY_ESCAPE;
    if (!strcmp(key,"UP"))     return KEY_UP;
    if (!strcmp(key,"DOWN"))   return KEY_DOWN;
    if (!strcmp(key,"LEFT"))   return KEY_LEFT;
    if (!strcmp(key,"RIGHT"))  return KEY_RIGHT;
    if (!strcmp(key,"F11"))       return KEY_F11;
    if (!strcmp(key,"BACKSPACE"))  return KEY_BACKSPACE;
    if (!strcmp(key,"TAB"))        return KEY_TAB;
    if (strlen(key) == 1 && key[0] >= 'A' && key[0] <= 'Z')
        return KEY_A + (key[0] - 'A');
    if (strlen(key) == 1 && key[0] >= '0' && key[0] <= '9')
        return KEY_ZERO + (key[0] - '0');
    return 0;
}

/* Some logical keys answer to more than one physical key: SHIFT/CTRL/ALT exist
 * on both sides of the keyboard, and +/- exist on the main row and again on the
 * keypad. A player holding the right shift, or using the numeric keypad, is
 * doing the ordinary thing — so these names resolve to whichever is active.
 * Returns 1 when `key` is one of them and writes the answer to *out. */
static inline int graph_multi_key(const char *key, int held, int *out) {
#define GRAPH_PAIR(a,b) do { \
    *out = held ? (IsKeyDown(a)    || IsKeyDown(b)) \
                : (IsKeyPressed(a) || IsKeyPressed(b)); \
    return 1; \
} while (0)
    if (!strcmp(key,"SHIFT")) GRAPH_PAIR(KEY_LEFT_SHIFT,   KEY_RIGHT_SHIFT);
    if (!strcmp(key,"CTRL"))  GRAPH_PAIR(KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL);
    if (!strcmp(key,"ALT"))   GRAPH_PAIR(KEY_LEFT_ALT,     KEY_RIGHT_ALT);
    if (!strcmp(key,"PLUS"))  GRAPH_PAIR(KEY_EQUAL,        KEY_KP_ADD);
    if (!strcmp(key,"MINUS")) GRAPH_PAIR(KEY_MINUS,        KEY_KP_SUBTRACT);
#undef GRAPH_PAIR
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: stub (default — no display, API-complete no-op)
 * ════════════════════════════════════════════════════════════════════ */
#else

typedef struct {
    int width, height;
    int fps_target;
    int frame_count;
    int should_close;
    int fullscreen;
    /* Mirrors the raylib backend — the camera transform is computed from these
     * fields on both, so screen_to_world behaves identically headless. */
    int    cam_on;
    double cam_x, cam_y, cam_rot, cam_zoom;
} GraphWin;

/* Off-screen surface. The stub keeps the dimensions so the cursor discipline
 * (create / use / release, and the invalid-cursor error) is exercised without
 * a display; nothing is drawn. */
typedef struct {
    int width, height;
    int has_target;
} GraphRT;

static GraphRT *graph_new_rt(int w, int h) {
    GraphRT *rt = (GraphRT *)calloc(1, sizeof(GraphRT));
    if (!rt) return NULL;
    rt->width = w; rt->height = h; rt->has_target = 0;
    return rt;
}

static GraphWin *graph_new_win(int w, int h, const char *title) {
    GraphWin *win = (GraphWin *)calloc(1, sizeof(GraphWin));
    win->width = w; win->height = h; win->fps_target = 60;
    fprintf(stderr,
        "[fluxa] std.graph: stub backend — window '%s' (%dx%d) created.\n"
        "  For real rendering: vendor raylib into vendor/raylib.h + vendor/libraylib.a\n"
        "  then rebuild with: make FLUXA_GRAPH_RAYLIB=1 build\n",
        title, w, h);
    return win;
}

typedef struct {
    int base_size;    /* size requested at load — used for stub metrics */
    int loaded;       /* mirrors the raylib-backend field; always 1 here */
} GraphFont;

/* Stub: validates the file exists (same error contract as the raylib
 * backend) and records the size for deterministic text_width metrics. */
static GraphFont *graph_new_font(const char *path, int base_size) {
    FILE *probe = fopen(path, "rb");
    if (!probe) return NULL;
    fclose(probe);
    GraphFont *f = (GraphFont *)calloc(1, sizeof(GraphFont));
    f->base_size = base_size;
    f->loaded    = 1;
    return f;
}

static int graph_key_code(const char *key) { (void)key; return 0; }
/* suppress unused-function in stub mode */
static inline void graph_key_code_unused_(void) { (void)graph_key_code; }

#endif /* FLUXA_GRAPH_RAYLIB */

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value graph_nil(void)    { Value v; v.type=VAL_NIL;    return v; }
static inline Value graph_bool(int b)  { Value v; v.type=VAL_BOOL;   v.as.boolean=b; return v; }
static inline Value graph_int(long n)  { Value v; v.type=VAL_INT;    v.as.integer=n; return v; }
static inline Value graph_float(double d){ Value v; v.type=VAL_FLOAT; v.as.real=d; return v; }
static inline Value graph_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=fxstr_new(s?s:""); return v; }

static inline Value graph_wrap(GraphWin *win) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=win;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphWin *graph_unwrap(const Value *v, ErrStack *err,
                                      int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"graph.%s: invalid window cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphWin *)v->as.dyn->items[0].as.ptr;
}

/* ── 3D ─────────────────────────────────────────────────────────────
 * rmodels is already linked; what was missing was a door. This is the
 * smallest surface that is actually useful: a camera, a way in and out of 3D
 * mode, and a mesh you can upload once and draw many times — plus a triangle
 * batch for callers who already hold their own geometry.
 *
 * This is not a replacement for image.fill_tris and does not compete with it.
 * fill_tris keeps every pixel under the caller's control and produces the same
 * bytes on every machine, which is what an image test, an emulator or an
 * offline render needs. What follows hands the work to the GPU and accepts the
 * GPU's rules in exchange for speed. */
typedef struct {
    double px, py, pz;   /* eye     */
    double tx, ty, tz;   /* target  */
    double fovy;         /* vertical field of view, degrees */
} GraphCam3;

/* An uploaded mesh. The stub records the shape so a program can be written and
 * its handle discipline exercised headless; only the raylib backend owns GPU
 * buffers. */
typedef struct {
    long tri_count;
    int  has_uv, has_color;
    int  uploaded;
#ifdef FLUXA_GRAPH_RAYLIB
    Mesh     mesh;
    Material mat;
    int      has_mat;
#endif
} GraphMesh3;

/* Fluxa arrays hold int or float per element; geometry is read through this so
 * either is accepted without widening any existing parameter. */
static inline float graph_arr_f(const Value *a, long i) {
    if (a[i].type == VAL_INT)   return (float)a[i].as.integer;
    if (a[i].type == VAL_FLOAT) return (float)a[i].as.real;
    return 0.0f;
}
static inline int graph_arr_u8(const Value *a, long i) {
    long v = (a[i].type == VAL_INT) ? a[i].as.integer
           : (a[i].type == VAL_FLOAT) ? (long)a[i].as.real : 0;
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

static inline Value graph_wrap_cam3(GraphCam3 *c) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=c;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphCam3 *graph_unwrap_cam3(const Value *v, ErrStack *err,
                                           int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),
            "graph.%s: invalid camera cursor — use graph.camera3d() to create one",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphCam3 *)v->as.dyn->items[0].as.ptr;
}

static inline Value graph_wrap_mesh3(GraphMesh3 *m) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=m;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphMesh3 *graph_unwrap_mesh3(const Value *v, ErrStack *err,
                                             int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),
            "graph.%s: invalid mesh cursor — use graph.mesh_upload() to create one",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphMesh3 *)v->as.dyn->items[0].as.ptr;
}

static inline Value graph_wrap_font(GraphFont *f) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=f;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphFont *graph_unwrap_font(const Value *v, ErrStack *err,
                                            int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),
            "graph.%s: invalid font cursor — use graph.load_font() to create one",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphFont *)v->as.dyn->items[0].as.ptr;
}

static inline Value graph_wrap_rt(GraphRT *rt) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=rt;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphRT *graph_unwrap_rt(const Value *v, ErrStack *err,
                                        int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),
            "graph.%s: invalid render target cursor — use graph.render_target() "
            "to create one",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphRT *)v->as.dyn->items[0].as.ptr;
}

/* ── 2D camera maths (backend-neutral) ───────────────────────────────
 * Kept here rather than delegated to the backend so both builds answer the
 * same thing. The convention matches raylib's Camera2D with offset at the
 * window centre: world = (screen - offset) / zoom, rotated, then + target. */
static inline void graph_cam_set(GraphWin *w, double x, double y,
                                  double rot, double zoom) {
    w->cam_on=1; w->cam_x=x; w->cam_y=y; w->cam_rot=rot; w->cam_zoom=zoom;
}
static inline void graph_cam_clear(GraphWin *w) { w->cam_on=0; }

/* to_world = 1 converts screen→world, 0 converts world→screen. With no camera
 * active both are the identity, which is the sane answer for a program that
 * asks before starting one. */
static inline void graph_cam_transform(const GraphWin *w, double px, double py,
                                        int to_world, double *ox, double *oy) {
    if (!w->cam_on) { *ox=px; *oy=py; return; }
    const double PI_ = 3.14159265358979323846;
    double rad = w->cam_rot * PI_ / 180.0;
    double cs = cos(rad), sn = sin(rad);
    double offx = (double)w->width * 0.5, offy = (double)w->height * 0.5;
    if (to_world) {
        double dx = (px - offx) / w->cam_zoom;
        double dy = (py - offy) / w->cam_zoom;
        *ox = w->cam_x + ( dx*cs + dy*sn);
        *oy = w->cam_y + (-dx*sn + dy*cs);
    } else {
        double dx = px - w->cam_x, dy = py - w->cam_y;
        double rx = dx*cs - dy*sn, ry = dx*sn + dy*cs;
        *ox = rx * w->cam_zoom + offx;
        *oy = ry * w->cam_zoom + offy;
    }
}

/* Name → backend code for mouse and gamepad. Returns -1 for an unknown name so
 * the caller reports it instead of silently acting on button 0. The numeric
 * values are raylib's; the stub never uses them but shares the validation, so
 * a typo is caught the same way in a headless test. */
static inline int graph_mouse_btn_code(const char *b) {
    if (!strcmp(b,"LEFT"))   return 0;
    if (!strcmp(b,"RIGHT"))  return 1;
    if (!strcmp(b,"MIDDLE")) return 2;
    return -1;
}
static inline int graph_pad_btn_code(const char *b) {
    if (!strcmp(b,"UP"))     return 1;
    if (!strcmp(b,"RIGHT"))  return 2;
    if (!strcmp(b,"DOWN"))   return 3;
    if (!strcmp(b,"LEFT"))   return 4;
    if (!strcmp(b,"Y"))      return 5;
    if (!strcmp(b,"X"))      return 6;
    if (!strcmp(b,"A"))      return 7;
    if (!strcmp(b,"B"))      return 8;
    if (!strcmp(b,"LB"))     return 9;
    if (!strcmp(b,"RB"))     return 11;
    if (!strcmp(b,"SELECT")) return 13;
    if (!strcmp(b,"START"))  return 15;
    return -1;
}
static inline int graph_pad_axis_code(const char *a) {
    if (!strcmp(a,"LEFT_X"))  return 0;
    if (!strcmp(a,"LEFT_Y"))  return 1;
    if (!strcmp(a,"RIGHT_X")) return 2;
    if (!strcmp(a,"RIGHT_Y")) return 3;
    if (!strcmp(a,"LT"))      return 4;
    if (!strcmp(a,"RT"))      return 5;
    return -1;
}

/* Wrap a captured RGBA buffer as an opaque dyn the script hands to std.image. */
static inline Value graph_wrap_img(FluxaImageBuf *b) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=b;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}


/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_graph_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define GRAPH_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"graph.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line); \
    *had_error=1; return graph_nil(); } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"graph.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line); \
    *had_error=1; return graph_nil(); } } while(0)

#define GET_WIN(idx,var) \
    GraphWin *(var)=graph_unwrap(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_CAM3(idx,var) \
    GraphCam3 *(var)=graph_unwrap_cam3(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_MESH3(idx,var) \
    GraphMesh3 *(var)=graph_unwrap_mesh3(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_FONT(idx,var) \
    GraphFont *(var)=graph_unwrap_font(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GRAPH_RT(idx,var) \
    GraphRT *(var)=graph_unwrap_rt(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) GRAPH_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

/* Optional trailing alpha. Absent means opaque — which is exactly what every
 * one of these primitives did before the argument existed, so adding it cannot
 * change what any existing program draws. */
#define GRAPH_ALPHA(idx,var) \
    long (var)=255; \
    if (argc > (idx)) { \
        if(args[(idx)].type!=VAL_INT) GRAPH_ERR("expected int alpha"); \
        (var)=args[(idx)].as.integer; \
        if((var)<0||(var)>255) GRAPH_ERR("alpha must be in the 0..255 range"); \
    }

/* GRAPH_NUM accepts int or float and yields a double. It exists only for the
 * functions added after v0.29 — every pre-existing function keeps GET_INT and
 * therefore keeps rejecting a float exactly as it always did. Widening an
 * existing parameter would change the contract of code that already works, so
 * the two macros coexist on purpose. */
#define GRAPH_NUM(idx,var) \
    if(args[(idx)].type!=VAL_INT && args[(idx)].type!=VAL_FLOAT) \
        GRAPH_ERR("expected int or float"); \
    double (var)=(args[(idx)].type==VAL_INT) ? (double)args[(idx)].as.integer \
                                             : args[(idx)].as.real;

#define GET_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) GRAPH_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

    /* graph.open_url(url) → bool : hand a URL to the system's default browser
     * (a support / donation page, for instance). Only http://, https:// and
     * mailto: are accepted — anything else is refused, so a URL that arrives
     * from config or a database can't reach for a local file or an odd scheme.
     * The URL goes straight to exec as one argument with no shell involved, so
     * it cannot smuggle a command. Returns true once the launch has started;
     * what the browser does next is outside our reach. Works on both backends
     * (opening a browser needs no display). IO: needs a danger block. */
    if (!strcmp(fn_name,"open_url")) {
        NEED(1); GET_STR(0,url);
        if (!graph_url_ok(url))
            GRAPH_ERR("open_url: only http://, https:// and mailto: URLs are allowed");
        if (!graph_launch_url(url))
            GRAPH_ERR("open_url: could not launch the system browser");
        return graph_bool(1);
    }

    if (!strcmp(fn_name,"version")) {
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_str("raylib/" RAYLIB_VERSION);
#else
        return graph_str("fluxa-graph/1.0 (stub — no display)");
#endif
    }

    if (!strcmp(fn_name,"init")) {
        NEED(3); GET_INT(0,w); GET_INT(1,h); GET_STR(2,title);
        GraphWin *win = graph_new_win((int)w, (int)h, title);
        if (!win)
            GRAPH_ERR("init: no usable OpenGL driver — see the Mesa3D fallback "
                      "in docs/WINDOWS.md");
        return graph_wrap(win);
    }

    if (!strcmp(fn_name,"close")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) UnloadRenderTexture(win->target);
        CloseWindow();
#endif
        free(win);
        if(args[0].type==VAL_DYN&&args[0].as.dyn)
            args[0].as.dyn->items[0].as.ptr=NULL;
        return graph_nil();
    }

    if (!strcmp(fn_name,"should_close")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(WindowShouldClose());
#else
        return graph_bool(win->should_close);
#endif
    }

    if (!strcmp(fn_name,"begin_frame")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            BeginTextureMode(win->target);   /* render the game at logical res */
        } else {
            BeginDrawing();
        }
#else
        win->frame_count++;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"end_frame")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            EndTextureMode();                 /* finish the offscreen render */
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sy = (float)sh / (float)win->height;
            if (sy < scale) scale = sy;        /* fit: the smaller ratio wins */
            float dw = win->width  * scale;
            float dh = win->height * scale;
            float ox = (sw - dw) * 0.5f;       /* center → letterbox/pillarbox */
            float oy = (sh - dh) * 0.5f;
            BeginDrawing();
            ClearBackground(BLACK);            /* bars around the game are black */
            Rectangle srcR = { 0, 0, (float)win->target.texture.width,
                                     -(float)win->target.texture.height };
            Rectangle dstR = { ox, oy, dw, dh };
            Vector2 origin = { 0, 0 };
            DrawTexturePro(win->target.texture, srcR, dstR, origin, 0.0f, WHITE);
            EndDrawing();
        } else {
            EndDrawing();
        }
#else
        (void)win;
#endif
        return graph_nil();
    }

    /* graph.capture(win) → dyn : snapshot the current frame as a neutral RGBA
     * image buffer, ready for std.image to resize/export. Call it after the
     * frame is drawn (after end_frame, or between begin_frame/end_frame on the
     * offscreen target). The returned dyn is owned by the script: release it
     * with image.free. On the stub backend it returns a blank buffer of the
     * logical size so game logic and tests run without a display. */
    if (!strcmp(fn_name,"capture")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        Image img;
        if (win->has_target) {
            /* pull the pixels straight from the offscreen render target */
            img = LoadImageFromTexture(win->target.texture);
        } else {
            /* no target: grab the visible framebuffer via a screen-sized texture */
            img = LoadImageFromScreen();
        }
        /* normalize to 32-bit RGBA regardless of the source pixel format */
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        /* the GPU texture is bottom-up; flip so row 0 is the top of the frame */
        ImageFlipVertical(&img);
        FluxaImageBuf *b = fluxa_imgbuf_from_rgba((const unsigned char *)img.data,
                                                  img.width, img.height);
        UnloadImage(img);
        if (!b) GRAPH_ERR("capture: out of memory");
        return graph_wrap_img(b);
#else
        {
            FluxaImageBuf *b = fluxa_imgbuf_new(win->width, win->height);
            if (!b) GRAPH_ERR("capture: out of memory");
            return graph_wrap_img(b);
        }
#endif
    }

    /* graph.draw_image(win, img, x, y [, scale]) → nil : draw an RGBA image
     * buffer (from std.image or graph.capture) onto the frame at (x, y). The
     * optional 5th argument scales it (1.0 = original size; 0.5 = half). The
     * uploaded GPU texture is CACHED on the image buffer and reused across
     * frames — re-uploaded only when the pixels change (resize/blit bump the
     * buffer's version) — so calling this every frame in the game loop is cheap.
     * The texture is released when the image is discarded. Completes the round
     * trip: graph.capture is graph→image; this is image→graph. */
    if (!strcmp(fn_name,"draw_image")) {
        NEED(4); GET_WIN(0,win);
        FluxaImageBuf *b = (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>0
                            && args[1].as.dyn->items[0].type==VAL_PTR)
                           ? (FluxaImageBuf *)args[1].as.dyn->items[0].as.ptr : NULL;
        if (!fluxa_imgbuf_valid(b)) GRAPH_ERR("draw_image: expected a live image handle");
        GET_INT(2,dx); GET_INT(3,dy);
        double scale = 1.0;
        if (argc >= 5) {
            if (args[4].type==VAL_FLOAT)      scale = args[4].as.real;
            else if (args[4].type==VAL_INT)   scale = (double)args[4].as.integer;
            else GRAPH_ERR("draw_image: scale must be a number");
            if (scale <= 0.0) GRAPH_ERR("draw_image: scale must be positive");
        }
#ifdef FLUXA_GRAPH_RAYLIB
        /* (re)upload the texture only when missing or stale */
        Texture2D *tex = (Texture2D *)b->gpu_cache;
        if (tex == NULL || b->gpu_version != b->version) {
            if (tex != NULL) { UnloadTexture(*tex); free(tex); b->gpu_cache = NULL; }
            Image img;
            img.data = b->rgba; img.width = b->width; img.height = b->height;
            img.mipmaps = 1; img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            tex = (Texture2D *)malloc(sizeof(Texture2D));
            if (!tex) GRAPH_ERR("draw_image: out of memory");
            *tex = LoadTextureFromImage(img);
            b->gpu_cache = tex;
            b->gpu_version = b->version;
        }
        if (scale == 1.0) {
            DrawTexture(*tex, (int)dx, (int)dy, WHITE);
        } else {
            DrawTextureEx(*tex, (Vector2){(float)dx,(float)dy}, 0.0f, (float)scale, WHITE);
        }
#else
        (void)win; (void)b; (void)dx; (void)dy; (void)scale;
#endif
        return graph_nil();
    }

    /* ═════════════════════════════════════════════════════════════════
     * Added in v0.30. Everything below is NEW dispatch names — no
     * pre-existing function changed shape, so a program written against
     * the older graph behaves identically. New parameters use GRAPH_NUM
     * (int or float); the old ones keep GET_INT.
     * ═════════════════════════════════════════════════════════════════ */

    /* graph.draw_image_rot(win, img, x, y, rot [, scale]) → nil
     *
     * draw_image with a rotation, and nothing else to think about. The pivot is
     * the image's own centre — that is what makes a sprite point where it moves
     * instead of orbiting its top-left corner — while (x, y) stays the top-left
     * position, exactly as in draw_image. So rot = 0 draws the identical pixels
     * draw_image would, which is worth relying on and is covered by a test.
     *
     * For a spritesheet region, a custom pivot, tint or alpha, use draw_sprite. */
    if (!strcmp(fn_name,"draw_image_rot")) {
        NEED(5); GET_WIN(0,win);
        FluxaImageBuf *b = (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>0
                            && args[1].as.dyn->items[0].type==VAL_PTR)
                           ? (FluxaImageBuf *)args[1].as.dyn->items[0].as.ptr : NULL;
        if (!fluxa_imgbuf_valid(b)) GRAPH_ERR("draw_image_rot: expected a live image handle");
        GRAPH_NUM(2,dx); GRAPH_NUM(3,dy); GRAPH_NUM(4,rot);
        double scale = 1.0;
        if (argc >= 6) {
            GRAPH_NUM(5,s);
            if (s <= 0.0) GRAPH_ERR("draw_image_rot: scale must be positive");
            scale = s;
        }
#ifdef FLUXA_GRAPH_RAYLIB
        Texture2D *tex = graph_img_texture(b);
        if (!tex) GRAPH_ERR("draw_image_rot: out of memory");
        float w = (float)b->width * (float)scale;
        float h = (float)b->height * (float)scale;
        Rectangle src = {0.0f, 0.0f, (float)b->width, (float)b->height};
        /* Destination is centred on the image's middle, and the pivot is that
         * same centre, so (x, y) keeps meaning top-left at any angle. */
        Rectangle dst = {(float)dx + w*0.5f, (float)dy + h*0.5f, w, h};
        Vector2   piv = {w*0.5f, h*0.5f};
        DrawTexturePro(*tex, src, dst, piv, (float)rot, WHITE);
#else
        (void)win; (void)b; (void)dx; (void)dy; (void)rot; (void)scale;
#endif
        return graph_nil();
    }

    /* graph.draw_image_tint(win,img,x,y,r,g,b,a,scale) → nil
     * Draw the whole image with color/alpha modulation without changing its
     * RGBA bytes. It shares the image's versioned texture cache with the other
     * image draw paths, so update_rgba triggers one upload on the next draw. */
    if (!strcmp(fn_name,"draw_image_tint")) {
        NEED(9); GET_WIN(0,win);
        FluxaImageBuf *b = (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>0
                            && args[1].as.dyn->items[0].type==VAL_PTR)
                           ? (FluxaImageBuf *)args[1].as.dyn->items[0].as.ptr : NULL;
        if (!fluxa_imgbuf_valid(b)) GRAPH_ERR("draw_image_tint: expected a live image handle");
        GRAPH_NUM(2,dx); GRAPH_NUM(3,dy);
        GET_INT(4,cr); GET_INT(5,cg); GET_INT(6,cb); GET_INT(7,ca);
        GRAPH_NUM(8,scale);
        if (cr<0 || cr>255 || cg<0 || cg>255 ||
            cb<0 || cb>255 || ca<0 || ca>255)
            GRAPH_ERR("draw_image_tint: color components must be in the 0..255 range");
        if (scale<=0.0) GRAPH_ERR("draw_image_tint: scale must be positive");
#ifdef FLUXA_GRAPH_RAYLIB
        Texture2D *tex=graph_img_texture(b);
        if (!tex) GRAPH_ERR("draw_image_tint: out of memory");
        DrawTextureEx(*tex,(Vector2){(float)dx,(float)dy},0.0f,(float)scale,
                      (Color){(unsigned char)cr,(unsigned char)cg,
                              (unsigned char)cb,(unsigned char)ca});
#else
        (void)win; (void)b; (void)dx; (void)dy; (void)cr; (void)cg;
        (void)cb; (void)ca; (void)scale;
#endif
        return graph_nil();
    }

    /* graph.draw_sprite(win, img, sx, sy, sw, sh, dx, dy, rot, r, g, b, a) → nil
     *
     * The full-control form: take the (sx, sy, sw, sh) region of a spritesheet,
     * draw it at (dx, dy) rotated about its own centre, tinted with r/g/b and
     * blended with alpha a (0-255). 13 arguments, which is inside the runtime's
     * 16-argument dispatch limit with margin to spare. */
    if (!strcmp(fn_name,"draw_sprite")) {
        NEED(13); GET_WIN(0,win);
        FluxaImageBuf *b = (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>0
                            && args[1].as.dyn->items[0].type==VAL_PTR)
                           ? (FluxaImageBuf *)args[1].as.dyn->items[0].as.ptr : NULL;
        if (!fluxa_imgbuf_valid(b)) GRAPH_ERR("draw_sprite: expected a live image handle");
        GRAPH_NUM(2,sx); GRAPH_NUM(3,sy); GRAPH_NUM(4,sw); GRAPH_NUM(5,sh);
        GRAPH_NUM(6,dx); GRAPH_NUM(7,dy); GRAPH_NUM(8,rot);
        GET_INT(9,cr); GET_INT(10,cg); GET_INT(11,cb); GET_INT(12,ca);
        if (sw <= 0.0 || sh <= 0.0)
            GRAPH_ERR("draw_sprite: source width and height must be positive");
        /* A region reaching past the sheet would sample undefined texels. */
        if (sx < 0.0 || sy < 0.0 ||
            sx + sw > (double)b->width || sy + sh > (double)b->height)
            GRAPH_ERR("draw_sprite: source rectangle falls outside the image");
#ifdef FLUXA_GRAPH_RAYLIB
        Texture2D *tex = graph_img_texture(b);
        if (!tex) GRAPH_ERR("draw_sprite: out of memory");
        Rectangle src = {(float)sx,(float)sy,(float)sw,(float)sh};
        Rectangle dst = {(float)dx + (float)sw*0.5f, (float)dy + (float)sh*0.5f,
                         (float)sw,(float)sh};
        Vector2   piv = {(float)sw*0.5f,(float)sh*0.5f};
        DrawTexturePro(*tex, src, dst, piv, (float)rot,
            (Color){(unsigned char)cr,(unsigned char)cg,
                    (unsigned char)cb,(unsigned char)ca});
#else
        (void)win; (void)b; (void)sx; (void)sy; (void)sw; (void)sh;
        (void)dx; (void)dy; (void)rot; (void)cr; (void)cg; (void)cb; (void)ca;
#endif
        return graph_nil();
    }

    /* ── Outline and ring shapes ─────────────────────────────────────
     * The filled forms (draw_rect, draw_circle) already exist and are
     * untouched; these are the outline counterparts, plus a ring. */

    if (!strcmp(fn_name,"draw_rect_lines")) {
        NEED(7); GET_WIN(0,win);
        GRAPH_NUM(1,x); GRAPH_NUM(2,y); GRAPH_NUM(3,w); GRAPH_NUM(4,h);
        GET_INT(5,r); GET_INT(6,g); NEED(8); GET_INT(7,bl);
        GRAPH_ALPHA(8,ca);
        (void)win;(void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)bl;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawRectangleLines((int)x,(int)y,(int)w,(int)h,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)bl,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_circle_lines")) {
        NEED(6); GET_WIN(0,win);
        GRAPH_NUM(1,x); GRAPH_NUM(2,y); GRAPH_NUM(3,rad);
        GET_INT(4,r); GET_INT(5,g); GET_INT(6,bl);
        GRAPH_ALPHA(7,ca);
        if (rad < 0.0) GRAPH_ERR("draw_circle_lines: radius must not be negative");
        (void)win;(void)x;(void)y;(void)rad;(void)r;(void)g;(void)bl;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawCircleLines((int)x,(int)y,(float)rad,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)bl,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_ring")) {
        NEED(7); GET_WIN(0,win);
        GRAPH_NUM(1,x); GRAPH_NUM(2,y); GRAPH_NUM(3,inner); GRAPH_NUM(4,outer);
        GET_INT(5,r); GET_INT(6,g); NEED(8); GET_INT(7,bl);
        GRAPH_ALPHA(8,ca);
        if (inner < 0.0 || outer < inner)
            GRAPH_ERR("draw_ring: need 0 <= inner_radius <= outer_radius");
        (void)win;(void)x;(void)y;(void)inner;(void)outer;(void)r;(void)g;(void)bl;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawRing((Vector2){(float)x,(float)y},(float)inner,(float)outer,
                 0.0f,360.0f,64,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)bl,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_triangle")) {
        NEED(9); GET_WIN(0,win);
        GRAPH_NUM(1,x1); GRAPH_NUM(2,y1); GRAPH_NUM(3,x2); GRAPH_NUM(4,y2);
        GRAPH_NUM(5,x3); GRAPH_NUM(6,y3);
        GET_INT(7,r); GET_INT(8,g); NEED(10); GET_INT(9,bl);
        GRAPH_ALPHA(10,ca);
        (void)win;(void)x1;(void)y1;(void)x2;(void)y2;(void)x3;(void)y3;
        (void)r;(void)g;(void)bl;
#ifdef FLUXA_GRAPH_RAYLIB
        /* Raylib fills only counter-clockwise triangles; order the vertices by
         * the sign of the cross product so either winding draws. */
        Vector2 a={(float)x1,(float)y1},bb={(float)x2,(float)y2},c={(float)x3,(float)y3};
        float cross=(bb.x-a.x)*(c.y-a.y)-(bb.y-a.y)*(c.x-a.x);
        Color col={(unsigned char)r,(unsigned char)g,(unsigned char)bl,255};
        if (cross < 0.0f) DrawTriangle(a,bb,c,col);
        else              DrawTriangle(a,c,bb,col);
#endif
        return graph_nil();
    }

    /* ── Off-screen render targets ───────────────────────────────────
     * An extra draw surface for post-processing or for composing a layer
     * once and blitting it many times. Same cursor discipline as fonts:
     * create it, pass it around as an argument, release it before close. */

    if (!strcmp(fn_name,"render_target")) {
        NEED(3); GET_WIN(0,win);
        GET_INT(1,w); GET_INT(2,h);
        if (w <= 0 || h <= 0) GRAPH_ERR("render_target: width and height must be positive");
        if (w > 16384 || h > 16384) GRAPH_ERR("render_target: size exceeds 16384 px");
        GraphRT *rt = graph_new_rt((int)w,(int)h);
        if (!rt) GRAPH_ERR("render_target: could not create the render target");
        (void)win;
        return graph_wrap_rt(rt);
    }

    if (!strcmp(fn_name,"begin_render_target")) {
        NEED(2); GET_WIN(0,win); GRAPH_RT(1,rt);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        EndTextureMode();                    /* leave the frame's target */
        BeginTextureMode(rt->target);
#else
        (void)rt;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"end_render_target")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        EndTextureMode();
        if (win->has_target) BeginTextureMode(win->target);   /* back to the frame */
#else
        (void)win;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_render_target")) {
        NEED(4); GET_WIN(0,win); GRAPH_RT(1,rt);
        GRAPH_NUM(2,x); GRAPH_NUM(3,y);
        (void)win;(void)rt;(void)x;(void)y;
#ifdef FLUXA_GRAPH_RAYLIB
        /* A render texture is stored bottom-up, so the source height is
         * negated to present it the right way round. */
        Rectangle src={0.0f,0.0f,(float)rt->target.texture.width,
                       -(float)rt->target.texture.height};
        DrawTextureRec(rt->target.texture,src,(Vector2){(float)x,(float)y},WHITE);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"release_render_target")) {
        NEED(2); GET_WIN(0,win); GRAPH_RT(1,rt);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        if (rt->has_target) { UnloadRenderTexture(rt->target); rt->has_target=0; }
#endif
        free(rt);
        /* Null the cursor so a second release is a no-op rather than a
         * double free — same close discipline the font cursor uses. */
        if (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>=1)
            args[1].as.dyn->items[0].as.ptr = NULL;
        return graph_nil();
    }

    /* ── Render states ───────────────────────────────────────────────── */

    if (!strcmp(fn_name,"set_blend_mode")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,mode);
        (void)win;
        int known = (!strcmp(mode,"ALPHA") || !strcmp(mode,"ADD") ||
                     !strcmp(mode,"MULTIPLY") || !strcmp(mode,"NONE"));
        if (!known) GRAPH_ERR("set_blend_mode: expected ALPHA, ADD, MULTIPLY or NONE");
#ifdef FLUXA_GRAPH_RAYLIB
        if (!strcmp(mode,"NONE")) EndBlendMode();
        else BeginBlendMode(!strcmp(mode,"ADD")      ? BLEND_ADDITIVE :
                            !strcmp(mode,"MULTIPLY") ? BLEND_MULTIPLIED :
                                                       BLEND_ALPHA);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"scissor")) {
        NEED(5); GET_WIN(0,win);
        GRAPH_NUM(1,x); GRAPH_NUM(2,y); GRAPH_NUM(3,w); GRAPH_NUM(4,h);
        if (w < 0.0 || h < 0.0) GRAPH_ERR("scissor: width and height must not be negative");
        (void)win;(void)x;(void)y;(void)w;(void)h;
#ifdef FLUXA_GRAPH_RAYLIB
        BeginScissorMode((int)x,(int)y,(int)w,(int)h);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"scissor_off")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        EndScissorMode();
#endif
        return graph_nil();
    }

    /* ── Text metrics and character input ────────────────────────────── */

    if (!strcmp(fn_name,"text_height")) {
        NEED(3); GET_WIN(0,win); GET_FONT(1,font); GET_INT(2,size);
        (void)win;
        if (size <= 0) GRAPH_ERR("text_height: size must be positive");
#ifdef FLUXA_GRAPH_RAYLIB
        Vector2 m = MeasureTextEx(font->font,"Ay",(float)size,1.0f);
        return graph_int((long)m.y);
#else
        (void)font;
        return graph_int(size);
#endif
    }

    /* Returns the Unicode code point typed this frame, or 0 when nothing was.
     * Call it in a loop to drain the queue: a fast typist can enter more than
     * one character between frames. */
    if (!strcmp(fn_name,"char_pressed")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_int(GetCharPressed());
#else
        return graph_int(0);
#endif
    }

    /* ── Mouse buttons and wheel ─────────────────────────────────────
     * mouse_pressed (left button only) still exists and is unchanged; these
     * are the general forms. */

    if (!strcmp(fn_name,"mouse_btn_pressed") || !strcmp(fn_name,"mouse_btn_down")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,btn); (void)win;
        int code = graph_mouse_btn_code(btn);
        if (code < 0) GRAPH_ERR("expected button LEFT, RIGHT or MIDDLE");
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(!strcmp(fn_name,"mouse_btn_pressed")
                          ? IsMouseButtonPressed(code) : IsMouseButtonDown(code));
#else
        return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_wheel")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        float m = GetMouseWheelMove();
        return graph_int(m > 0.0f ? 1 : (m < 0.0f ? -1 : 0));
#else
        return graph_int(0);
#endif
    }

    /* ── Gamepad ─────────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"pad_connected")) {
        NEED(2); GET_WIN(0,win); GET_INT(1,id); (void)win;
        if (id < 0 || id > 3) GRAPH_ERR("pad_connected: gamepad id must be 0..3");
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(IsGamepadAvailable((int)id));
#else
        return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"pad_pressed") || !strcmp(fn_name,"pad_down")) {
        NEED(3); GET_WIN(0,win); GET_INT(1,id); GET_STR(2,btn); (void)win;
        if (id < 0 || id > 3) GRAPH_ERR("gamepad id must be 0..3");
        int code = graph_pad_btn_code(btn);
        if (code < 0) GRAPH_ERR("unknown gamepad button "
            "(A, B, X, Y, LB, RB, START, SELECT, UP, DOWN, LEFT, RIGHT)");
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(!strcmp(fn_name,"pad_pressed")
                          ? IsGamepadButtonPressed((int)id,code)
                          : IsGamepadButtonDown((int)id,code));
#else
        return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"pad_axis")) {
        NEED(3); GET_WIN(0,win); GET_INT(1,id); GET_STR(2,axis); (void)win;
        if (id < 0 || id > 3) GRAPH_ERR("pad_axis: gamepad id must be 0..3");
        int code = graph_pad_axis_code(axis);
        if (code < 0) GRAPH_ERR("pad_axis: expected LEFT_X, LEFT_Y, RIGHT_X, "
                                "RIGHT_Y, LT or RT");
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_float((double)GetGamepadAxisMovement((int)id,code));
#else
        return graph_float(0.0);
#endif
    }

    /* ── 2D camera ───────────────────────────────────────────────────
     * Between begin_cam2d and end_cam2d, drawing happens in world
     * coordinates. screen_to_world / world_to_screen convert either way and
     * are what a mouse click needs to become a position in the world. */

    if (!strcmp(fn_name,"begin_cam2d")) {
        NEED(5); GET_WIN(0,win);
        GRAPH_NUM(1,x); GRAPH_NUM(2,y); GRAPH_NUM(3,rot); GRAPH_NUM(4,zoom);
        if (zoom <= 0.0) GRAPH_ERR("begin_cam2d: zoom must be positive");
        graph_cam_set(win,x,y,rot,zoom);
#ifdef FLUXA_GRAPH_RAYLIB
        Camera2D cam;
        cam.target   = (Vector2){(float)x,(float)y};
        cam.offset   = (Vector2){(float)win->width*0.5f,(float)win->height*0.5f};
        cam.rotation = (float)rot;
        cam.zoom     = (float)zoom;
        BeginMode2D(cam);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"end_cam2d")) {
        NEED(1); GET_WIN(0,win);
        graph_cam_clear(win);
#ifdef FLUXA_GRAPH_RAYLIB
        EndMode2D();
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"screen_to_world") || !strcmp(fn_name,"world_to_screen")) {
        NEED(3); GET_WIN(0,win); GRAPH_NUM(1,px); GRAPH_NUM(2,py);
        double ox, oy;
        /* Computed from the stored camera on both backends so the maths is
         * identical with or without a display — a headless test can check the
         * round trip, and the stub is not a second implementation that could
         * drift from the real one. */
        graph_cam_transform(win,px,py,!strcmp(fn_name,"screen_to_world"),&ox,&oy);
        FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) GRAPH_ERR("out of memory");
        memset(d,0,sizeof(*d));
        d->items=(Value *)malloc(sizeof(Value)*2);
        if (!d->items) { free(d); GRAPH_ERR("out of memory"); }
        d->items[0]=graph_float(ox); d->items[1]=graph_float(oy);
        d->count=2; d->cap=2;
        Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
    }

    /* ── Window control and cursor ───────────────────────────────────── */

    if (!strcmp(fn_name,"set_window_title")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,title); (void)win;(void)title;
#ifdef FLUXA_GRAPH_RAYLIB
        SetWindowTitle(title);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"set_window_size")) {
        NEED(3); GET_WIN(0,win); GET_INT(1,w); GET_INT(2,h);
        if (w <= 0 || h <= 0) GRAPH_ERR("set_window_size: width and height must be positive");
        if (w > 16384 || h > 16384) GRAPH_ERR("set_window_size: size exceeds 16384 px");
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        SetWindowSize((int)w,(int)h);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"hide_cursor") || !strcmp(fn_name,"show_cursor")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        if (!strcmp(fn_name,"hide_cursor")) HideCursor(); else ShowCursor();
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"clear")) {
        NEED(4); GET_WIN(0,win);
        GET_INT(1,r); GET_INT(2,g); GET_INT(3,b);
        (void)win; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        ClearBackground((Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"fps")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_int(GetFPS());
#else
        return graph_int(win->fps_target);
#endif
    }

    if (!strcmp(fn_name,"fullscreen")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        (void)win;
        ToggleFullscreen();
        return graph_bool(IsWindowFullscreen());
#else
        win->fullscreen = !win->fullscreen;
        return graph_bool(win->fullscreen);
#endif
    }

    /* graph.is_fullscreen(win) → bool : report the current mode WITHOUT changing
     * it. graph.fullscreen() toggles, so there was no way to ask; mirroring the
     * state in the program desyncs as soon as the window manager changes it
     * (alt+enter, a WM shortcut), which is exactly when you need the truth. */
    if (!strcmp(fn_name,"is_fullscreen")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        (void)win;
        return graph_bool(IsWindowFullscreen());
#else
        return graph_bool(win->fullscreen);
#endif
    }

    if (!strcmp(fn_name,"set_fps")) {
        NEED(2); GET_WIN(0,win); GET_INT(1,fps);
        win->fps_target = (int)fps;
#ifdef FLUXA_GRAPH_RAYLIB
        SetTargetFPS((int)fps);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_rect")) {
        NEED(8); GET_WIN(0,win);
        GET_INT(1,x); GET_INT(2,y); GET_INT(3,w); GET_INT(4,h);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        GRAPH_ALPHA(8,ca);
        (void)win; (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawRectangle((int)x,(int)y,(int)w,(int)h,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_circle")) {
        NEED(7); GET_WIN(0,win);
        GET_INT(1,x); GET_INT(2,y); GET_INT(3,radius);
        GET_INT(4,r); GET_INT(5,g); GET_INT(6,b);
        GRAPH_ALPHA(7,ca);
        (void)win; (void)x; (void)y; (void)radius; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawCircle((int)x,(int)y,(float)radius,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_line")) {
        NEED(8); GET_WIN(0,win);
        GET_INT(1,x1); GET_INT(2,y1); GET_INT(3,x2); GET_INT(4,y2);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        GRAPH_ALPHA(8,ca);
        (void)win; (void)x1; (void)y1; (void)x2; (void)y2; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawLine((int)x1,(int)y1,(int)x2,(int)y2,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_text")) {
        NEED(8); GET_WIN(0,win);
        GET_STR(1,text); GET_INT(2,x); GET_INT(3,y); GET_INT(4,size);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        GRAPH_ALPHA(8,ca);
        (void)win; (void)text; (void)x; (void)y; (void)size; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawText(text,(int)x,(int)y,(int)size,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    /* ── graph.load_font(win, path, size) → dyn font cursor ─────── */
    if (!strcmp(fn_name,"load_font")) {
        NEED(3); GET_WIN(0,win); GET_STR(1,path); GET_INT(2,size);
        (void)win;
        if (size < 1 || size > 512) GRAPH_ERR("font size must be 1-512");
        GraphFont *f = graph_new_font(path, (int)size);
        if (!f) GRAPH_ERR("cannot open font file");
#ifdef FLUXA_GRAPH_RAYLIB
        if (!f->loaded) {
            free(f);
            GRAPH_ERR("failed to load font (unsupported or corrupt file)");
        }
#endif
        return graph_wrap_font(f);
    }

    /* ── graph.draw_text_font(win, font, text, x, y, size, r, g, b) ─ */
    if (!strcmp(fn_name,"draw_text_font")) {
        NEED(9); GET_WIN(0,win); GET_FONT(1,fnt);
        GET_STR(2,text); GET_INT(3,x); GET_INT(4,y); GET_INT(5,size);
        GET_INT(6,r); GET_INT(7,g); GET_INT(8,b);
        GRAPH_ALPHA(9,ca);
        (void)win; (void)fnt; (void)text; (void)x; (void)y; (void)size;
        (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawTextEx(fnt->font, text,
            (Vector2){(float)x,(float)y}, (float)size,
            (float)size / 10.0f,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    /* ── graph.text_width(win, font, text, size) → int (pixels) ──── */
    if (!strcmp(fn_name,"text_width")) {
        NEED(4); GET_WIN(0,win); GET_FONT(1,fnt);
        GET_STR(2,text); GET_INT(3,size);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        Vector2 m = MeasureTextEx(fnt->font, text, (float)size,
                                  (float)size / 10.0f);
        return graph_int((long)m.x);
#else
        /* deterministic stub metric: ~0.6em average advance per byte */
        (void)fnt;
        return graph_int((long)strlen(text) * size * 6 / 10);
#endif
    }

    /* ── graph.unload_font(win, font) → nil ──────────────────────── */
    if (!strcmp(fn_name,"unload_font")) {
        NEED(2); GET_WIN(0,win); GET_FONT(1,fnt);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        if (fnt->loaded) UnloadFont(fnt->font);
#endif
        free(fnt);
        if (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>=1)
            args[1].as.dyn->items[0].as.ptr=NULL;   /* prevent double-free */
        return graph_nil();
    }

    if (!strcmp(fn_name,"key_pressed")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,key);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        {
            int multi = 0;
            if (graph_multi_key(key, 0, &multi)) return graph_bool(multi);
        }
        return graph_bool(IsKeyPressed(graph_key_code(key)));
#else
        (void)key; return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"key_down")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,key);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        {
            int multi = 0;
            if (graph_multi_key(key, 1, &multi)) return graph_bool(multi);
        }
        return graph_bool(IsKeyDown(graph_key_code(key)));
#else
        (void)key; return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_x")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sy = (float)sh / (float)win->height;
            if (sy < scale) scale = sy;
            float ox = (sw - win->width * scale) * 0.5f;
            int lx = (int)(((float)GetMouseX() - ox) / scale);
            return graph_int(lx);
        }
        return graph_int(GetMouseX());
#else
        (void)win; return graph_int(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_y")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sx = (float)sh / (float)win->height;
            if (sx < scale) scale = sx;
            float oy = (sh - win->height * scale) * 0.5f;
            int ly = (int)(((float)GetMouseY() - oy) / scale);
            return graph_int(ly);
        }
        return graph_int(GetMouseY());
#else
        (void)win; return graph_int(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_pressed")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
#else
        return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"dt")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_float((double)GetFrameTime());
#else
        return graph_float(1.0 / (double)(win->fps_target > 0 ? win->fps_target : 60));
#endif
    }

    /* ── graph.draw_tris(win, verts, count [, img] [, uvs] [, colors]) → int
     * ── graph.draw_tris3d(win, verts, count [, img] [, uvs] [, colors]) → int
     *
     * A batch of triangles with an optional bound texture, per-vertex texture
     * coordinates and per-vertex colour — what draw_triangle cannot express,
     * and deliberately not offered per triangle: one call per triangle would
     * put the cost straight back into the interpreter, which is the thing the
     * batch shape exists to avoid.
     *
     *   verts   2D: 6 numbers per triangle (x, y per vertex), screen space.
     *           3D: 9 numbers per triangle (x, y, z), world space — call it
     *           between graph.begin_3d and graph.end_3d.
     *   img     an image handle to bind as texture, or nil for untextured.
     *   uvs     6 numbers per triangle (u, v per vertex, 0..1), or nil.
     *   colors  12 numbers per triangle (r, g, b, a per vertex), or nil for
     *           opaque white.
     *
     * Returns the number of triangles submitted. */
    if (!strcmp(fn_name,"draw_tris") || !strcmp(fn_name,"draw_tris3d")) {
        const int dim = (fn_name[9] == '3') ? 3 : 2;   /* draw_tris3d vs draw_tris */
        NEED(3); GET_WIN(0,win);
        if (args[1].type!=VAL_ARR || !args[1].as.arr.data)
            GRAPH_ERR("draw_tris: verts must be an arr");
        GET_INT(2,tri_count);
        if (tri_count < 0) GRAPH_ERR("draw_tris: count must not be negative");
        if (tri_count == 0) return graph_int(0);
        if ((long)args[1].as.arr.size < tri_count*3*dim)
            GRAPH_ERR("draw_tris: verts holds fewer numbers than the count needs");
        const Value *vp = args[1].as.arr.data;

        FluxaImageBuf *img = NULL;
        if (argc >= 4 && args[3].type != VAL_NIL) {
            img = (args[3].type==VAL_DYN && args[3].as.dyn &&
                   args[3].as.dyn->count>0 &&
                   args[3].as.dyn->items[0].type==VAL_PTR)
                  ? (FluxaImageBuf *)args[3].as.dyn->items[0].as.ptr : NULL;
            if (!fluxa_imgbuf_valid(img))
                GRAPH_ERR("draw_tris: img must be a live image handle or nil");
        }
        const Value *uv = NULL;
        if (argc >= 5 && args[4].type == VAL_ARR && args[4].as.arr.data) {
            if ((long)args[4].as.arr.size < tri_count*6)
                GRAPH_ERR("draw_tris: uvs holds fewer than count*6 numbers");
            uv = args[4].as.arr.data;
        } else if (argc >= 5 && args[4].type != VAL_NIL) {
            GRAPH_ERR("draw_tris: uvs must be an arr or nil");
        }
        const Value *co = NULL;
        if (argc >= 6 && args[5].type == VAL_ARR && args[5].as.arr.data) {
            if ((long)args[5].as.arr.size < tri_count*12)
                GRAPH_ERR("draw_tris: colors holds fewer than count*12 numbers");
            co = args[5].as.arr.data;
        } else if (argc >= 6 && args[5].type != VAL_NIL) {
            GRAPH_ERR("draw_tris: colors must be an arr or nil");
        }
        (void)win; (void)vp; (void)uv; (void)co; (void)img;
#ifdef FLUXA_GRAPH_RAYLIB
        Texture2D *tex = NULL;
        if (img) {
            tex = graph_img_texture(img);
            if (!tex) GRAPH_ERR("draw_tris: out of memory uploading the texture");
        }
        /* rlgl batches in runs of RL_TRIANGLES; rlCheckRenderBatchLimit gives
         * the batch a chance to flush before a run would overflow it. */
        rlSetTexture(tex ? tex->id : 0);
        for (long t = 0; t < tri_count; t++) {
            rlBegin(RL_TRIANGLES);
            for (int k = 0; k < 3; k++) {
                long vi = (t*3 + k);
                if (co) {
                    long c = vi*4;
                    rlColor4ub((unsigned char)graph_arr_u8(co,c),
                               (unsigned char)graph_arr_u8(co,c+1),
                               (unsigned char)graph_arr_u8(co,c+2),
                               (unsigned char)graph_arr_u8(co,c+3));
                } else {
                    rlColor4ub(255,255,255,255);
                }
                if (uv) rlTexCoord2f(graph_arr_f(uv,vi*2), graph_arr_f(uv,vi*2+1));
                else    rlTexCoord2f(0.0f, 0.0f);
                if (dim == 3)
                    rlVertex3f(graph_arr_f(vp,vi*3), graph_arr_f(vp,vi*3+1),
                               graph_arr_f(vp,vi*3+2));
                else
                    rlVertex2f(graph_arr_f(vp,vi*2), graph_arr_f(vp,vi*2+1));
            }
            rlEnd();
        }
        rlSetTexture(0);
#endif
        return graph_int(tri_count);
    }

    /* ── graph.camera3d(px,py,pz, tx,ty,tz [, fovy]) → dyn ────────── */
    if (!strcmp(fn_name,"camera3d")) {
        NEED(6);
        GRAPH_NUM(0,px); GRAPH_NUM(1,py); GRAPH_NUM(2,pz);
        GRAPH_NUM(3,tx); GRAPH_NUM(4,ty); GRAPH_NUM(5,tz);
        double fovy = 45.0;
        if (argc >= 7) { GRAPH_NUM(6,f); fovy = f; }
        if (fovy <= 0.0 || fovy >= 180.0)
            GRAPH_ERR("camera3d: fovy must be between 0 and 180 degrees");
        GraphCam3 *c = (GraphCam3 *)calloc(1,sizeof(GraphCam3));
        if (!c) GRAPH_ERR("camera3d: out of memory");
        c->px=px; c->py=py; c->pz=pz;
        c->tx=tx; c->ty=ty; c->tz=tz; c->fovy=fovy;
        return graph_wrap_cam3(c);
    }

    /* ── graph.camera3d_set(cam, px,py,pz, tx,ty,tz [, fovy]) → nil ─
     * Moving a camera every frame should not allocate one every frame. */
    if (!strcmp(fn_name,"camera3d_set")) {
        NEED(7); GET_CAM3(0,c);
        GRAPH_NUM(1,px); GRAPH_NUM(2,py); GRAPH_NUM(3,pz);
        GRAPH_NUM(4,tx); GRAPH_NUM(5,ty); GRAPH_NUM(6,tz);
        if (argc >= 8) {
            GRAPH_NUM(7,f);
            if (f <= 0.0 || f >= 180.0)
                GRAPH_ERR("camera3d_set: fovy must be between 0 and 180 degrees");
            c->fovy = f;
        }
        c->px=px; c->py=py; c->pz=pz;
        c->tx=tx; c->ty=ty; c->tz=tz;
        return graph_nil();
    }

    /* ── graph.camera3d_free(cam) → nil ───────────────────────────── */
    if (!strcmp(fn_name,"camera3d_free")) {
        NEED(1); GET_CAM3(0,c);
        free(c);
        args[0].as.dyn->items[0].as.ptr = NULL;   /* a second free is caught */
        return graph_nil();
    }

    /* ── graph.begin_3d(win, cam) / graph.end_3d(win) → nil ────────
     * Everything drawn between the two is in world space. */
    if (!strcmp(fn_name,"begin_3d")) {
        NEED(2); GET_WIN(0,win); GET_CAM3(1,c);
        (void)win; (void)c;
#ifdef FLUXA_GRAPH_RAYLIB
        Camera3D cam;
        cam.position   = (Vector3){(float)c->px,(float)c->py,(float)c->pz};
        cam.target     = (Vector3){(float)c->tx,(float)c->ty,(float)c->tz};
        cam.up         = (Vector3){0.0f,1.0f,0.0f};
        cam.fovy       = (float)c->fovy;
        cam.projection = CAMERA_PERSPECTIVE;
        BeginMode3D(cam);
#endif
        return graph_nil();
    }
    if (!strcmp(fn_name,"end_3d")) {
        NEED(1); GET_WIN(0,win);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        EndMode3D();
#endif
        return graph_nil();
    }

    /* ── graph.mesh_upload(verts, tri_count [, uvs] [, colors]) → dyn
     *   verts   9 numbers per triangle: x, y, z for each of three vertices
     *   uvs     6 per triangle, or nil
     *   colors  12 per triangle — r, g, b, a per vertex — or nil
     * Uploaded once and drawn many times; that is the whole point of holding
     * a handle instead of passing geometry on every call. */
    if (!strcmp(fn_name,"mesh_upload")) {
        NEED(2);
        if (args[0].type!=VAL_ARR || !args[0].as.arr.data)
            GRAPH_ERR("mesh_upload: verts must be an arr");
        GET_INT(1,tri_count);
        if (tri_count <= 0) GRAPH_ERR("mesh_upload: count must be positive");
        if ((long)args[0].as.arr.size < tri_count*9)
            GRAPH_ERR("mesh_upload: verts holds fewer than count*9 numbers");
        const Value *vp = args[0].as.arr.data;

        const Value *uv = NULL;
        if (argc >= 3 && args[2].type == VAL_ARR && args[2].as.arr.data) {
            if ((long)args[2].as.arr.size < tri_count*6)
                GRAPH_ERR("mesh_upload: uvs holds fewer than count*6 numbers");
            uv = args[2].as.arr.data;
        } else if (argc >= 3 && args[2].type != VAL_NIL) {
            GRAPH_ERR("mesh_upload: uvs must be an arr or nil");
        }

        const Value *co = NULL;
        if (argc >= 4 && args[3].type == VAL_ARR && args[3].as.arr.data) {
            if ((long)args[3].as.arr.size < tri_count*12)
                GRAPH_ERR("mesh_upload: colors holds fewer than count*12 numbers");
            co = args[3].as.arr.data;
        } else if (argc >= 4 && args[3].type != VAL_NIL) {
            GRAPH_ERR("mesh_upload: colors must be an arr or nil");
        }

        (void)vp; (void)uv; (void)co;   /* the stub records shape, not data */
        GraphMesh3 *m = (GraphMesh3 *)calloc(1,sizeof(GraphMesh3));
        if (!m) GRAPH_ERR("mesh_upload: out of memory");
        m->tri_count = tri_count;
        m->has_uv    = uv != NULL;
        m->has_color = co != NULL;
#ifdef FLUXA_GRAPH_RAYLIB
        long vcount = tri_count*3;
        memset(&m->mesh, 0, sizeof(m->mesh));
        m->mesh.vertexCount   = (int)vcount;
        m->mesh.triangleCount = (int)tri_count;
        m->mesh.vertices  = (float *)MemAlloc((unsigned int)(vcount*3*sizeof(float)));
        m->mesh.texcoords = (float *)MemAlloc((unsigned int)(vcount*2*sizeof(float)));
        m->mesh.colors    = (unsigned char *)MemAlloc((unsigned int)(vcount*4));
        if (!m->mesh.vertices || !m->mesh.texcoords || !m->mesh.colors) {
            MemFree(m->mesh.vertices); MemFree(m->mesh.texcoords);
            MemFree(m->mesh.colors); free(m);
            GRAPH_ERR("mesh_upload: out of memory");
        }
        for (long i = 0; i < vcount*3; i++) m->mesh.vertices[i] = graph_arr_f(vp,i);
        for (long i = 0; i < vcount*2; i++)
            m->mesh.texcoords[i] = uv ? graph_arr_f(uv,i) : 0.0f;
        for (long i = 0; i < vcount*4; i++)
            m->mesh.colors[i] = co ? (unsigned char)graph_arr_u8(co,i) : 255;
        UploadMesh(&m->mesh, false);
        m->mat = LoadMaterialDefault();
        m->has_mat = 1;
#endif
        m->uploaded = 1;
        return graph_wrap_mesh3(m);
    }

    /* ── graph.draw_mesh(win, mesh, x, y, z [, scale] [, r,g,b[,a]]) → nil */
    if (!strcmp(fn_name,"draw_mesh")) {
        NEED(5); GET_WIN(0,win); GET_MESH3(1,m);
        GRAPH_NUM(2,mx); GRAPH_NUM(3,my); GRAPH_NUM(4,mz);
        double scale = 1.0;
        if (argc >= 6) { GRAPH_NUM(5,sc); scale = sc; }
        long cr=255,cg=255,cb=255;
        if (argc >= 9) { GET_INT(6,rr); GET_INT(7,gg); GET_INT(8,bb);
                         cr=rr; cg=gg; cb=bb; }
        GRAPH_ALPHA(9,ca);
        (void)win; (void)m; (void)mx; (void)my; (void)mz; (void)scale;
        (void)cr; (void)cg; (void)cb; (void)ca;
        if (!m->uploaded) GRAPH_ERR("draw_mesh: mesh has been freed");
#ifdef FLUXA_GRAPH_RAYLIB
        if (m->has_mat) {
            m->mat.maps[MATERIAL_MAP_DIFFUSE].color =
                (Color){(unsigned char)cr,(unsigned char)cg,
                        (unsigned char)cb,(unsigned char)ca};
        }
        /* Scale then translate, built by hand: raylib keeps the matrix
         * helpers in raymath.h, and one uniform scale with a translation is
         * not worth pulling a second header in for. Translation sits in
         * m12..m14 of raylib's layout. */
        Matrix t;
        memset(&t, 0, sizeof(t));
        t.m0 = (float)scale; t.m5 = (float)scale; t.m10 = (float)scale;
        t.m12 = (float)mx;   t.m13 = (float)my;   t.m14 = (float)mz;
        t.m15 = 1.0f;
        DrawMesh(m->mesh, m->mat, t);
#endif
        return graph_nil();
    }

    /* ── graph.mesh_free(mesh) → nil ──────────────────────────────── */
    if (!strcmp(fn_name,"mesh_free")) {
        NEED(1); GET_MESH3(0,m);
#ifdef FLUXA_GRAPH_RAYLIB
        if (m->uploaded) UnloadMesh(m->mesh);
        if (m->has_mat)  UnloadMaterial(m->mat);
#endif
        m->uploaded = 0;
        free(m);
        args[0].as.dyn->items[0].as.ptr = NULL;
        return graph_nil();
    }

    /* ── graph.draw_cube(win, x,y,z, w,h,l, r,g,b [,a]) → nil ─────── */
    if (!strcmp(fn_name,"draw_cube")) {
        NEED(10); GET_WIN(0,win);
        GRAPH_NUM(1,cx); GRAPH_NUM(2,cy); GRAPH_NUM(3,cz);
        GRAPH_NUM(4,cw); GRAPH_NUM(5,ch); GRAPH_NUM(6,cl);
        GET_INT(7,r); GET_INT(8,g); GET_INT(9,b);
        GRAPH_ALPHA(10,ca);
        (void)win;(void)cx;(void)cy;(void)cz;(void)cw;(void)ch;(void)cl;
        (void)r;(void)g;(void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawCube((Vector3){(float)cx,(float)cy,(float)cz},
                 (float)cw,(float)ch,(float)cl,
                 (Color){(unsigned char)r,(unsigned char)g,
                         (unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    /* ── graph.draw_line3d(win, x1,y1,z1, x2,y2,z2, r,g,b [,a]) → nil */
    if (!strcmp(fn_name,"draw_line3d")) {
        NEED(10); GET_WIN(0,win);
        GRAPH_NUM(1,ax); GRAPH_NUM(2,ay); GRAPH_NUM(3,az);
        GRAPH_NUM(4,bx); GRAPH_NUM(5,by); GRAPH_NUM(6,bz);
        GET_INT(7,r); GET_INT(8,g); GET_INT(9,b);
        GRAPH_ALPHA(10,ca);
        (void)win;(void)ax;(void)ay;(void)az;(void)bx;(void)by;(void)bz;
        (void)r;(void)g;(void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawLine3D((Vector3){(float)ax,(float)ay,(float)az},
                   (Vector3){(float)bx,(float)by,(float)bz},
                   (Color){(unsigned char)r,(unsigned char)g,
                           (unsigned char)b,(unsigned char)ca});
#endif
        return graph_nil();
    }

    /* ── graph.draw_grid(win, slices, spacing) → nil ──────────────── */
    if (!strcmp(fn_name,"draw_grid")) {
        NEED(3); GET_WIN(0,win); GET_INT(1,slices); GRAPH_NUM(2,spacing);
        (void)win; (void)slices; (void)spacing;
        if (slices < 1 || slices > 4096)
            GRAPH_ERR("draw_grid: slices must be between 1 and 4096");
#ifdef FLUXA_GRAPH_RAYLIB
        DrawGrid((int)slices, (float)spacing);
#endif
        return graph_nil();
    }

#undef GRAPH_ERR
#undef NEED
#undef GET_WIN
#undef GET_FONT
#undef GET_CAM3
#undef GET_MESH3
#undef GET_INT
#undef GET_STR

    snprintf(errbuf,sizeof(errbuf),"graph.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line);
    *had_error=1; return graph_nil();
}

FLUXA_LIB_EXPORT(
    name      = "graph",
    toml_key  = "std.graph",
    owner     = "graph",
    call      = fluxa_std_graph_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_GRAPH_H */
